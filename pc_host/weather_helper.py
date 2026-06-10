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
    data = requests.get("https://wttr.in/Shanghai?format=j1", timeout=timeout).json()
    current = data["current_condition"][0]
    temp = int(current["temp_C"])
    description = current["weatherDesc"][0]["value"]
    return temp, map_condition(description), description
