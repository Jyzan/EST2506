#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

#include "hw_memmap.h"
#include "hw_i2c.h"
#include "hw_types.h"
#include "hw_ints.h"
#include "gpio.h"
#include "i2c.h"
#include "pin_map.h"
#include "sysctl.h"
#include "systick.h"
#include "interrupt.h"
#include "uart.h"
#include "timer.h"

#define SYS_CLOCK_HZ         20000000U
#define SYSTICK_HZ           1000U
#define UART_BAUD            115200U
#define LINE_MAX             64    /* §4: max command line length (excl. CR/LF) */
#define DISP_LEN             8     /* §2: number of 7-seg digits */
#define DISPLAY_TIMER_HZ     1000U /* 1 ms scan tick, hardware-driven */
#define KEY_SCAN_TICKS       20U   /* sample TCA keys every 20 scan ticks (ISR) */
#define KEY_DEBOUNCE_COUNT   1U
#define ADD_REPEAT_MS        200U
#define FUNC_LONG_MS         800U
#define SERIAL_LED_MS        100U
#define WEATHER_VALID_MS     1800000UL

#define I2C_BUS_HZ           400000U
#define TCA6424_AUTO_INC     0x80     /* command-byte AI bit: reg auto-increment */
#define TCA6424_I2CADDR      0x22
#define PCA9557_I2CADDR      0x18
#define PCA9557_OUTPUT       0x01
#define PCA9557_CONFIG       0x03
#define TCA6424_INPUT_PORT0  0x00
#define TCA6424_OUTPUT_PORT1 0x05
#define TCA6424_OUTPUT_PORT2 0x06
#define TCA6424_CONFIG_PORT0 0x0c
#define TCA6424_CONFIG_PORT1 0x0d
#define TCA6424_CONFIG_PORT2 0x0e

#define DISP_TIME   0
#define DISP_DATE   1
#define DISP_YEAR   2

#define MODE_DAY    0
#define MODE_NIGHT  1

#define WEATHER_SUN     0
#define WEATHER_CLD     1
#define WEATHER_OVC     2
#define WEATHER_RAI     3
#define WEATHER_SNO     4
#define WEATHER_FOG     5

/* ---- §3 key codes / events ---- */
typedef enum {
    KEY_NONE = 0, KEY_FUNC, KEY_SHIFT, KEY_ADD, KEY_SAVE,
    KEY_DISP, KEY_SPEED, KEY_FORMAT, KEY_EXT, KEY_USER1, KEY_USER2
} key_code_t;
#define KEY_COUNT 10   /* hardware bit = key_code - 1, bits 0..9 */

typedef enum { KEV_NONE, KEV_DOWN, KEV_UP, KEV_LONG, KEV_REPEAT } key_event_t;

/* ---- §9 edit FSM ---- */
typedef enum { ST_IDLE, ST_EDIT_DATE, ST_EDIT_TIME, ST_EDIT_ALARM } edit_state_t;

/* ---- §7 display format ---- */
typedef enum { FMT_LEFT, FMT_RIGHT } format_t;

/* ---- §6 date / time ---- */
typedef struct { uint16_t y; uint8_t m, d, wday; } date_t;
typedef struct { uint8_t h, mi, s; } time_t_;

/* ---- §8 alarm ---- */
typedef struct { time_t_ t; uint8_t enabled, ringing; } alarm_t;

/* ============================ §1 system timebase ============================ */
static uint32_t g_sys_clock;
volatile uint32_t g_tick_ms;
volatile uint8_t flag_5ms, flag_10ms, flag_100ms, flag_1s;

/* ============================ §4 UART RX/TX state ============================ */
volatile uint8_t rx_line_ready;     /* number of complete lines waiting in the ring */
char cmd_line[LINE_MAX];            /* current command line handed to Process_Command */
static volatile bool g_line_too_long;

#define RX_RING_SIZE 256U
static char g_rx_ring[RX_RING_SIZE];
static volatile uint16_t g_rx_head;
static volatile uint16_t g_rx_tail;
/* The RX ISR assembles a line here so a partial (or over-length) line is never
   pushed into the ring; only complete lines are committed atomically. */
static char g_rx_asm[LINE_MAX + 2];
static uint8_t g_rx_asm_len;
static bool g_rx_asm_ovf;

#define UART_TX_SIZE 512U
static char g_uart_tx[UART_TX_SIZE];
static volatile uint16_t g_tx_head;
static volatile uint16_t g_tx_tail;

/* ============================ §6/§8 clock state ============================ */
static date_t   g_date = {2026, 6, 1, 0};
static time_t_  g_time = {0, 0, 0};
static date_t   g_edit_date = {2026, 6, 1, 0};
static time_t_  g_edit_time = {0, 0, 0};
static alarm_t  g_alarm = {{12, 0, 0}, 0, 0};
static alarm_t  g_edit_alarm = {{12, 0, 0}, 0, 0};
/* alarm runtime fields (doc struct is the minimal set) */
static uint32_t g_alarm_ring_start_ms = 0;
static uint32_t g_alarm_last_beep_ms = 0;
static uint8_t  g_alarm_beep_on = 0;
static uint16_t g_ring_limit_ms = 10000;

/* ============================ mode / misc state ============================ */
static format_t g_format = FMT_LEFT;
static uint8_t g_mode = MODE_DAY;
static uint8_t g_display_mode = DISP_TIME;
static uint8_t g_scroll_speed = 0;
static edit_state_t g_edit_state = ST_IDLE;
static uint8_t g_edit_field = 0;
static uint32_t g_edit_last_ms = 0;
static bool g_blink_on = true;

static char g_weather_text[17] = "--C---";
static int8_t g_weather_temp = 0;
static uint8_t g_weather_code = WEATHER_CLD;
static bool g_weather_valid = false;
static uint32_t g_weather_update_ms = 0;
static uint32_t g_weather_until_ms = 0;
static bool g_countdown_active = false;
static uint32_t g_countdown_end_ms = 0;

/* ============================ §2 display state ============================ */
uint8_t disp_buf[DISP_LEN];           /* per-digit ASCII character */
uint8_t disp_dp[DISP_LEN];            /* per-digit decimal point (0/1) */
uint8_t disp_blink_mask = 0;          /* per-digit blink enable (0 = steady) */
uint8_t disp_on = 1;                  /* whole-display on/off */
static uint8_t g_scan_pos = 0;
static uint8_t g_scan_limit = 8;

static uint8_t g_led_byte = 0x00;
static volatile bool g_led_dirty = true;
static bool g_led_override = false;
static uint32_t g_led_override_ms = 0;   /* time of last *SET:LED, for 10s auto-release */

static volatile uint8_t g_tca_key_raw = 0;        /* key bits sampled in timer ISR */
static volatile uint8_t g_i2c_fail_count = 0;     /* consecutive scan I2C failures */
static volatile uint16_t g_i2c_recover_count = 0; /* total bus recoveries */
static uint16_t g_i2c_recover_reported = 0;
static volatile bool g_i2c_recover_request = false;

static uint32_t g_serial_activity_until_ms = 0;
static volatile bool g_tx_is_led_evt = false;  /* gate: LED-event TX must not re-arm RXTX */
static char g_message[33] = "";
static uint32_t g_message_dp = 0;           /* dp bitmap for message display (1 bit/char) */
static uint32_t g_message_until_ms = 0;
static bool g_ntp_synced = false;
static uint32_t g_ntp_last_sync_ms = 0;

static char g_last_sent_disp[9] = "";
static uint8_t g_last_sent_dp = 0xff;
static uint8_t g_last_sent_led = 0xff;

/* ============================ §7 scroll module ============================ */
static char scroll_buf[40];           /* §7: N >= 32 */
static uint8_t scroll_len = 0;
static uint32_t scroll_src_dp = 0;    /* dp bit per source character (0..scroll_len-1) */
static uint8_t scroll_off = 0;        /* marquee step index s (0..scroll_len+6) */
static uint32_t scroll_last_ms = 0;
static char g_scroll_cur[40] = "";    /* last content handed to Scroll_Set */
static bool scroll_completed = false; /* set true once the last frame was shown */

/* ============================ §3 key module ============================ */
static uint16_t g_key_stable = 0;
static uint16_t g_key_last_raw = 0;
static uint8_t g_key_same_count = 0;
static uint8_t g_key_fsm[KEY_COUNT];        /* 0 idle, 1 pressed, 2 long */
static uint32_t g_key_press_ms[KEY_COUNT];
static uint32_t g_key_repeat_ms[KEY_COUNT];
static bool g_func_armed = false;           /* FUNC pressed while editing (defer) */
static bool g_user1_armed = false;

#define KEY_EVQ_SIZE 16U
static key_code_t g_evq_code[KEY_EVQ_SIZE];
static key_event_t g_evq_ev[KEY_EVQ_SIZE];
static volatile uint8_t g_evq_head, g_evq_tail;

static const char *KEY_NAMES[11] = {
    "", "FUNC", "SHIFT", "ADD", "SAVE", "DISP", "SPEED", "FORMAT", "EXT", "USER1", "USER2"
};
static const uint8_t SEG_DIGIT[10] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};

