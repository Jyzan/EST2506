"""NTP 网络对时 多服务器容错 返回标准时间与本地时钟偏差"""
import datetime as dt

import ntplib


SERVERS = ("ntp.aliyun.com", "cn.ntp.org.cn", "ntp.ntsc.ac.cn")


def fetch_network_time(timeout=3):
    """依次尝试三个 NTP 服务器获取网络时间 全部失败则抛出异常 返回 网络时间 datetime 本地时钟偏差毫秒 服务器地址 三项"""
    last_error = None
    for server in SERVERS:
        try:
            response = ntplib.NTPClient().request(server, version=3, timeout=timeout)
            network_time = dt.datetime.fromtimestamp(response.tx_time)
            # offset 已按 NTP 四时间戳算法扣除往返延迟 反映本地时钟相对标准时间的真实偏差
            # 正值表示本地时钟慢 负值表示本地时钟快 不再被网络单程延迟系统性带偏
            delta_ms = int(response.offset * 1000)
            return network_time, delta_ms, server
        except Exception as exc:
            last_error = exc
    raise RuntimeError(f"NTP unavailable: {last_error}")
