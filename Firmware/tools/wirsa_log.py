# Records the WiRSa's USB serial output to one file, across unplugs, re-enumeration and deep sleep.
# Usage: wirsa_log.py FILE [SECONDS]. Port: /dev/t7s3 (the udev name), else the newest /dev/ttyACM*.
import glob, os, serial, sys, time

out = open(sys.argv[1], "ab", buffering=0)
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 10 * 86400)

def port():
    for n in ("/dev/t7s3", "/dev/wirsa"):
        if os.path.exists(n): return n
    p = sorted(glob.glob("/dev/ttyACM*"), key=os.path.getmtime)
    return p[-1] if p else None

def note(s): out.write(("\n=== %s %s ===\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), s)).encode())

state = None
while time.time() < end:
    p = port()
    if not p:
        if state != "gone": note("no port (unplugged or asleep)"); state = "gone"
        time.sleep(1); continue
    try:
        s = serial.Serial(p, 115200, timeout=1)
        note("port open " + p); state = "open"
        while time.time() < end:
            d = s.read(4096)
            if d: out.write(d)
    except Exception as e:
        if state != "lost": note("port lost: %s" % str(e)[:80]); state = "lost"
        time.sleep(1)