/* ============================ forward declarations ============================ */
static uint8_t I2C0_WriteByte(uint8_t dev, uint8_t reg, uint8_t data);
static uint8_t I2C0_WriteTwo(uint8_t dev, uint8_t reg, uint8_t d0, uint8_t d1);
static uint8_t I2C0_ReadByte(uint8_t dev, uint8_t reg);
static void Clock_Init(void);
static void GPIO_Init(void);
static void I2C0_Init(void);
static void I2C0_BusRecover(void);
static void I2C0_SetSpeed(void);
static void Display_Init(void);
static void Display_Refresh(void);
static void Display_SetStr(const char *s, uint8_t dp_bitmap);
static uint8_t Disp_Bitmap(void);
static void UART_Init(uint32_t baud);
static void UART_RxIsrHandler(uint8_t byte);
static bool UART_GetLine(char *out, uint16_t max);
static void Timer0_Init(void);
static void UART_PutString(const char *s);
static void UART_Printf(const char *fmt, ...);
static void Mark_Serial_Activity(void);
static void Build_Display(void);
static void Set_LED(uint8_t value);
static void Send_Display_Event(void);
static void Send_LED_Event(void);
static void Send_Key_Event(key_code_t key);
static void Process_Command(char *line);
static void Key_Scan(void);
static key_event_t Key_GetEvent(key_code_t *out);
static void Dispatch_Key(key_event_t ev, key_code_t code);
static void Handle_Key(key_code_t key);
static void Scroll_Set(const char *text, uint32_t dp_bitmap);
static void Scroll_Tick(void);
static void Scroll_Ensure(const char *text, uint32_t dp_bitmap);
static void Scroll_Reset(void);
static void Time_Tick_1s(void);
static void Update_Status_LED(void);
static void Alarm_Service(void);
static void Startup_Show(void);
static uint8_t SegCode(char c);
static bool Parse_U8(const char *s, uint8_t *out);
static bool Parse_U16(const char *s, uint16_t *out);
static void Upper_Copy(char *dst, const char *src, uint16_t max);
static bool Token_Matches(const char *token, const char *pattern);
static uint8_t Days_In_Month(uint16_t year, uint8_t month);
static void Normalize_Date(void);
static void Calc_Wday(date_t *d);
static void Send_OK_Value(const char *value);
static void Show_Ntp_Status(void);
static uint8_t Tick_TimedOut(uint32_t start, uint32_t span_ms);

/* ============================ §1 SysTick ============================ */
void SysTick_Handler(void)
{
    g_tick_ms++;
    if ((g_tick_ms % 5U) == 0U)    flag_5ms = 1;
    if ((g_tick_ms % 10U) == 0U)   flag_10ms = 1;
    if ((g_tick_ms % 100U) == 0U)  flag_100ms = 1;
    if ((g_tick_ms % 1000U) == 0U) flag_1s = 1;
}

static uint8_t Tick_TimedOut(uint32_t start, uint32_t span_ms)
{
    return (uint8_t)((g_tick_ms - start) >= span_ms);
}

/* Hardware-timed display scan. Runs every 1 ms regardless of what the
   main loop is doing, so the dynamic scan never stalls. All I2C bus
   access lives here (display refresh, key sampling, LED flush) so the
   main loop never contends with the scan on the shared I2C0 bus. */
void TIMER0A_Handler(void)
{
    static uint8_t key_div = 0;

    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);

    Display_Refresh();

    if (g_led_dirty) {
        g_led_dirty = false;
        I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, (uint8_t)~g_led_byte);
    }

    if (++key_div >= KEY_SCAN_TICKS) {
        uint8_t tca = I2C0_ReadByte(TCA6424_I2CADDR, TCA6424_INPUT_PORT0);
        uint16_t raw = 0;
        uint8_t i;
        key_div = 0;
        for (i = 0; i < 8; i++) {
            if ((tca & (1U << i)) == 0) raw |= (1U << i);
        }
        g_tca_key_raw = (uint8_t)raw;
    }
}

/* §4: UART RX is interrupt-driven. The UART ISR has the highest priority of
   the active interrupts (above the timer scan), so a byte arriving always
   preempts a blocking I2C transaction and the 16-byte RX FIFO never overruns.
   This handler only touches ring buffers, never I2C, so preempting the scan
   is safe. */
void UART0_Handler(void)
{
    uint32_t status = UARTIntStatus(UART0_BASE, true);

    UARTIntClear(UART0_BASE, status);

    if (status & (UART_INT_RX | UART_INT_RT)) {
        while (UARTCharsAvail(UART0_BASE)) {
            int32_t ch = UARTCharGetNonBlocking(UART0_BASE);
            if (ch >= 0) UART_RxIsrHandler((uint8_t)ch);
        }
    }

    if (status & (UART_INT_TX)) {
        while (g_tx_head != g_tx_tail && UARTSpaceAvail(UART0_BASE)) {
            UARTCharPutNonBlocking(UART0_BASE, g_uart_tx[g_tx_tail]);
            g_tx_tail = (uint16_t)((g_tx_tail + 1U) % UART_TX_SIZE);
        }
        if (g_tx_head == g_tx_tail) {
            UARTIntDisable(UART0_BASE, UART_INT_TX);
        }
    }
}

/* Assemble a line in g_rx_asm; on CR/LF commit the complete line (plus a '\n'
   delimiter) into the byte ring atomically. Over-length lines are discarded
   and flagged so the main loop can emit ERROR LINE TOO LONG. */
static void UART_RxIsrHandler(uint8_t byte)
{
    uint8_t i;

    if (byte == '\r' || byte == '\n') {
        if (g_rx_asm_ovf) {
            g_rx_asm_ovf = false;
            g_rx_asm_len = 0;
            g_line_too_long = true;
            return;
        }
        if (g_rx_asm_len == 0) return;   /* empty line: ignore */
        Mark_Serial_Activity();
        for (i = 0; i < g_rx_asm_len; i++) {
            uint16_t next = (uint16_t)((g_rx_head + 1U) % RX_RING_SIZE);
            if (next != g_rx_tail) {
                g_rx_ring[g_rx_head] = g_rx_asm[i];
                g_rx_head = next;
            }
        }
        {
            uint16_t next = (uint16_t)((g_rx_head + 1U) % RX_RING_SIZE);
            if (next != g_rx_tail) {
                g_rx_ring[g_rx_head] = '\n';
                g_rx_head = next;
            }
        }
        rx_line_ready++;
        g_rx_asm_len = 0;
        return;
    }

    if (g_rx_asm_len >= LINE_MAX) {
        g_rx_asm_ovf = true;   /* keep consuming until CR/LF */
        return;
    }
    g_rx_asm[g_rx_asm_len++] = (char)byte;
}

/* Extract one complete line (terminated by '\n' in the ring) into out. */
static bool UART_GetLine(char *out, uint16_t max)
{
    uint16_t len = 0;

    if (rx_line_ready == 0) return false;

    while (g_rx_tail != g_rx_head) {
        char c = g_rx_ring[g_rx_tail];
        g_rx_tail = (uint16_t)((g_rx_tail + 1U) % RX_RING_SIZE);
        if (c == '\n') {
            out[len] = '\0';
            if (rx_line_ready) rx_line_ready--;
            return true;
        }
        if (len < max - 1U) out[len++] = c;
    }
    out[len] = '\0';
    return len > 0;
}

int main(void)
{
    Clock_Init();
    GPIO_Init();
    I2C0_Init();
    UART_Init(UART_BAUD);
    Timer0_Init();

    SysTickPeriodSet(g_sys_clock / SYSTICK_HZ);
    SysTickEnable();
    SysTickIntEnable();
    IntPriorityGroupingSet(3);
    IntPrioritySet(FAULT_SYSTICK, 0xE0);      /* lowest: 1 ms timebase only sets flags */
    IntPrioritySet(INT_UART0, 0x20);          /* highest: the 16-byte RX FIFO fills in
                                                 ~1.4 ms @115200, so receiving a byte
                                                 must preempt the display scan or the
                                                 FIFO overruns. The UART ISR only
                                                 touches ring buffers, never I2C. */
    IntPrioritySet(INT_TIMER0A, 0x40);        /* display scan / I2C below UART */
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    IntEnable(INT_UART0);
    IntEnable(INT_TIMER0A);
    TimerEnable(TIMER0_BASE, TIMER_A);
    IntMasterEnable();

    Set_LED(0x00);
    Startup_Show();

    while (1) {
        if (flag_10ms) {
            key_code_t k;
            key_event_t e;
            flag_10ms = 0;

            Key_Scan();
            while ((e = Key_GetEvent(&k)) != KEV_NONE) {
                Dispatch_Key(e, k);
            }

            g_blink_on = (((g_tick_ms / 500U) & 1U) == 0U);
            Build_Display();
            Update_Status_LED();

            /* Push display/LED changes promptly (PC mirror latency < 200 ms). */
            {
                uint8_t bm = Disp_Bitmap();
                if (memcmp(disp_buf, g_last_sent_disp, DISP_LEN) != 0 || bm != g_last_sent_dp) {
                    Send_Display_Event();
                    memcpy(g_last_sent_disp, disp_buf, DISP_LEN);
                    g_last_sent_disp[DISP_LEN] = '\0';
                    g_last_sent_dp = bm;
                }
                if (g_led_byte != g_last_sent_led) {
                    Send_LED_Event();
                    g_last_sent_led = g_led_byte;
                }
            }
        }

        Alarm_Service();

        while (rx_line_ready) {
            if (UART_GetLine(cmd_line, LINE_MAX)) {
                Process_Command(cmd_line);
            } else {
                break;
            }
        }

        if (g_line_too_long) {
            g_line_too_long = false;
            UART_PutString("ERROR LEN\r\n");
        }

        if (g_i2c_recover_request) {
            /* Recover the stuck bus here, not in the timer ISR. Pause the
               scan ISR so it can't touch I2C mid-recovery, then bit-bang. */
            g_i2c_recover_request = false;
            IntDisable(INT_TIMER0A);
            I2C0_BusRecover();
            IntEnable(INT_TIMER0A);
        }

        if (g_i2c_recover_count != g_i2c_recover_reported) {
            g_i2c_recover_reported = g_i2c_recover_count;
            UART_Printf("*EVT:I2C_RECOVER %u\r\n", g_i2c_recover_reported);
        }

        if (flag_1s) {
            flag_1s = 0;
            Time_Tick_1s();
            /* 1 Hz full heartbeat (§16: send every second even if unchanged). */
            Send_Display_Event();
            memcpy(g_last_sent_disp, disp_buf, DISP_LEN);
            g_last_sent_disp[DISP_LEN] = '\0';
            g_last_sent_dp = Disp_Bitmap();
            Send_LED_Event();
            g_last_sent_led = g_led_byte;
        }
    }
}

