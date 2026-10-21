"""Pulse —— 桌面悬浮小组件：续航、功耗、网速、高占用进程。

为了让组件本身尽量省电：
  - 所有数据直接走 Win32 API（ctypes），不用 WMI，由守护线程定时采样并推送界面；
  - 窗口最小化时不采样；迷你模式下不扫描进程、不画曲线。

数据来源：
  - 电池：CallNtPowerInformation(SystemBatteryState) + GetSystemPowerStatus
  - 电源模式：PowerGetEffectiveOverlayScheme
  - 网速：GetIfEntry2，只统计物理网卡（排除 Clash 等 TUN 虚拟网卡，否则代理流量会算两遍）
  - 进程：NtQuerySystemInformation 一次取全部进程的 CPU 时间
  - CPU / 内存：GetSystemTimes / GlobalMemoryStatusEx
"""

import ctypes
import json
import math
import os
import struct
import threading
import time
import uuid
from ctypes import wintypes as wt

import webview
from webview.window import FixPoint

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(HERE, "settings.json")
FULL_SIZE = (320, 560)
COMPACT_SIZE = (380, 40)
NCPU = os.cpu_count() or 1
ETA_TAU = 30  # 功率平滑的时间常数（秒），续航估计不会随瞬时负载乱跳
PROC_INTERVAL = 3  # 进程扫描间隔（秒）
POWER_MODES = {
    "961cc777-2547-4f9d-8174-7d86181b8a7a": "最佳能效",
    "3af9b8d9-7c97-431d-ad78-34a8bfea439f": "更长续航",
    "00000000-0000-0000-0000-000000000000": "平衡",
    "ded574b5-45a0-4f42-8737-46345c09c238": "最佳性能",
}

kernel32 = ctypes.windll.kernel32
powrprof = ctypes.windll.powrprof
iphlpapi = ctypes.windll.iphlpapi
ntdll = ctypes.windll.ntdll
user32 = ctypes.windll.user32
U64 = ctypes.c_uint64


class SYSTEM_BATTERY_STATE(ctypes.Structure):
    _fields_ = [
        ("AcOnLine", ctypes.c_ubyte), ("BatteryPresent", ctypes.c_ubyte),
        ("Charging", ctypes.c_ubyte), ("Discharging", ctypes.c_ubyte),
        ("Spare1", ctypes.c_ubyte * 3), ("Tag", ctypes.c_ubyte),
        ("MaxCapacity", wt.DWORD), ("RemainingCapacity", wt.DWORD),
        ("Rate", ctypes.c_long), ("EstimatedTime", wt.DWORD),
        ("DefaultAlert1", wt.DWORD), ("DefaultAlert2", wt.DWORD),
    ]


