def with_crlf(line: str) -> str:
    return line.rstrip("\r\n") + "\r\n"


def is_heartbeat(line: str) -> bool:
    # *EVT:CD STATE 在运行时 1Hz 发送, 与 DISP/LED 同视为心跳避免刷屏;
    # *EVT:CD DONE 为一次性事件, 不过滤。
    return (line.startswith("*PONG") or line.startswith("*EVT:DISP")
            or line.startswith("*EVT:LED") or line.startswith("*EVT:CD STATE"))


def parse_disp_event(line: str):
    body = line[len("*EVT:DISP "):]
    if len(body) < 3:
        raise ValueError("short DISP event")
    payload = body[:-3][:8].ljust(8).replace("_", " ")
    dp = int(body[-2:], 16)
    return payload, dp


def parse_led_event(line: str) -> int:
    return int(line.split()[-1], 16)


def parse_cd_event(line: str):
    """解析 *EVT:CD STATE <state> <remain> <total> <scene>。

    返回 (state, remain, total, scene) 或 None(格式不符)。
    """
    tokens = line.split()
    # tokens = ["*EVT:CD", "STATE", state, remain, total, scene]
    if len(tokens) < 6 or tokens[1] != "STATE":
        return None
    return parse_cd_status_tokens(tokens[2:])


def parse_cd_status_payload(payload: str):
    """解析 *GET:COUNTDOWN 的 OK 载荷: <state> <remain> <total> <scene>。"""
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
    name = name.upper()
    aliases = {"SHFT": "SHIFT", "USR1": "USER1", "USR2": "USER2", "K7": "FORMAT", "K8": "EXT"}
    return aliases.get(name, name)


def display_key_name(name: str) -> str:
    aliases = {"SHIFT": "SHFT", "USER1": "USR1", "USER2": "USR2", "FORMAT": "K7", "EXT": "K8"}
    return aliases.get(name.upper(), name.upper())
