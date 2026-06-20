# S800 智能联网时钟系统

基于 TI TM4C1294NCPDT 和 Python PyQt5 的智能时钟课程项目。S800 板可独立完成时钟、日期、闹钟和按键设置，PC 上位机通过 USB 虚拟串口提供控制、状态监视与数字孪生界面。

> 系统设计、协议说明、扩展功能和演示内容见对应简介 PDF。

## 项目结构

```text
.
├── mcu/
│   ├── Inc/                            芯片头文件
│   ├── Driverlib/                      TivaWare 驱动库
│   ├── src/main.c                      MCU 自编程序
│   └── obj/exp.axf                     UniFlash 烧写文件
├── pc_host/
│   ├── main.py                         程序入口与业务控制
│   ├── astral_helper.py                昼夜模式计算
│   ├── chart_widget.py                 数据看板组件
│   ├── log_store.py                    事件数据存储
│   ├── ntp_helper.py                   NTP 网络对时
│   ├── protocol.py                     串口协议解析
│   ├── serial_worker.py                后台串口通信
│   ├── twin_panel.py                   数字孪生组件
│   ├── ui_main_window.py               PyQt 主窗口界面类
│   ├── weather_helper.py               天气获取与转换
│   ├── main_window.ui                  Qt Designer 界面文件
│   └── requirements.txt                Python 依赖
├── docs/
│   ├── 大作业524442910013-江彦佐.pdf    简介文档
│   └── 演示视频.mp4
└── README.md                           烧写与运行说明
```

## 开发环境

- Windows 10 或 Windows 11
- Code Composer Studio UniFlash
- TI TM4C1294NCPDT 开发板
- Python 3.11.9
- PyQt5 5.15.9、pyserial 3.5
- 串口参数：115200，8N1，无流控

## MCU 烧写

使用 UniFlash 烧写 `mcu/obj/exp.axf`：

1. 使用 Micro USB 连接 S800 板并打开 UniFlash。
2. 在主界面展开 `New Configuration`。
3. 在 `Enter Device Name` 中输入 `TM4C1294NCPDT`，选择 `TIVA TM4C1294NCPDT`。
4. 确认 `Selected Connection` 为 `Stellaris In-Circuit Debug Interface`，然后点击 `Start`。
5. 在左侧选中 `Program`，在 `Load Image` 中点击 `Browse`。
6. 将文件类型切换为 `All Files (*.*)`，选择项目中的 `mcu/obj/exp.axf`。
7. 可勾选 `Run Target After Program Load/Flash Operation`，使程序烧写后自动运行。
8. 控制台出现绿色的 `[SUCCESS] Program Load completed successfully.` 即表示烧写成功。

## PC 上位机安装与运行

在项目根目录按所用终端执行。

PowerShell：
```powershell
cd pc_host
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python main.py
```

CMD：
```bat
cd pc_host
py -3.11 -m venv .venv
.venv\Scripts\activate.bat
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe main.py
```

程序启动后选择 S800 对应的 COM 端口并点击“打开”。连接成功后可使用控制面板、数字孪生镜像、日志、网络对时、天气和数据看板。