class SYSTEM_POWER_STATUS(ctypes.Structure):
    _fields_ = [
        ("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte),
        ("BatteryLifePercent", ctypes.c_ubyte), ("SystemStatusFlag", ctypes.c_ubyte),
        ("BatteryLifeTime", wt.DWORD), ("BatteryFullLifeTime", wt.DWORD),
    ]


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [
        ("dwLength", wt.DWORD), ("dwMemoryLoad", wt.DWORD),
        ("ullTotalPhys", U64), ("ullAvailPhys", U64), ("ullTotalPageFile", U64),
        ("ullAvailPageFile", U64), ("ullTotalVirtual", U64), ("ullAvailVirtual", U64),
        ("ullAvailExtendedVirtual", U64),
    ]


class MIB_IF_ROW2(ctypes.Structure):
    _fields_ = [
        ("InterfaceLuid", U64), ("InterfaceIndex", wt.ULONG), ("InterfaceGuid", ctypes.c_byte * 16),
        ("Alias", ctypes.c_wchar * 257), ("Description", ctypes.c_wchar * 257),
        ("PhysicalAddressLength", wt.ULONG), ("PhysicalAddress", ctypes.c_ubyte * 32),
        ("PermanentPhysicalAddress", ctypes.c_ubyte * 32), ("Mtu", wt.ULONG), ("Type", wt.ULONG),
        ("TunnelType", ctypes.c_int), ("MediaType", ctypes.c_int), ("PhysicalMediumType", ctypes.c_int),
        ("AccessType", ctypes.c_int), ("DirectionType", ctypes.c_int),
        ("Flags", ctypes.c_ubyte),  # bit0 HardwareInterface, bit1 FilterInterface
        ("OperStatus", ctypes.c_int), ("AdminStatus", ctypes.c_int), ("MediaConnectState", ctypes.c_int),
        ("NetworkGuid", ctypes.c_byte * 16), ("ConnectionType", ctypes.c_int),
        ("TransmitLinkSpeed", U64), ("ReceiveLinkSpeed", U64),
        ("InOctets", U64), ("InUcastPkts", U64), ("InNUcastPkts", U64), ("InDiscards", U64),
        ("InErrors", U64), ("InUnknownProtos", U64), ("InUcastOctets", U64), ("InMulticastOctets", U64),
        ("InBroadcastOctets", U64), ("OutOctets", U64), ("OutUcastPkts", U64), ("OutNUcastPkts", U64),
        ("OutDiscards", U64), ("OutErrors", U64), ("OutUcastOctets", U64), ("OutMulticastOctets", U64),
        ("OutBroadcastOctets", U64), ("OutQLen", U64),
    ]


def read_battery():
    bs = SYSTEM_BATTERY_STATE()
    ps = SYSTEM_POWER_STATUS()
    if powrprof.CallNtPowerInformation(5, None, 0, ctypes.byref(bs), ctypes.sizeof(bs)) != 0:
        return None
    if not kernel32.GetSystemPowerStatus(ctypes.byref(ps)):
        return None
    rate = bs.Rate if abs(bs.Rate) < 500_000 else 0  # 0x80000000 表示未知
    return {
        "present": bool(bs.BatteryPresent),
        "ac": bool(bs.AcOnLine),
        "charging": bool(bs.Charging),
        "discharging": bool(bs.Discharging),
        "rate_mw": rate,  # 正数充电，负数放电
        "remaining_mwh": bs.RemainingCapacity,
        "full_mwh": bs.MaxCapacity,
        "percent": ps.BatteryLifePercent if ps.BatteryLifePercent <= 100 else None,
        "life_s": ps.BatteryLifeTime if ps.BatteryLifeTime != 0xFFFFFFFF else None,
        "saver": ps.SystemStatusFlag == 1,
    }


def read_power_mode():
    g = (ctypes.c_ubyte * 16)()
    if powrprof.PowerGetEffectiveOverlayScheme(ctypes.byref(g)) != 0:
        return None
    return POWER_MODES.get(str(uuid.UUID(bytes_le=bytes(g))))


def read_cpu_times():
    idle, kern, user = U64(), U64(), U64()
    kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user))
    return idle.value, kern.value + user.value  # kernel 时间已包含 idle


def read_mem_percent():
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.dwMemoryLoad


class NetCounter:
    """只统计物理网卡（HardwareInterface 且不是过滤驱动层），每 30 秒重新发现一次网卡。"""

    def __init__(self):
        self.indices = ()
        self._found_at = -1e9
        self._row = MIB_IF_ROW2()

    def _discover(self):
        ptr = ctypes.c_void_p()
        if iphlpapi.GetIfTable2(ctypes.byref(ptr)) != 0:
            return
        try:
            n = ctypes.c_ulong.from_address(ptr.value).value
            rows = (MIB_IF_ROW2 * n).from_address(ptr.value + 8)
            self.indices = tuple(
                r.InterfaceIndex for r in rows if r.Flags & 1 and not r.Flags & 2 and r.OperStatus == 1
            )
        finally:
            iphlpapi.FreeMibTable(ptr)
        self._found_at = time.monotonic()

    def totals(self):
        if time.monotonic() - self._found_at > 30:
            self._discover()
        rx = tx = 0
        row = self._row
        for idx in self.indices:
            row.InterfaceLuid = 0
            row.InterfaceIndex = idx
            if iphlpapi.GetIfEntry2(ctypes.byref(row)) == 0:
                rx += row.InOctets
                tx += row.OutOctets
        return self.indices, rx, tx