static void Clock_Init(void)
{
    g_sys_clock = SysCtlClockFreqSet((SYSCTL_XTAL_16MHZ | SYSCTL_OSC_INT |
                                      SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480), SYS_CLOCK_HZ);
}

static void GPIO_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOK);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOJ));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOK));

    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1,
                     GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    GPIOPinTypeGPIOOutput(GPIO_PORTK_BASE, GPIO_PIN_5);
    GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
}

static void I2C0_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_I2C0));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB));

    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    I2C0_SetSpeed();

    Display_Init();
}

/* Run the bus at 200 kHz. TivaWare's bool API only offers 100 k/400 k;
   400 k proved marginal on this board (long traces + UART switching
   noise), 100 k would eat most of the 1 ms scan budget. So init at the
   standard rate, then override the clock divider:
   SCL = SysClk / (2*(SCL_LP+SCL_HP)*(TPR+1)) with LP=6, HP=4. */
static void I2C0_SetSpeed(void)
{
    I2CMasterInitExpClk(I2C0_BASE, g_sys_clock, false);
    HWREG(I2C0_BASE + I2C_O_MTPR) = (g_sys_clock / (20U * I2C_BUS_HZ)) - 1U;
    I2CMasterEnable(I2C0_BASE);
}

/* §2: initialize both expanders' direction/output registers. Used at startup
   and after a bus recovery so the chips are in a known state. */
static void Display_Init(void)
{
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xff);
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00);
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00);
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00);
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, (uint8_t)~g_led_byte);
}

/* Free a hung I2C bus. If a transaction was aborted mid-byte a slave may
   still be driving SDA low, which keeps the master perpetually "busy" and
   every later write fails -> digits stay dark forever. The cure is to take
   the pins to GPIO and manually pulse SCL up to 9 times until the slave
   releases SDA, issue a STOP, then hand the pins back to the I2C hardware
   and re-init. This is the routine that makes the dark state self-heal. */
static void I2C0_BusRecover(void)
{
    uint32_t i;

    GPIOPinTypeGPIOOutputOD(GPIO_PORTB_BASE, GPIO_PIN_2);   /* SCL */
    GPIOPinTypeGPIOInput(GPIO_PORTB_BASE, GPIO_PIN_3);       /* SDA, read */
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, GPIO_PIN_2);

    for (i = 0; i < 9U; i++) {
        if (GPIOPinRead(GPIO_PORTB_BASE, GPIO_PIN_3) & GPIO_PIN_3) {
            break;  /* SDA released */
        }
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, 0);
        SysCtlDelay(g_sys_clock / 600000U);   /* ~5 us low */
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, GPIO_PIN_2);
        SysCtlDelay(g_sys_clock / 600000U);   /* ~5 us high */
    }

    GPIOPinTypeGPIOOutputOD(GPIO_PORTB_BASE, GPIO_PIN_3);
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_3, 0);
    SysCtlDelay(g_sys_clock / 600000U);
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, GPIO_PIN_2);
    SysCtlDelay(g_sys_clock / 600000U);
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_3, GPIO_PIN_3);
    SysCtlDelay(g_sys_clock / 600000U);

    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);
    I2C0_SetSpeed();

    Display_Init();
    g_i2c_fail_count = 0;
    g_i2c_recover_count++;
}

static void UART_Init(uint32_t baud)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0));
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA));

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    UARTConfigSetExpClk(UART0_BASE, g_sys_clock, baud,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOEnable(UART0_BASE);
    /* RX trips the interrupt at 1/8 full plus the receive-timeout, so bytes are
       pulled out of the FIFO eagerly and it never overruns. */
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX1_8, UART_FIFO_RX1_8);
}

static void Timer0_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER0));

    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER0_BASE, TIMER_A, g_sys_clock / DISPLAY_TIMER_HZ - 1U);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
}

/* Bounded spin. This runs inside the 1 ms display-scan ISR, so the cap
   must stay short: a 200 kHz byte completes in ~45 us (~3000 cycles), and
   2000 iterations (~300 us) covers a normal transfer while bailing fast on
   a wedged bus so the UART RX FIFO is never starved long enough to overrun. */
static bool I2C0_WaitIdle(void)
{
    uint32_t timeout = 2000U;
    while (I2CMasterBusy(I2C0_BASE)) {
        if (--timeout == 0U) {
            I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
            timeout = 2000U;
            while (I2CMasterBusy(I2C0_BASE) && --timeout);
            return false;
        }
    }
    return true;
}

/* TM4C erratum/race: the BUSY flag takes a few system clocks to assert
   after I2CMasterControl(). Polling I2CMasterBusy() immediately can read
   "idle" while the transfer is still starting, which lets the caller
   overwrite MDR mid-transmission and corrupt the byte on the wire. Wait
   for BUSY to assert (bounded spin) before waiting for it to clear. */
static bool I2C0_WaitTransfer(void)
{
    uint32_t spin = 0;
    while (!I2CMasterBusy(I2C0_BASE)) {
        if (++spin > 200U) break;  /* transfer already finished */
    }
    return I2C0_WaitIdle();
}

static uint8_t I2C0_WriteByte(uint8_t dev, uint8_t reg, uint8_t data)
{
    uint8_t err;
    if (!I2C0_WaitIdle()) return 1;
    I2CMasterSlaveAddrSet(I2C0_BASE, dev, false);
    I2CMasterDataPut(I2C0_BASE, reg);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    if (!I2C0_WaitTransfer()) return 1;
    err = (uint8_t)I2CMasterErr(I2C0_BASE);
    if (err != 0U) {
        I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
        I2C0_WaitTransfer();
        return err;
    }
    I2CMasterDataPut(I2C0_BASE, data);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
    if (!I2C0_WaitTransfer()) return 1;
    err |= (uint8_t)I2CMasterErr(I2C0_BASE);
    return err;
}

/* Write two consecutive registers in one bus transaction using the
   TCA6424 auto-increment bit (caller ORs TCA6424_AUTO_INC into reg). */
static uint8_t I2C0_WriteTwo(uint8_t dev, uint8_t reg, uint8_t d0, uint8_t d1)
{
    uint8_t err;
    if (!I2C0_WaitIdle()) return 1;
    I2CMasterSlaveAddrSet(I2C0_BASE, dev, false);
    I2CMasterDataPut(I2C0_BASE, reg);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    if (!I2C0_WaitTransfer()) return 1;
    err = (uint8_t)I2CMasterErr(I2C0_BASE);
    if (err != 0U) {
        I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
        I2C0_WaitTransfer();
        return err;
    }
    I2CMasterDataPut(I2C0_BASE, d0);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_CONT);
    if (!I2C0_WaitTransfer()) return 1;
    err |= (uint8_t)I2CMasterErr(I2C0_BASE);
    I2CMasterDataPut(I2C0_BASE, d1);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
    if (!I2C0_WaitTransfer()) return 1;
    err |= (uint8_t)I2CMasterErr(I2C0_BASE);
    return err;
}

static uint8_t I2C0_ReadByte(uint8_t dev, uint8_t reg)
{
    uint8_t value;
    if (!I2C0_WaitIdle()) return 0xff;
    I2CMasterSlaveAddrSet(I2C0_BASE, dev, false);
    I2CMasterDataPut(I2C0_BASE, reg);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
    if (!I2C0_WaitTransfer()) return 0xff;
    if (I2CMasterErr(I2C0_BASE) != 0U) return 0xff;
    I2CMasterSlaveAddrSet(I2C0_BASE, dev, true);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);
    if (!I2C0_WaitTransfer()) return 0xff;
    value = (uint8_t)I2CMasterDataGet(I2C0_BASE);
    return value;
}

static void UART_PutString(const char *s)
{
    /* Non-blocking: queue into the TX ring buffer and let the UART
       TX interrupt drain it, so the display scan never stalls.
       RXTX (LED3) is a true serial-activity indicator: every TX marks
       activity here, and RX marks it in UART_RxIsrHandler. The lone
       exception is the LED *EVT itself (g_tx_is_led_evt) — counting it
       would re-arm the window on every LED change and self-oscillate
       (LED 01 <-> 09), so that one transmission is gated out. */
    if (!g_tx_is_led_evt) {
        Mark_Serial_Activity();
    }
    UARTIntDisable(UART0_BASE, UART_INT_TX);
    while (*s) {
        uint16_t next = (uint16_t)((g_tx_head + 1U) % UART_TX_SIZE);
        if (next == g_tx_tail) {
            break; /* buffer full: drop the rest, never block */
        }
        g_uart_tx[g_tx_head] = *s++;
        g_tx_head = next;
    }
    while (g_tx_head != g_tx_tail && UARTSpaceAvail(UART0_BASE)) {
        UARTCharPutNonBlocking(UART0_BASE, g_uart_tx[g_tx_tail]);
        g_tx_tail = (uint16_t)((g_tx_tail + 1U) % UART_TX_SIZE);
    }
    if (g_tx_head != g_tx_tail) {
        UARTIntEnable(UART0_BASE, UART_INT_TX);
    }
}

