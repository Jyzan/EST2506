import datetime as dt

from astral import LocationInfo
from astral.sun import sun


CITY = LocationInfo("Shanghai", "China", "Asia/Shanghai", 31.2304, 121.4737)


def current_daynight(hour=None, minute=None):
    times = sun(CITY.observer, date=dt.date.today(), tzinfo=CITY.timezone)
    tz = times["sunrise"].tzinfo
    if hour is not None and minute is not None:
        now = dt.datetime.now(tz).replace(hour=hour, minute=minute, second=0, microsecond=0)
    else:
        now = dt.datetime.now(tz)
    mode = "DAY" if times["sunrise"] <= now <= times["sunset"] else "NIGHT"
    return mode, times["sunrise"], times["sunset"]