class ProcScanner:
    """一次 NtQuerySystemInformation 拿到所有进程的 CPU 时间，按进程名汇总。"""

    def __init__(self):
        self._buf = ctypes.create_string_buffer(1 << 20)
        self._last = None
        self._me = os.getpid()

    def _snapshot(self):
        ret = wt.ULONG()
        while True:
            st = ntdll.NtQuerySystemInformation(5, self._buf, len(self._buf), ctypes.byref(ret)) & 0xFFFFFFFF
            if st != 0xC0000004:  # STATUS_INFO_LENGTH_MISMATCH
                break
            self._buf = ctypes.create_string_buffer(ret.value + (1 << 16))
        if st != 0:
            return {}
        raw = self._buf.raw[: ret.value]
        base = ctypes.addressof(self._buf)
        out = {}
        off = 0
        while True:
            nxt, = struct.unpack_from("<I", raw, off)
            user, kern = struct.unpack_from("<qq", raw, off + 40)
            nlen, = struct.unpack_from("<H", raw, off + 56)
            ptr, = struct.unpack_from("<Q", raw, off + 64)
            pid, ppid = struct.unpack_from("<QQ", raw, off + 80)
            name = raw[ptr - base: ptr - base + nlen].decode("utf-16-le", "replace") if ptr else ""
            out[pid] = (name, user + kern, ppid)
            if not nxt:
                break
            off += nxt
        return out

    def _my_tree(self, snap):
        kids = {}
        for pid, (_, _, ppid) in snap.items():
            kids.setdefault(ppid, []).append(pid)
        tree, stack = set(), [self._me]
        while stack:
            p = stack.pop()
            if p in tree:
                continue
            tree.add(p)
            stack.extend(kids.get(p, ()))
        return tree

    def scan(self):
        """返回 (前 3 名 [(名称, CPU%)], 本组件 CPU%)；第一次调用只建立基线。"""
        now = time.monotonic()
        snap = self._snapshot()
        last, self._last = self._last, (now, snap)
        if not last:
            return None, None
        t0, prev = last
        total = (now - t0) * 1e7 * NCPU
        mine = self._my_tree(snap)
        agg, self_cpu = {}, 0
        for pid, (name, cpu, _) in snap.items():
            old = prev.get(pid)
            if not pid or not old or old[0] != name:
                continue
            d = cpu - old[1]
            if pid in mine:
                self_cpu += d
            else:
                key = name[:-4] if name.lower().endswith(".exe") else name
                agg[key] = agg.get(key, 0) + d
        top = sorted(agg.items(), key=lambda kv: -kv[1])[:3]
        return [(k, v / total * 100) for k, v in top], self_cpu / total * 100


