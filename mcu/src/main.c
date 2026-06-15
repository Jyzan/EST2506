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
#define LINE_MAX             64    // §4 串口命令最大长度 不含回车换行
#define DISP_LEN             8     // §2 数码管位数 共 8 位
#define DISPLAY_TIMER_HZ     1000U // §2 定时器驱动数码管动态扫描 1ms 周期
#define KEY_SCAN_TICKS       20U   // §3 每 20 次扫描中断采样一次 TCA 按键
#define KEY_DEBOUNCE_COUNT   3U   // 需连续 3 次稳定采样才认边沿 抑制 EXT 等键抖动多触发
#define ADD_REPEAT_MS        200U
#define FUNC_LONG_MS         800U
#define SERIAL_LED_MS        100U
#define WEATHER_VALID_MS     1800000UL

#define I2C_BUS_HZ           400000U
#define TCA6424_AUTO_INC     0x80     // 命令字节自动递增位 连续写多寄存器时自动跳转
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

// ---- §3 按键码与按键事件类型 ----
typedef enum {
    KEY_NONE = 0, KEY_FUNC, KEY_SHIFT, KEY_ADD, KEY_SAVE,
    KEY_DISP, KEY_SPEED, KEY_FORMAT, KEY_EXT, KEY_USER1, KEY_USER2
} key_code_t;
#define KEY_COUNT 10   // 硬件按键位 = key_code - 1 共 10 位

typedef enum { KEV_NONE, KEV_DOWN, KEV_UP, KEV_LONG, KEV_REPEAT } key_event_t;

// ---- §9 编辑有限状态机 ----
typedef enum { ST_IDLE, ST_EDIT_DATE, ST_EDIT_TIME, ST_EDIT_ALARM } edit_state_t;

// ---- §7 显示方向枚举 ----
typedef enum { FMT_LEFT, FMT_RIGHT } format_t;

// ---- §6 日期与时间数据结构 ----
typedef struct { uint16_t y; uint8_t m, d, wday; } date_t;
typedef struct { uint8_t h, mi, s; } time_t_;

// ---- §8 闹钟数据结构 ----
typedef struct { time_t_ t; uint8_t enabled, ringing; } alarm_t;

// ============================ §1 系统时基 1ms 节拍 ============================
static uint32_t g_sys_clock;
volatile uint32_t g_tick_ms;
volatile uint8_t flag_5ms, flag_10ms, flag_100ms, flag_1s;

// ============================ §4 串口接收 / 发送缓冲区 ============================
volatile uint8_t rx_line_ready;     // 环形缓冲区中已完整接收的行数
char cmd_line[LINE_MAX];            // 当前交给 Process_Command 解析的命令行
static volatile bool g_line_too_long;

#define RX_RING_SIZE 256U
static char g_rx_ring[RX_RING_SIZE];
static volatile uint16_t g_rx_head;
static volatile uint16_t g_rx_tail;
/* 接收中断将字节组装成行后写入环形缓冲区，超长行丢弃并置标志位，
   避免半行 / 超长行被主循环取走导致解析错位 */
static char g_rx_asm[LINE_MAX + 2];
static uint8_t g_rx_asm_len;
static bool g_rx_asm_ovf;

#define UART_TX_SIZE 512U
static char g_uart_tx[UART_TX_SIZE];
static volatile uint16_t g_tx_head;
static volatile uint16_t g_tx_tail;

// ============================ §6 §8 时钟 / 闹钟运行状态 ============================
static date_t   g_date = {2026, 6, 1, 0};
static time_t_  g_time = {0, 0, 0};
static date_t   g_edit_date = {2026, 6, 1, 0};
static time_t_  g_edit_time = {0, 0, 0};
static alarm_t  g_alarm = {{0, 0, 0}, 0, 0};
static alarm_t  g_edit_alarm = {{0, 0, 0}, 0, 0};
// 闹钟运行时字段 响铃起始时刻 / 上次蜂鸣翻转时刻 / 当前蜂鸣电平 / 最大响铃时长
static uint32_t g_alarm_ring_start_ms = 0;
static uint32_t g_alarm_last_beep_ms = 0;
static uint8_t  g_alarm_beep_on = 0;
static uint16_t g_ring_limit_ms = 10000;
static bool     g_beep_active = false;         // BEEP 远程蜂鸣长响标志 不走节奏翻转

// ============================ 运行模式 / 杂项状态 ============================
static format_t g_format = FMT_LEFT;
static uint8_t g_mode = MODE_DAY;
static uint8_t g_display_mode = DISP_TIME;
static uint8_t g_scroll_speed = 0;
static edit_state_t g_edit_state = ST_IDLE;
static uint8_t g_edit_field = 0;
static uint32_t g_edit_last_ms = 0;
static bool g_blink_on = true;

static char g_weather_text[17] = "--~C---";
static int8_t g_weather_temp = 0;
static uint8_t g_weather_code = WEATHER_CLD;
static bool g_weather_valid = false;
static uint32_t g_weather_update_ms = 0;
static uint32_t g_weather_until_ms = 0;
// ---- §自主功能 情景倒计时 状态机 ----
typedef enum { CD_IDLE, CD_EDIT, CD_RUN, CD_PAUSE, CD_DONE } cd_state_t;
static cd_state_t g_cd_state = CD_IDLE;
static uint16_t g_cd_total_s = 300;            // 设定总时长(秒) 1-3599
static uint16_t g_cd_remain_s = 0;             // 剩余秒 支持暂停 在 Time_Tick_1s 中递减
static uint8_t  g_cd_min = 0;                  // 编辑分(0-59) 默认 0
static uint8_t  g_cd_sec = 0;                  // 编辑秒(0-59) 默认 0
static uint8_t  g_cd_field = 0;                // 编辑字段 0=分 1=秒 2=情景
static uint8_t  g_cd_scene = 0;                // 情景 0=滚动文本 1=闪烁庆祝 2=静默
static char     g_cd_msg[33] = "CONGRATULATIONS";  // 到点滚动文本 默认值 由 PC UI 下发覆盖 保留大小写
static char     g_cd_active_msg[33] = "CONGRATULATIONS";
static uint32_t g_cd_done_ms = 0;              // DONE 情景起始时刻 超时兜底
static const char *CD_STATE_NAMES[5] = {"IDLE", "EDIT", "RUN", "PAUSE", "DONE"};

// ============================ §2 数码管显示状态 ============================
uint8_t disp_buf[DISP_LEN];           // 每位显示的 ASCII 字符
uint8_t disp_dp[DISP_LEN];            // 每位小数点状态 0 灭 1 亮
uint8_t disp_blink_mask = 0;          // 每位闪烁使能 0 常亮
uint8_t disp_on = 1;                  // 整屏亮灭开关
static uint8_t g_scan_pos = 0;
static uint8_t g_scan_limit = 8;

static uint8_t g_led_byte = 0x00;
static volatile bool g_led_dirty = true;
static bool g_led_override = false;
static uint32_t g_led_override_ms = 0;   // 最后一次 *SET:LED 的时刻 用于 10s 自动退出接管

static volatile uint8_t g_tca_key_raw = 0;        // 定时器中断中采样的 TCA 按键电平
static volatile uint8_t g_i2c_fail_count = 0;     // 连续 I2C 扫描失败计数
static volatile uint16_t g_i2c_recover_count = 0; // I2C 总线恢复累计次数
static uint16_t g_i2c_recover_reported = 0;
static volatile bool g_i2c_recover_request = false;

static uint32_t g_serial_activity_until_ms = 0;
static volatile bool g_tx_is_led_evt = false;  // 门控 发送 LED 事件时不重新激活串口活动指示灯
static char g_message[33] = "";
static uint32_t g_message_dp = 0;           // 消息小数点 bitmap 每字符 1 bit
static uint32_t g_message_until_ms = 0;
static bool g_ntp_synced = false;
static uint32_t g_ntp_last_sync_ms = 0;

static char g_last_sent_disp[9] = "";
static uint8_t g_last_sent_dp = 0xff;
static uint8_t g_last_sent_led = 0xff;

// ============================ §7 流水滚动模块 ============================
static char scroll_buf[40];           // 滚动源文本缓冲区 长度 >= 32
static uint8_t scroll_len = 0;
static uint32_t scroll_src_dp = 0;    // 源文本每字符的小数点 bit
static uint8_t scroll_off = 0;        // 流水步进偏移 s 从 0 到 scroll_len+6
static uint32_t scroll_last_ms = 0;
static char g_scroll_cur[40] = "";    // 上一次传给 Scroll_Set 的文本 用于判重
static bool scroll_completed = false; // 最后一帧显示完毕 标记整趟流水结束

// ============================ §3 按键扫描模块 ============================
static uint16_t g_key_stable = 0;
static uint16_t g_key_last_raw = 0;
static uint8_t g_key_same_count = 0;
static uint8_t g_key_fsm[KEY_COUNT];        // 每键状态机 0 空闲 1 按下 2 长按
static uint32_t g_key_press_ms[KEY_COUNT];
static uint32_t g_key_repeat_ms[KEY_COUNT];
static bool g_func_armed = false;           // 编辑中按下 FUNC 暂不处理 等抬起/长按再决定
static bool g_user1_armed = false;
static bool g_ext_armed = false;            // EXT 短按启动/暂停 长按停止 等抬起/长按再决定
static bool g_ext_long = false;             // 本次 EXT 已触发长按 抬起时不再当短按处理

#define KEY_EVQ_SIZE 16U
static key_code_t g_evq_code[KEY_EVQ_SIZE];
static key_event_t g_evq_ev[KEY_EVQ_SIZE];
static volatile uint8_t g_evq_head, g_evq_tail;

static const char *KEY_NAMES[11] = {
    "", "FUNC", "SHIFT", "ADD", "SAVE", "DISP", "SPEED", "FORMAT", "EXT", "USER1", "USER2"
};
static const uint8_t SEG_DIGIT[10] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};

