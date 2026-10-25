"""Timestamped serial capture that does NOT reset the board (DTR/RTS stay deasserted).

    python tools/debug/serlog.py /dev/ttyACM0 1800 soak.log [reset]

Only one process may have the port open: a second opener (idf.py monitor, another
serlog) fights over DTR/RTS and resets the chip repeatedly. Exits on a port error
instead of reopening, for the same reason.
"""
import serial, sys, time
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
reset = len(sys.argv) > 4 and sys.argv[4] == "reset"
s = serial.Serial()
s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
if reset:
    s.rts = True; time.sleep(0.1); s.rts = False
t0 = time.time(); buf = b""
with open(out, "w") as f:
    while time.time() - t0 < secs:
        try:
            d = s.read(4096)
        except serial.SerialException as e:
            f.write("%9.3f SERIAL-EXC %s\n" % (time.time() - t0, e))
            break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            f.write("%9.3f %s\n" % (time.time() - t0, line.decode(errors="replace").rstrip()))
            f.flush()