class Monitor:
    def __init__(self):
        self.lock = threading.Lock()
        self.net = NetCounter()
        self.procs = ProcScanner()
        self._t = None
        self._net = None
        self._cpu = None
        self.rx_total = self.tx_total = 0
        self._ema = None
        self._mode = None
        self._unplug_t = None
        self._unplug_since_start = False
        self._energy_wh = 0.0
        self._measured_s = 0.0
        self._battery = None
        self._battery_ok = False
        self._pmode = None
        self._pmode_at = -1e9
        self._proc_at = -1e9
        self._top = []
        self._self_pct = None

    def sample(self, full):
        with self.lock:
            return self._sample(full)

    def _sample(self, full):
        now = time.monotonic()
        dt = now - self._t if self._t else None
        self._t = now

        # 网络
        net = self.net.totals()
        down = up = 0.0
        if dt and self._net and self._net[0] == net[0]:
            d_rx = max(0, net[1] - self._net[1])
            d_tx = max(0, net[2] - self._net[2])
            down, up = d_rx / dt, d_tx / dt
            self.rx_total += d_rx
            self.tx_total += d_tx
        self._net = net

        # CPU / 内存
        cpu_t = read_cpu_times()
        cpu = 0.0
        if self._cpu and cpu_t[1] > self._cpu[1]:
            cpu = max(0.0, 100 * (1 - (cpu_t[0] - self._cpu[0]) / (cpu_t[1] - self._cpu[1])))
        self._cpu = cpu_t

        # 电池
        b = read_battery()
        battery_ok = b is not None
        if battery_ok:
            self._battery = b
        else:
            b = self._battery or {
                "present": False, "rate_mw": 0, "percent": None,
                "remaining_mwh": 0, "full_mwh": 0, "saver": False, "life_s": None,
            }
        rate = b["rate_mw"]
        if not b["present"]:
            mode = "nobattery"
        elif rate < 0 or (b["discharging"] and not b["ac"]):
            mode = "discharging"
        elif b["charging"] and rate > 0:
            mode = "charging"
        else:
            mode = "ac"

        p = abs(rate)
        if mode in ("discharging", "charging") and p:
            if self._mode != mode or self._ema is None or not dt:
                self._ema = p
            else:
                self._ema += (1 - math.exp(-dt / ETA_TAU)) * (p - self._ema)
        else:
            self._ema = None

        if mode == "discharging":
            if self._mode != "discharging":
                self._unplug_t = now
                self._unplug_since_start = self._mode is None
                self._energy_wh = 0.0
                self._measured_s = 0.0
            elif dt and dt <= 5 and p and battery_ok and self._battery_ok:
                self._energy_wh += p / 1000 * dt / 3600
                self._measured_s += dt  # 跳过暂停采样和读取失败的时间，平均功耗只用实测秒数
        else:
            self._unplug_t = None
        self._mode = mode
        self._battery_ok = battery_ok

        eta = eta20 = pph = None
        if self._ema:
            if mode == "discharging":
                eta = b["remaining_mwh"] / self._ema * 3600
                if b["full_mwh"]:
                    pph = self._ema / b["full_mwh"] * 100
                    if b["percent"] is not None and b["percent"] > 20:
                        eta20 = (b["remaining_mwh"] - 0.2 * b["full_mwh"]) / self._ema * 3600
            elif mode == "charging" and b["full_mwh"] > b["remaining_mwh"]:
                eta = (b["full_mwh"] - b["remaining_mwh"]) / self._ema * 3600
                pph = self._ema / b["full_mwh"] * 100

        eta_source = "pulse"
        if mode == "discharging" and eta is None and b["life_s"] is not None:
            eta = b["life_s"]
            eta_source = "system"

        on_bat = now - self._unplug_t if self._unplug_t else None
        avg_w = self._energy_wh / (self._measured_s / 3600) if on_bat and self._measured_s > 60 else None

        if now - self._pmode_at > 5:
            self._pmode = read_power_mode()
            self._pmode_at = now

        if full and now - self._proc_at >= PROC_INTERVAL:
            top, self_pct = self.procs.scan()
            self._proc_at = now
            if top is not None:
                self._top, self._self_pct = top, self_pct
        elif not full:
            self.procs._last = None  # 迷你模式不扫描；回到完整模式时重新建基线

        return {
            "ok": True,
            "down": down,
            "up": up,
            "rx_total": self.rx_total,
            "tx_total": self.tx_total,
            "cpu": cpu,
            "mem": read_mem_percent(),
            "mode": mode,
            "percent": b["percent"],
            "power_w": p / 1000 if mode in ("discharging", "charging") and p else None,
            "eta_s": eta,
            "eta_source": eta_source,
            "eta20_s": eta20,
            "pct_per_h": pph,
            "on_battery_s": on_bat,
            "since_start": self._unplug_since_start,
            "avg_w": avg_w,
            "remaining_wh": b["remaining_mwh"] / 1000 if b["present"] else None,
            "full_wh": b["full_mwh"] / 1000 if b["present"] and b["full_mwh"] else None,
            "saver": b["saver"],
            "power_mode": self._pmode,
            "top": self._top if full else None,
            "self_pct": self._self_pct if full else None,
        }