static void UART_Printf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UART_PutString(buf);
}

static void Mark_Serial_Activity(void)
{
    /* Only record the activity window. The LED3 bit itself is owned solely by
       Update_Status_LED (it adds 0x08 while g_serial_activity_until_ms is in the
       future, and only outside night mode). Poking g_led_byte directly here
       would bypass that masking and flash the physical LED in night mode. */
    g_serial_activity_until_ms = g_tick_ms + SERIAL_LED_MS;
}

/* ============================ §2 display ============================ */
static uint8_t Disp_Bitmap(void)
{
    uint8_t i, b = 0;
    for (i = 0; i < DISP_LEN; i++) {
        if (disp_dp[i]) b |= (uint8_t)(1U << i);
    }
    return b;
}

/* §2: set the 8-digit text plus a decimal-point bitmap. Expands dp_bitmap into
   the per-digit disp_dp[] array, then commits chars + dp + scan limit atomically
   w.r.t. the scan ISR so it never reads a half-updated frame. The scan limit
   trims trailing blank digits to kill ghosting. */
static void Display_SetStr(const char *s, uint8_t dp_bitmap)
{
    uint8_t i, limit, done = 0;
    char nb[DISP_LEN];
    uint8_t nd[DISP_LEN];

    for (i = 0; i < DISP_LEN; i++) {
        if (!done && s[i] == '\0') done = 1;
        nb[i] = done ? ' ' : s[i];
        nd[i] = (dp_bitmap & (1U << i)) ? 1U : 0U;
    }

    if (memcmp(disp_buf, nb, DISP_LEN) == 0 && memcmp(disp_dp, nd, DISP_LEN) == 0) {
        return;
    }

    limit = DISP_LEN;
    while (limit > 1U && nb[limit - 1U] == ' ' && nd[limit - 1U] == 0U) {
        limit--;
    }

    IntDisable(INT_TIMER0A);
    memcpy(disp_buf, nb, DISP_LEN);
    memcpy(disp_dp, nd, DISP_LEN);
    g_scan_limit = limit;
    if (g_scan_pos >= g_scan_limit) {
        g_scan_pos = 0;
    }
    IntEnable(INT_TIMER0A);
}

static void Set_LED(uint8_t value)
{
    if (g_led_byte == value) {
        return;
    }
    g_led_byte = value;
    g_led_dirty = true;   /* flushed to PCA9557 by the timer ISR */
}

static uint8_t SegCode(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return SEG_DIGIT[c - '0'];
    switch (c) {
        case 'A': return 0x77;
        case 'B': return 0x7c;
        case 'C': return 0x39;
        case 'D': return 0x5e;
        case 'E': return 0x79;
        case 'F': return 0x71;
        case 'G': return 0x3d;
        case 'H': return 0x76;
        case 'I': return 0x10;
        case 'J': return 0x0e;
        case 'K': return 0x7a;
        case 'L': return 0x38;
        case 'M': return 0x55;
        case 'N': return 0x54;
        case 'O': return 0x5c;
        case 'P': return 0x73;
        case 'Q': return 0x67;
        case 'R': return 0x50;
        case 'S': return 0x64;
        case 'T': return 0x78;
        case 'U': return 0x3e;
        case 'V': return 0x62;
        case 'W': return 0x6a;
        case 'X': return 0x36;
        case 'Y': return 0x6e;
        case 'Z': return 0x49;
        case '-': return 0x40;
        case '_': return 0x08;
        default: return 0x00;
    }
}

/* §2: refresh exactly one digit. Called from the 1 ms timer ISR. Honors
   disp_on (whole-display blank) and disp_blink_mask (per-digit blink). */
static void Display_Refresh(void)
{
    static bool blanked = false;
    uint8_t code;
    uint8_t err = 0;
    bool blink_off;

    if (!disp_on) {
        /* Blank once on entry, not every 1 ms tick: hammering the I2C bus
           with two blocking writes per tick can stall this high-rate ISR
           long enough to starve the UART RX FIFO. */
        if (!blanked) {
            I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
            I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1, 0x00);
            blanked = true;
        }
        return;
    }
    blanked = false;

    if (g_scan_pos >= g_scan_limit) {
        g_scan_pos = 0;
    }

    blink_off = (disp_blink_mask & (1U << g_scan_pos)) && !g_blink_on;
    code = blink_off ? 0U : SegCode((char)disp_buf[g_scan_pos]);
    if (!blink_off && disp_dp[g_scan_pos]) code |= 0x80;

    /* Blank the digit select to kill ghosting, then write segments (PORT1)
       and the new digit select (PORT2) in a single auto-increment burst. */
    err |= I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
    err |= I2C0_WriteTwo(TCA6424_I2CADDR,
                         (uint8_t)(TCA6424_OUTPUT_PORT1 | TCA6424_AUTO_INC),
                         code, (uint8_t)(1U << g_scan_pos));

    if (err != 0U) {
        if (g_i2c_fail_count < 255U) g_i2c_fail_count++;
        if (g_i2c_fail_count >= 3U) {
            /* Don't recover here: I2C0_BusRecover() bit-bangs SCL with
               SysCtlDelay for several ms, which would starve the UART RX
               FIFO and drop bytes. Ask the main loop to do it instead. */
            g_i2c_recover_request = true;
        }
    } else {
        g_i2c_fail_count = 0;
    }

    g_scan_pos++;
    if (g_scan_pos >= g_scan_limit) {
        g_scan_pos = 0;
    }
}

/* ============================ §7 scroll ============================ */
static void Scroll_Set(const char *text, uint32_t dp_bitmap)
{
    strncpy(scroll_buf, text, sizeof(scroll_buf) - 1);
    scroll_buf[sizeof(scroll_buf) - 1] = '\0';
    scroll_len = (uint8_t)strlen(scroll_buf);
    scroll_src_dp = dp_bitmap;
    scroll_off = 0;
    scroll_last_ms = g_tick_ms;
    scroll_completed = false;
}

/* Render an 8-digit marquee over scroll_buf as a single linear pass (no wrap):
   the text sweeps fully across the screen and the pass ends the instant the
   last character has been shown at the far edge — Build_Display then returns to
   the clock, so the head never re-appears.

   step s = scroll_off runs 0 .. (scroll_len + DISP_LEN - 2). For slot j:
     FMT_LEFT  : text marches left  (enters at the right, exits at the left),
                 last char rests at the far LEFT on the final frame.
     FMT_RIGHT : text marches right (enters at the left, exits at the right),
                 first char rests at the far RIGHT on the final frame.
   §3.3 / FAQ Q7: FORMAT selects the direction. */
static void Scroll_Tick(void)
{
    char win[DISP_LEN + 1];
    uint8_t dp = 0, j;
    uint16_t interval = g_scroll_speed ? 250U : 500U;
    uint8_t s_max;

    if (scroll_len == 0U) { Display_SetStr("        ", 0); return; }
    s_max = (uint8_t)(scroll_len + DISP_LEN - 2U);   /* index of the last frame */

    if (!scroll_completed && Tick_TimedOut(scroll_last_ms, interval)) {
        scroll_last_ms = g_tick_ms;
        if (scroll_off >= s_max) {
            scroll_completed = true;   /* §12: last frame has had its full dwell */
        } else {
            scroll_off++;
        }
    }

    for (j = 0; j < DISP_LEN; j++) {
        int16_t ti;
        if (g_format == FMT_LEFT) {
            ti = (int16_t)scroll_off - (int16_t)(DISP_LEN - 1U) + (int16_t)j;
        } else {
            ti = (int16_t)(scroll_len - 1U) - (int16_t)scroll_off + (int16_t)j;
        }
        if (ti >= 0 && ti < (int16_t)scroll_len) {
            win[j] = scroll_buf[ti];
            /* §3.3 dp rule: LEFT (left->right) uses the source char's own dp on
               this digit (本位); RIGHT (right->left) uses the *next* source
               char's dp on this digit (下一位), so the separator sits one digit
               ahead just like the static FMT_RIGHT "提前一位" mirroring. */
            if (g_format == FMT_LEFT) {
                if (scroll_src_dp & (1UL << ti)) dp |= (uint8_t)(1U << j);
            } else {
                if ((ti + 1) < (int16_t)scroll_len &&
                    (scroll_src_dp & (1UL << (ti + 1)))) {
                    dp |= (uint8_t)(1U << j);
                }
            }
        } else {
            win[j] = ' ';
        }
    }
    win[DISP_LEN] = '\0';
    Display_SetStr(win, dp);
}

/* Set the source only when the content changes (preserving the offset across
   frames), then advance/render. */
static void Scroll_Ensure(const char *text, uint32_t dp_bitmap)
{
    if (strcmp(text, g_scroll_cur) != 0) {
        strncpy(g_scroll_cur, text, sizeof(g_scroll_cur) - 1);
        g_scroll_cur[sizeof(g_scroll_cur) - 1] = '\0';
        Scroll_Set(text, dp_bitmap);
    }
    Scroll_Tick();
}

static void Scroll_Reset(void)
{
    g_scroll_cur[0] = '\0';
    scroll_off = 0;
    scroll_last_ms = g_tick_ms;
}

