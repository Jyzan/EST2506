"""昼夜模式计算 根据上海地理位置和当前时间判断日出日落"""
import datetime as dt

from astral import LocationInfo
from astral.sun import sun


CITY = LocationInfo("Shanghai", "China", "Asia/Shanghai", 31.2304, 121.4737)


def current_daynight(hour=None, minute=None):
    """计算当前时刻的昼夜模式 用于自动下发 SET MODE DAY 或 NIGHT 可选传入时分参数 用于从 MCU 获取时间后精确判断 返回 模式字符串 DAY 或 NIGHT 日出时刻 日落时刻 三项"""
    times = sun(CITY.observer, date=dt.date.today(), tzinfo=CITY.timezone)
    tz = times["sunrise"].tzinfo
    if hour is not None and minute is not None:
        now = dt.datetime.now(tz).replace(hour=hour, minute=minute, second=0, microsecond=0)
    else:
        now = dt.datetime.now(tz)
    mode = "DAY" if times["sunrise"] <= now <= times["sunset"] else "NIGHT"
    return mode, times["sunrise"], times["sunset"]
