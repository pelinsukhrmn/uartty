# sterm

A lightweight serial terminal for FPGA and embedded boards, plus a small
UART bring-up design to point it at. Single binary, no external
dependencies, runs on Linux, macOS and Windows.

```
make
./sterm /dev/ttyUSB1 -b 115200        # Linux
sterm COM5 -b 115200                  # Windows
```

## Why

`minicom` needs a config dance, `screen` mangles your terminal on exit, and
`picocom` cannot hex-dump. `sterm` does the parts you actually need when you
are bringing up a UART core on a Basys 3 or an Arty A7.

## Build

**Linux / macOS**

```
make
```

**Windows**, in an *MSYS2 MINGW64* shell (not the MSYS shell):

```
pacman -S mingw-w64-x86_64-gcc make
make
```

The result is an ordinary statically linked console `.exe`. Copy it anywhere
on `PATH` and run it from `cmd.exe` or PowerShell with no MSYS2 or Cygwin DLL
alongside it.

Cross-building the same `.exe` from Linux:

```
make CROSS=x86_64-w64-mingw32-
```

## Options

| Flag | Meaning |
|---|---|
| `-b, --baud N` | baud rate (default 115200; arbitrary rates supported) |
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
| `--poll` | poll the port instead of waiting on a driver event (Windows) |
| `-L, --list` | list available serial devices |
| `-q, --quiet` | suppress status messages |

## Keys

Press the escape key (Ctrl-A) then:

```
q  quit          h  help         c  clear screen
e  local echo    x  hex dump     t  timestamps
b  send BREAK    d  toggle DTR   r  toggle RTS
m  modem lines   s  send file    l  toggle logging
Ctrl-A  send a literal 0x01
```

Ctrl-C is *not* a quit key: it is passed through to the target as byte 0x03,
which is usually what you want when there is a shell on the other end. Use
Ctrl-A q.

## Port names

| Platform | Accepted |
|---|---|
| Linux | `/dev/ttyUSB1`, `/dev/ttyACM0`, `/dev/serial/by-id/...` |
| Windows | `COM5`, `com5`, `5`, `\\.\COM5`, `/dev/com5` |

On Windows, `/dev/ttyS4` and `/dev/ttyUSB1` are rejected on purpose: `ttyS` is
off by one against `COM`, and on a two-channel FT2232H a wrong guess could
open the JTAG side. Run `sterm -L` and use the `COMn` it prints.

`sterm -L` on Windows prints friendly names from SetupAPI, which is how you
tell which port belongs to the board:

```
  COM3     Communications Port
  COM5     USB Serial Port (COM5)
```

## Working alongside Vivado

Basys 3 and Arty A7 both expose an FT2232HQ with two channels: **A is JTAG**,
driven by Vivado's Hardware Manager through its own FTDI driver, and **B is
the UART**, which the OS enumerates as a COM port (`/dev/ttyUSB1` on Linux).
Digilent's reference manual states the two functions behave entirely
independently, and in practice that holds: you can leave `sterm` connected
while you program a bitstream.

The only real conflict is two programs on the same port. Quit Vitis Serial
Terminal or PuTTY before starting `sterm` - the port is opened exclusively
(`TIOCEXCL` on POSIX, share mode 0 on Windows) so the second one loses.

**Do not use WSL2 + `usbipd` for this.** Attaching the device hands the whole
FT2232HQ to Linux, channel A included, and Vivado on the Windows side loses
the board. Build the native `.exe` instead.

## The FPGA side

`fpga/` holds a minimal UART bring-up design for the Arty A7-100T, so you can
verify the terminal against known-good hardware before trusting it with your
own core.

```
cd fpga
./sim.sh                                    # Icarus Verilog, no Vivado
vivado -mode batch -source build.tcl        # -> uart_echo_top.bit
```

It gives you two independent checks, which is the point - an echo test that
fails tells you nothing about *which* direction is broken:

| Check | Exercises |
|---|---|
| type a character, see it echoed | RX and TX together |
| press `btn[0]`, a banner is sent | TX alone |
| `led[3:0]` shows the last byte's low nibble | RX alone, no terminal needed |

So: press `btn[0]` and if `ARTY A7-100T UART OK` appears, TX and your baud
settings are right. Then type `A` and watch `led[0]` light - that is RX
working even if the echo path is broken.

### Signal directions

This is the single most common way to lose an afternoon on this board. The
port names in Digilent's master XDC are from the **PC's** point of view, not
the FPGA's:

| XDC port | Pin | Direction in your top module |
|---|---|---|
| `uart_rxd_out` | D10 | **output** - the PC's RX line, so the FPGA drives it |
| `uart_txd_in` | A9 | **input** - the PC's TX line, so the FPGA reads it |