def load_cfg():
    try:
        with open(CFG_PATH, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


class Api:
    def __init__(self, monitor, cfg):
        self._m = monitor
        self._cfg = cfg
        self._wake = threading.Event()
        self._closed = threading.Event()
        self._thread = None

    def _start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._push, daemon=True)
            self._thread.start()

    def _push(self):
        while not self._closed.is_set():
            self._wake.clear()
            if _hwnd and user32.IsIconic(_hwnd):
                self._closed.wait(1)
                continue
            try:
                d = self._m.sample(not self._cfg.get("compact", False))
                if self._closed.is_set():
                    break
                if not (_hwnd and user32.IsIconic(_hwnd)):
                    _window.run_js("window.render && window.render(" + json.dumps(d) + ")")
            except Exception:
                pass  # 窗口关闭或页面重载时可能暂时无法推送
            self._wake.wait(2 if self._cfg.get("compact", False) else 1)

    def _stop(self):
        self._closed.set()
        self._wake.set()

    def get_state(self):
        return {"on_top": self._cfg.get("on_top", True), "compact": self._cfg.get("compact", False)}

    def toggle_top(self):
        self._cfg["on_top"] = not self._cfg.get("on_top", True)
        _window.on_top = self._cfg["on_top"]
        self._save()
        return self._cfg["on_top"]

    def set_compact(self, compact):
        self._cfg["compact"] = bool(compact)
        self._wake.set()
        self._save()

    def fit(self, width, height):
        """按内容大小调整窗口；窗口在屏幕右半边时固定右边缘，在下半边时固定下边缘。"""
        w, h = int(math.ceil(width)), int(math.ceil(height))
        if (w, h) == (_window.width, _window.height):
            return
        x, y, cw, ch = _window.x, _window.y, _window.width, _window.height
        fp = FixPoint.NORTH | FixPoint.WEST
        for s in webview.screens:
            if s.x <= x + cw / 2 < s.x + s.width and s.y <= y + ch / 2 < s.y + s.height:
                fp = (FixPoint.EAST if x + cw / 2 > s.x + s.width / 2 else FixPoint.WEST) | (
                    FixPoint.SOUTH if y + ch / 2 > s.y + s.height / 2 else FixPoint.NORTH
                )
                break
        _window.resize(w, h, fix_point=fp)
        self._save()  # 记录的是调整后的位置和大小，下次启动不会跑出屏幕

    def minimize(self):
        _window.minimize()

    def close(self):
        _window.destroy()

    def _save(self):
        try:
            self._cfg.update(x=_window.x, y=_window.y, w=_window.width, h=_window.height)
            with open(CFG_PATH, "w", encoding="utf-8") as f:
                json.dump(self._cfg, f)
        except Exception:
            pass


def style_window(window):
    """Win11：DWM 原生圆角 + 深色 + 细边框。WebView2 做不到逐像素透明，所以不用 CSS 圆角。"""
    global _hwnd
    try:
        _hwnd = window.native.Handle.ToInt32()
        dwm = ctypes.windll.dwmapi

        def setattr_(attr, value):
            v = ctypes.c_int(value)
            dwm.DwmSetWindowAttribute(_hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))

        setattr_(20, 1)  # DWMWA_USE_IMMERSIVE_DARK_MODE
        setattr_(33, 2)  # DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND
        setattr_(34, 0x00453428)  # DWMWA_BORDER_COLOR，COLORREF 为 0x00BBGGRR → #283445
    except Exception:
        pass


_window = None
_hwnd = None


def main():
    global _window
    cfg = load_cfg()
    w, h = COMPACT_SIZE if cfg.get("compact") else FULL_SIZE
    w, h = cfg.get("w", w), cfg.get("h", h)
    x, y = cfg.get("x"), cfg.get("y")
    try:
        screens = webview.screens
        if x is None or y is None:
            x, y = screens[0].x + screens[0].width - w - 24, screens[0].y + 24
        # 换了显示器或分辨率时，把窗口拉回屏幕内
        s = next((s for s in screens if s.x <= x < s.x + s.width and s.y <= y < s.y + s.height), screens[0])
        x = max(s.x, min(x, s.x + s.width - w))
        y = max(s.y, min(y, s.y + s.height - h))
    except Exception:
        pass

    api = Api(Monitor(), cfg)
    _window = webview.create_window(
        "Pulse",
        url=os.path.join(HERE, "ui.html"),
        js_api=api,
        width=w,
        height=h,
        x=x,
        y=y,
        min_size=(120, 32),
        resizable=False,
        frameless=True,
        easy_drag=False,
        shadow=True,
        on_top=cfg.get("on_top", True),
        background_color="#0c111d",
    )
    _window.events.shown += lambda: style_window(_window)
    _window.events.loaded += api._start
    _window.events.closing += api._save
    _window.events.closed += api._stop
    webview.start()


if __name__ == "__main__":
    main()
