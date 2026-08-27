/* plat_win32.c - native Windows backend for sterm.
 *
 * Builds a plain .exe that runs in cmd.exe with no MSYS2/Cygwin runtime.
 * Link with -lsetupapi.
 */

#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

/* GUID_DEVCLASS_PORTS, spelled out so we do not need <initguid.h>. */
static const GUID guid_ports =
    { 0x4d36e978, 0xe325, 0x11ce, { 0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18 } };

static HANDLE hSer      = INVALID_HANDLE_VALUE;
static HANDLE hIn       = INVALID_HANDLE_VALUE;
static HANDLE hOut      = INVALID_HANDLE_VALUE;
static HANDLE hErr      = INVALID_HANDLE_VALUE;

/* Fetched on first use, not in console_init(): usage text and every die()
 * happen while parsing arguments, long before the console is set up, and
 * writing those to INVALID_HANDLE_VALUE loses them silently. */
static HANDLE std_out(void)
{
    if (hOut == INVALID_HANDLE_VALUE) hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    return hOut;
}

static HANDLE std_err(void)
{
    if (hErr == INVALID_HANDLE_VALUE) hErr = GetStdHandle(STD_ERROR_HANDLE);
    return hErr;
}

static OVERLAPPED ovEvt, ovRd, ovWr;
static DWORD  comm_mask     = 0;
static BOOL   wait_armed    = FALSE;

/* WaitCommEvent is the efficient way to block on the port, but not every
 * driver implements it properly - some virtual COM drivers and Wine fail or
 * crash on it. If it ever fails with anything other than ERROR_IO_PENDING we
 * stop using it and poll cbInQue instead for the rest of the session. 5 ms
 * of added latency is invisible in a terminal. */
static BOOL   use_polling   = FALSE;
static BOOL   force_polling = FALSE;
#define POLL_MS 5

static HANDLE hQuitEvt      = NULL;
static HANDLE hInDataEvt    = NULL;   /* thread -> main: bytes available   */
static HANDLE hInWantEvt    = NULL;   /* main -> thread: give me more      */
static HANDLE hInThread     = NULL;

static unsigned char in_buf[512];
static DWORD  in_len = 0, in_off = 0;
static volatile LONG in_eof = 0;

static DWORD  con_in_saved = 0, con_out_saved = 0;
static BOOL   con_saved_ok = FALSE, con_is_tty = FALSE, raw_active = FALSE;

/* Windows has no way to read back DTR/RTS, so shadow what we set. */
static BOOL dtr_state = TRUE, rts_state = TRUE;

static char errbuf[256];

/* --------------------------------------------------------------------- */

const char *st_errmsg(void)
{
    DWORD e = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, e, 0, errbuf, sizeof errbuf - 1, NULL);
    if (!n) { snprintf(errbuf, sizeof errbuf, "error %lu", (unsigned long)e); return errbuf; }
    /* FormatMessage loves trailing CRLF and periods */
    while (n && (errbuf[n-1]=='\r' || errbuf[n-1]=='\n' ||
                 errbuf[n-1]=='.'  || errbuf[n-1]==' ')) errbuf[--n] = '\0';
    return errbuf;
}

/* Accepts COM5, com5, 5, \\.\COM5 and Cygwin's /dev/com5. Deliberately
 * rejects /dev/ttyS4 and /dev/ttyUSB1: ttyS is off by one against COM and
 * ttyUSB has no Windows meaning, so guessing would silently open the wrong
 * port - which on a two-channel FT2232H is the JTAG side. */
static bool normalize_port(const char *in, char *out, size_t n, const char **why)
{
    const char *p = in;

    if (!_strnicmp(p, "\\\\.\\", 4)) { snprintf(out, n, "%s", p); return true; }

    if (!_strnicmp(p, "/dev/tty", 8) || !_strnicmp(p, "tty", 3)) {
        *why = "Linux tty names do not map cleanly to COM ports on Windows; "
               "use COM5 (see sterm -L)";
        return false;
    }
    if (!_strnicmp(p, "/dev/", 5)) p += 5;      /* /dev/com5 */
    if (!_strnicmp(p, "com", 3))   p += 3;

    if (*p < '0' || *p > '9') {
        *why = "expected a port like COM5";
        return false;
    }
    for (const char *q = p; *q; q++)
        if (*q < '0' || *q > '9') { *why = "expected a port like COM5"; return false; }

    snprintf(out, n, "\\\\.\\COM%s", p);
    return true;
}

