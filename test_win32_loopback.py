#!/usr/bin/env python3
"""End-to-end Windows test for sterm.exe over a virtual null-modem pair.

This is the Windows counterpart of test_pty.py: it drives real serial traffic
through the real driver stack, which is the part Wine cannot emulate.

Setup, once:

  1. Install com0com (signed build):  https://com0com.sourceforge.net/
  2. Create a pair, e.g. COM10 <-> COM11:
       cd "C:\\Program Files (x86)\\com0com"
       setupc.exe install PortName=COM10 PortName=COM11
  3. pip install pyserial

Then:

  python test_win32_loopback.py sterm.exe COM10 COM11

sterm opens the first port; this script plays the role of the board on the
second. Add --poll to test the polling path instead of WaitCommEvent - worth
running both, since com0com's event support has historically been uneven.
"""
import subprocess, sys, time, os

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

if len(sys.argv) < 4:
    sys.exit(__doc__)

EXE, STERM_PORT, BOARD_PORT = sys.argv[1], sys.argv[2], sys.argv[3]
EXTRA = ["--poll"] if "--poll" in sys.argv else []
ok = True

def check(label, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + label)
    ok = ok and cond

def run(args, from_board=b"", from_user=b"", wait=1.0):
    """Start sterm on STERM_PORT, act as the board on BOARD_PORT."""
    board = serial.Serial(BOARD_PORT, 115200, timeout=0)
    try:
        p = subprocess.Popen([EXE, STERM_PORT] + args + EXTRA,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        time.sleep(1.0)                      # let it open and configure
        if from_board:
            board.write(from_board); board.flush()
        if from_user:
            p.stdin.write(from_user); p.stdin.flush()
        time.sleep(wait)
        back = board.read(4096)
        try:
            p.stdin.close()
        except OSError:
            pass
        p.terminate()
        try:
            out = p.stdout.read()
        except Exception:
            out = b""
        p.wait(timeout=5)
        return out, back
    finally:
        board.close()

# 1. board -> screen
out, _ = run(["-q", "-b", "115200"], from_board=b"FPGA UART ready\r\n")
check("RX passthrough", b"FPGA UART ready" in out)

# 2. hex dump
out, _ = run(["-q", "-x"], from_board=b"AB\x00\xff")
check("hex dump", b"41 42 00 ff" in out and b"|AB..|" in out)

# 3. timestamps
out, _ = run(["-q", "-t"], from_board=b"line1\r\n")
check("timestamp prefix", out.count(b"[") >= 1 and b"line1" in out)

# 4. screen -> board, Enter becomes CR
_, back = run(["-q", "--eol", "cr"], from_user=b"led on\n")
check("TX eol=cr", back == b"led on\r")

_, back = run(["-q", "--eol", "crlf"], from_user=b"x\n")
check("TX eol=crlf", back == b"x\r\n")

_, back = run(["-q", "--eol", "lf"], from_user=b"x\n")
check("TX eol=lf", back == b"x\n")

# 5. binary cleanliness - NUL and high bytes must survive both ways
out, _ = run(["-q", "-x"], from_board=bytes(range(256)))
check("all 256 byte values received", b"00 01 02" in out and b"fd fe ff" in out)

# 6. logging
log = os.path.abspath("_loop.log")
if os.path.exists(log):
    os.remove(log)
run(["-q", "-g", log], from_board=b"logged bytes\r\n")
data = open(log, "rb").read() if os.path.exists(log) else b""
check("log file raw", b"logged bytes" in data)

# 7. exclusive open - a second sterm on the same port must be refused
board = serial.Serial(BOARD_PORT, 115200, timeout=0)
p1 = subprocess.Popen([EXE, STERM_PORT, "-q"] + EXTRA,
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT)
time.sleep(1.0)
p2 = subprocess.run([EXE, STERM_PORT, "-q"] + EXTRA, capture_output=True, timeout=20)
check("second instance refused", p2.returncode != 0)
check("refusal says the port is busy", b"busy" in p2.stdout + p2.stderr)
try:
    p1.stdin.close()
except OSError:
    pass
p1.terminate(); p1.wait(timeout=5); board.close()

# 8. throughput - a burst must arrive intact and in order
payload = (b"".join(b"%04d" % i for i in range(500)))     # 2000 bytes
out, _ = run(["-q"], from_board=payload, wait=3.0)
check("2000-byte burst arrives intact", payload in out)

print()
print("ALL TESTS PASSED" if ok else "FAILURES")
sys.exit(0 if ok else 1)
