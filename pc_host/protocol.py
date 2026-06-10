def with_crlf(line: str) -> str:
    return line.rstrip("\r\n") + "\r\n"


def is_heartbeat(line: str) -> bool:
    return line.startswith("*PONG") or line.startswith("*EVT:DISP") or line.startswith("*EVT:LED")


def parse_disp_event(line: str):
    body = line[len("*EVT:DISP "):]
    if len(body) < 3:
        raise ValueError("short DISP event")
    payload = body[:-3][:8].ljust(8).replace("_", " ")
    dp = int(body[-2:], 16)
    return payload, dp


def parse_led_event(line: str) -> int:
    return int(line.split()[-1], 16)


def normalize_key_name(name: str) -> str:
    name = name.upper()
    aliases = {"SHFT": "SHIFT", "USR1": "USER1", "USR2": "USER2", "K7": "FORMAT", "K8": "EXT"}
    return aliases.get(name, name)


def display_key_name(name: str) -> str:
    aliases = {"SHIFT": "SHFT", "USER1": "USR1", "USER2": "USR2", "FORMAT": "K7", "EXT": "K8"}
    return aliases.get(name.upper(), name.upper())