bool st_serial_open(const char *port, const st_params *p, char *err, size_t errlen)
{
    char path[64];
    DCB dcb;
    COMMTIMEOUTS to;
    const char *why = "bad port name";

    if (!normalize_port(port, path, sizeof path, &why)) {
        snprintf(err, errlen, "%s: %s", port, why);
        return false;
    }

    /* share mode 0 = exclusive, the equivalent of TIOCEXCL */
    hSer = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hSer == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
            snprintf(err, errlen, "%s does not exist (run sterm -L)", path);
        else if (e == ERROR_ACCESS_DENIED)
            snprintf(err, errlen, "%s is busy - another terminal or Vitis "
                                  "has it open", path);
        else
            snprintf(err, errlen, "open %s: %s", path, st_errmsg());
        return false;
    }

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(hSer, &dcb)) {
        snprintf(err, errlen, "%s is not a serial port: %s", path, st_errmsg());
        CloseHandle(hSer); hSer = INVALID_HANDLE_VALUE;
        return false;
    }

    /* Unlike termios, DCB.BaudRate is a plain integer, so non-standard
     * rates need no special case on Windows. */
    dcb.BaudRate = p->baud;
    dcb.ByteSize = (BYTE)p->databits;
    dcb.StopBits = p->stopbits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.Parity   = p->parity == 'e' ? EVENPARITY :
                   p->parity == 'o' ? ODDPARITY  : NOPARITY;
    dcb.fParity  = p->parity != 'n';
    dcb.fBinary  = TRUE;
    dcb.fNull    = FALSE;             /* do not silently drop received NULs */
    dcb.fAbortOnError = FALSE;        /* a framing error must not wedge the port */
    dcb.fErrorChar    = FALSE;

    dcb.fOutxCtsFlow = p->rtscts;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fRtsControl  = p->rtscts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fOutX = dcb.fInX = p->xonxoff;
    dcb.XonChar  = 0x11;
    dcb.XoffChar = 0x13;
    dcb.XonLim   = 2048;
    dcb.XoffLim  = 512;

    if (!SetCommState(hSer, &dcb)) {
        snprintf(err, errlen, "baud %u / %d%c%d rejected by the driver: %s",
                 p->baud, p->databits, p->parity, p->stopbits, st_errmsg());
        CloseHandle(hSer); hSer = INVALID_HANDLE_VALUE;
        return false;
    }

    /* Reads return whatever is already buffered, immediately. The event
     * loop, not the driver, decides when to block. */
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    SetCommTimeouts(hSer, &to);

    SetupComm(hSer, 8192, 8192);
    PurgeComm(hSer, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    memset(&ovEvt, 0, sizeof ovEvt);
    memset(&ovRd,  0, sizeof ovRd);
    memset(&ovWr,  0, sizeof ovWr);
    ovEvt.hEvent = CreateEventA(NULL, TRUE,  FALSE, NULL);
    ovRd.hEvent  = CreateEventA(NULL, TRUE,  FALSE, NULL);
    ovWr.hEvent  = CreateEventA(NULL, TRUE,  FALSE, NULL);

    SetCommMask(hSer, EV_RXCHAR | EV_ERR | EV_BREAK);
    dtr_state = rts_state = TRUE;
    return true;
}

void st_serial_close(void)
{
    if (hSer == INVALID_HANDLE_VALUE) return;
    SetCommMask(hSer, 0);
    CancelIo(hSer);
    CloseHandle(hSer);
    hSer = INVALID_HANDLE_VALUE;
    if (ovEvt.hEvent) CloseHandle(ovEvt.hEvent);
    if (ovRd.hEvent)  CloseHandle(ovRd.hEvent);
    if (ovWr.hEvent)  CloseHandle(ovWr.hEvent);
    ovEvt.hEvent = ovRd.hEvent = ovWr.hEvent = NULL;
}

