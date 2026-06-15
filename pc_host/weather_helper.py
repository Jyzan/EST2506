"""天气获取 从 wttr.in 拉取上海实时天气并映射为 S800 协议天气码"""
import requests


COND_MAP = {
    "sunny": "SUN",
    "clear": "SUN",
    "partly cloudy": "CLD",
    "cloudy": "CLD",
    "overcast": "OVC",
    "rain": "RAI",
    "light rain": "RAI",
    "heavy rain": "RAI",
    "snow": "SNO",
    "light snow": "SNO",
    "fog": "FOG",
    "mist": "FOG",
}


def map_condition(description: str) -> str:
    """将 wttr.in 返回的天气描述文本映射为 S800 协议天气码 六种代码 SUN 晴 CLD 多云 OVC 阴 RAI 雨 SNO 雪 FOG 雾"""
    lower = description.strip().lower()
    if lower in COND_MAP:
        return COND_MAP[lower]
    if "rain" in lower or "shower" in lower or "thunder" in lower:
        return "RAI"
    if "snow" in lower:
        return "SNO"
    if "fog" in lower or "mist" in lower:
        return "FOG"
    if "overcast" in lower:
        return "OVC"
    if "sun" in lower or "clear" in lower:
        return "SUN"
    return "CLD"


def fetch_shanghai_weather(timeout=5):
    """从 wttr.in 获取上海当前天气 返回 温度摄氏度 天气码 天气描述 三项"""
    data = requests.get("https://wttr.in/Shanghai?format=j1", timeout=timeout).json()
    current = data["current_condition"][0]
    temp = int(current["temp_C"])
    description = current["weatherDesc"][0]["value"]
    return temp, map_condition(description), description
