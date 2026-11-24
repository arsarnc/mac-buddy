#!/usr/bin/env python3
"""Send Mac stats to the ESP32 Mac Monitor screen over USB, once per second.

Shows CPU, GPU, memory, network, CPU temperature, battery and disk on the board.
Run with ./run.sh (sets up its own Python environment the first time). Ctrl+C to stop.
Send this process SIGUSR1 (./screenshot.sh does) to save a PNG of every page into ../screenshots/.
"""
import ctypes
import ctypes.util
import glob
import json
import os
import re
import signal
import struct
import zlib
import socket
import subprocess
import sys
import time

import psutil
import serial

from claude_usage import ClaudeUsage

BAUD = 115200
# MAC_BUDDY_ANON=1 ./run.sh  -> generic Mac name, IP and project names (for public screenshots)
ANON = os.environ.get("MAC_BUDDY_ANON") == "1"
PAGES = ["1-overview", "2-cpu", "3-gpu", "4-memory", "5-network", "6-claude"]
SHOT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "screenshots")
shot_wanted = False


# ---------- Screenshots ----------
def write_png(path, w, h, rgb565):
    """Save a little-endian RGB565 frame as PNG (no extra libraries needed)."""
    rows = bytearray()
    px = struct.unpack(f"<{w * h}H", rgb565)
    for y in range(h):
        rows.append(0)  # filter: none
        for v in px[y * w:(y + 1) * w]:
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def read_exact(ser, n, timeout):
    data, end = bytearray(), time.time() + timeout
    while len(data) < n and time.time() < end:
        chunk = ser.read(n - len(data))
        if chunk:
            data += chunk
        else:
            time.sleep(0.002)
    return bytes(data)


def read_line(ser, prefix, timeout):
    """Read lines until one starts with prefix (skipping the board's log output)."""
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        c = ser.read(1)
        if not c:
            time.sleep(0.002)
            continue
        if c == b"\n":
            if buf.startswith(prefix):
                return buf.decode(errors="replace")
            buf = b""
        else:
            buf += c
    return None


BLOCK_ROWS = 16  # 16 rows = 10 KB, fits the board's USB buffer