/* True if bytes are already queued. Also the cheapest liveness check. */
static BOOL serial_pending(void)
{
    DWORD errs = 0;
    COMSTAT cs;
    if (hSer == INVALID_HANDLE_VALUE) return FALSE;
    memset(&cs, 0, sizeof cs);
    if (!ClearCommError(hSer, &errs, &cs)) return TRUE;  /* let read() report */
    return cs.cbInQue > 0;
}

static bool port_is_gone(DWORD e)
{
    return e == ERROR_ACCESS_DENIED || e == ERROR_DEVICE_REMOVED ||
           e == ERROR_BAD_COMMAND   || e == ERROR_OPERATION_ABORTED ||
           e == ERROR_INVALID_HANDLE || e == ERROR_GEN_FAILURE ||
           e == ERROR_NOT_READY;
}

long st_serial_read(void *buf, size_t n)
{
    DWORD got = 0, errs = 0;
    COMSTAT cs;

    if (hSer == INVALID_HANDLE_VALUE) return ST_HUP;

    /* Clears the sticky framing/overrun flags that would otherwise stall the
     * port, and tells us if the port has gone away. cbInQue is deliberately
     * not used to size the read: the timeouts make ReadFile return whatever
     * is buffered straight away, and not every driver fills COMSTAT in. */
    memset(&cs, 0, sizeof cs);
    if (!ClearCommError(hSer, &errs, &cs))
        return port_is_gone(GetLastError()) ? ST_HUP : ST_ERR;

    ResetEvent(ovRd.hEvent);
    if (!ReadFile(hSer, buf, (DWORD)n, &got, &ovRd)) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING)
            return port_is_gone(e) ? ST_HUP : ST_ERR;
        if (!GetOverlappedResult(hSer, &ovRd, &got, TRUE))
            return port_is_gone(GetLastError()) ? ST_HUP : ST_ERR;
    }
    return (long)got;
}

long st_serial_write(const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t left = n;

    if (hSer == INVALID_HANDLE_VALUE) return ST_ERR;

    while (left) {
        DWORD wrote = 0;
        ResetEvent(ovWr.hEvent);
        if (!WriteFile(hSer, p, (DWORD)left, &wrote, &ovWr)) {
            if (GetLastError() != ERROR_IO_PENDING) return ST_ERR;
            if (!GetOverlappedResult(hSer, &ovWr, &wrote, TRUE)) return ST_ERR;
        }
        if (wrote == 0) return ST_ERR;
        p    += wrote;
        left -= wrote;
    }
    return (long)n;
}

void st_serial_drain(void)
{
    if (hSer != INVALID_HANDLE_VALUE) FlushFileBuffers(hSer);
}

void st_set_poll_mode(bool on)
{
    force_polling = on ? TRUE : FALSE;
    if (on) use_polling = TRUE;
}

void st_serial_break(void)
{
    if (hSer == INVALID_HANDLE_VALUE) return;
    EscapeCommFunction(hSer, SETBREAK);
    Sleep(250);
    EscapeCommFunction(hSer, CLRBREAK);
}

bool st_modem_get(int line)
{
    DWORD st = 0;
    if (line == ST_DTR) return dtr_state;   /* not readable on Windows */
    if (line == ST_RTS) return rts_state;
    if (hSer == INVALID_HANDLE_VALUE) return false;
    if (!GetCommModemStatus(hSer, &st)) return false;
    switch (line) {
        case ST_CTS: return (st & MS_CTS_ON)  != 0;
        case ST_DSR: return (st & MS_DSR_ON)  != 0;
        case ST_DCD: return (st & MS_RLSD_ON) != 0;
        default:     return false;
    }
}

void st_modem_set(int line, bool on)
{
    if (hSer == INVALID_HANDLE_VALUE) return;
    if (line == ST_DTR) {
        EscapeCommFunction(hSer, on ? SETDTR : CLRDTR);
        dtr_state = on;
    } else if (line == ST_RTS) {
        EscapeCommFunction(hSer, on ? SETRTS : CLRRTS);
        rts_state = on;
    }
}

/* ---- console --------------------------------------------------------- */