Get this backwards and synthesis will not complain; you will simply see
nothing. LD10 and LD9 on the board are TX and RX traffic LEDs and are the
fastest way to find out which side is silent.

### Flow control

The Arty A7 wires only two signals (TXD/RXD) to the FPGA - there is no
RTS/CTS path. Use `-f n`, the default. `-f h` will hang on transmit because
CTS never asserts. `Ctrl-A m` shows the modem lines if you want to confirm.

### Baud rates on a 100 MHz clock

`uart_rx.v` and `uart_tx.v` take `CLK_HZ` and `BAUD` parameters and derive
`DIV = CLK_HZ / BAUD`.

| Baud | DIV | Actual | Error |
|---|---|---|---|
| 115200 | 868 | 115207 | 0.006% |
| 921600 | 108 | 925926 | 0.47% |
| 1000000 | 100 | 1000000 | 0% |

1 Mbaud divides exactly, which removes baud quantisation from the list of
suspects during bring-up. The FT2232H handles it, and so does `sterm` on both
platforms.

## Layout

| File | Contents |
|---|---|
| `sterm.c` | platform-neutral core, no OS headers |
| `plat.h` | the interface the core is written against |
| `plat_posix.c` | termios, poll, self-pipe for signals |
| `plat_win32.c` | DCB, overlapped I/O, `WaitCommEvent`, console VT mode |
| `custom_baud.c` | Linux `BOTHER` helper for non-standard rates |
| `fpga/uart_rx.v` | 8N1 receiver, mid-bit sampling, start-bit glitch rejection |
| `fpga/uart_tx.v` | 8N1 transmitter |
| `fpga/uart_echo_top.v` | echo + banner + LED nibble |
| `fpga/arty_a7_100t.xdc` | pin and timing constraints |
| `fpga/tb_uart.v` | self-checking functional testbench |
| `fpga/tb_rates.v` | same design at real 100 MHz board rates |
| `test_pty.py` | POSIX end-to-end over a pty |
| `test_win32_cli.py` | Windows CLI surface, no port needed |
| `test_win32_loopback.py` | Windows end-to-end over a com0com pair |

## Test

Three layers, because no single one covers the whole thing.

**POSIX, no hardware.** `test_pty.py` spawns a pseudo-terminal pair and
exercises RX, TX, hex dump, timestamps, EOL translation and logging. POSIX
only; it relies on the `pty` module.

```
python3 test_pty.py
```

**Windows CLI, no COM port.** `test_win32_cli.py` covers argument validation,
port-name handling and every error path. It runs natively on Windows and also
under Wine, which makes it usable from Linux CI:

```
python test_win32_cli.py sterm.exe                 # on Windows
python3 test_win32_cli.py sterm.exe --wine         # on Linux
```

This layer earns its keep. Before it existed, `sterm --help` and every error
message on Windows printed *nothing at all* - the std handles were only
fetched during console setup, which happens after argument parsing, so
everything before that went to `INVALID_HANDLE_VALUE` and vanished. Reading
the code had not caught it; running the binary caught it in one command.

**Windows end-to-end, virtual null-modem.** `test_win32_loopback.py` drives
real traffic through the real driver stack using a com0com pair. This is the
part Wine cannot help with - its serial emulation faults in `WaitCommEvent`,
does not populate `COMSTAT`, and blocks in `WriteFile`, all reproducible with
a thirty-line program that uses nothing but documented APIs.

```
setupc.exe install PortName=COM10 PortName=COM11    # once
pip install pyserial
python test_win32_loopback.py sterm.exe COM10 COM11
python test_win32_loopback.py sterm.exe COM10 COM11 --poll
```

Run it both ways: `--poll` selects the polling path instead of
`WaitCommEvent`, and virtual COM drivers are exactly where that difference
shows up.

**The board itself** is the fourth layer, and the only one that proves the
whole chain. Flash `fpga/uart_echo_top.bit`, then `sterm COM5 -b 115200` and
press `btn[0]`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Random-looking bytes | baud mismatch. `-x` to confirm: garbage is a wrong rate, a consistent one-bit shift is a framing bug |
| Nothing at all, LD9 dark | wrong COM port, or the PC is not transmitting |
| Nothing at all, LD9 lights | FPGA side: check the `uart_rxd_out` / `uart_txd_in` directions |
| Transmit hangs | `-f h` on a board with no CTS wired. Use `-f n` |
| Permission denied (Linux) | `sudo usermod -aG dialout $USER`, then log out and back in |
| Port is busy (Windows) | Vitis Serial Terminal or PuTTY still has it open |
| Windows: opens but never receives | a driver whose `WaitCommEvent` does not work. Try `--poll` |