/* ============================ display build ============================ */
static void Build_Display(void)
{
    char text[40];
    char base[9];
    uint8_t dp = 0;
    uint8_t i;

    if (g_countdown_active) {
        uint32_t remain_ms = (g_countdown_end_ms > g_tick_ms) ? (g_countdown_end_ms - g_tick_ms) : 0;
        uint32_t remain_s = (remain_ms + 999U) / 1000U;
        snprintf(text, sizeof(text), "CD%02lu%02lu  ",
                 (unsigned long)(remain_s / 60U), (unsigned long)(remain_s % 60U));
        dp = 0x08;
        if (remain_s == 0) {
            g_countdown_active = false;
            g_alarm.ringing = 1;
            g_alarm_ring_start_ms = g_tick_ms;
            g_ring_limit_ms = 10000;
            UART_PutString("*EVT:COUNTDOWN_DONE\r\n");
        }
    } else if (g_weather_until_ms > g_tick_ms) {
        char wt[9];
        uint8_t wl;
        if (!g_weather_valid) {
            Display_SetStr("--C---  ", 0);
        } else if ((g_tick_ms - g_weather_update_ms) > WEATHER_VALID_MS && !g_blink_on) {
            Display_SetStr("        ", 0);
        } else {
            wl = (uint8_t)strlen(g_weather_text);
            for (i = 0; i < 8; i++) wt[i] = (i < wl) ? g_weather_text[i] : ' ';
            wt[8] = '\0';
            Display_SetStr(wt, 0);
        }
        return;
    } else if (g_message_until_ms > g_tick_ms && g_message[0]) {
        if (strlen(g_message) <= 8) {
            char mb[9];
            uint8_t ml = (uint8_t)strlen(g_message);
            for (i = 0; i < 8; i++) mb[i] = (i < ml) ? g_message[i] : ' ';
            mb[8] = '\0';
            Display_SetStr(mb, (uint8_t)g_message_dp);
        } else {
            Scroll_Ensure(g_message, g_message_dp);
            /* Return to the clock the instant one full pass completes, so the
               message never loops into a partial second scroll. */
            if (scroll_completed) g_message_until_ms = 0;
        }
        return;
    } else if (g_mode == MODE_NIGHT) {
        snprintf(text, sizeof(text), "%02u%02u    ", g_time.h, g_time.mi);
        dp = 0x02;
    } else if (g_edit_state == ST_EDIT_DATE) {
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 (uint8_t)(g_edit_date.y % 100U), g_edit_date.m, g_edit_date.d);
        dp = 0x0a;
    } else if (g_edit_state == ST_IDLE && g_display_mode == DISP_DATE) {
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 (uint8_t)(g_date.y % 100U), g_date.m, g_date.d);
        dp = 0x0a;
    } else if (g_edit_state == ST_IDLE && g_display_mode == DISP_YEAR) {
        /* Full year date YYYY.MMDD: 8 digits fill the display exactly, so it is
           shown statically (FAQ Q7: 8 digits never scroll). Single dot after the
           year only (idx3), per the YYYY.MMDD format (FAQ Q12). */
        char yd[9];
        snprintf(yd, sizeof(yd), "%04u%02u%02u", g_date.y, g_date.m, g_date.d);
        Display_SetStr(yd, (uint8_t)(1U << 3));
        return;
    } else if (g_edit_state == ST_EDIT_ALARM) {
        snprintf(text, sizeof(text), "%02u%02u%02u A",
                 g_edit_alarm.t.h, g_edit_alarm.t.mi, g_edit_alarm.t.s);
        dp = 0x0a;
    } else if (g_edit_state == ST_EDIT_TIME) {
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 g_edit_time.h, g_edit_time.mi, g_edit_time.s);
        dp = 0x0a;
    } else {
        snprintf(text, sizeof(text), "%02u%02u%02u  ", g_time.h, g_time.mi, g_time.s);
        dp = 0x0a;
    }

    for (i = 0; i < 8; i++) base[i] = text[i] ? text[i] : ' ';
    base[8] = '\0';

    /* Blank the highlighted edit field BEFORE any FORMAT reversal, using the
       logical (LEFT) digit positions. This way the blanked pair is mirrored
       together with the value digits below, so in RIGHT mode the highlight
       lands on the same field's reversed digits instead of the left edge. */
    if (g_edit_state != ST_IDLE && !g_blink_on) {
        uint8_t start = 0;
        if (g_edit_field == 1) start = 2;
        else if (g_edit_field == 2) start = 4;
        base[start] = ' ';
        base[start + 1] = ' ';
    }

    if (g_format == FMT_RIGHT) {
        /* §3.3 / FAQ Q10: RIGHT mirrors the full 8-digit field, so the value
           moves to the right side (e.g. "123045  " -> "  540321"). A dot that
           sat to the right of digit p (LEFT) must move to digit (6-p) so the
           separators stay between the same number pairs ("提前一位"):
           dp 0x0A (bits 1,3) -> 0x28 (bits 5,3). */
        char rev[9];
        uint8_t rdp = 0;
        for (i = 0; i < 8; i++) {
            rev[i] = base[7 - i];
        }
        rev[8] = '\0';
        for (i = 0; i < 7; i++) {
            if (dp & (1U << i)) rdp |= (uint8_t)(1U << (6 - i));
        }
        strcpy(base, rev);
        dp = rdp;
    }

    Display_SetStr(base, dp);
}

static void Send_Display_Event(void)
{
    char enc[9];
    uint8_t i;
    /* §16: fixed 8 chars, pad with spaces (no dp here), then a space and the
       2-digit dp bitmap in hex, rebuilt from the per-digit disp_dp[] array. */
    for (i = 0; i < 8; i++) {
        char c = (char)disp_buf[i];
        enc[i] = (c == '\0') ? ' ' : c;
    }
    enc[8] = '\0';
    UART_Printf("*EVT:DISP %s %02X\r\n", enc, Disp_Bitmap());
}

static void Send_LED_Event(void)
{
    /* Gate this one TX so reporting the RXTX bit doesn't itself re-arm the
       RXTX activity window (would self-oscillate). */
    g_tx_is_led_evt = true;
    UART_Printf("*EVT:LED %02X\r\n", g_led_byte);
    g_tx_is_led_evt = false;
}

static void Send_Key_Event(key_code_t key)
{
    if (key >= KEY_FUNC && key <= KEY_USER2) {
        UART_Printf("*EVT:KEY %s\r\n", KEY_NAMES[key]);
    }
}

/* ============================ §3 key scan / events ============================ */
static void Key_Push(key_code_t c, key_event_t e)
{
    uint8_t next = (uint8_t)((g_evq_head + 1U) % KEY_EVQ_SIZE);
    if (next != g_evq_tail) {
        g_evq_code[g_evq_head] = c;
        g_evq_ev[g_evq_head] = e;
        g_evq_head = next;
    }
}

static key_event_t Key_GetEvent(key_code_t *out)
{
    key_event_t e;
    if (g_evq_head == g_evq_tail) return KEV_NONE;
    *out = g_evq_code[g_evq_tail];
    e = g_evq_ev[g_evq_tail];
    g_evq_tail = (uint8_t)((g_evq_tail + 1U) % KEY_EVQ_SIZE);
    return e;
}

/* §3: sample keys (TCA bits from the ISR + USER1/2 GPIO), debounce, run a small
   per-key state machine and push DOWN/UP/LONG/REPEAT events into the queue. */
static void Key_Scan(void)
{
    uint16_t raw = g_tca_key_raw;
    uint32_t pj;
    uint8_t b;

    pj = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    if ((pj & GPIO_PIN_0) == 0) raw |= (1U << (KEY_USER1 - 1));
    if ((pj & GPIO_PIN_1) == 0) raw |= (1U << (KEY_USER2 - 1));

    if (raw == g_key_last_raw) {
        if (g_key_same_count < 3) g_key_same_count++;
    } else {
        g_key_same_count = 0;
        g_key_last_raw = raw;
    }

    if (g_key_same_count >= KEY_DEBOUNCE_COUNT) {
        uint16_t prev = g_key_stable;
        uint16_t pressed = (uint16_t)(raw & ~prev);
        uint16_t released = (uint16_t)(~raw & prev);
        g_key_stable = raw;

        for (b = 0; b < KEY_COUNT; b++) {
            key_code_t code = (key_code_t)(b + 1);
            if (pressed & (1U << b)) {
                g_key_fsm[b] = 1;
                g_key_press_ms[b] = g_tick_ms;
                g_key_repeat_ms[b] = g_tick_ms;
                Key_Push(code, KEV_DOWN);
            }
            if (released & (1U << b)) {
                g_key_fsm[b] = 0;
                Key_Push(code, KEV_UP);
            }
        }
    }

    /* Long-press + auto-repeat are time-driven, independent of edge detection. */
    for (b = 0; b < KEY_COUNT; b++) {
        key_code_t code = (key_code_t)(b + 1);
        if (g_key_fsm[b] == 0) continue;
        if (g_key_fsm[b] == 1 && Tick_TimedOut(g_key_press_ms[b], FUNC_LONG_MS)) {
            g_key_fsm[b] = 2;
            Key_Push(code, KEV_LONG);
        }
        if (code == KEY_ADD && g_edit_state != ST_IDLE) {
            if (Tick_TimedOut(g_key_repeat_ms[b], ADD_REPEAT_MS)) {
                g_key_repeat_ms[b] = g_tick_ms;
                Key_Push(code, KEV_REPEAT);
            }
        }
    }
}

/* Translate a key event into the documented behavior. FUNC and USER1 defer
   their action to UP/LONG so a long press can override the short action. */