static DWORD WINAPI stdin_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        DWORD got = 0;
        if (WaitForSingleObject(hInWantEvt, INFINITE) != WAIT_OBJECT_0) break;
        if (!ReadFile(hIn, in_buf, sizeof in_buf, &got, NULL) || got == 0) {
            InterlockedExchange(&in_eof, 1);
            SetEvent(hInDataEvt);
            break;
        }
        in_len = got;
        in_off = 0;
        SetEvent(hInDataEvt);
    }
    return 0;
}

static void console_init(void)
{
    DWORD mode;
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    (void)std_out();
    (void)std_err();

    con_is_tty = GetConsoleMode(hIn, &mode) != 0;
    if (con_is_tty) {
        con_in_saved = mode;
        if (GetConsoleMode(hOut, &mode)) { con_out_saved = mode; con_saved_ok = TRUE; }
        /* Make \033[2J and friends work in cmd.exe. */
        SetConsoleMode(hOut, con_out_saved | ENABLE_PROCESSED_OUTPUT |
                             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    /* The FPGA is likely to be sending UTF-8, and cmd defaults to cp437. */
    SetConsoleOutputCP(CP_UTF8);
}

bool st_con_isatty(void) { return con_is_tty != FALSE; }

void st_con_raw(void)
{
    if (!con_is_tty || raw_active) return;
    /* Clearing ENABLE_PROCESSED_INPUT is what makes Ctrl-C arrive as byte
     * 0x03 for the target instead of killing sterm - the same thing
     * cfmakeraw() does by clearing ISIG. Ctrl-A q is the way out. */
    SetConsoleMode(hIn, ENABLE_VIRTUAL_TERMINAL_INPUT);
    raw_active = TRUE;
}

void st_con_cooked(void)
{
    if (!con_is_tty || !raw_active) return;
    SetConsoleMode(hIn, con_in_saved);
    raw_active = FALSE;
}

long st_con_read(void *buf, size_t n)
{
    DWORD avail;
    if (InterlockedCompareExchange(&in_eof, 0, 0)) return 0;
    avail = in_len - in_off;
    if (avail == 0) return 0;
    if (n > avail) n = avail;
    memcpy(buf, in_buf + in_off, n);
    in_off += (DWORD)n;
    if (in_off >= in_len) {            /* buffer drained, ask for more */
        in_len = in_off = 0;
        ResetEvent(hInDataEvt);
        SetEvent(hInWantEvt);
    }
    return (long)n;
}

void st_write_stdout(const void *buf, size_t n)
{
    HANDLE h = std_out();
    const unsigned char *p = buf;
    DWORD w;
    while (n) {
        if (!WriteFile(h, p, (DWORD)n, &w, NULL) || w == 0) return;
        p += w; n -= w;
    }
}

void st_write_stderr(const void *buf, size_t n)
{
    HANDLE h = std_err();
    const unsigned char *p = buf;
    DWORD w;
    while (n) {
        if (!WriteFile(h, p, (DWORD)n, &w, NULL) || w == 0) return;
        p += w; n -= w;
    }
}

/* ---- event loop ------------------------------------------------------ */

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        if (hQuitEvt) SetEvent(hQuitEvt);
        return TRUE;
    }
    return FALSE;
}

void st_signals_init(void)
{
    hQuitEvt   = CreateEventA(NULL, TRUE,  FALSE, NULL);
    hInDataEvt = CreateEventA(NULL, TRUE,  FALSE, NULL);
    hInWantEvt = CreateEventA(NULL, FALSE, FALSE, NULL);
    console_init();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetEvent(hInWantEvt);                    /* kick the reader off */
    hInThread = CreateThread(NULL, 0, stdin_thread, NULL, 0, NULL);
}

static void arm_comm_wait(void)
{
    if (wait_armed || use_polling || force_polling ||
        hSer == INVALID_HANDLE_VALUE) return;
    comm_mask = 0;
    ResetEvent(ovEvt.hEvent);
    if (WaitCommEvent(hSer, &comm_mask, &ovEvt)) {
        SetEvent(ovEvt.hEvent);              /* completed inline */
        wait_armed = TRUE;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        wait_armed = TRUE;
    } else {
        use_polling = TRUE;                  /* driver cannot do it; poll */
    }
}

