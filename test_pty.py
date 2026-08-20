import os, pty, subprocess, time, sys

def run(args, tx_from_board=b"", tx_from_user=b"", wait=0.4):
    master, slave = pty.openpty()
    name = os.ttyname(slave)
    p = subprocess.Popen(["./sterm", name] + args,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    os.close(slave)
    time.sleep(0.2)
    if tx_from_board:
        os.write(master, tx_from_board)
    if tx_from_user:
        p.stdin.write(tx_from_user); p.stdin.flush()
    time.sleep(wait)
    os.set_blocking(master, False)
    try:
        back = os.read(master, 4096)
    except BlockingIOError:
        back = b""
    p.stdin.close()
    p.terminate()
    out = p.stdout.read()
    os.close(master)
    return out, back

ok = True
def check(label, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + label)
    ok = ok and cond

# 1. board -> screen
out, _ = run(["-q", "-b", "115200"], tx_from_board=b"FPGA UART ready\r\n")
check("RX passthrough", b"FPGA UART ready" in out)

# 2. hex dump
out, _ = run(["-q", "-x"], tx_from_board=b"AB\x00\xff")
check("hex dump", b"41 42 00 ff" in out and b"|AB..|" in out)

# 3. timestamps
out, _ = run(["-q", "-t"], tx_from_board=b"line1\r\n")
check("timestamp prefix", out.count(b"[") >= 1 and b"line1" in out)

# 4. user -> board, Enter becomes CR
_, back = run(["-q", "--eol", "cr"], tx_from_user=b"led on\n")
check("TX eol=cr", back == b"led on\r", )

# 5. eol crlf
_, back = run(["-q", "--eol", "crlf"], tx_from_user=b"x\n")
check("TX eol=crlf", back == b"x\r\n")

# 6. logging
if os.path.exists("/tmp/l.log"): os.remove("/tmp/l.log")
run(["-q", "-g", "/tmp/l.log"], tx_from_board=b"logged bytes\r\n")
data = open("/tmp/l.log","rb").read()
check("log file raw", b"logged bytes" in data)

# 7. bad args
r = subprocess.run(["./sterm", "-d", "9", "/dev/null"], capture_output=True)
check("arg validation", r.returncode != 0)

# 8. non-tty device rejected
r = subprocess.run(["./sterm", "/dev/null"], capture_output=True)
check("non-tty rejected", b"not a tty" in r.stderr)

# 9. help / list
r = subprocess.run(["./sterm", "--help"], capture_output=True)
check("help", b"lightweight UART terminal" in r.stdout)
r = subprocess.run(["./sterm", "--list"], capture_output=True)
check("list ports", r.returncode == 0)

sys.exit(0 if ok else 1)
