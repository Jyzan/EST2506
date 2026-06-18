# S800 智能联网时钟系统 — MCU 端

## 目录结构

```
mcu/
├── Driverlib/          # TivaWare 硬件驱动库
├── Inc/                # 共用头文件
├── src/
│   └── main.c          # 自编代码集中于此
└── obj/
    └── exp.axf         # 编译产物，可直接烧写

```

## 开发环境

| 项目 | 说明 |
|------|------|
| IDE | Keil uVision 5 |
| 芯片 | TI TM4C1294NCPDT |
| 依赖库 | TivaWare |

## 烧写

`obj/exp.axf` 为已编译完成的固件，可直接烧写，无需重新编译。
1. 用 Micro-USB 线连接 S800 板到 PC
2. 打开 Keil uVision 5，菜单 **Flash → Download (F8)**，在文件选择对话框中选中 `obj/exp.axf`
3. 按板上的 **RESET** 键运行

## 硬件资源

| 资源 | 用途 |
|------|------|
| 8 位 I2C 七段数码管 | 时间 / 日期 / 消息显示 |
| 8 位 I2C 按键 | 本地设置（FUNC SHIFT ADD SAVE DISP SPEED FORMAT EXT）|
| GPIO USER1 / USER2 | NTP 对时请求 / 天气短显 |
| 8 位 LED | 心跳 / 闹钟 / 编辑 / 收发 / 天气 / NTP 指示 |
| 蜂鸣器 (PK5) | 闹钟响铃 / 远程蜂鸣 / 倒计时完成 |

## 按键映射

| 按键 | 短按 | 长按 |
|------|------|------|
| FUNC | 编辑模式循环切换；响铃中关闹钟 | 保存并退出（等效 SAVE）|
| SHIFT | 编辑中切换高亮字段 | — |
| ADD | 当前字段 +1 | 连加（≥5 Hz）|
| SAVE | 保存并退出编辑 | — |
| DISP | 时间 → 日期 → 年份 循环 | — |
| SPEED | 流水速度 2 级切换 | — |
| FORMAT | 流水方向 LEFT / RIGHT | — |
| EXT | 预留键，上报 EVT KEY EXT | — |
| USER1 | 请求 PC 对时 | NTP 同步状态短显 |
| USER2 | 天气短显 5 秒 | — |

## LED 含义

| LED | 名称 | 含义 | 状态 |
|-----|------|------|------|
| LED0 | HB | 系统心跳 | 1 Hz 闪烁 |
| LED1 | ALM | 闹钟 | 使能常亮 / 响铃快闪 |
| LED2 | EDIT | 编辑模式 | 编辑态常亮 |
| LED3 | RX/TX | 串口活动 | 收发后亮 100 ms |
| LED4 | SUN | 天气晴（扩展） | 常亮 |
| LED5 | RAI/SNO | 雨雪（扩展） | 慢闪 |
| LED6 | HOT | 高温 ≥30°C（扩展） | 常亮 |
| LED7 | NTP | 对时状态（扩展） | 已同步常亮 / 超 24 h 慢闪 / 未同步灭 |

前 4 位为基础必做，后 4 位在完成扩展功能 E1 / E2 时启用。

## 串口协议命令总表

波特率 115200，8N1，无流控，ASCII 编码，行结束符 CR LF 或 LF 兼容，单帧最大 64 字节。

| 命令 | 参数 | 应答 | 说明 |
|------|------|------|------|
| *RST | — | OK | 复位时钟 / 日期 / 闹钟 |
| *PING | — | *PONG \<秒\> | 心跳 |
| *SET:DATE | YEAR / MONTH / DATE 任意组合 | OK | 设置日期 |
| *SET:TIME | HOUR / MINute / SECond 任意组合 | OK | 设置时间 |
| *SET:ALARM | HOUR / MINute / SECond / OFF | OK | 设置闹钟或关闭 |
| *SET:DISP | ON / OFF | OK | 数码管亮灭 |
| *SET:FORMAT | LEFT / RIGHT | OK | 显示方向 |
| *SET:MSG | ≤32 字节文本 保留大小写 | OK | 滚动消息 |
| *SET:BEEP | 10–5000 ms | OK | 远程蜂鸣 |
| *SET:LED | 2 位十六进制 | OK | 远程直控 LED |
| *SET:KEY | 按键名 共 10 种 | OK | 模拟按键 不回 EVT KEY |
| *SET:MODE | DAY / NIGHT | OK | 昼夜模式 扩展 |
| *GET | DATE / TIME / ALARM / DISPlay / FORMAT | OK \<数据\> | 查询状态 |

- 容错规则：大小写不敏感、空格 / Tab 容错、大写字母必输小写可省略。
- 错误回应：ERROR SYNTAX / PARAM / RANGE / LEN / BUSY。
- FORMAT RIGHT 下所有 GET 应答和 EVT DISP 上报均逆序传输。
