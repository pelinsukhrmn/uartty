# sterm

A lightweight serial terminal for FPGA and embedded boards. Single binary,
POSIX `termios` only, no external dependencies.

```
make
./sterm /dev/ttyUSB1 -b 115200
```

## Why

`minicom` needs a config dance, `screen` mangles your terminal on exit, and
`picocom` cannot hex-dump. `sterm` is ~700 lines of C that does the parts you
actually need when you are bringing up a UART core on a Basys 3 or an STM32.

## Options

| Flag | Meaning |
|---|---|
| `-b, --baud N` | baud rate (default 115200; arbitrary rates on Linux via `BOTHER`) |
| `-d, --databits N` | 5..8 (default 8) |
| `-p, --parity n\|e\|o` | parity (default n) |
| `-s, --stopbits N` | 1 or 2 (default 1) |
| `-f, --flow n\|h\|s` | none / RTS-CTS / XON-XOFF |
| `-e, --echo` | local echo |
| `-x, --hex` | hex dump received bytes |
| `-t, --timestamp` | timestamp each received line |
| `--eol cr\|lf\|crlf` | what Enter transmits (default `cr`) |
| `--no-crlf` | do not add LF after a received bare CR |
| `-g, --log FILE` | append raw RX bytes to a file |
| `--tx-delay MS` | pause per 256-byte block when sending a file |
| `--escape CHAR` | escape key, `a`..`z` (default `a` = Ctrl-A) |
| `-L, --list` | list available serial devices |

## Keys

Press the escape key (Ctrl-A) then:

```
q  quit          h  help         c  clear screen
e  local echo    x  hex dump     t  timestamps
b  send BREAK    d  toggle DTR   r  toggle RTS
s  send file     l  toggle logging
Ctrl-A  send a literal 0x01
```

## Notes

- Basys 3 / Arty enumerate as an FT2232H; the UART is usually the **second**
  channel, i.e. `/dev/ttyUSB1`. Use `./sterm -L` to see what is present.
- Permission denied? `sudo usermod -aG dialout $USER`, then log out and back in.
- `TIOCEXCL` is set on open, so a second terminal cannot steal the port.
- Hex mode is the fastest way to tell whether a garbled screen is a baud
  mismatch (bytes look random) or a framing bug (bytes are shifted by one bit).

## Test

`test_pty.py` spawns a pseudo-terminal pair and exercises RX, TX, hex dump,
timestamps, EOL translation and logging without any hardware attached.

```
python3 test_pty.py
```