static void Dispatch_Key(key_event_t ev, key_code_t code)
{
    if (ev == KEV_DOWN && g_message_until_ms > g_tick_ms && code >= KEY_DISP) {
        g_message_until_ms = 0;   /* §12: any non-edit key interrupts a message */
    }

    switch (code) {
        case KEY_FUNC:
            if (ev == KEV_DOWN) {
                Send_Key_Event(KEY_FUNC);
                if (g_alarm.ringing) {
                    Handle_Key(KEY_FUNC);     /* §8: FUNC stops the alarm immediately */
                    g_func_armed = false;
                } else if (g_edit_state == ST_IDLE) {
                    Handle_Key(KEY_FUNC);     /* enter edit on press */
                    g_func_armed = false;
                } else {
                    g_func_armed = true;      /* in edit: defer to UP (cycle) / LONG (save) */
                }
            } else if (ev == KEV_LONG) {
                if (g_func_armed) {
                    Handle_Key(KEY_SAVE);
                    g_func_armed = false;
                }
            } else if (ev == KEV_UP) {
                if (g_func_armed) {
                    Handle_Key(KEY_FUNC);     /* short press: cycle edit field/state */
                    g_func_armed = false;
                }
            }
            break;
        case KEY_USER1:
            if (ev == KEV_DOWN) {
                g_user1_armed = true;
            } else if (ev == KEV_LONG) {
                if (g_user1_armed) {
                    Show_Ntp_Status();        /* §17: long press shows NTP status */
                    g_user1_armed = false;
                }
            } else if (ev == KEV_UP) {
                if (g_user1_armed) {
                    Send_Key_Event(KEY_USER1); /* short press: report (PC auto-NTP) */
                    g_user1_armed = false;
                }
            }
            break;
        case KEY_ADD:
            if (ev == KEV_DOWN) {
                Send_Key_Event(KEY_ADD);
                Handle_Key(KEY_ADD);
            } else if (ev == KEV_REPEAT) {
                Handle_Key(KEY_ADD);
            }
            break;
        default:
            if (ev == KEV_DOWN) {
                Send_Key_Event(code);
                Handle_Key(code);
            }
            break;
    }
}

static void Handle_Key(key_code_t key)
{
    g_edit_last_ms = g_tick_ms;

    if (g_alarm.ringing && key == KEY_FUNC) {
        g_alarm.ringing = 0;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        UART_PutString("*EVT:ALARM_OFF\r\n");
        return;
    }

    switch (key) {
        case KEY_FUNC:
            if (g_edit_state == ST_IDLE) {
                g_edit_date = g_date;
                g_edit_time = g_time;
                g_edit_alarm = g_alarm;
                g_edit_state = ST_EDIT_DATE;
            } else if (g_edit_state == ST_EDIT_DATE) {
                g_edit_time = g_time;
                g_edit_state = ST_EDIT_TIME;
            } else if (g_edit_state == ST_EDIT_TIME) {
                g_edit_alarm = g_alarm;
                g_edit_state = ST_EDIT_ALARM;
            } else {
                g_edit_state = ST_IDLE;
            }
            g_edit_field = 0;
            break;
        case KEY_SHIFT:
            /* §9: SHIFT moves the highlighted field to the left. */
            if (g_edit_state != ST_IDLE) g_edit_field = (uint8_t)((g_edit_field + 2U) % 3U);
            break;
        case KEY_ADD:
            if (g_edit_state == ST_EDIT_DATE) {
                if (g_edit_field == 0) {
                    g_edit_date.y++;
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) {
                        g_edit_date.d = Days_In_Month(g_edit_date.y, g_edit_date.m);
                    }
                } else if (g_edit_field == 1) {
                    g_edit_date.m++;
                    if (g_edit_date.m > 12) g_edit_date.m = 1;
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) {
                        g_edit_date.d = Days_In_Month(g_edit_date.y, g_edit_date.m);
                    }
                } else {
                    g_edit_date.d++;
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) g_edit_date.d = 1;
                }
            } else if (g_edit_state == ST_EDIT_TIME) {
                if (g_edit_field == 0) g_edit_time.h = (uint8_t)((g_edit_time.h + 1U) % 24U);
                else if (g_edit_field == 1) g_edit_time.mi = (uint8_t)((g_edit_time.mi + 1U) % 60U);
                else g_edit_time.s = (uint8_t)((g_edit_time.s + 1U) % 60U);
            } else if (g_edit_state == ST_EDIT_ALARM) {
                if (g_edit_field == 0) g_edit_alarm.t.h = (uint8_t)((g_edit_alarm.t.h + 1U) % 24U);
                else if (g_edit_field == 1) g_edit_alarm.t.mi = (uint8_t)((g_edit_alarm.t.mi + 1U) % 60U);
                else g_edit_alarm.t.s = (uint8_t)((g_edit_alarm.t.s + 1U) % 60U);
                g_edit_alarm.enabled = 1;
            }
            break;
        case KEY_SAVE:
            if (g_edit_state == ST_EDIT_DATE) {
                g_date.y = g_edit_date.y;
                g_date.m = g_edit_date.m;
                g_date.d = g_edit_date.d;
                Normalize_Date();
                UART_Printf("*EVT:EDIT DATE %04u.%02u.%02u\r\n", g_date.y, g_date.m, g_date.d);
            } else if (g_edit_state == ST_EDIT_TIME) {
                g_time = g_edit_time;
                UART_Printf("*EVT:EDIT TIME %02u.%02u.%02u\r\n", g_time.h, g_time.mi, g_time.s);
            } else if (g_edit_state == ST_EDIT_ALARM) {
                g_alarm.t = g_edit_alarm.t;
                g_alarm.enabled = 1;
                UART_Printf("*EVT:EDIT ALARM %02u.%02u.%02u\r\n", g_alarm.t.h, g_alarm.t.mi, g_alarm.t.s);
            }
            g_edit_state = ST_IDLE;
            break;
        case KEY_DISP:
            g_display_mode = (uint8_t)((g_display_mode + 1U) % 3U);
            Scroll_Reset();
            break;
        case KEY_SPEED:
            g_scroll_speed ^= 1U;
            break;
        case KEY_FORMAT:
            g_format = (g_format == FMT_LEFT) ? FMT_RIGHT : FMT_LEFT;
            break;
        case KEY_USER1:
            /* Reached only via *SET:KEY USER1 (PC virtual long-press). A
               physical USER1 short press emits *EVT:KEY USER1 instead and
               never lands here; a physical long press calls Show_Ntp_Status
               directly. So mirror the long-press behavior: show NTP status. */
            Show_Ntp_Status();
            break;
        case KEY_USER2:
            Scroll_Reset();
            g_weather_until_ms = g_tick_ms + 5000U;
            break;
        default:
            break;
    }
}

/* ============================ §6 time / §8 alarm ============================ */
static void Time_Tick_1s(void)
{
    g_time.s++;
    if (g_time.s >= 60) {
        g_time.s = 0;
        g_time.mi++;
        if (g_time.mi >= 60) {
            g_time.mi = 0;
            g_time.h++;
            if (g_time.h >= 24) {
                g_time.h = 0;
                g_date.d++;
                Normalize_Date();
            }
        }
    }

    if (g_edit_state != ST_IDLE && Tick_TimedOut(g_edit_last_ms, 5000U)) {
        g_edit_state = ST_IDLE;   /* §9: 5 s no-op exits without saving */
    }

    if (g_alarm.enabled && !g_alarm.ringing &&
        g_time.h == g_alarm.t.h && g_time.mi == g_alarm.t.mi && g_time.s == g_alarm.t.s) {
        g_alarm.ringing = 1;
        g_alarm_ring_start_ms = g_tick_ms;
        /* §8: base ring auto-stops at 8.7s so that the §15 rain/snow bonus
           (+3 beeps = 3 cycles x 200ms on + 200ms off = 1200ms) keeps the
           total within the <=10s limit (8800 + 1200 = 10000ms). */
        g_ring_limit_ms = 8800;
        if (g_weather_valid &&
            (g_weather_code == WEATHER_RAI || g_weather_code == WEATHER_SNO)) {
            g_ring_limit_ms = (uint16_t)(8800U + 3U * 400U);
        }
        g_alarm_last_beep_ms = 0;
        UART_PutString("*EVT:ALARM\r\n");
    }

    Update_Status_LED();
}

static void Update_Status_LED(void)
{
    /* §11: LED0 = 1 Hz system heartbeat (500 ms on / 500 ms off). Kept as the
       base in every mode, so night mode below naturally retains only the
       heartbeat, and *SET:LED override (g_led_override) stops it. */
    uint8_t led = (((g_tick_ms / 500U) & 1U) == 0U) ? 0x01U : 0x00U;
    bool fast = (((g_tick_ms / 200U) & 1U) == 0U);
    bool slow = (((g_tick_ms / 500U) & 1U) == 0U);

    /* FAQ Q11 (bonus): if no *SET:LED arrives for 10s, auto-release the takeover
       and let the LED meanings resume. */
    if (g_led_override && Tick_TimedOut(g_led_override_ms, 10000U)) {
        g_led_override = false;
    }

    if (g_mode != MODE_NIGHT) {
        if (g_alarm.ringing) {
            if (fast) led |= 0x02;
        } else if (g_alarm.enabled) {
            led |= 0x02;
        }
        if (g_edit_state != ST_IDLE) led |= 0x04;
        if (g_serial_activity_until_ms > g_tick_ms) led |= 0x08;
        if (g_weather_valid) {
            if (g_weather_code == WEATHER_SUN) led |= 0x10;
            if ((g_weather_code == WEATHER_RAI || g_weather_code == WEATHER_SNO) && slow) led |= 0x20;
            if (g_weather_temp >= 30) led |= 0x40;
        }
        if (g_ntp_synced) {
            if ((g_tick_ms - g_ntp_last_sync_ms) > 86400000UL) {
                if (slow) led |= 0x80;
            } else {
                led |= 0x80;
            }
        }
        /* §15 weather linkage: while the alarm rings in high temperature
           (>=30C), all 8 LEDs slow-blink together as an extra alert. This
           overrides the normal indicators only for the duration of the ring. */
        if (g_alarm.ringing && g_weather_valid && g_weather_temp >= 30) {
            led = slow ? 0xFFU : 0x00U;
        }
    }

    if (!g_led_override) {
        Set_LED(led);
    }
}

