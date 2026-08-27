#!/usr/bin/env python3
"""Windows CLI tests for sterm.exe - everything that needs no live COM port.

Runs natively on Windows, and under Wine on Linux for CI:

    python3 test_win32_cli.py sterm.exe
    WINEPREFIX=~/.wine python3 test_win32_cli.py sterm.exe --wine

These cover argument validation, port-name handling and the error paths.
They are worth having on their own: the "no output at all" bug in --help and
in every die() on Windows was invisible until something actually ran the
binary, and no amount of reading the code had caught it.
"""
import os, subprocess, sys

EXE = sys.argv[1] if len(sys.argv) > 1 else "sterm.exe"
WINE = "--wine" in sys.argv
RUNNER = ["/usr/lib/wine/wine64"] if WINE else []

ok = True

def run(args, timeout=30):
    p = subprocess.run(RUNNER + [EXE] + args, capture_output=True, timeout=timeout)
    # Wine writes its own diagnostics to stderr; keep only our own lines
    err = b"\n".join(l for l in p.stderr.splitlines() if not l.startswith(b"wine:"))
    return p.returncode, p.stdout, err

def check(label, cond):
    global ok
    print(("PASS  " if cond else "FAIL  ") + label)
    ok = ok and cond

# --- help and usage -----------------------------------------------------
rc, out, err = run(["--help"])
check("--help exits 0", rc == 0)
check("--help actually prints something", len(out) > 200)
check("--help mentions the tool", b"lightweight UART terminal" in out)
check("--help shows a Windows example", b"COM" in out)

rc, out, err = run([])
check("no args exits 2", rc == 2)
check("no args prints usage to stderr", b"usage:" in err)

# --- argument validation ------------------------------------------------
for bad, needle in [
    (["-b", "abc", "COM1"], b"--baud"),
    (["-b", "0", "COM1"],   b"--baud"),
    (["-d", "9", "COM1"],   b"--databits"),
    (["-d", "x", "COM1"],   b"--databits"),
    (["-s", "3", "COM1"],   b"--stopbits"),
    (["-p", "q", "COM1"],   b"--parity"),
    (["-f", "z", "COM1"],   b"--flow"),
    (["--eol", "nope", "COM1"], b"--eol"),
    (["--escape", "ab", "COM1"], b"--escape"),
]:
    rc, out, err = run(bad)
    check("rejects %-24s" % " ".join(bad[:-1]), rc != 0 and needle in err)

# an error message with no text is the bug this file exists to catch
rc, out, err = run(["-b", "abc", "COM1"])
check("error messages are not silent", len(err.strip()) > 10)

# --- port name handling -------------------------------------------------
rc, out, err = run(["/dev/ttyS4"])
check("rejects /dev/ttyS4 rather than guessing", rc != 0 and b"COM" in err)
rc, out, err = run(["/dev/ttyUSB1"])
check("rejects /dev/ttyUSB1", rc != 0 and b"COM" in err)
rc, out, err = run(["garbage"])
check("rejects a non-port name", rc != 0 and b"COM5" in err)

rc, out, err = run(["COM99"])
check("missing port names itself in the error", rc != 0 and b"COM99" in err)
check("missing port suggests -L", b"-L" in err)

# COM5 / com5 / 5 must all normalise to the same thing, so the error text
# for a port that is absent should be identical for all three
msgs = set()
for spelling in ["COM99", "com99", "99"]:
    rc, out, err = run([spelling])
    msgs.add(err.strip())
check("COM99 / com99 / 99 normalise identically", len(msgs) == 1)

# --- listing ------------------------------------------------------------
rc, out, err = run(["-L"])
check("-L exits 0", rc == 0)
check("-L prints something", len(out) > 0)

print()
print("ALL TESTS PASSED" if ok else "FAILURES")
sys.exit(0 if ok else 1)