// ============================ 函数前向声明 ============================
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
static bool Command_Matches(const char *token, const char *pattern);
static uint8_t Days_In_Month(uint16_t year, uint8_t month);
static void Normalize_Date(void);
static void Calc_Wday(date_t *d);
static void Send_OK_Value(const char *value);
static void Show_Ntp_Status(void);
static bool Countdown_HandleKey(key_code_t key);
static void Send_CD_Event(void);
static void Countdown_Start(void);
static void Countdown_Stop(void);
static bool Countdown_HandleCommand(char *line, char **argv, uint8_t argc);
static uint8_t Tick_TimedOut(uint32_t start, uint32_t span_ms);

/* §1 SysTick 每 1ms 触发一次 递增全局节拍计数器并设置各时间片标志位。
   中断优先级设为最低 仅置位标志 不在中断内做重活 */
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

/* 硬件定时器驱动显示扫描 每 1ms 触发一次 与主循环完全解耦。
   所有 I2C 总线操作集中在此中断内完成 包括数码管刷新 / LED 状态写入 / 按键采样。
   主循环不直接操作 I2C0 总线 避免与扫描竞争 */
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

/* §4 串口中断服务 接收侧由 RX FIFO 触发 优先级高于定时器扫描。
   当 I2C 事务阻塞时 到达的字节可抢占并安全存入环形缓冲区。
   ISR 仅操作环形缓冲 绝不访问 I2C 总线 因此抢占扫描是安全的 */
void UART0_Handler(void)
{
    uint32_t status = UARTIntStatus(UART0_BASE, true);

    UARTIntClear(UART0_BASE, status);

    if (status & (UART_INT_RX | UART_INT_RT)) {
        // 检测帧错误与接收溢出等硬件错误 出现时作废当前组装中的行
        // 防止被破坏的字节拼入命令 等待下一个回车换行重新同步
        if (UARTRxErrorGet(UART0_BASE) != 0U) {
            UARTRxErrorClear(UART0_BASE);
            g_rx_asm_len = 0;
            g_rx_asm_ovf = true;
        }
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

/* 在 g_rx_asm 中逐字节组装一行 遇到 CR/LF 时将完整行写入环形缓冲区。
   超长行丢弃并设 g_line_too_long 标志 主循环随后回传 ERROR LEN。
   空行忽略不提交 */
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
        if (g_rx_asm_len == 0) return;   // 忽略空行
        Mark_Serial_Activity();
        {
            // 先确认环形缓冲能容纳整行连同行尾换行符 容不下则整行丢弃
            // 杜绝写入半行或漏写换行符却仍递增行计数导致后续命令永久错位
            uint16_t used = (uint16_t)((g_rx_head - g_rx_tail + RX_RING_SIZE) % RX_RING_SIZE);
            uint16_t free_slots = (uint16_t)(RX_RING_SIZE - 1U - used);
            if (free_slots < (uint16_t)(g_rx_asm_len + 1U)) {
                g_rx_asm_len = 0;
                return;
            }
        }
        for (i = 0; i < g_rx_asm_len; i++) {
            g_rx_ring[g_rx_head] = g_rx_asm[i];
            g_rx_head = (uint16_t)((g_rx_head + 1U) % RX_RING_SIZE);
        }
        g_rx_ring[g_rx_head] = '\n';
        g_rx_head = (uint16_t)((g_rx_head + 1U) % RX_RING_SIZE);
        rx_line_ready++;
        g_rx_asm_len = 0;
        return;
    }

    if (g_rx_asm_len >= LINE_MAX) {
        g_rx_asm_ovf = true;   // 继续消耗字节直到 CR/LF
        return;
    }
    g_rx_asm[g_rx_asm_len++] = (char)byte;
}

// 从环形缓冲区取出一行 以 '\n' 作为行分隔符 写入 out
static bool UART_GetLine(char *out, uint16_t max)
{
    uint16_t len = 0;

    if (rx_line_ready == 0) return false;

    while (g_rx_tail != g_rx_head) {
        char c = g_rx_ring[g_rx_tail];
        g_rx_tail = (uint16_t)((g_rx_tail + 1U) % RX_RING_SIZE);
        if (c == '\n') {
            out[len] = '\0';
            // 关串口中断保护行计数自减 避免与中断中的自增形成读改写竞争丢更新
            IntDisable(INT_UART0);
            if (rx_line_ready) rx_line_ready--;
            IntEnable(INT_UART0);
            return true;
        }
        if (len < max - 1U) out[len++] = c;
    }
    // 扫到环底仍无换行符 说明行计数与缓冲内容失配 丢弃残缺字节并清零计数
    // 让接收链路重新同步 而不是把不完整的命令交给解析器
    out[0] = '\0';
    IntDisable(INT_UART0);
    rx_line_ready = 0;
    g_rx_tail = g_rx_head;
    IntEnable(INT_UART0);
    return false;
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
    IntPrioritySet(FAULT_SYSTICK, 0xE0);      // SysTick 优先级最低 仅置位标志不做重活
    IntPrioritySet(INT_UART0, 0x20);          /* 串口优先级最高 115200 下 16 字节 RX FIFO 约 1.4ms 填满
                                                 必须能抢占显示扫描 否则丢字节
                                                 ISR 仅操作环形缓冲 不碰 I2C */
    IntPrioritySet(INT_TIMER0A, 0x40);        // 显示扫描 / I2C 优先级在串口之下
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

            // 扫描按键并消费全部事件
            Key_Scan();
            while ((e = Key_GetEvent(&k)) != KEV_NONE) {
                Dispatch_Key(e, k);
            }

            // 生成全局闪烁基准 500ms 周期方波 供编辑态高亮和 LED 慢闪复用
            g_blink_on = (((g_tick_ms / 500U) & 1U) == 0U);
            // 根据当前模式/状态构建显示内容 并刷新 LED 含义
            Build_Display();
            Update_Status_LED();

            // §16 显示或 LED 变化时立即上报 保证 PC 镜像延迟 < 200ms
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
            // 在主循环中恢复 I2C 总线 先暂停扫描 ISR 避免恢复期间触碰 I2C
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
            Build_Display();  // 秒变化后刷新显示缓冲 避免心跳上报旧帧
            // 1Hz 全量心跳 即使无变化也每秒发送一次
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

/* I2C 时钟设为 200kHz。TivaWare 标准 API 仅提供 100k/400k 两档
   400k 在长走线 + 串口开关噪声下不可靠 100k 会吃掉 1ms 扫描预算的大部分。
   因此先用标准速率初始化 再手动改写时钟分频 MTPR 寄存器微调。
   SCL 计算公式 = SysClk / (2 * (SCL_LP + SCL_HP) * (TPR + 1))
   其中 LP=6 HP=4 */
static void I2C0_SetSpeed(void)
{
    I2CMasterInitExpClk(I2C0_BASE, g_sys_clock, false);
    HWREG(I2C0_BASE + I2C_O_MTPR) = (g_sys_clock / (20U * I2C_BUS_HZ)) - 1U;
    I2CMasterEnable(I2C0_BASE);
}

/* §2 初始化两个 I2C 扩展芯片的方向与输出寄存器。
   启动时调用一次 总线恢复后也需重配 保证芯片处于已知状态 */
static void Display_Init(void)
{
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xff);
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00);
    I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00);
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00);
    I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, (uint8_t)~g_led_byte);
}

/* I2C 总线卡死恢复 若传输中途被打断 从机可能持续拉低 SDA
   导致主机永远忙 后续所有 I2C 写操作失败 数码管全暗。
   恢复流程 将 SCL/SDA 切为 GPIO 手动翻转 SCL 最多 9 次
   直到从机释放 SDA 再发 STOP 最后交还 I2C 硬件并重配。
   此函数在 main 中调用 不在 ISR 内 避免阻塞中断 */
static void I2C0_BusRecover(void)
{
    uint32_t i;

    GPIOPinTypeGPIOOutputOD(GPIO_PORTB_BASE, GPIO_PIN_2);   // SCL 设为开漏输出
    GPIOPinTypeGPIOInput(GPIO_PORTB_BASE, GPIO_PIN_3);       // SDA 设为输入检测释放状态
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, GPIO_PIN_2);

    for (i = 0; i < 9U; i++) {
        if (GPIOPinRead(GPIO_PORTB_BASE, GPIO_PIN_3) & GPIO_PIN_3) {
            break;  // SDA 已释放
        }
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, 0);
        SysCtlDelay(g_sys_clock / 600000U);   // SCL 低电平约 5us
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_2, GPIO_PIN_2);
        SysCtlDelay(g_sys_clock / 600000U);   // SCL 高电平约 5us
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
    // RX 在 FIFO 达 1/8 满或接收超时时触发中断 及时取走字节 永不溢出
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

/* 带超时的 I2C 忙等待 运行在 1ms 扫描 ISR 内 所以上限必须很短。
   200kHz 下一个字节约 45us 2000 次轮询约 300us 覆盖正常传输
   若总线卡死则快速退出 避免饿死 UART RX FIFO */
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

/* TM4C 硬件勘误 发出 I2CMasterControl 后 BUSY 标志需要几个时钟周期才置位。
   如果马上轮询 I2CMasterBusy 可能读到 "空闲" 但传输实际已开始
   后续代码可能中途改写 MDR 寄存器导致线上数据损坏。
   因此先等待 BUSY 置位 再等它清除 */