static void Show_Ntp_Status(void)
{
    /* FAQ Q13: short display "n SY xx", n = hours since last sync (0-9, clamped),
       xx = OK (SYNCED) / DR (DRIFT) / NO (UNSYNCED, n blanked). */
    if (g_ntp_synced) {
        uint32_t hours = (g_tick_ms - g_ntp_last_sync_ms) / 3600000UL;
        const char *code = ((g_tick_ms - g_ntp_last_sync_ms) > 86400000UL) ? "DR" : "OK";
        if (hours > 9UL) hours = 9UL;
        snprintf(g_message, sizeof(g_message), "%luSY%s", (unsigned long)hours, code);
    } else {
        snprintf(g_message, sizeof(g_message), " SYNO");
    }
    /* "n. SY. xx": dot after n (digit 0) and after SY (digit 2). */
    g_message_dp = (uint8_t)((1U << 0) | (1U << 2));
    g_message_until_ms = g_tick_ms + 3000U;
    Scroll_Reset();
}

static void Alarm_Service(void)
{
    if (!g_alarm.ringing) return;
    if (Tick_TimedOut(g_alarm_ring_start_ms, g_ring_limit_ms)) {
        g_alarm.ringing = 0;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        UART_PutString("*EVT:ALARM_OFF\r\n");
        return;
    }
    if (g_mode == MODE_NIGHT) {
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        return;
    }
    if ((g_tick_ms - g_alarm_last_beep_ms) >= 200U) {
        g_alarm_last_beep_ms = g_tick_ms;
        g_alarm_beep_on = !g_alarm_beep_on;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, g_alarm_beep_on ? GPIO_PIN_5 : 0);
    }
}

static uint8_t Days_In_Month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = ((year % 4U == 0U) && (year % 100U != 0U)) || (year % 400U == 0U);
    if (month == 2) return leap ? 29U : 28U;
    if (month >= 1 && month <= 12) return days[month - 1U];
    return 31U;
}

/* §6: weekday via Zeller's congruence (0 = Sunday .. 6 = Saturday). */
static void Calc_Wday(date_t *d)
{
    uint16_t y = d->y;
    uint8_t m = d->m;
    uint16_t k, j, h;
    if (m < 3) { m += 12; y--; }
    k = y % 100U;
    j = y / 100U;
    h = (uint16_t)((d->d + (13U * (m + 1U)) / 5U + k + k / 4U + j / 4U + 5U * j) % 7U);
    d->wday = (uint8_t)((h + 6U) % 7U);
}

static void Normalize_Date(void)
{
    if (g_date.m < 1) g_date.m = 1;
    if (g_date.m > 12) { g_date.m = 1; g_date.y++; }
    while (g_date.d > Days_In_Month(g_date.y, g_date.m)) {
        g_date.d = 1;
        g_date.m++;
        if (g_date.m > 12) { g_date.m = 1; g_date.y++; }
    }
    Calc_Wday(&g_date);
}

/* Build an 8-bit LED mask mirroring which of the 8 digits are lit:
   bit i (LED i+1) is set when digit i shows a non-blank character. */
static uint8_t Startup_Led_Mask(const char *s)
{
    uint8_t m = 0, i;
    for (i = 0; i < 8 && s[i] != '\0'; i++) {
        if (s[i] != ' ') m |= (uint8_t)(1U << i);
    }
    return m;
}

static void Startup_Show(void)
{
    /* §5 power-on sequence: every frame 800-1000ms, total within 4-8s,
       with at least one blink per stage. The timer ISR keeps the display
       scanned; here we just stage the frames. Total = 5.6s. */
    uint32_t t;

    /* Throughout the splash, each LED mirrors the digit at the same position:
       LED i lights when digit i is lit, and blinks off together with it. The
       remaining (blank) digits keep their LEDs off. */

    /* 1) all 8 digits + 8 LEDs on, then off -> one blink. */
    Display_SetStr("88888888", 0xff); Set_LED(Startup_Led_Mask("88888888"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));

    /* 2) last 8 digits of the student id, blink once. */
    Display_SetStr("42910013", 0x00); Set_LED(Startup_Led_Mask("42910013"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 200U));
    Display_SetStr("42910013", 0x00); Set_LED(Startup_Led_Mask("42910013"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 300U));

    /* 3) name pinyin, blink once. */
    Display_SetStr("YANZUO  ", 0x00); Set_LED(Startup_Led_Mask("YANZUO  "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 200U));
    Display_SetStr("YANZUO  ", 0x00); Set_LED(Startup_Led_Mask("YANZUO  "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 300U));

    /* 4) version v1.0 held for 1s (dp after the '1'). */
    Display_SetStr("V10     ", 0x02); Set_LED(Startup_Led_Mask("V10     "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 1000U));
}

static void Upper_Copy(char *dst, const char *src, uint16_t max)
{
    uint16_t i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static bool Token_Matches(const char *token, const char *pattern)
{
    char t[24], p[24];
    uint8_t req = 0, i;
    Upper_Copy(t, token, sizeof(t));
    Upper_Copy(p, pattern, sizeof(p));
    for (i = 0; pattern[i]; i++) if (isupper((unsigned char)pattern[i])) req++;
    if (strlen(t) < req || strlen(t) > strlen(p)) return false;
    return strncmp(p, t, strlen(t)) == 0;
}

static bool Parse_U8(const char *s, uint8_t *out)
{
    long v;
    char *end;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 255) return false;
    *out = (uint8_t)v;
    return true;
}

static bool Parse_U16(const char *s, uint16_t *out)
{
    long v;
    char *end;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 9999) return false;
    *out = (uint16_t)v;
    return true;
}

static void Send_OK_Value(const char *value)
{
    char out[32];
    uint8_t len = (uint8_t)strlen(value), i;
    if (g_format == FMT_LEFT) {
        UART_Printf("OK %s\r\n", value);
        return;
    }
    for (i = 0; i < len; i++) out[i] = value[len - 1U - i];
    out[len] = '\0';
    UART_Printf("OK %s\r\n", out);
}

