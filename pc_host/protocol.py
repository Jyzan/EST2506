"""S800 串口协议解析工具 处理 EVT DISP LED CD KEY 等事件报文与心跳过滤"""

def with_crlf(line: str) -> str:
    """确保字符串以 CR LF 结尾 用于串口发送"""
    return line.rstrip("\r\n") + "\r\n"


def is_heartbeat(line: str) -> bool:
    # 心跳仅含 C1 的 PING 与 PONG 1Hz 保活 以及 C3 的 DISP 与 LED 事件 1Hz 显示心跳
    # 倒计时状态属业务数据 文档未列为心跳 不过滤
    return (line.startswith("*PING") or line.startswith("*PONG")
            or line.startswith("*EVT:DISP") or line.startswith("*EVT:LED"))


def parse_disp_event(line: str):
    """解析 EVT DISP 报文 从 8 字符定长文本和 2 位十六进制 dp 中 提取显示内容和每一位的小数点状态 返回 8 字符显示文本 dp 位图整数值 两项"""
    body = line[len("*EVT:DISP "):]
    if len(body) < 3:
        raise ValueError("short DISP event")
    enc = body[:-3][:8].ljust(8)
    dp = int(body[-2:], 16)
    # 协议约定 _ 为定长填空的空位占位符 解码时一律还原为空格
    # 全工程中唯一要真实显示下划线字形的是未对时短显 _SYNO 含逆序的 ONYS_
    # 这是固定文案 单独识别后保留其字面 _ 其余场景的 _ 全部按填充处理
    # 字面下划线紧贴 SYNO 一侧 另一端的 _ 是填充 需去掉避免出现 _SYNO___
    # LEFT 下划线在前导 去尾部填充 RIGHT 逆序后下划线在尾部 去前导填充
    if "SYNO" in enc:
        return enc.rstrip("_").ljust(8), dp
    if "ONYS" in enc:
        return enc.lstrip("_").rjust(8), dp
    return enc.replace("_", " "), dp


def parse_led_event(line: str) -> int:
    """解析 EVT LED 报文 返回 LED 字节的整数值 每 bit 对应一位 LED"""
    return int(line.split()[-1], 16)


def parse_cd_event(line: str):
    """解析 EVT CD STATE 报文 格式为 STATE 后跟四个字段 返回 state remain total scene 四项 格式不符则返回 None"""
    tokens = line.split()
    # tokens 示例 EVT CD STATE 后跟 state remain total scene 四个字段
    if len(tokens) < 6 or tokens[1] != "STATE":
        return None
    return parse_cd_status_tokens(tokens[2:])


def parse_cd_status_payload(payload: str):
    """解析 GET COUNTDOWN 的 OK 载荷 格式同 STATE 的四个字段"""
    return parse_cd_status_tokens(payload.split())


def parse_cd_status_tokens(tokens):
    if len(tokens) < 4:
        return None
    state = tokens[0].upper()
    if state not in ("IDLE", "EDIT", "RUN", "PAUSE", "DONE"):
        return None
    try:
        remain = int(tokens[1])
        total = int(tokens[2])
        scene = int(tokens[3])
    except ValueError:
        return None
    if remain < 0 or total < 0 or scene < 0:
        return None
    return state, remain, total, scene


def normalize_key_name(name: str) -> str:
    """将按键别名统一为正式名称 如 K7 转为 FORMAT USR1 转为 USER1"""
    name = name.upper()
    aliases = {"SHFT": "SHIFT", "USR1": "USER1", "USR2": "USER2", "K7": "FORMAT", "K8": "EXT"}
    return aliases.get(name, name)


def display_key_name(name: str) -> str:
    """将正式按键名转为显示缩写 如 FORMAT 转为 K7 USER1 转为 USR1"""
    aliases = {"SHIFT": "SHFT", "USER1": "USR1", "USER2": "USR2", "FORMAT": "K7", "EXT": "K8"}
    return aliases.get(name.upper(), name.upper())