int st_wait(int timeout_ms)
{
    HANDLE h[3];
    DWORD n = 0, r;
    int ev = 0;

    if (WaitForSingleObject(hQuitEvt, 0) == WAIT_OBJECT_0) return ST_EV_QUIT;

    /* WaitCommEvent only fires on *new* bytes, so anything already sitting
     * in the driver buffer has to be reported without waiting. */
    if (serial_pending()) ev |= ST_EV_SERIAL;
    if (in_len > in_off || InterlockedCompareExchange(&in_eof, 0, 0))
        ev |= ST_EV_INPUT;
    if (ev) return ev;

    arm_comm_wait();

    if (use_polling) {
        /* Only stdin and quit are waitable; the port gets re-checked each
         * time round. timeout_ms == 0 must still mean "do not block". */
        DWORD slice = (timeout_ms == 0) ? 0 :
                      (timeout_ms < 0 || timeout_ms > POLL_MS) ? POLL_MS
                                                               : (DWORD)timeout_ms;
        h[0] = hInDataEvt;
        h[1] = hQuitEvt;
        r = WaitForMultipleObjects(2, h, FALSE, slice);
        if (r == WAIT_OBJECT_0)          ev |= ST_EV_INPUT;
        else if (r == WAIT_OBJECT_0 + 1) return ST_EV_QUIT;
        /* Always let the reader look. With the immediate-return timeouts we
         * set, ReadFile costs one syscall and yields 0 bytes when the port
         * is quiet - which is cheaper than trusting cbInQue, and works on
         * drivers that do not populate COMSTAT at all. */
        ev |= ST_EV_SERIAL;
        return ev;
    }

    h[n++] = ovEvt.hEvent;
    h[n++] = hInDataEvt;
    h[n++] = hQuitEvt;

    r = WaitForMultipleObjects(n, h, FALSE,
                               timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (r == WAIT_TIMEOUT || r == WAIT_FAILED) return 0;

    switch (r - WAIT_OBJECT_0) {
        case 0: {
            DWORD got;
            wait_armed = FALSE;
            GetOverlappedResult(hSer, &ovEvt, &got, FALSE);
            ev |= ST_EV_SERIAL;      /* read() decides if it is data or a hup */
            break;
        }
        case 1: ev |= ST_EV_INPUT; break;
        case 2: ev |= ST_EV_QUIT;  break;
        default: break;
    }
    return ev;
}

void st_sleep_ms(unsigned ms) { Sleep(ms); }

void st_stamp(char *buf, size_t n)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(buf, n, "[%02d:%02d:%02d.%03d] ",
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

/* ---- port enumeration ------------------------------------------------ */

/* Friendly names matter here: on a Basys 3 the FT2232H shows up twice and
 * only the second channel is the UART. */
void st_list_ports(void)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    char line[512];
    DWORD i;
    int found = 0;

    set = SetupDiGetClassDevsA(&guid_ports, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        st_write_stdout("no serial ports found\r\n", 23);
        return;
    }

    dev.cbSize = sizeof dev;
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char name[64] = "", desc[256] = "";
        DWORD len = sizeof name;
        HKEY k = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0,
                                      DIREG_DEV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            RegQueryValueExA(k, "PortName", NULL, NULL, (LPBYTE)name, &len);
            RegCloseKey(k);
        }
        if (!name[0] || _strnicmp(name, "COM", 3) != 0) continue;

        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME,
                                               NULL, (PBYTE)desc, sizeof desc, NULL))
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC,
                                              NULL, (PBYTE)desc, sizeof desc, NULL);

        {
            int ln = snprintf(line, sizeof line, "  %-8s %s\r\n", name, desc);
            if (ln > 0) st_write_stdout(line, (size_t)ln);
        }
        found++;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) {
        st_write_stdout("no serial ports found\r\n", 23);
    } else {
        static const char note[] =
            "\r\nOn a Basys 3 / Arty the FT2232H exposes two channels; the UART\r\n"
            "is the one Vivado is not using for JTAG - usually the higher COM.\r\n";
        st_write_stdout(note, sizeof note - 1);
    }
}

const char *st_port_example(void) { return "sterm COM5 -b 115200 -t"; }

#endif /* _WIN32 */