static void Process_Command(char *line)
{
    static char work[96];  /* static to avoid stack allocation */
    char *argv[14];
    uint8_t argc = 0;
    char *tok;
    uint8_t i;

    Upper_Copy(work, line, sizeof(work));
    tok = strtok(work, " \t");
    while (tok && argc < 14) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0) return;

    if (Token_Matches(argv[0], "*PING")) {
        UART_Printf("*PONG %lu\r\n", (unsigned long)(g_tick_ms / 1000U));
        return;
    }

    if (Token_Matches(argv[0], "*RST")) {
        g_date.y = 2026; g_date.m = 6; g_date.d = 1;
        g_time.h = 0; g_time.mi = 0; g_time.s = 0;
        Calc_Wday(&g_date);
        g_alarm.t.h = 12; g_alarm.t.mi = 0; g_alarm.t.s = 0;
        g_alarm.enabled = 0; g_alarm.ringing = 0;
        disp_on = 1; g_format = FMT_LEFT; g_mode = MODE_DAY;
        g_led_override = false;
        g_ntp_synced = false;
        g_message_until_ms = 0;
        g_weather_until_ms = 0;
        Update_Status_LED();
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*NTP")) {
        if (argc < 2 || !Token_Matches(argv[1], "SYNC")) {
            UART_PutString("ERROR SYNTAX\r\n");
            return;
        }
        g_ntp_synced = true;
        g_ntp_last_sync_ms = g_tick_ms;
        Update_Status_LED();
        UART_PutString("OK\r\n");
        return;
    }

    /* GET: accept both "*GET DATE" (space) and "*GET:DATE" (colon) forms.
       argv[] is already upper-cased, so a colon-form sub-command is simply
       the substring after "*GET:". */
    if (Token_Matches(argv[0], "*GET") || strncmp(argv[0], "*GET:", 5) == 0) {
        char value[24];
        const char *sub;
        if (strncmp(argv[0], "*GET:", 5) == 0 && argv[0][5] != '\0') {
            sub = argv[0] + 5;                 /* "*GET:DATE" form */
        } else {
            /* "*GET DATE" or "*GET: DATE" (space after colon): sub is argv[1]. */
            if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
            sub = argv[1];
        }
        if (Token_Matches(sub, "DATE"))
            snprintf(value, sizeof(value), "%04u.%02u.%02u", g_date.y, g_date.m, g_date.d);
        else if (Token_Matches(sub, "TIME"))
            snprintf(value, sizeof(value), "%02u.%02u.%02u", g_time.h, g_time.mi, g_time.s);
        else if (Token_Matches(sub, "ALARM"))
            snprintf(value, sizeof(value), "%02u.%02u.%02u %s", g_alarm.t.h, g_alarm.t.mi, g_alarm.t.s, g_alarm.enabled ? "ON" : "OFF");
        else if (Token_Matches(sub, "DISPlay"))
            snprintf(value, sizeof(value), "%s", disp_on ? "ON" : "OFF");
        else if (Token_Matches(sub, "FORMAT"))
            snprintf(value, sizeof(value), "%s", g_format == FMT_LEFT ? "LEFT" : "RIGHT");
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        Send_OK_Value(value);
        return;
    }

    if (Token_Matches(argv[0], "*SET:KEY")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        for (i = 1; i <= KEY_COUNT; i++) {
            if (Token_Matches(argv[1], KEY_NAMES[i])) {
                Handle_Key((key_code_t)i);   /* §10: *SET:KEY does NOT emit *EVT:KEY */
                UART_PutString("OK\r\n");
                return;
            }
        }
        UART_PutString("ERROR PARAM\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:DISPlay")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "ON")) disp_on = 1;
        else if (Token_Matches(argv[1], "OFF")) disp_on = 0;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:FORMAT")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "LEFT")) g_format = FMT_LEFT;
        else if (Token_Matches(argv[1], "RIGHT")) g_format = FMT_RIGHT;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:MODE")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "DAY")) g_mode = MODE_DAY;
        else if (Token_Matches(argv[1], "NIGHT")) g_mode = MODE_NIGHT;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_Printf("OK\r\n*EVT:MODE %s\r\n", g_mode == MODE_DAY ? "DAY" : "NIGHT");
        return;
    }

    if (Token_Matches(argv[0], "*SET:LED")) {
        unsigned int val;
        if (argc < 2 || sscanf(argv[1], "%x", &val) != 1 || val > 0xff) {
            UART_PutString("ERROR PARAM\r\n"); return;
        }
        if (val == 0U) {
            g_led_override = false;
            Update_Status_LED();
        } else {
            g_led_override = true;
            g_led_override_ms = g_tick_ms;   /* arm the 10s auto-release window */
            Set_LED((uint8_t)val);
        }
        UART_PutString("OK\r\n");
        Send_LED_Event();
        return;
    }

    if (Token_Matches(argv[0], "*SET:BEEP")) {
        uint16_t ms;
        if (argc < 2 || !Parse_U16(argv[1], &ms) || ms < 10 || ms > 5000) {
            UART_PutString("ERROR RANGE\r\n"); return;
        }
        g_alarm.ringing = 1;
        g_alarm_ring_start_ms = g_tick_ms;
        g_ring_limit_ms = ms;
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:MSG")) {
        const char *raw = strchr(line, ' ');
        uint8_t rlen = 0, len = 0;
        if (!raw) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        while (*raw == ' ' || *raw == '\t') raw++;
        while (raw[rlen] && raw[rlen] != '\r' && raw[rlen] != '\n' && rlen < 32) rlen++;
        /* §12: message must be <= 32 bytes; reject anything longer. */
        if (raw[rlen] && raw[rlen] != '\r' && raw[rlen] != '\n') {
            UART_PutString("ERROR RANGE\r\n"); return;
        }
        /* §3.3: an embedded '.' is the decimal point of the character before it
           (e.g. "8.8" = two digits with a dp after the first). Strip the dots
           into g_message_dp (one bit per visible character) so the marquee can
           render them. A leading dot with no preceding char is dropped. */
        g_message_dp = 0;
        {
            uint8_t i;
            for (i = 0; i < rlen; i++) {
                if (raw[i] == '.') {
                    if (len > 0) g_message_dp |= (1UL << (len - 1U));
                } else if (len < 32) {
                    g_message[len++] = raw[i];
                }
            }
        }
        g_message[len] = '\0';
        /* <=8: static for 3s. >8: a generous safety cap; the actual return to
           the clock is driven by scroll_completed after exactly one full pass
           (Build_Display), so this only bounds the window if scrolling stalls.
           Uses the slow step (500ms) + margin so it never cuts a pass short at
           either speed. */
        g_message_until_ms = g_tick_ms + (len <= 8U ? 3000U :
                              ((uint32_t)(len + 8U) * 500U + 2000U));
        Scroll_Reset();
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:WEA")) {
        static const char *cond_names[] = {"SUN", "CLD", "OVC", "RAI", "SNO", "FOG"};
        char cond[8];
        long temp;
        char *end;
        uint8_t code;

        if (argc < 3) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        temp = strtol(argv[1], &end, 10);
        if (*end != '\0') { UART_PutString("ERROR PARAM\r\n"); return; }
        if (temp < -40 || temp > 50) { UART_PutString("ERROR RANGE\r\n"); return; }
        Upper_Copy(cond, argv[2], sizeof(cond));

        if (Token_Matches(cond, "SUN")) code = WEATHER_SUN;
        else if (Token_Matches(cond, "CLD")) code = WEATHER_CLD;
        else if (Token_Matches(cond, "OVC")) code = WEATHER_OVC;
        else if (Token_Matches(cond, "RAI")) code = WEATHER_RAI;
        else if (Token_Matches(cond, "SNO")) code = WEATHER_SNO;
        else if (Token_Matches(cond, "FOG")) code = WEATHER_FOG;
        else { UART_PutString("ERROR PARAM\r\n"); return; }

        g_weather_temp = (int8_t)temp;
        g_weather_code = code;
        g_weather_valid = true;
        g_weather_update_ms = g_tick_ms;
        snprintf(g_weather_text, sizeof(g_weather_text), "%ldC%s", temp, cond_names[code]);
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:COUNTDOWN")) {
        uint16_t sec;
        if (argc < 2 || !Parse_U16(argv[1], &sec) || sec == 0 || sec > 3599) {
            UART_PutString("ERROR RANGE\r\n"); return;
        }
        g_countdown_end_ms = g_tick_ms + ((uint32_t)sec * 1000U);
        g_countdown_active = true;
        UART_PutString("OK\r\n");
        return;
    }

    if (Token_Matches(argv[0], "*SET:DATE") || Token_Matches(argv[0], "*SET:TIME") || Token_Matches(argv[0], "*SET:ALARM")) {
        if (g_edit_state != ST_IDLE) { UART_PutString("ERROR BUSY\r\n"); return; }
        bool is_date = Token_Matches(argv[0], "*SET:DATE");
        bool is_time = Token_Matches(argv[0], "*SET:TIME");
        if (!is_date && !is_time && argc == 2 && Token_Matches(argv[1], "OFF")) {
            g_alarm.enabled = 0;
            g_alarm.ringing = 0;
            g_alarm_beep_on = 0;
            GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
            UART_PutString("OK\r\n");
            UART_PutString("*EVT:ALARM_OFF\r\n");
            return;
        }
        {
            uint8_t field_count = 0;
            uint8_t value_start;
            for (i = 1; i < argc; i++) {
                if (is_date &&
                    (Token_Matches(argv[i], "YEAR") ||
                     Token_Matches(argv[i], "MONTH") ||
                     Token_Matches(argv[i], "DATE"))) {
                    field_count++;
                } else if (!is_date &&
                           (Token_Matches(argv[i], "HOUR") ||
                            Token_Matches(argv[i], "MINute") ||
                            Token_Matches(argv[i], "SECond"))) {
                    field_count++;
                } else {
                    break;
                }
            }
            value_start = (uint8_t)(1U + field_count);
            if (field_count == 0U || argc != (uint8_t)(value_start + field_count)) {
                UART_PutString("ERROR SYNTAX\r\n");
                return;
            }
            for (i = 0; i < field_count; i++) {
                uint8_t field = (uint8_t)(1U + i);
                uint8_t value = (uint8_t)(value_start + i);
                uint8_t u8; uint16_t u16;
                if (is_date && Token_Matches(argv[field], "YEAR")) {
                    if (!Parse_U16(argv[value], &u16)) { UART_PutString("ERROR PARAM\r\n"); return; }
                    g_date.y = u16;
                } else if (is_date && Token_Matches(argv[field], "MONTH")) {
                    if (!Parse_U8(argv[value], &u8) || u8 < 1 || u8 > 12) { UART_PutString("ERROR RANGE\r\n"); return; }
                    g_date.m = u8;
                } else if (is_date && Token_Matches(argv[field], "DATE")) {
                    if (!Parse_U8(argv[value], &u8) || u8 < 1 || u8 > 31) { UART_PutString("ERROR RANGE\r\n"); return; }
                    g_date.d = u8;
                } else if (!is_date && Token_Matches(argv[field], "HOUR")) {
                    if (!Parse_U8(argv[value], &u8) || u8 > 23) { UART_PutString("ERROR RANGE\r\n"); return; }
                    if (is_time) g_time.h = u8; else g_alarm.t.h = u8;
                } else if (!is_date && Token_Matches(argv[field], "MINute")) {
                    if (!Parse_U8(argv[value], &u8) || u8 > 59) { UART_PutString("ERROR RANGE\r\n"); return; }
                    if (is_time) g_time.mi = u8; else g_alarm.t.mi = u8;
                } else if (!is_date && Token_Matches(argv[field], "SECond")) {
                    if (!Parse_U8(argv[value], &u8) || u8 > 59) { UART_PutString("ERROR RANGE\r\n"); return; }
                    if (is_time) g_time.s = u8; else g_alarm.t.s = u8;
                } else {
                    UART_PutString("ERROR PARAM\r\n");
                    return;
                }
            }
        }
        if (is_date) Normalize_Date();
        if (!is_date && !is_time) g_alarm.enabled = 1;
        UART_PutString("OK\r\n");
        if (is_date) UART_Printf("*EVT:EDIT DATE %04u.%02u.%02u\r\n", g_date.y, g_date.m, g_date.d);
        else if (is_time) UART_Printf("*EVT:EDIT TIME %02u.%02u.%02u\r\n", g_time.h, g_time.mi, g_time.s);
        else UART_Printf("*EVT:EDIT ALARM %02u.%02u.%02u\r\n", g_alarm.t.h, g_alarm.t.mi, g_alarm.t.s);
        return;
    }

    UART_PutString("ERROR SYNTAX\r\n");
}
