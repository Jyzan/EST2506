# S800 PC 上位机

Python 3.11 + PyQt5 数字孪生控制台，通过 USB 虚拟串口与 S800 板双向通信。

## 文件清单

| 文件 | 用途 |
|------|------|
| `main.py` | 程序入口，主窗口全部逻辑|
| `serial_worker.py` | 后台串口线程，异步收发 |
| `protocol.py` | 协议解析，事件报文解码 |
| `twin_panel.py` | 数字孪生面板：7SEG、LED、按键、倒计时进度环 |
| `ntp_helper.py` | NTP 网络对时，三服务器容错 |
| `weather_helper.py` | wttr.in 天气获取与协议码映射 |
| `astral_helper.py` | 日出日落计算，自动昼夜模式 |
| `chart_widget.py` | 数据看板，三张 matplotlib 图表 |
| `log_store.py` | 事件 CSV 持久化存储 |
| `ui/main_window.ui` | Qt Designer 界面布局文件 |
| `requirements.txt` | Python 依赖清单 |

## 快速启动

**环境要求**：Python 3.11.9

```bash
cd pc_host
python -m venv .venv
.venv\Scripts\Activate.ps1          # Windows PowerShell
pip install -r requirements.txt
python main.py
```

## 启动后操作

1. 在下拉框中选择 S800 板对应的 COM 口
2. 点击 **打开** 按钮，连接建立后状态栏显示「已连接」「在线」
3. 即可通过控制面板下发命令、观察镜像面板同步、查看四色日志

![主界面全貌](images/主界面全貌.jpg)

## 界面说明

### 数字孪生面板

上方 8 位七段数码管 + 8 位圆形 LED，中间 8 个 I2C 按键，下方 GPIO 按键。点击虚拟按键等效按下板上物理按键，板上按键按下时对应虚拟按钮高亮 200 ms。

![数字孪生面板](images/数字孪生面板.jpg)

### 协议容错演示

控制面板「协议」标签页提供参数组合下拉框（≥3 种）、缩写演示按钮、大小写混合演示按钮，可直观验证容错三件套。

![协议容错演示1](images/协议容错演示1.jpg)

![协议容错演示2](images/协议容错演示2.jpg)

### 数据看板

三张并列图表：闹钟触发时间折线图、NTP 对时误差柱状图、按键热度横向条形图。

![数据可视化看板](images/数据可视化看板.jpg)

### 错误处理

串口占用、网络超时、MCU 返回 ERROR、心跳超时 3 秒等异常均弹窗告警或红色日志标注，GUI 不崩溃。

![错误弹窗提示](images/错误弹窗提示.jpg)

## 依赖

| 包 | 版本 | 用途 |
|---|------|------|
| PyQt5 | ==5.15.9 | GUI 框架 |
| pyserial | >=3.5 | 串口通信 |
| ntplib | >=0.4.0 | NTP 对时 |
| requests | >=2.28 | 天气 API 请求 |
| astral | >=3.2 | 日出日落计算 |
| matplotlib | >=3.5 | 数据图表 |
| pyttsx3 | 可选 | 倒计时语音播报 |