def take_screenshots(ser):
    """Ask the board to render each page, then pull its frame buffer in checked blocks."""
    os.makedirs(SHOT_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    saved = []
    try:
        for i, name in enumerate(PAGES):
            header = None
            for _ in range(3):  # the board's USB can stall for several seconds; ask again
                ser.reset_input_buffer()
                ser.write(f"SHOT {i}\n".encode())
                header = read_line(ser, b"SHOT ", 8)
                if header:
                    break
            if not header:
                print(f"\rScreenshot {name}: no answer from the board (old firmware?)")
                break
            _, w, h = header.split()
            w, h = int(w), int(h)
            frame = bytearray()
            for y in range(0, h, BLOCK_ROWS):
                n = min(BLOCK_ROWS, h - y)
                for attempt in range(6):
                    ser.reset_input_buffer()
                    ser.write(f"ROWS {y} {n}\n".encode())
                    line = read_line(ser, b"ROWS ", 3)
                    if line:
                        want = int(line.split()[3])
                        data = read_exact(ser, n * w * 2, 3)
                        if len(data) == n * w * 2 and sum(data) == want:
                            frame += data
                            break
                else:
                    raise IOError(f"block at row {y} kept failing")
            path = os.path.normpath(os.path.join(SHOT_DIR, f"{stamp}-{name}.png"))
            write_png(path, w, h, bytes(frame))
            saved.append(path)
    except (IOError, ValueError, IndexError) as e:
        print(f"\rScreenshot failed: {e}")
    finally:
        ser.write(b"DONE\n")
    return saved


# ---------- CPU temperature (Apple Silicon HID sensors, no sudo needed) ----------
class TempReader:
    def __init__(self):
        self.client = None
        try:
            iokit = ctypes.cdll.LoadLibrary(ctypes.util.find_library("IOKit"))
            cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
            vp, i32, i64 = ctypes.c_void_p, ctypes.c_int32, ctypes.c_int64
            cf.CFStringCreateWithCString.restype = vp
            cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_uint32]
            cf.CFNumberCreate.restype = vp
            cf.CFNumberCreate.argtypes = [vp, ctypes.c_long, vp]
            cf.CFDictionaryCreate.restype = vp
            cf.CFDictionaryCreate.argtypes = [vp, vp, vp, ctypes.c_long, vp, vp]
            cf.CFArrayGetCount.restype = ctypes.c_long
            cf.CFArrayGetCount.argtypes = [vp]
            cf.CFArrayGetValueAtIndex.restype = vp
            cf.CFArrayGetValueAtIndex.argtypes = [vp, ctypes.c_long]
            cf.CFRelease.argtypes = [vp]
            cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
            iokit.IOHIDEventSystemClientCreate.restype = vp
            iokit.IOHIDEventSystemClientCreate.argtypes = [vp]
            iokit.IOHIDEventSystemClientSetMatching.argtypes = [vp, vp]
            iokit.IOHIDEventSystemClientCopyServices.restype = vp
            iokit.IOHIDEventSystemClientCopyServices.argtypes = [vp]
            iokit.IOHIDServiceClientCopyEvent.restype = vp
            iokit.IOHIDServiceClientCopyEvent.argtypes = [vp, i64, i32, i64]
            iokit.IOHIDEventGetFloatValue.restype = ctypes.c_double
            iokit.IOHIDEventGetFloatValue.argtypes = [vp, i32]
            iokit.IOHIDServiceClientCopyProperty.restype = vp
            iokit.IOHIDServiceClientCopyProperty.argtypes = [vp, vp]
            self.iokit, self.cf = iokit, cf

            def cfstr(s):
                return cf.CFStringCreateWithCString(None, s.encode(), 0x08000100)

            def cfnum(v):
                n = ctypes.c_int32(v)
                return cf.CFNumberCreate(None, 3, ctypes.byref(n))

            kcb = ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryKeyCallBacks")
            vcb = ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryValueCallBacks")
            keys = (vp * 2)(cfstr("PrimaryUsagePage"), cfstr("PrimaryUsage"))
            vals = (vp * 2)(cfnum(0xFF00), cfnum(5))  # Apple vendor page, temperature sensors
            match = cf.CFDictionaryCreate(None, keys, vals, 2, ctypes.addressof(kcb), ctypes.addressof(vcb))
            self.client = iokit.IOHIDEventSystemClientCreate(None)
            iokit.IOHIDEventSystemClientSetMatching(self.client, match)
            self.product = cfstr("Product")
        except Exception as e:  # not Apple Silicon / API changed: just skip temperature
            print("Temperature unavailable:", e)
            self.client = None

    def cpu_temp(self):
        """Hottest SoC die sensor (the 'PMU tdie*' sensors), in C, or None."""
        if not self.client:
            return None
        iokit, cf = self.iokit, self.cf
        services = iokit.IOHIDEventSystemClientCopyServices(self.client)
        if not services:
            return None
        hottest = None
        buf = ctypes.create_string_buffer(64)
        for i in range(cf.CFArrayGetCount(services)):
            svc = cf.CFArrayGetValueAtIndex(services, i)
            name = iokit.IOHIDServiceClientCopyProperty(svc, self.product)
            if not name:
                continue
            cf.CFStringGetCString(name, buf, 64, 0x08000100)
            cf.CFRelease(name)
            if b"tdie" not in buf.value:
                continue
            ev = iokit.IOHIDServiceClientCopyEvent(svc, 15, 0, 0)  # 15 = temperature event
            if not ev:
                continue
            t = iokit.IOHIDEventGetFloatValue(ev, 15 << 16)
            cf.CFRelease(ev)
            if 0 < t < 130:
                hottest = t if hottest is None else max(hottest, t)
        cf.CFRelease(services)
        return hottest


GPU_KEYS = {"gpu": rb'"Device Utilization %"=(\d+)', "gR": rb'"Renderer Utilization %"=(\d+)',
            "gT": rb'"Tiler Utilization %"=(\d+)', "gpuM": rb'"In use system memory"=(\d+)',
            "gA": rb'"Alloc system memory"=(\d+)'}


def gpu_stats():
    """GPU busy % (+ renderer / tiler %) and memory in use / allocated (GB), from the IOAccelerator driver."""
    info = {"gpu": -1}
    try:
        out = subprocess.run(["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"], capture_output=True, timeout=2).stdout
    except (OSError, subprocess.TimeoutExpired):
        return info
    for key, pattern in GPU_KEYS.items():
        m = re.search(pattern, out)
        if m:
            v = int(m.group(1))
            info[key] = round(v / 2**30, 1) if key in ("gpuM", "gA") else v
    return info


