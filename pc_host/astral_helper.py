import datetime as dt

from astral import LocationInfo
from astral.sun import sun


CITY = LocationInfo("Shanghai", "China", "Asia/Shanghai", 31.2304, 121.4737)


def current_daynight():
    times = sun(CITY.observer, date=dt.date.today(), tzinfo=CITY.timezone)
    now = dt.datetime.now(times["sunrise"].tzinfo)
    mode = "DAY" if times["sunrise"] <= now <= times["sunset"] else "NIGHT"
    return mode, times["sunrise"], times["sunset"]