static bool I2C0_WaitTransfer(void)
{
    uint32_t spin = 0;
    while (!I2CMasterBusy(I2C0_BASE)) {
        if (++spin > 200U) break;  // BUSY 极快置位 传输已完成
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

/* 利用 TCA6424 自动递增位 一次总线事务连续写入相邻两个寄存器。
   调用方需将 TCA6424_AUTO_INC 与起始寄存器地址按位或后传入 */
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
    /* 非阻塞发送 将字符串写入 TX 环形缓冲区 由 UART TX 中断消费。
       发送 LED 事件时设 g_tx_is_led_evt 门控 避免自身触发串口活动指示灯
       否则每次 LED 变化 TX 又点亮 LED3 形成自激振荡 */
    if (!g_tx_is_led_evt) {
        Mark_Serial_Activity();
    }
    UARTIntDisable(UART0_BASE, UART_INT_TX);
    while (*s) {
        uint16_t next = (uint16_t)((g_tx_head + 1U) % UART_TX_SIZE);
        if (next == g_tx_tail) {
            break; // 缓冲区满 丢弃剩余字节 绝不阻塞
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
    /* 仅记录串口活动时间窗口 LED3 由 Update_Status_LED 统一管理。
       在这里直接写 g_led_byte 会绕过夜间模式抑制导致物理 LED 误亮 */
    g_serial_activity_until_ms = g_tick_ms + SERIAL_LED_MS;
}

// ============================ §2 数码管显示驱动 ============================
// 将 8 位小数点状态打包为一个字节 每 bit 对应一位
static uint8_t Disp_Bitmap(void)
{
    uint8_t i, b = 0;
    for (i = 0; i < DISP_LEN; i++) {
        if (disp_dp[i]) b |= (uint8_t)(1U << i);
    }
    return b;
}

/* 设置 8 位数码管文本与小数点 bitmap。先将 dp_bitmap 展开到每位的 disp_dp 数组
   再原子写入 disp_buf / disp_dp / g_scan_limit 三个字段 确保扫描 ISR 不会读到半帧。
   g_scan_limit 裁掉尾部空白位 消除鬼影 */
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
    g_led_dirty = true;   // 置脏标志 定时器 ISR 下次扫描时写入 PCA9557
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
        case '~': return 0x63;   // 度符号 上方四段 a+b+f+g
        default: return 0x00;
    }
}

/* §2 每次刷新一位数码管 由 1ms 定时器 ISR 调用。
   支持 disp_on 整屏熄灭和 disp_blink_mask 按位闪烁 */
static void Display_Refresh(void)
{
    static bool blanked = false;
    uint8_t code;
    uint8_t err = 0;
    bool blink_off;

    if (!disp_on) {
        /* 仅在首次进入熄灭态时写一次 I2C 若每个 tick 都写两次字节
           会在高频 ISR 中卡住足够长时间饿死 UART RX FIFO */
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

    // 先清零位选消除鬼影 再通过一次自动递增突发写入段码 PORT1 和新位选 PORT2
    err |= I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, 0x00);
    err |= I2C0_WriteTwo(TCA6424_I2CADDR,
                         (uint8_t)(TCA6424_OUTPUT_PORT1 | TCA6424_AUTO_INC),
                         code, (uint8_t)(1U << g_scan_pos));

    if (err != 0U) {
        if (g_i2c_fail_count < 255U) g_i2c_fail_count++;
        if (g_i2c_fail_count >= 3U) {
            /* 不在 ISR 内恢复 I2C0_BusRecover 需要数 ms 级 bit-bang
               会饿死 UART RX FIFO 丢字节 交给主循环执行 */
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

// ============================ §7 流水滚动模块 ============================
// 设置滚动源文本与小数点 bitmap 复位偏移与完成标志
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

/* 以单趟不循环方式渲染流水 文本从屏幕边界扫过 最后一帧停在远端边界即完成。
   LEFT 方向 文本从右往左走 最后一个字符停在最左侧
   RIGHT 方向 文本从左往右走 第一个字符停在最右侧
   步进间隔由 g_scroll_speed 控制 慢速 500ms/步 快速 250ms/步 */
static void Scroll_Tick(void)
{
    char win[DISP_LEN + 1];
    uint8_t dp = 0, j;
    uint16_t interval = g_scroll_speed ? 250U : 500U;
    uint8_t s_max;

    if (scroll_len == 0U) { Display_SetStr("        ", 0); return; }
    // 步进范围 文本从右侧进入 8 位数码管 完全扫过后步进停止
    s_max = (uint8_t)(scroll_len + DISP_LEN - 2U);

    if (!scroll_completed && Tick_TimedOut(scroll_last_ms, interval)) {
        scroll_last_ms = g_tick_ms;
        if (scroll_off >= s_max) {
            scroll_completed = true;
        } else {
            scroll_off++;
        }
    }

    // 根据当前步进偏移和方向 计算 8 位窗口中每个位置应显示源文本的哪个字符
    for (j = 0; j < DISP_LEN; j++) {
        int16_t ti;
        // LEFT 窗口右边缘与步进偏移对齐 RIGHT 左边缘对齐
        if (g_format == FMT_LEFT) {
            ti = (int16_t)scroll_off - (int16_t)(DISP_LEN - 1U) + (int16_t)j;
        } else {
            ti = (int16_t)(scroll_len - 1U) - (int16_t)scroll_off + (int16_t)j;
        }
        if (ti >= 0 && ti < (int16_t)scroll_len) {
            win[j] = scroll_buf[ti];  // ti 有效 取对应字符
            // 小数点跟随规则 LEFT 方向用本位小数点 RIGHT 方向用下一位小数点
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

// 仅在内容变化时重置源文本 否则保持偏移并按步进渲染
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

// ============================ 显示内容构建 ============================
static void Build_Display(void)
{
    char text[40];
    char base[9];
    uint8_t dp = 0;
    uint8_t i;

    // 显示关闭时写入空格 确保 PC 镜像同步全暗
    if (!disp_on) { Display_SetStr("        ", 0); return; }

    // 显示优先级从高到低 情景倒计时 > 天气短显 > 消息/流水 > 时钟
    // 倒计时逻辑直接内联 避免额外函数调用栈压(与旧 countdown 同模)
    if (g_cd_state != CD_IDLE) {
        // ---- 情景倒计时显示 (内联) ----
        if (g_cd_state == CD_EDIT) {
            uint8_t cd_mm, cd_ss;
            char cd_base[9];
            cd_mm = g_cd_min; cd_ss = g_cd_sec;
            if (g_cd_field == 2) {
                text[0]='C'; text[1]='d'; text[2]=' '; text[3]=' ';
                text[4]='S'; text[5]='C';
                text[6]=(uint8_t)('0'+g_cd_scene); text[7]=' '; text[8]='\0'; dp=0x00;
            } else {
                text[0]='C'; text[1]='d';
                text[2]=(uint8_t)('0'+cd_mm/10U); text[3]=(uint8_t)('0'+cd_mm%10U);
                text[4]=(uint8_t)('0'+cd_ss/10U); text[5]=(uint8_t)('0'+cd_ss%10U);
                text[6]=' '; text[7]=' '; text[8]='\0'; dp=0x08;
            }
            for(i=0;i<8;i++)cd_base[i]=text[i]?text[i]:' ';cd_base[8]='\0';
            if(!g_blink_on){uint8_t st=(uint8_t)(2U+g_cd_field*2U);cd_base[st]=' ';cd_base[st+1]=' ';}
            // RIGHT 镜像还原
            if(g_format==FMT_RIGHT){char rev[9];uint8_t rdp=0;uint8_t j;
                for(j=0;j<8;j++)rev[j]=cd_base[7-j];rev[8]='\0';
                for(j=0;j<7;j++){if(dp&(1U<<j))rdp|=(uint8_t)(1U<<(6-j));}
                for(j=0;j<8;j++)cd_base[j]=rev[j];dp=rdp;}
            Display_SetStr(cd_base,dp);
        } else if (g_cd_state == CD_RUN || g_cd_state == CD_PAUSE) {
            uint8_t cd_mm=(uint8_t)(g_cd_remain_s/60U),cd_ss=(uint8_t)(g_cd_remain_s%60U);
            text[0]='C';text[1]='d';
            text[2]=(uint8_t)('0'+cd_mm/10U);text[3]=(uint8_t)('0'+cd_mm%10U);
            text[4]=(uint8_t)('0'+cd_ss/10U);text[5]=(uint8_t)('0'+cd_ss%10U);
            text[6]=' ';text[7]=' ';text[8]='\0';dp=0x08;
            if(g_cd_state==CD_PAUSE&&!g_blink_on){for(i=0;i<8;i++)text[i]=' ';dp=0x00;}
            if(g_format==FMT_RIGHT){char rev[9];uint8_t rdp=0;uint8_t j;
                for(j=0;j<8;j++)rev[j]=text[7-j];rev[8]='\0';
                for(j=0;j<7;j++){if(dp&(1U<<j))rdp|=(uint8_t)(1U<<(6-j));}
                for(j=0;j<8;j++)text[j]=rev[j];dp=rdp;}
            Display_SetStr(text,dp);
        } else if (g_cd_state == CD_DONE) {
            if(g_cd_scene==0){Scroll_Ensure(g_cd_active_msg,0);if(scroll_completed)Countdown_Stop();}
            else if(g_cd_scene==1){
                if(g_blink_on){char dpy[9]="DONE    ";Display_SetStr(dpy,0x00);}
                else Display_SetStr("        ",0x00);
                if(Tick_TimedOut(g_cd_done_ms,10000U))Countdown_Stop();}
            else{char dpy[9]="--00--  ";Display_SetStr(dpy,0x00);
                if(Tick_TimedOut(g_cd_done_ms,10000U))Countdown_Stop();}
        } else { g_cd_state=CD_IDLE; }  // 防御: 状态值异常时回 IDLE
        // countdown 占用了显示 跳过天气/消息/时钟
        return;
    } else if (g_weather_until_ms > g_tick_ms) {
        // USER2 触发的天气短显 5 秒窗口
        char wt[9];
        uint8_t wdp = 0;
        uint8_t wl;
        if (!g_weather_valid) {
            // 尚无天气数据 显示占位符
            memcpy(wt, "--~C--- ", 9);
        } else if ((g_tick_ms - g_weather_update_ms) > WEATHER_VALID_MS && !g_blink_on) {
            // 数据过期 在闪烁的灭半周期全暗以示过期警告
            memcpy(wt, "        ", 9);
        } else {
            wl = (uint8_t)strlen(g_weather_text);
            for (i = 0; i < 8; i++) wt[i] = (i < wl) ? g_weather_text[i] : ' ';
            wt[8] = '\0';
        }
        if (g_format == FMT_RIGHT) {
            char rev[9]; uint8_t rdp = 0; uint8_t j;
            for (j = 0; j < 8; j++) rev[j] = wt[7 - j];
            rev[8] = '\0';
            for (j = 0; j < 7; j++) { if (wdp & (1U << j)) rdp |= (uint8_t)(1U << (6 - j)); }
            for (j = 0; j < 8; j++) wt[j] = rev[j];
            wdp = rdp;
        }
        Display_SetStr(wt, wdp);
        return;
    } else if (g_message_until_ms > g_tick_ms && g_message[0]) {
        // PC 下发的滚动消息 根据长度决定静态还是流水
        if (strlen(g_message) <= 8) {
            // 短消息静态显示 右填空格
            char mb[9];
            uint8_t ml = (uint8_t)strlen(g_message);
            uint8_t mdp = (uint8_t)g_message_dp;
            for (i = 0; i < 8; i++) mb[i] = (i < ml) ? g_message[i] : ' ';
            mb[8] = '\0';
            if (g_format == FMT_RIGHT) {
                char rev[9]; uint8_t rdp = 0; uint8_t j;
                for (j = 0; j < 8; j++) rev[j] = mb[7 - j];
                rev[8] = '\0';
                for (j = 0; j < 7; j++) { if (mdp & (1U << j)) rdp |= (uint8_t)(1U << (6 - j)); }
                for (j = 0; j < 8; j++) mb[j] = rev[j];
                mdp = rdp;
            }
            Display_SetStr(mb, mdp);
        } else {
            // RIGHT 模式滚动下一位小数点规则 需对 bitmap 左移一位补偿
            uint32_t sd = g_format == FMT_RIGHT ? g_message_dp << 1U : g_message_dp;
            Scroll_Ensure(g_message, sd);
            // 完整播放一遍后立即返回时钟 不会出现循环重播
            if (scroll_completed) g_message_until_ms = 0;
        }
        return;
    } else if (g_mode == MODE_NIGHT) {
        // 夜间模式仅显示时和分 4 位 其余空白
        snprintf(text, sizeof(text), "%02u%02u    ", g_time.h, g_time.mi);
        dp = 0x02;
    } else if (g_edit_state == ST_EDIT_DATE) {
        // 编辑日期态 显示编辑值 yy.mm.dd
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 (uint8_t)(g_edit_date.y % 100U), g_edit_date.m, g_edit_date.d);
        dp = 0x0a;
    } else if (g_edit_state == ST_IDLE && g_display_mode == DISP_DATE) {
        // 时钟显示态 日期模式 yy.mm.dd
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 (uint8_t)(g_date.y % 100U), g_date.m, g_date.d);
        dp = 0x0a;
    } else if (g_edit_state == ST_IDLE && g_display_mode == DISP_YEAR) {
        // YYYY.MMDD 格式恰好 8 位 静态显示无需滚动 小数点仅在年份后第 3 位
        char yd[9];
        snprintf(yd, sizeof(yd), "%04u%02u%02u", g_date.y, g_date.m, g_date.d);
        Display_SetStr(yd, (uint8_t)(1U << 3));
        return;
    } else if (g_edit_state == ST_EDIT_ALARM) {
        // 编辑闹钟态 显示编辑值 hh.mm.ss 末尾 A 表示闹钟
        snprintf(text, sizeof(text), "%02u%02u%02u A",
                 g_edit_alarm.t.h, g_edit_alarm.t.mi, g_edit_alarm.t.s);
        dp = 0x0a;
    } else if (g_edit_state == ST_EDIT_TIME) {
        // 编辑时间态 显示编辑值 hh.mm.ss
        snprintf(text, sizeof(text), "%02u%02u%02u  ",
                 g_edit_time.h, g_edit_time.mi, g_edit_time.s);
        dp = 0x0a;
    } else {
        // 默认 时钟走时态 显示当前时间 hh.mm.ss
        snprintf(text, sizeof(text), "%02u%02u%02u  ", g_time.h, g_time.mi, g_time.s);
        dp = 0x0a;
    }

    for (i = 0; i < 8; i++) base[i] = text[i] ? text[i] : ' ';
    base[8] = '\0';

    /* 编辑态闪烁时先按逻辑 LEFT 位置抹除高亮字段 再做 FORMAT 镜像反转。
       这样可以保证 RIGHT 模式下高亮位置也与反转后的值对齐 不会错位 */
    if (g_edit_state != ST_IDLE && !g_blink_on) {
        uint8_t start = 0;
        if (g_edit_field == 1) start = 2;
        else if (g_edit_field == 2) start = 4;
        base[start] = ' ';
        base[start + 1] = ' ';
    }

    if (g_format == FMT_RIGHT) {
        /* RIGHT 模式将 8 位整体镜像反转 小数点也需跟随逆序到对称位置。
           例 dp 0x0A = bit1+bit3 反转后 dp 0x28 = bit5+bit3 分隔符保持在相同数字对之间 */
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
    // 构建 *EVT:DISP 报文 8 字符定长 空位填 _ 后跟空格和 2 位十六进制 dp bitmap
    for (i = 0; i < 8; i++) {
        char c = (char)disp_buf[i];
        enc[i] = (c == '\0' || c == ' ') ? '_' : c;
    }
    enc[8] = '\0';
    UART_Printf("*EVT:DISP %s %02X\r\n", enc, Disp_Bitmap());
}

static void Send_LED_Event(void)
{
    // 门控 本次 TX 不触发串口活动指示灯 避免自激振荡
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

// ============================ §3 按键扫描与事件队列 ============================
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

/* 从定时器 ISR 读取 TCA 按键电平 加上 GPIO 读取 USER1/USER2
   经过软件去抖后 运行逐键状态机 将 DOWN/UP/LONG/REPEAT 事件写入队列 */
static void Key_Scan(void)
{
    // 从定时器 ISR 采样的 TCA 原始电平读取 8 个 I2C 按键
    uint16_t raw = g_tca_key_raw;
    uint32_t pj;
    uint8_t b;

    // USER1 和 USER2 是独立 GPIO 输入 低电平按下 补充进 raw
    pj = GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    if ((pj & GPIO_PIN_0) == 0) raw |= (1U << (KEY_USER1 - 1));
    if ((pj & GPIO_PIN_1) == 0) raw |= (1U << (KEY_USER2 - 1));

    // 软件去抖 连续采样若干次稳定后才视为有效
    if (raw == g_key_last_raw) {
        if (g_key_same_count < 3) g_key_same_count++;
    } else {
        g_key_same_count = 0;
        g_key_last_raw = raw;
    }

    // 去抖通过后 检测上升沿/下降沿 推入事件队列
    if (g_key_same_count >= KEY_DEBOUNCE_COUNT) {
        uint16_t prev = g_key_stable;
        uint16_t pressed = (uint16_t)(raw & ~prev);      // 新按下的位
        uint16_t released = (uint16_t)(~raw & prev);     // 新释放的位
        g_key_stable = raw;

        for (b = 0; b < KEY_COUNT; b++) {
            key_code_t code = (key_code_t)(b + 1);
            if (pressed & (1U << b)) {
                // 按下 进入 PRESSED 状态 记录时间戳
                g_key_fsm[b] = 1;
                g_key_press_ms[b] = g_tick_ms;
                g_key_repeat_ms[b] = g_tick_ms;
                Key_Push(code, KEV_DOWN);
            }
            if (released & (1U << b)) {
                // 释放 回到 IDLE 状态
                g_key_fsm[b] = 0;
                Key_Push(code, KEV_UP);
            }
        }
    }

    // 长按与连发由时间驱动 与边沿检测独立
    for (b = 0; b < KEY_COUNT; b++) {
        key_code_t code = (key_code_t)(b + 1);
        if (g_key_fsm[b] == 0) continue;
        if (g_key_fsm[b] == 1 && Tick_TimedOut(g_key_press_ms[b], FUNC_LONG_MS)) {
            g_key_fsm[b] = 2;
            Key_Push(code, KEV_LONG);
        }
        if ((code == KEY_ADD && (g_edit_state != ST_IDLE || g_cd_state == CD_EDIT)) ||
            (code == KEY_SAVE && g_cd_state == CD_EDIT)) {
            if (Tick_TimedOut(g_key_repeat_ms[b], ADD_REPEAT_MS)) {
                g_key_repeat_ms[b] = g_tick_ms;
                Key_Push(code, KEV_REPEAT);
            }
        }
    }
}

/* 将按键事件分发到 Handle_Key 执行对应功能。
   FUNC 和 USER1 采用延迟判断 按下时暂存 等抬起或长按后再决定短按/长按动作 */
static void Dispatch_Key(key_event_t ev, key_code_t code)
{
    if (ev == KEV_DOWN && g_message_until_ms > g_tick_ms && code >= KEY_DISP) {
        g_message_until_ms = 0;   // 任意非编辑键打断消息显示
    }

    switch (code) {
        case KEY_FUNC:
            if (ev == KEV_DOWN) {
                Send_Key_Event(KEY_FUNC);
                if (g_alarm.ringing) {
                    Handle_Key(KEY_FUNC);     // 响铃中 FUNC 立即止铃
                    g_func_armed = false;
                } else if (g_cd_state == CD_DONE) {
                    g_func_armed = true;      // 静默 DONE 下允许长按 FUNC 退出
                } else if (g_cd_state != CD_IDLE) {
                    g_func_armed = false;     // 倒计时运行中 FUNC 不进时钟编辑 避免冲突
                } else if (g_edit_state == ST_IDLE) {
                    Handle_Key(KEY_FUNC);     // 空闲态短按即进入编辑
                    g_func_armed = false;
                } else {
                    g_func_armed = true;      // 编辑中暂缓 等抬起循环 / 长按保存
                }
            } else if (ev == KEV_LONG) {
                // 同一次物理按压在 KEV_DOWN 已上报过一次 EVT KEY FUNC
                // 长按不再重复上报 仅执行保存或退出动作 避免 PC LOG 多出一次 FUNC
                if (g_func_armed) {
                    if (g_cd_state == CD_DONE) Handle_Key(KEY_FUNC);
                    else Handle_Key(KEY_SAVE);
                    g_func_armed = false;
                }
            } else if (ev == KEV_UP) {
                if (g_func_armed) {
                    if (g_cd_state != CD_DONE) {
                        Handle_Key(KEY_FUNC); // 短按 循环切换编辑字段或状态
                    }
                    g_func_armed = false;
                }
            }
            break;
        case KEY_USER1:
            if (ev == KEV_DOWN) {
                g_user1_armed = true;
            } else if (ev == KEV_LONG) {
                if (g_user1_armed) {
                    Show_Ntp_Status();        // 长按显示 NTP 同步状态
                    g_user1_armed = false;
                }
            } else if (ev == KEV_UP) {
                if (g_user1_armed) {
                    Send_Key_Event(KEY_USER1); // 短按上报事件 PC 收到后自动触发 NTP 对时
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
        case KEY_SAVE:
            if (ev == KEV_DOWN) {
                Send_Key_Event(KEY_SAVE);
                Handle_Key(KEY_SAVE);
            } else if (ev == KEV_REPEAT && g_cd_state == CD_EDIT) {
                Handle_Key(KEY_SAVE);
            }
            break;
        case KEY_EXT:
            // 短按 启动/暂停/继续(走 Countdown_HandleKey) 长按 停止/取消倒计时
            if (ev == KEV_DOWN) {
                Send_Key_Event(KEY_EXT);  // 保留 §3.7 EXT 触发 *EVT:KEY EXT
                g_ext_armed = true;
                g_ext_long = false;
            } else if (ev == KEV_LONG) {
                // 同一次物理按压在 KEV_DOWN 已上报过一次 EVT KEY EXT
                // 长按不再重复上报 仅执行停止倒计时 避免 PC LOG 多出一次 EXT
                if (g_ext_armed) {
                    if (g_cd_state != CD_IDLE) Countdown_Stop();
                    g_ext_armed = false;
                    g_ext_long = true;  // 标记本次为长按 抬起时不再当短按进编辑
                }
            } else if (ev == KEV_UP) {
                // 长按已处理过就直接清标记 不再走短按 避免抬起又进入倒计时编辑
                if (g_ext_long) {
                    g_ext_long = false;
                } else if (g_ext_armed) {
                    Handle_Key(KEY_EXT);
                    g_ext_armed = false;
                }
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
    // 每次按键操作刷新编辑超时计时
    g_edit_last_ms = g_tick_ms;

    // 流水消息播放中 任意非编辑键立即中断 与物理按键一致
    // 编辑键为 FUNC SHIFT ADD SAVE 即 key 小于 KEY_DISP 不打断
    // 物理键已在 Dispatch_Key 按下沿打断 这里覆盖虚拟键 SET KEY 直达路径
    if (g_message_until_ms > g_tick_ms && key >= KEY_DISP) {
        g_message_until_ms = 0;
    }

    // 响铃中 FUNC 无条件止铃 优先级最高; 若在倒计时 DONE 中也结束倒计时
    if (g_alarm.ringing && key == KEY_FUNC) {
        g_alarm.ringing = 0;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        UART_PutString("*EVT:ALARM_OFF\r\n");
        if (g_cd_state == CD_DONE) Countdown_Stop();
        return;
    }

    // 情景倒计时按键 EXT/SHIFT/ADD 在非 IDLE 态优先消费 不落入时钟编辑逻辑
    if (Countdown_HandleKey(key)) {
        return;
    }

    switch (key) {
        case KEY_FUNC:
            // FUNC 循环切换编辑状态 空闲→编辑日期→编辑时间→编辑闹钟→空闲
            if (g_edit_state == ST_IDLE) {
                // 进入编辑前快照日期当前值 放弃时回退
                // 时间与闹钟不取运行值 首次为静态初值 00.00.00 之后保留上次编辑值
                g_edit_date = g_date;
                g_edit_state = ST_EDIT_DATE;
            } else if (g_edit_state == ST_EDIT_DATE) {
                g_edit_state = ST_EDIT_TIME;
            } else if (g_edit_state == ST_EDIT_TIME) {
                g_edit_state = ST_EDIT_ALARM;
            } else {
                g_edit_state = ST_IDLE;
            }
            g_edit_field = 0;
            break;
        case KEY_SHIFT:
            // 编辑态下 SHIFT 循环左移高亮字段
            if (g_edit_state != ST_IDLE) g_edit_field = (uint8_t)((g_edit_field + 2U) % 3U);
            break;
        case KEY_ADD:
            // ADD 根据当前编辑状态和目标字段 +1 含进位与范围钳制
            if (g_edit_state == ST_EDIT_DATE) {
                if (g_edit_field == 0) {
                    g_edit_date.y++;
                    // 年份递增后检查日期是否溢出当月 如闰年 2.29→3.1 在正常化中也会处理
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) {
                        g_edit_date.d = Days_In_Month(g_edit_date.y, g_edit_date.m);
                    }
                } else if (g_edit_field == 1) {
                    g_edit_date.m++;
                    if (g_edit_date.m > 12) g_edit_date.m = 1;  // 月溢出回绕
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) {
                        g_edit_date.d = Days_In_Month(g_edit_date.y, g_edit_date.m);
                    }
                } else {
                    g_edit_date.d++;
                    if (g_edit_date.d > Days_In_Month(g_edit_date.y, g_edit_date.m)) {
                        g_edit_date.d = 1;
                        g_edit_date.m++;
                        if (g_edit_date.m > 12) {
                            g_edit_date.m = 1;
                            g_edit_date.y++;
                        }
                    }
                }
            } else if (g_edit_state == ST_EDIT_TIME) {
                // 时/分/秒按真实时间进位 23:59:59 -> 00:00:00
                if (g_edit_field == 0) {
                    g_edit_time.h = (uint8_t)((g_edit_time.h + 1U) % 24U);
                } else if (g_edit_field == 1) {
                    g_edit_time.mi++;
                    if (g_edit_time.mi >= 60U) {
                        g_edit_time.mi = 0;
                        g_edit_time.h = (uint8_t)((g_edit_time.h + 1U) % 24U);
                    }
                } else {
                    g_edit_time.s++;
                    if (g_edit_time.s >= 60U) {
                        g_edit_time.s = 0;
                        g_edit_time.mi++;
                        if (g_edit_time.mi >= 60U) {
                            g_edit_time.mi = 0;
                            g_edit_time.h = (uint8_t)((g_edit_time.h + 1U) % 24U);
                        }
                    }
                }
            } else if (g_edit_state == ST_EDIT_ALARM) {
                if (g_edit_field == 0) {
                    g_edit_alarm.t.h = (uint8_t)((g_edit_alarm.t.h + 1U) % 24U);
                } else if (g_edit_field == 1) {
                    g_edit_alarm.t.mi++;
                    if (g_edit_alarm.t.mi >= 60U) {
                        g_edit_alarm.t.mi = 0;
                        g_edit_alarm.t.h = (uint8_t)((g_edit_alarm.t.h + 1U) % 24U);
                    }
                } else {
                    g_edit_alarm.t.s++;
                    if (g_edit_alarm.t.s >= 60U) {
                        g_edit_alarm.t.s = 0;
                        g_edit_alarm.t.mi++;
                        if (g_edit_alarm.t.mi >= 60U) {
                            g_edit_alarm.t.mi = 0;
                            g_edit_alarm.t.h = (uint8_t)((g_edit_alarm.t.h + 1U) % 24U);
                        }
                    }
                }
                g_edit_alarm.enabled = 1;  // 编辑闹钟后自动使能
            }
            break;
        case KEY_SAVE:
            // 编辑值写入运行态 上报 *EVT:EDIT 事件
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
            /* 仅通过 *SET:KEY USER1 到达 PC 虚拟长按。
               物理短按上报 *EVT:KEY USER1 不经过此处
               物理长按直接调用 Show_Ntp_Status 也不经过此分支。
               所以此分支对应虚拟长按行为 显示 NTP 同步状态 */
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

// ============================ §6 时间走时 / §8 闹钟逻辑 ============================
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
        g_edit_state = ST_IDLE;   // 5 秒无操作自动退出编辑 不保存
    }

    // 情景倒计时按秒递减/到点(内联 零栈压)
    if (g_cd_state == CD_EDIT) {
        if (Tick_TimedOut(g_edit_last_ms, 5000U)) { g_cd_state = CD_IDLE; Send_CD_Event(); }
    } else if (g_cd_state == CD_RUN) {
        if (g_cd_remain_s > 0U) g_cd_remain_s--;
        if (g_cd_remain_s == 0U) {
            g_cd_state = CD_DONE; g_cd_done_ms = g_tick_ms;
            Scroll_Reset();
            if (g_cd_scene == 0) {
                Scroll_Ensure(g_cd_active_msg, 0);
            }
            if (g_cd_scene != 2) {
                g_alarm.ringing = 1; g_alarm_ring_start_ms = g_tick_ms;
                g_alarm_last_beep_ms = 0; g_ring_limit_ms = 10000U;
            }
            UART_PutString("*EVT:CD DONE\r\n");
        } else {
            Send_CD_Event();
        }
    }

    if (g_alarm.enabled && !g_alarm.ringing &&
        g_time.h == g_alarm.t.h && g_time.mi == g_alarm.t.mi && g_time.s == g_alarm.t.s) {
        g_alarm.ringing = 1;
        g_alarm_ring_start_ms = g_tick_ms;
        // 基础响铃 8.8 秒自动停止 雨雪天气追加 3 声共 1200ms 总计不超 10 秒
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
    /* LED0 为 1Hz 系统心跳 500ms 亮 / 500ms 灭 作为所有模式下的基准位。
       夜间模式只保留心跳 *SET:LED 接管后心跳也被暂停 */
    uint8_t led = (((g_tick_ms / 500U) & 1U) == 0U) ? 0x01U : 0x00U;
    // 快速闪烁 200ms 周期 用于闹钟响铃指示
    bool fast = (((g_tick_ms / 200U) & 1U) == 0U);
    // 慢速闪烁 500ms 周期 用于雨雪/高温/NTP 漂移等扩展指示
    bool slow = (((g_tick_ms / 500U) & 1U) == 0U);

    // 10 秒内未收到新的 *SET:LED 自动退出接管模式 恢复 LED 原有含义
    if (g_led_override && Tick_TimedOut(g_led_override_ms, 10000U)) {
        g_led_override = false;
    }

    // 夜间模式只保留心跳 LED0 其余全灭
    if (g_mode != MODE_NIGHT) {
        // LED1 闹钟使能常亮 / 响铃时快闪
        if (g_alarm.ringing) {
            if (fast) led |= 0x02;
        } else if (g_alarm.enabled) {
            led |= 0x02;
        }
        // LED2 编辑模式常亮
        if (g_edit_state != ST_IDLE) led |= 0x04;
        // LED3 串口收发活动 收发后亮约 100ms
        if (g_serial_activity_until_ms > g_tick_ms) led |= 0x08;
        // 扩展指示 LED4-7 仅在天气/对时有效
        if (g_weather_valid) {
            if (g_weather_code == WEATHER_SUN) led |= 0x10;               // LED4 晴 常亮
            if ((g_weather_code == WEATHER_RAI || g_weather_code == WEATHER_SNO) && slow) led |= 0x20;  // LED5 雨雪 慢闪
            if (g_weather_temp >= 30) led |= 0x40;                         // LED6 高温 常亮
        }
        if (g_ntp_synced) {
            if ((g_tick_ms - g_ntp_last_sync_ms) > 86400000UL) {
                if (slow) led |= 0x80;  // 超过 24h 未同步 LED7 慢闪
            } else {
                led |= 0x80;            // 已同步 LED7 常亮
            }
        }
        // 高温 >=30°C 且闹钟响铃时 8 位 LED 整体慢闪 覆盖原有指示 仅响铃期间有效
        if (g_alarm.ringing && g_weather_valid && g_weather_temp >= 30) {
            led = slow ? 0xFFU : 0x00U;
        }
    }

    // 情景倒计时 LED 接管(内联 零栈压): RUN/PAUSE 作进度条, DONE 全闪
    if (g_cd_state != CD_IDLE && g_mode != MODE_NIGHT) {
        if (g_cd_state == CD_RUN || g_cd_state == CD_PAUSE) {
            uint8_t lit;
            if (g_cd_total_s == 0U) { lit = 0; }
            else lit = (uint8_t)(((uint32_t)g_cd_remain_s * 8U + g_cd_total_s - 1U) / g_cd_total_s);
            led = (uint8_t)(lit >= 8U ? 0xFFU : (uint8_t)((1U << lit) - 1U));
            if (g_cd_state == CD_PAUSE && !slow) led = 0x00U;
        } else if (g_cd_state == CD_DONE) {
            led = fast ? 0xFFU : 0x00U;
        }
    }

    if (!g_led_override) {
        Set_LED(led);
    }
}

static void Show_Ntp_Status(void)
{
    // 短显 NTP 同步状态 "n SY xx" n=距上次同步小时数 0-9 钳位 xx=OK/DR/NO
    if (g_ntp_synced) {
        uint32_t hours = (g_tick_ms - g_ntp_last_sync_ms) / 3600000UL;
        const char *code = ((g_tick_ms - g_ntp_last_sync_ms) > 86400000UL) ? "DR" : "OK";
        if (hours > 9UL) hours = 9UL;
        snprintf(g_message, sizeof(g_message), "%luSY%s", (unsigned long)hours, code);
    } else {
        snprintf(g_message, sizeof(g_message), "_SYNO");
    }
    // 小数点位于 n 后第 0 位和 SY 后第 2 位
    g_message_dp = (uint8_t)((1U << 0) | (1U << 2));
    g_message_until_ms = g_tick_ms + 3000U;
    Scroll_Reset();
}

// ============================ §自主功能 情景倒计时 ============================
/*
 * 动机: 把原有单段倒计时升级为多段状态机——板上按键可独立编辑与运行, 到点按情景
 *      (滚动文本/闪烁庆祝/静默) 收尾, 8LED 作进度条, PC 端进度环+TTS 语音联动。
 * 设计: CD_IDLE→EDIT→RUN⇄PAUSE→DONE→IDLE。EXT 作启动/暂停/跳过键,
 *      SHIFT/ADD 在编辑态切字段与增值。到点复用闹钟蜂鸣 + Scroll 流水。
 * 实现: 以下 Countdown_* 函数族全部集中在本节, 对原有函数的接入仅单行守卫。
 * 关键代码位置: Countdown_HandleKey(§L1583) / Build_Display 内联倒计时显示(§L1014) /
 *              Update_Status_LED 内联倒计时 LED(§L1574) / Time_Tick_1s 内联递减(§L1497) /
 *              Countdown_HandleCommand(§L1774) / Process_Command 的 strncmp 派发(§L2237)。
 */

/* Send_CD_Event: 状态切换时通过 UART 上报, PC 据此同步进度环。
   仅发固定格式字符串避免 vsnprintf 栈分配。 */
static void Send_CD_Event(void)
{
    uint8_t idx = (uint8_t)g_cd_state;
    char buf[48];
    uint8_t pos = 0;
    const char *src;
    if (idx > 4U) idx = 0U;
    src = CD_STATE_NAMES[idx];
    buf[pos++] = '*'; buf[pos++] = 'E'; buf[pos++] = 'V'; buf[pos++] = 'T';
    buf[pos++] = ':'; buf[pos++] = 'C'; buf[pos++] = 'D'; buf[pos++] = ' ';
    buf[pos++] = 'S'; buf[pos++] = 'T'; buf[pos++] = 'A'; buf[pos++] = 'T';
    buf[pos++] = 'E'; buf[pos++] = ' ';
    while (*src) buf[pos++] = *src++;
    buf[pos++] = ' ';
    /* 手工写 remain 数值 (最多4位十进制) */
    {
        uint16_t r = g_cd_remain_s;
        uint8_t d[5]; int8_t di = 0;
        if (r == 0U) d[di++] = '0';
        else { while (r) { d[di++] = (uint8_t)('0' + (r % 10U)); r /= 10U; } }
        while (di) buf[pos++] = d[--di];
    }
    buf[pos++] = ' ';
    {
        uint16_t t = g_cd_total_s;
        uint8_t d[5]; int8_t di = 0;
        if (t == 0U) d[di++] = '0';
        else { while (t) { d[di++] = (uint8_t)('0' + (t % 10U)); t /= 10U; } }
        while (di) buf[pos++] = d[--di];
    }
    buf[pos++] = ' ';
    buf[pos++] = (uint8_t)('0' + g_cd_scene);
    buf[pos++] = '\r'; buf[pos++] = '\n'; buf[pos] = '\0';
    UART_PutString(buf);
}

// 启动倒计时 把编辑分秒合成总秒 钳到 1-3599 复位剩余并进入 RUN
static void Countdown_Start(void)
{
    uint16_t total = (uint16_t)(g_cd_min * 60U + g_cd_sec);
    if (total == 0U) return;   // 00.00 不启动 必须先设非零时长
    if (total > 3599U) total = 3599U;
    strncpy(g_cd_active_msg, g_cd_msg, sizeof(g_cd_active_msg) - 1U);
    g_cd_active_msg[sizeof(g_cd_active_msg) - 1U] = '\0';
    if (g_cd_active_msg[0] == '\0' ||
        (g_cd_active_msg[0] == 'D' && g_cd_active_msg[1] == 'O' &&
         g_cd_active_msg[2] == 'N' && g_cd_active_msg[3] == 'E')) {
        strcpy(g_cd_active_msg, "CONGRATULATIONS");
    }
    g_cd_total_s = total;
    g_cd_remain_s = total;
    g_cd_state = CD_RUN;
    Scroll_Reset();
    Send_CD_Event();
}

// 停止/取消倒计时 回到 IDLE 同时止住可能的到点响铃 LED/显示随之恢复
static void Countdown_Stop(void)
{
    g_cd_state = CD_IDLE;
    g_cd_remain_s = 0;
    if (g_alarm.ringing) {
        g_alarm.ringing = 0;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
    }
    Scroll_Reset();
    Send_CD_Event();
}

/* 倒计时按键处理 仅在非 IDLE 态消费 EXT/SHIFT/ADD 返回 true 表示已处理。
   IDLE 态下 EXT 进入编辑 其余键放行给原有逻辑 */
static bool Countdown_HandleKey(key_code_t key)
{
    if (g_cd_state == CD_IDLE) {
        // 空闲态 EXT 进入编辑 要求不在时钟编辑中 避免两套编辑冲突
        if (key == KEY_EXT && g_edit_state == ST_IDLE) {
            g_cd_state = CD_EDIT;
            g_cd_field = 0;
            Scroll_Reset();
            Send_CD_Event();
            return true;
        }
        return false;
    }

        /* 仅消费倒计时相关键(EXT/SHIFT/ADD/SAVE)。其余键(DISP/SPEED/FORMAT 等)放行,
       使 FORMAT 仍可在倒计时中翻转方向、SPEED 调滚动速度;
       FUNC 已在 Dispatch_Key 中单独守卫不进时钟编辑 */
    switch (g_cd_state) {
        case CD_EDIT:
            if (key == KEY_SHIFT) {
                g_cd_field = (uint8_t)((g_cd_field + 1U) % 2U);  // 分秒二字段互相切换
                return true;
            }
            if (key == KEY_ADD) {
                if (g_cd_field == 0) g_cd_min = (uint8_t)((g_cd_min + 1U) % 60U);
                else g_cd_sec = (uint8_t)((g_cd_sec + 1U) % 60U);
                return true;
            }
            if (key == KEY_SAVE) {
                if (g_cd_field == 0) g_cd_min = (uint8_t)((g_cd_min == 0U) ? 59U : (g_cd_min - 1U));
                else g_cd_sec = (uint8_t)((g_cd_sec == 0U) ? 59U : (g_cd_sec - 1U));
                return true;
            }
            if (key == KEY_EXT) {
                // 时长仍为 00.00 时不允许启动 必须先改成非零 留在编辑态等待修改
                if (g_cd_min == 0U && g_cd_sec == 0U) return true;
                Countdown_Start();   // 确认并启动
                return true;
            }
            return false;   // 其余键放行(如 FORMAT)
        case CD_RUN:
            if (key == KEY_EXT) { g_cd_state = CD_PAUSE; Send_CD_Event(); return true; }
            return false;
        case CD_PAUSE:
            if (key == KEY_EXT) { g_cd_state = CD_RUN; Send_CD_Event(); return true; }
            return false;
        case CD_DONE:
            if (key == KEY_EXT || key == KEY_FUNC) { Countdown_Stop(); return true; }
            return false;
        default:
            return false;
    }
}

/* *SET:COUNTDOWN 子命令解析 返回 true 表示已处理(含错误应答)。
   支持 TIME/SCENE/MSG/START/PAUSE/RESUME/STOP 及向后兼容的纯秒数形式 */
static bool Countdown_HandleCommand(char *line, char **argv, uint8_t argc)
{
    uint16_t v16;
    uint8_t v8;

    if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return true; }

    if (Token_Matches(argv[1], "TIME")) {
        if (argc < 3 || !Parse_U16(argv[2], &v16) || v16 == 0U || v16 > 3599U) {
            UART_PutString("ERROR RANGE\r\n"); return true;
        }
        g_cd_total_s = v16;
        g_cd_min = (uint8_t)(v16 / 60U);
        g_cd_sec = (uint8_t)(v16 % 60U);
        UART_PutString("OK\r\n");
        return true;
    }
    if (Token_Matches(argv[1], "SCENE")) {
        if (argc < 3 || !Parse_U8(argv[2], &v8) || v8 > 2U) {
            UART_PutString("ERROR RANGE\r\n"); return true;
        }
        g_cd_scene = v8;
        UART_PutString("OK\r\n");
        return true;
    }
    if (Token_Matches(argv[1], "MSG")) {
        const char *raw = line;
        uint8_t spaces = 0, len = 0;
        while (*raw && spaces < 2U) {
            if (*raw == ' ' || *raw == '\t') {
                while (*raw == ' ' || *raw == '\t') raw++;
                spaces++;
            } else { raw++; }
        }
        if (!*raw) { UART_PutString("ERROR SYNTAX\r\n"); return true; }
        while (raw[len] && raw[len] != '\r' && raw[len] != '\n' && len < 32U) len++;
        if (raw[len] && raw[len] != '\r' && raw[len] != '\n') {
            UART_PutString("ERROR RANGE\r\n"); return true;
        }
        memcpy(g_cd_msg, raw, len); g_cd_msg[len] = '\0';
        g_cd_scene = 0;
        Scroll_Reset();
        UART_PutString("OK\r\n");
        return true;
    }
    if (Token_Matches(argv[1], "START")) {
        Countdown_Start(); UART_PutString("OK\r\n"); return true;
    }
    if (Token_Matches(argv[1], "PAUSE")) {
        if (g_cd_state == CD_RUN) { g_cd_state = CD_PAUSE; Send_CD_Event(); }
        UART_PutString("OK\r\n"); return true;
    }
    if (Token_Matches(argv[1], "RESUME")) {
        if (g_cd_state == CD_PAUSE) { g_cd_state = CD_RUN; Send_CD_Event(); }
        UART_PutString("OK\r\n"); return true;
    }
    if (Token_Matches(argv[1], "STOP")) {
        Countdown_Stop(); UART_PutString("OK\r\n"); return true;
    }
    // 向后兼容 *SET:COUNTDOWN <sec> 设时长并立即启动
    if (Parse_U16(argv[1], &v16)) {
        if (v16 == 0U || v16 > 3599U) { UART_PutString("ERROR RANGE\r\n"); return true; }
        g_cd_min = (uint8_t)(v16 / 60U); g_cd_sec = (uint8_t)(v16 % 60U);
        Countdown_Start(); UART_PutString("OK\r\n"); return true;
    }

    UART_PutString("ERROR PARAM\r\n");
    return true;
}

static void Alarm_Service(void)
{
    if (!g_alarm.ringing) return;
    // 超时自动止铃
    if (Tick_TimedOut(g_alarm_ring_start_ms, g_ring_limit_ms)) {
        g_alarm.ringing = 0;
        g_beep_active = false;                    // 长响标志复位
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        UART_PutString("*EVT:ALARM_OFF\r\n");
        return;
    }
    // BEEP 远程蜂鸣长响 不走节奏翻转 保持蜂鸣器持续导通
    if (g_beep_active) return;
    // 夜间模式抑制蜂鸣器 闹钟不响
    if (g_mode == MODE_NIGHT) {
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
        return;
    }
    // 200ms 周期翻转蜂鸣器 200ms 响 / 200ms 停 节奏式响铃
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

// 蔡勒公式计算星期 0 = 周日 6 = 周六
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

// 日期规范化 处理月份溢出与月末进位 自动修正星期
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

// 构建 8 位 LED 掩码 LED i+1 点亮当且仅当数码管第 i 位显示非空字符
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
    /* 开机画面 每阶段 800-1000ms 总时长 4-8s 每阶段至少闪烁一次 总计约 5.6s。
       每阶段 LED 与数码管同位同步亮灭 空位对应的 LED 保持熄灭 */
    uint32_t t;

    // 阶段 1 全亮 88888888 + LED 全亮 -> 全灭
    Display_SetStr("88888888", 0xff); Set_LED(Startup_Led_Mask("88888888"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));

    // 阶段 2 学号后 8 位闪烁一次
    Display_SetStr("42910013", 0x00); Set_LED(Startup_Led_Mask("42910013"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 200U));
    Display_SetStr("42910013", 0x00); Set_LED(Startup_Led_Mask("42910013"));
    t = g_tick_ms; while (!Tick_TimedOut(t, 300U));

    // 阶段 3 姓名拼音闪烁一次
    Display_SetStr("YANZUO  ", 0x00); Set_LED(Startup_Led_Mask("YANZUO  "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 900U));
    Display_SetStr("        ", 0x00); Set_LED(0x00);
    t = g_tick_ms; while (!Tick_TimedOut(t, 200U));
    Display_SetStr("YANZUO  ", 0x00); Set_LED(Startup_Led_Mask("YANZUO  "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 300U));

    // 阶段 4 版本号 v1.0 保持 1s 小数点在第 1 位后
    Display_SetStr("V10     ", 0x02); Set_LED(Startup_Led_Mask("V10     "));
    t = g_tick_ms; while (!Tick_TimedOut(t, 1000U));
}

static void Upper_Copy(char *dst, const char *src, uint16_t max)
{
    uint16_t i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

/* 缩写匹配 algorithm 把 token 和 pattern 都转为大写后比对。
   pattern 中大写字母为必输部分 小写字母可省略。
   如 pattern = "MINute" 则 "MIN"/"MINU"/"MINUT"/"MINUTE" 均合法
   但 "MI" 不合法 因为大写 N 未输入 */
static bool Token_Matches(const char *token, const char *pattern)
{
    char t[24], p[24];
    uint8_t req = 0, i;
    Upper_Copy(t, token, sizeof(t));
    Upper_Copy(p, pattern, sizeof(p));
    // 统计 pattern 中大写字母数量 即最少必输字符数
    for (i = 0; pattern[i]; i++) if (isupper((unsigned char)pattern[i])) req++;
    // token 长度必须 >= 必输字符数 <= 完整 pattern 长度
    if (strlen(t) < req || strlen(t) > strlen(p)) return false;
    return strncmp(p, t, strlen(t)) == 0;
}

static bool Command_Matches(const char *token, const char *pattern)
{
    if (Token_Matches(token, pattern)) return true;
    if (pattern[0] == '*' && Token_Matches(token, pattern + 1)) return true;
    return false;
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

// GET 应答通过此函数输出 自动处理 RIGHT 模式的字符串逆序
static void Send_OK_Value(const char *value)
{
    char out[32];
    uint8_t len = (uint8_t)strlen(value), i;
    if (g_format == FMT_LEFT) {
        UART_Printf("OK %s\r\n", value);
        return;
    }
    // RIGHT 模式下逐字符逆序
    for (i = 0; i < len; i++) out[i] = value[len - 1U - i];
    out[len] = '\0';
    UART_Printf("OK %s\r\n", out);
}

static void Process_Command(char *line)
{
    static char work[96];  // 静态分配 避免栈上临时分配
    char *argv[14];
    uint8_t argc = 0;
    char *tok;
    uint8_t i;
    char *sync;

    // 若 UART 边界抖动导致行首混入残留字节, 从第一个 '*' 重新同步。
    // 若首字节 '*' 偶发丢失, 后续 Command_Matches 会接受无 '*' 命令头。
    sync = strchr(line, '*');
    if (sync) line = sync;

    // 统一转为大写后按空格/Tab 分割为 token 数组
    Upper_Copy(work, line, sizeof(work));
    tok = strtok(work, " \t");
    while (tok && argc < 14) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (argc == 0) return;

    // ---- *PING 心跳应答 ----
    if (Command_Matches(argv[0], "*PING")) {
        UART_Printf("*PONG %lu\r\n", (unsigned long)(g_tick_ms / 1000U));
        return;
    }

    // ---- *RST 复位 恢复出厂默认值 ----
    if (Command_Matches(argv[0], "*RST")) {
        g_date.y = 2026; g_date.m = 6; g_date.d = 1;
        g_time.h = 0; g_time.mi = 0; g_time.s = 0;
        Calc_Wday(&g_date);
        g_alarm.t.h = 0; g_alarm.t.mi = 0; g_alarm.t.s = 0;
        g_alarm.enabled = 0; g_alarm.ringing = 0;
        disp_on = 1; g_format = FMT_LEFT; g_mode = MODE_DAY;
        g_led_override = false;
        g_ntp_synced = false;
        g_message_until_ms = 0;
        g_weather_until_ms = 0;
        g_cd_state = CD_IDLE; g_cd_remain_s = 0;   // 复位情景倒计时
        g_edit_state = ST_IDLE; g_edit_field = 0; g_edit_last_ms = 0;  // 复位编辑状态 防止 BUSY
        g_line_too_long = false; g_rx_asm_len = 0;   // 清除串口行组装残留
        Update_Status_LED();  // 恢复后重新计算 LED 状态
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *NTP SYNC 标记对时完成 ----
    if (Command_Matches(argv[0], "*NTP")) {
        if (argc < 2 || !Token_Matches(argv[1], "SYNC")) {
            UART_PutString("ERROR SYNTAX\r\n");
            return;
        }
        g_ntp_synced = true;
        g_ntp_last_sync_ms = g_tick_ms;
        // 对时完成后中断流水回到时钟显示 使虚拟 USR1 短按效果与物理键一致
        // 物理 USR1 短按已在按下沿中断 此处覆盖虚拟短按经 NTP 流程的路径
        g_message_until_ms = 0;
        Update_Status_LED();
        UART_PutString("OK\r\n");
        return;
    }

    // 同时支持空格形式和冒号形式的 GET 命令 "*GET DATE" 和 "*GET:DATE"
    // 逐字符比较 argv[0][0..4] 与 "*GET:" 避免 strncmp 在 ARMCC 上的内存段问题
    if (Command_Matches(argv[0], "*GET") ||
        (argv[0][0]=='*' && argv[0][1]=='G' && argv[0][2]=='E' && argv[0][3]=='T' && argv[0][4]==':') ||
        (argv[0][0]=='G' && argv[0][1]=='E' && argv[0][2]=='T' && argv[0][3]==':')) {
        char value[24];
        const char *sub;
        if (argv[0][0]=='*' && argv[0][1]=='G' && argv[0][2]=='E' && argv[0][3]=='T' && argv[0][4]==':' && argv[0][5] != '\0') {
            sub = argv[0] + 5;                 // 冒号形式 子命令紧跟在 *GET: 后面
        } else if (argv[0][0]=='G' && argv[0][1]=='E' && argv[0][2]=='T' && argv[0][3]==':' && argv[0][4] != '\0') {
            sub = argv[0] + 4;                 // 兼容首字节 '*' 丢失的 GET:xxx
        } else {
            // 空格形式 子命令为下一个 token
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
        else if (Token_Matches(sub, "COUNTDOWN")) {
            /* 手工构建 OK <state> <remain> <total> <scene> 字符串避免栈分配 */
            char gbuf[48]; uint8_t p = 0; const char *src; uint16_t v; uint8_t d[5]; int8_t di;
            gbuf[p++] = 'O'; gbuf[p++] = 'K'; gbuf[p++] = ' ';
            src = CD_STATE_NAMES[((uint8_t)g_cd_state <= 4U) ? (uint8_t)g_cd_state : 0U];
            while (*src) gbuf[p++] = *src++;
            gbuf[p++] = ' ';
            v = g_cd_remain_s; di = 0;
            if (v == 0U) d[di++] = '0';
            else { while (v) { d[di++] = (uint8_t)('0' + (v % 10U)); v /= 10U; } }
            while (di) gbuf[p++] = d[--di];
            gbuf[p++] = ' ';
            v = g_cd_total_s; di = 0;
            if (v == 0U) d[di++] = '0';
            else { while (v) { d[di++] = (uint8_t)('0' + (v % 10U)); v /= 10U; } }
            while (di) gbuf[p++] = d[--di];
            gbuf[p++] = ' ';
            gbuf[p++] = (uint8_t)('0' + g_cd_scene);
            gbuf[p++] = '\r'; gbuf[p++] = '\n'; gbuf[p] = '\0';
            UART_PutString(gbuf);
            return;
        }
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        Send_OK_Value(value);
        return;
    }

    // ---- *SET:KEY 模拟物理按键 不回报 *EVT:KEY 防环回 ----
    if (Command_Matches(argv[0], "*SET:KEY")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        for (i = 1; i <= KEY_COUNT; i++) {
            if (Token_Matches(argv[1], KEY_NAMES[i])) {
                Handle_Key((key_code_t)i);   // *SET:KEY 直接执行动作不回报 *EVT:KEY 防环回
                UART_PutString("OK\r\n");
                return;
            }
        }
        UART_PutString("ERROR PARAM\r\n");
        return;
    }

    // ---- *SET:DISPLAY 数码管整屏开关 ----
    if (Command_Matches(argv[0], "*SET:DISPlay")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "ON")) disp_on = 1;
        else if (Token_Matches(argv[1], "OFF")) disp_on = 0;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *SET:FORMAT 显示方向 LEFT/RIGHT ----
    if (Command_Matches(argv[0], "*SET:FORMAT")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "LEFT")) g_format = FMT_LEFT;
        else if (Token_Matches(argv[1], "RIGHT")) g_format = FMT_RIGHT;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *SET:MODE 昼夜模式切换 切换后上报 *EVT:MODE ----
    if (Command_Matches(argv[0], "*SET:MODE")) {
        if (argc < 2) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        if (Token_Matches(argv[1], "DAY")) g_mode = MODE_DAY;
        else if (Token_Matches(argv[1], "NIGHT")) g_mode = MODE_NIGHT;
        else { UART_PutString("ERROR PARAM\r\n"); return; }
        UART_Printf("OK\r\n*EVT:MODE %s\r\n", g_mode == MODE_DAY ? "DAY" : "NIGHT");
        return;
    }

    // ---- *SET:LED 远程直控 LED 00 退出接管 非00 进入接管 ----
    if (Command_Matches(argv[0], "*SET:LED")) {
        unsigned int val;
        if (argc < 2 || sscanf(argv[1], "%x", &val) != 1 || val > 0xff) {
            UART_PutString("ERROR PARAM\r\n"); return;
        }
        if (val == 0U) {
            g_led_override = false;       // 00 退出接管
            Update_Status_LED();          // 立即恢复正常 LED 含义
        } else {
            g_led_override = true;
            g_led_override_ms = g_tick_ms;   // 启动 10s 自动退出接管计时
            Set_LED((uint8_t)val);
        }
        UART_PutString("OK\r\n");
        Send_LED_Event();
        return;
    }

    // ---- *SET:BEEP 远程蜂鸣 借闹钟机制实现 范围 10-5000ms ----
    if (Command_Matches(argv[0], "*SET:BEEP")) {
        uint16_t ms;
        if (argc < 2 || !Parse_U16(argv[1], &ms) || ms < 10 || ms > 5000) {
            UART_PutString("ERROR RANGE\r\n"); return;
        }
        g_alarm.ringing = 1;
        g_alarm_ring_start_ms = g_tick_ms;
        g_ring_limit_ms = ms;
        g_beep_active = true;                     // BEEP 长响不走节奏翻转
        g_alarm_beep_on = 1;
        g_alarm_last_beep_ms = 0;
        GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, GPIO_PIN_5);  // 立即打开蜂鸣器
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *SET:MSG 滚动消息 ≤32 字节 保留原大小写 ----
    if (Command_Matches(argv[0], "*SET:MSG")) {
        // 从原始 line 中取消息文本 避免 toupper 破坏大小写
        const char *raw = strchr(line, ' ');
        uint8_t rlen = 0, len = 0;
        if (!raw) { UART_PutString("ERROR SYNTAX\r\n"); return; }
        while (*raw == ' ' || *raw == '\t') raw++;
        while (raw[rlen] && raw[rlen] != '\r' && raw[rlen] != '\n' && rlen < 32) rlen++;
        // 消息长度不可超过 32 字节
        if (raw[rlen] && raw[rlen] != '\r' && raw[rlen] != '\n') {
            UART_PutString("ERROR RANGE\r\n"); return;
        }
        /* 解析内嵌小数点 每个 '.' 是它前面那个字符的小数点。
           将 '.' 剥离存入 g_message_dp bitmap 流水渲染时按位点亮。
           开头孤立的小数点无前导字符则丢弃 */
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
        /* 短消息 <=8 位静态显示 3s 长消息由 scroll_completed 驱动整趟播完后返回
           这里的超时仅作安全兜底 取慢速步进 + 余量 确保不会中途截断流水 */
        g_message_until_ms = g_tick_ms + (len <= 8U ? 3000U :
                              ((uint32_t)(len + 8U) * 500U + 2000U));
        Scroll_Reset();
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *SET:WEA 天气数据下发 温度 -40~+50 天气码 6 种 ----
    if (Command_Matches(argv[0], "*SET:WEA")) {
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

        // 天气描述映射到内部编码
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
        // 构建短显文本 如 "25~CSUN" ~ 渲染为上方四段
        snprintf(g_weather_text, sizeof(g_weather_text), "%ld~C%s", temp, cond_names[code]);
        UART_PutString("OK\r\n");
        return;
    }

    // ---- *SET:COUNTDOWN 情景倒计时 自主增加功能 子命令族 ----
    if (Command_Matches(argv[0], "*SET:COUNTDOWN")) {
        Countdown_HandleCommand(line, argv, argc);
        return;
    }

    if (Command_Matches(argv[0], "*SET:DATE") || Command_Matches(argv[0], "*SET:TIME") || Command_Matches(argv[0], "*SET:ALARM")) {
        bool is_date = Command_Matches(argv[0], "*SET:DATE");
        bool is_time = Command_Matches(argv[0], "*SET:TIME");
        // 编辑态下拒绝外部写入 防止 PC 覆盖正在编辑的值
        if (g_edit_state != ST_IDLE) { UART_PutString("ERROR BUSY\r\n"); return; }
        // *SET:ALARM OFF 关闭闹钟并停止响铃
        if (!is_date && !is_time && argc == 2 && Token_Matches(argv[1], "OFF")) {
            g_alarm.enabled = 0;
            g_alarm.ringing = 0;
            g_alarm_beep_on = 0;
            GPIOPinWrite(GPIO_PORTK_BASE, GPIO_PIN_5, 0);
            UART_PutString("OK\r\n");
            UART_PutString("*EVT:ALARM_OFF\r\n");
            if (g_cd_state == CD_DONE) Countdown_Stop();
            return;
        }
        {
            // 解析参数 先扫描字段名 token 直到遇到非字段名 token 即值开始
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
                    break;  // 字段名结束 后面是值
                }
            }
            // 值应在字段名之后 数量必须匹配 支持任意参数组合
            value_start = (uint8_t)(1U + field_count);
            if (field_count == 0U || argc != (uint8_t)(value_start + field_count)) {
                UART_PutString("ERROR SYNTAX\r\n");
                return;
            }
            // 逐字段 + 值配对解析 DATE 可写年/月/日任意组合 TIME/ALARM 同理
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
        // DATE 写入后做日期规范化 闹钟写入后自动使能
        if (is_date) Normalize_Date();
        if (!is_date && !is_time) g_alarm.enabled = 1;
        UART_PutString("OK\r\n");
        // *SET 成功后主动上报编辑事件 方便 PC 同步
        if (is_date) UART_Printf("*EVT:EDIT DATE %04u.%02u.%02u\r\n", g_date.y, g_date.m, g_date.d);
        else if (is_time) UART_Printf("*EVT:EDIT TIME %02u.%02u.%02u\r\n", g_time.h, g_time.mi, g_time.s);
        else UART_Printf("*EVT:EDIT ALARM %02u.%02u.%02u\r\n", g_alarm.t.h, g_alarm.t.mi, g_alarm.t.s);
        return;
    }

    // 所有已知命令都未匹配 返回语法错误
    UART_PutString("ERROR SYNTAX\r\n");
}
