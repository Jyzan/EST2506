import datetime as dt

import ntplib


SERVERS = ("ntp.aliyun.com", "cn.ntp.org.cn", "ntp.ntsc.ac.cn")


def fetch_network_time(timeout=3):
    last_error = None
    for server in SERVERS:
        try:
            response = ntplib.NTPClient().request(server, version=3, timeout=timeout)
            network_time = dt.datetime.fromtimestamp(response.tx_time)
            delta_ms = int((network_time - dt.datetime.now()).total_seconds() * 1000)
            return network_time, delta_ms, server
        except Exception as exc:
            last_error = exc
    raise RuntimeError(f"NTP unavailable: {last_error}")