VM_LINE = re.compile(r"^(.+?):\s+(\d+)\.?$", re.M)


def memory_details():
    """Memory pressure + Activity Monitor style breakdown (GB). Pressure level: 0 normal, 1 warning, 2 critical."""
    info = {}
    try:
        out = subprocess.run(["sysctl", "-n", "kern.memorystatus_vm_pressure_level", "kern.memorystatus_level"],
                             capture_output=True, text=True, timeout=2).stdout.split()
        info["mpl"] = {1: 0, 2: 1, 4: 2}.get(int(out[0]), 0)
        info["mp"] = 100 - int(out[1])  # memory pressure % (100 - free %)
        vm = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=2).stdout
        page = int(re.search(r"page size of (\d+)", vm).group(1))
        pages = {k: int(v) for k, v in VM_LINE.findall(vm)}
        gb = lambda key: round(pages.get(key, 0) * page / 2**30, 1)
        info["mApp"] = round(max(pages.get("Anonymous pages", 0) - pages.get("Pages purgeable", 0), 0) * page / 2**30, 1)
        info["mWir"] = gb("Pages wired down")
        info["mCmp"] = gb("Pages occupied by compressor")
        info["mCac"] = gb("File-backed pages")
    except (OSError, subprocess.TimeoutExpired, ValueError, IndexError, AttributeError):
        pass
    info["swp"] = round(psutil.swap_memory().used / 2**30, 1)
    return info


def top_processes(n=5):
    """Top processes by CPU % and by memory, grouped by app name like 'Safari (12)'. Uses ps: no sudo needed."""
    try:
        out = subprocess.run(["ps", "-Aceo", "pcpu=,rss=,comm="], capture_output=True, text=True, timeout=2).stdout
    except (OSError, subprocess.TimeoutExpired):
        return [], []
    groups = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        cpu, rss, name = float(parts[0]), int(parts[1]), parts[2].strip()
        g = groups.setdefault(name, [0.0, 0, 0])
        g[0] += cpu
        g[1] += rss
        g[2] += 1

    def label(name, count):
        name = name[:20]
        return f"{name} ({count})" if count > 1 else name

    by_cpu = sorted(groups.items(), key=lambda kv: -kv[1][0])[:n]
    by_mem = sorted(groups.items(), key=lambda kv: -kv[1][1])[:n]
    return ([[label(k, v[2]), round(v[0])] for k, v in by_cpu],
            [[label(k, v[2]), round(v[1] / 2**20, 2)] for k, v in by_mem])


def network_info():
    """Default interface, its kind (Wi-Fi / Ethernet ...) and IPv4 address."""
    info = {"if": "", "it": "", "ip": ""}
    try:
        route = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True, timeout=2).stdout
        m = re.search(r"interface:\s*(\S+)", route)
        if not m:
            return info
        dev = m.group(1)
        info["if"] = dev
        ports = subprocess.run(["networksetup", "-listallhardwareports"], capture_output=True, text=True,
                               timeout=3).stdout
        m = re.search(r"Hardware Port: (.+)\nDevice: " + re.escape(dev) + r"\n", ports)
        info["it"] = (m.group(1) if m else "VPN / other")[:16]
        for addr in psutil.net_if_addrs().get(dev, []):
            if addr.family == socket.AF_INET:
                info["ip"] = addr.address
    except (OSError, subprocess.TimeoutExpired):
        pass
    return info


def host_name():
    try:
        name = subprocess.run(["scutil", "--get", "ComputerName"], capture_output=True, text=True).stdout.strip()
    except OSError:
        name = ""
    return (name or socket.gethostname().split(".")[0])[:30]


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def open_port(path):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = path, BAUD, 0, 2
    s.dtr = s.rts = False  # don't reset the board when the port opens
    s.open()
    return s


def main():
    def want_shot(*_):
        global shot_wanted
        shot_wanted = True

    signal.signal(signal.SIGUSR1, want_shot)
    temps = TempReader()
    host = host_name()
    psutil.cpu_percent(None)
    psutil.cpu_percent(percpu=True)
    psutil.cpu_times_percent(None)
    try:
        p_cores = int(subprocess.run(["sysctl", "-n", "hw.perflevel0.logicalcpu"], capture_output=True,
                                     text=True).stdout)
    except (OSError, ValueError):
        p_cores = 0
    top_cpu, top_mem, net_info, tick = [], [], {}, 0
    claude = ClaudeUsage()
    last_net, last_t = psutil.net_io_counters(), time.time()
    port, ser = None, None
    print("Mac Monitor: sending stats to the ESP32 screen (Ctrl+C to stop)" + (" [anonymized]" if ANON else ""))

    while True:
        time.sleep(1)
        if ser is None:
            port = find_port()
            if not port:
                print("\rWaiting for the board on USB...      ", end="", flush=True)
                continue
            try:
                ser = open_port(port)
                print(f"\rConnected to {port}                  ")
            except serial.SerialException as e:
                print(f"\rCan't open {port}: {e}")
                continue

        now = time.time()
        net = psutil.net_io_counters()
        dt = max(now - last_t, 0.001)
        rx = (net.bytes_recv - last_net.bytes_recv) / dt
        tx = (net.bytes_sent - last_net.bytes_sent) / dt
        last_net, last_t = net, now

        mem = psutil.virtual_memory()
        used = mem.total - mem.available
        disk = psutil.disk_usage("/System/Volumes/Data")
        bat = psutil.sensors_battery()
        temp = temps.cpu_temp()
        gpu = gpu_stats()
        mem_info = memory_details()
        if tick % 2 == 0:
            top_cpu, top_mem = top_processes()
        claude_info = None
        if tick % 10 == 0:
            net_info = network_info()
            try:
                claude_info = claude.summary()
                if ANON:
                    claude_info["pj"] = [[f"project-{chr(97 + i)}", v] for i, (_, v) in enumerate(claude_info["pj"])]
            except Exception as e:  # never let the log reader stop the monitor
                print(f"\rClaude usage: {e}")
        tick += 1
        times = psutil.cpu_times_percent(None)
        swap = psutil.swap_memory()

        msg = {
            "mac": 1,
            "host": "MacBook" if ANON else host,
            "cpu": round(psutil.cpu_percent(None), 1),
            "ram": round(used * 100 / mem.total, 1),
            "cores": [round(c) for c in psutil.cpu_percent(percpu=True)],
            "pc": p_cores,
            "cu": round(times.user + times.nice),
            "cs": round(times.system),
            "la": [round(x, 2) for x in os.getloadavg()],
            "tc": top_cpu,
            "tm": top_mem,
            **gpu,
            "ramU": round(used / 2**30, 1),
            "ramT": round(mem.total / 2**30),
            "tmp": round(temp, 1) if temp else -1,
            "bat": round(bat.percent) if bat else -1,
            "chg": int(bool(bat and bat.power_plugged and bat.percent < 100)),
            "ac": int(bool(bat and bat.power_plugged)),
            "rx": round(rx),
            "tx": round(tx),
            "dU": round((disk.total - disk.free) / 1e9, 1),  # APFS: count space used by other volumes too
            "dT": round(disk.total / 1e9, 1),
            **mem_info,
            "swT": round(swap.total / 2**30, 1),
            **({**net_info, "ip": "192.168.1.42"} if ANON and net_info.get("ip") else net_info),
            "rxT": round(net.bytes_recv / 1e9, 2),
            "txT": round(net.bytes_sent / 1e9, 2),
            **({"cl": claude_info} if claude_info else {}),
            "t": int(now),
            "tz": -time.altzone if time.localtime().tm_isdst > 0 else -time.timezone,
        }
        try:
            ser.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            ser.reset_input_buffer()  # ignore the board's own log output
            global shot_wanted
            if shot_wanted:
                shot_wanted = False
                saved = take_screenshots(ser)
                print(f"\rSaved {len(saved)} screenshot(s) to {os.path.normpath(SHOT_DIR)}")
                with open(os.path.join(SHOT_DIR, ".done"), "w") as f:
                    f.write("\n".join(saved))
        except (serial.SerialException, OSError):
            print("\rBoard disconnected.                   ")
            try:
                ser.close()
            except Exception:
                pass
            ser = None


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)
