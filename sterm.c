/* sterm - lightweight serial terminal. Ctrl-A h for help.
 *
 * Platform-neutral core. Everything OS specific is behind plat.h and lives
 * in plat_posix.c or plat_win32.c.
 */

#include <ctype.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

#define STERM_VERSION "1.1"
#define RXBUF  4096
#define OUTBUF 16384

enum eol { EOL_CR, EOL_LF, EOL_CRLF };

static struct {
    const char *port;
    st_params   ser;
    bool echo, hex, stamp, rx_crlf, quiet;
    enum eol eol;
    int  esc;
    unsigned tx_delay;
    bool poll_mode;
    const char *logpath;
} cfg = {
    .ser = { .baud = 115200, .databits = 8, .stopbits = 1, .parity = 'n' },
    .eol = EOL_CR, .esc = 0x01, .rx_crlf = true,
};

static FILE *logfp = NULL;
static bool  running = true;

/* ---- buffered output -------------------------------------------------
 * The old version mixed printf() with raw write() on fd 1, so status text
 * and UART data could come out in the wrong order, and it rendered RX at
 * one write() syscall per byte. Everything now funnels through here. */

static char   obuf[OUTBUF];
static size_t olen = 0;

static void out_flush(void)
{
    if (!olen) return;
    st_write_stdout(obuf, olen);
    olen = 0;
}

static void out(const void *p, size_t n)
{
    if (n >= sizeof obuf) { out_flush(); st_write_stdout(p, n); return; }
    if (olen + n > sizeof obuf) out_flush();
    memcpy(obuf + olen, p, n);
    olen += n;
}

static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    out(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static void msg(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    int n;
    if (cfg.quiet) return;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    outs("\r\n");
    out(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    outs("\r\n");
    out_flush();
}

static void die(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    st_write_stderr("sterm: ", 7);
    if (n > 0) st_write_stderr(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    st_write_stderr("\n", 1);
    st_con_cooked();
    st_serial_close();
    exit(1);
}

/* ---- RX rendering ---------------------------------------------------- */

static unsigned char hexbuf[16];
static size_t hexlen = 0;
static unsigned long hexoff = 0;
static bool at_line_start = true;

static void hex_flush(void)
{
    if (!hexlen) return;
    outf("%08lx  ", hexoff);
    for (size_t i = 0; i < 16; i++) {
        if (i < hexlen) outf("%02x ", hexbuf[i]);
        else            outs("   ");
        if (i == 7) outs(" ");
    }
    outs(" |");
    for (size_t i = 0; i < hexlen; i++) {
        char c = isprint(hexbuf[i]) ? (char)hexbuf[i] : '.';
        out(&c, 1);
    }
    outs("|\r\n");
    hexoff += hexlen;
    hexlen = 0;
}

static void rx_render(const unsigned char *b, size_t n)
{
    if (cfg.hex) {
        for (size_t i = 0; i < n; i++) {
            hexbuf[hexlen++] = b[i];
            if (hexlen == 16) hex_flush();
        }
        return;
    }
    /* Copy runs of ordinary bytes in one go instead of one at a time. */
    for (size_t i = 0; i < n; ) {
        size_t j;
        if (cfg.stamp && at_line_start && b[i] != '\n' && b[i] != '\r') {
            char ts[32];
            st_stamp(ts, sizeof ts);
            outs(ts);
            at_line_start = false;
        }
        j = i;
        while (j < n && b[j] != '\r' && b[j] != '\n') j++;
        if (j > i) { out(b + i, j - i); i = j; continue; }

        out(&b[i], 1);
        if (b[i] == '\r' && cfg.rx_crlf && !(i + 1 < n && b[i + 1] == '\n'))
            outs("\n");
        at_line_start = true;
        i++;
    }
}

static bool pump_serial(void)
{
    unsigned char buf[RXBUF];
    long n = st_serial_read(buf, sizeof buf);

    if (n == ST_HUP) { msg("port closed"); return false; }
    if (n == ST_ERR) { msg("read error: %s", st_errmsg()); return false; }
    if (n <= 0) return true;

    rx_render(buf, (size_t)n);
    if (logfp) { fwrite(buf, 1, (size_t)n, logfp); fflush(logfp); }
    return true;
}

/* ---- escape commands -------------------------------------------------- */

static void show_help(void)
{
    char k = (char)('A' + cfg.esc - 1);
    msg("sterm %s  %s @ %u %d%c%d%s", STERM_VERSION, cfg.port, cfg.ser.baud,
        cfg.ser.databits, (char)toupper((unsigned char)cfg.ser.parity),
        cfg.ser.stopbits, cfg.ser.rtscts ? " rtscts" : "");
    outf("  Ctrl-%c q   quit\r\n", k);
    outf("  Ctrl-%c h   this help\r\n", k);
    outf("  Ctrl-%c c   clear screen\r\n", k);
    outf("  Ctrl-%c e   local echo      (now %s)\r\n", k, cfg.echo ? "on" : "off");
    outf("  Ctrl-%c x   hex dump        (now %s)\r\n", k, cfg.hex ? "on" : "off");
    outf("  Ctrl-%c t   timestamps      (now %s)\r\n", k, cfg.stamp ? "on" : "off");
    outf("  Ctrl-%c b   send BREAK\r\n", k);
    outf("  Ctrl-%c d   toggle DTR      (now %d)\r\n", k, st_modem_get(ST_DTR));
    outf("  Ctrl-%c r   toggle RTS      (now %d)\r\n", k, st_modem_get(ST_RTS));
    outf("  Ctrl-%c m   modem lines\r\n", k);
    outf("  Ctrl-%c s   send a file\r\n", k);
    outf("  Ctrl-%c l   logging         (now %s)\r\n", k, logfp ? "on" : "off");
    outf("  Ctrl-%c Ctrl-%c  send a literal 0x%02x\r\n", k, k, cfg.esc);
    out_flush();
}

static bool prompt_line(const char *label, char *buf, size_t n)
{
    long r;
    st_con_cooked();
    outs("\r\n");
    outs(label);
    out_flush();
    r = st_con_read(buf, n - 1);
    st_con_raw();
    if (r <= 0) return false;
    buf[r] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] != '\0';
}

/* Sending a file used to block the whole loop: no RX, and Ctrl-A q was
 * dead until the transfer finished. Now the event loop is pumped between
 * chunks so the board's output still shows up. */
static void send_file(void)
{
    char path[512];
    unsigned char chunk[256];
    unsigned long total = 0;
    FILE *f;
    size_t r;

    if (!st_con_isatty()) return;
    if (!prompt_line("file to send: ", path, sizeof path)) { msg("cancelled"); return; }

    f = fopen(path, "rb");
    if (!f) { msg("cannot open %s", path); return; }

    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
        int ev;
        if (st_serial_write(chunk, r) == ST_ERR) { msg("write failed"); break; }
        total += r;
        if (cfg.tx_delay) {
            st_serial_drain();
            st_sleep_ms(cfg.tx_delay);
        }
        ev = st_wait(0);
        if (ev & ST_EV_SERIAL) { pump_serial(); out_flush(); }
        if (ev & ST_EV_QUIT)   { running = false; break; }
    }
    fclose(f);
    st_serial_drain();
    msg("sent %lu bytes", total);
}

static void toggle_log(void)
{
    char path[512];
    if (logfp) { fclose(logfp); logfp = NULL; msg("logging off"); return; }
    if (!st_con_isatty()) return;
    if (!prompt_line("log file: ", path, sizeof path)) { msg("cancelled"); return; }
    logfp = fopen(path, "ab");
    if (!logfp) msg("cannot open %s", path);
    else msg("logging to %s", path);
}

static bool esc_command(unsigned char c)
{
    if (c == (unsigned char)cfg.esc) { st_serial_write(&c, 1); return true; }

    switch (c) {
        case 'q': case 'Q': return false;
        case 'h': case 'H': case '?': show_help(); break;
        case 'c': case 'C': outs("\033[2J\033[H"); out_flush(); break;
        case 'e': cfg.echo  = !cfg.echo;  msg("local echo %s", cfg.echo ? "on" : "off"); break;
        case 'x': hex_flush(); cfg.hex = !cfg.hex;
                  msg("hex dump %s", cfg.hex ? "on" : "off"); break;
        case 't': cfg.stamp = !cfg.stamp; msg("timestamps %s", cfg.stamp ? "on" : "off"); break;
        case 'b': st_serial_break(); msg("BREAK sent"); break;
        case 'd': { bool v = !st_modem_get(ST_DTR);
                    st_modem_set(ST_DTR, v); msg("DTR = %d", v); break; }
        case 'r': { bool v = !st_modem_get(ST_RTS);
                    st_modem_set(ST_RTS, v); msg("RTS = %d", v); break; }
        case 'm': msg("DTR=%d RTS=%d  CTS=%d DSR=%d DCD=%d",
                      st_modem_get(ST_DTR), st_modem_get(ST_RTS),
                      st_modem_get(ST_CTS), st_modem_get(ST_DSR),
                      st_modem_get(ST_DCD)); break;
        case 's': send_file(); break;
        case 'l': toggle_log(); break;
        default: break;
    }
    return true;
}

static bool pump_input(void)
{
    static bool esc_pending = false;
    unsigned char buf[512], txbuf[1024];
    size_t tx = 0;
    long n = st_con_read(buf, sizeof buf);

    if (n == 0) return false;              /* EOF on stdin */
    if (n < 0) return n == -3;             /* -3 = would block, keep going */

    for (long i = 0; i < n; i++) {
        unsigned char c = buf[i];

        if (esc_pending) {
            esc_pending = false;
            if (tx) { st_serial_write(txbuf, tx); tx = 0; }
            if (!esc_command(c)) return false;
            continue;
        }
        if (st_con_isatty() && c == (unsigned char)cfg.esc) { esc_pending = true; continue; }

        if (c == '\r' || c == '\n') {
            const char *s = cfg.eol == EOL_CR ? "\r" : cfg.eol == EOL_LF ? "\n" : "\r\n";
            size_t sl = strlen(s);
            if (tx + sl > sizeof txbuf) { st_serial_write(txbuf, tx); tx = 0; }
            memcpy(txbuf + tx, s, sl);
            tx += sl;
            if (cfg.echo) outs("\r\n");
        } else {
            if (tx + 1 > sizeof txbuf) { st_serial_write(txbuf, tx); tx = 0; }
            txbuf[tx++] = c;
            if (cfg.echo) out(&c, 1);
        }
    }
    if (tx) st_serial_write(txbuf, tx);
    if (cfg.echo) out_flush();
    return true;
}

static void run(void)
{
    while (running) {
        int ev = st_wait(-1);
        if (ev & ST_EV_QUIT) break;
        if (ev & ST_EV_SERIAL) { if (!pump_serial()) break; }
        if (ev & ST_EV_INPUT)  { if (!pump_input())  break; }
        out_flush();
    }
    hex_flush();
    out_flush();
}

/* ---- argument parsing ------------------------------------------------- */

static unsigned parse_uint(const char *s, const char *what, unsigned lo, unsigned hi)
{
    char *end;
    unsigned long v;
    if (!s || !*s) die("%s needs a number", what);
    v = strtoul(s, &end, 10);
    if (*end || v < lo || v > hi) die("%s must be %u..%u, got \"%s\"", what, lo, hi, s);
    return (unsigned)v;
}

static void usage(bool to_stderr)
{
    char b[2048];
    int n = snprintf(b, sizeof b,
"sterm %s - lightweight UART terminal\n"
"\n"
"usage: sterm [options] <device>\n"
"\n"
"  -b, --baud N        baud rate (default 115200)\n"
"  -d, --databits N    5..8 (default 8)\n"
"  -p, --parity n|e|o  parity (default n)\n"
"  -s, --stopbits N    1 or 2 (default 1)\n"
"  -f, --flow n|h|s    none / RTS-CTS / XON-XOFF (default n)\n"
"  -e, --echo          local echo\n"
"  -x, --hex           hex dump received bytes\n"
"  -t, --timestamp     prefix each received line with a timestamp\n"
"      --eol cr|lf|crlf  what Enter transmits (default cr)\n"
"      --no-crlf       do not add LF after a received bare CR\n"
"  -g, --log FILE      append raw received bytes to FILE\n"
"      --tx-delay MS   pause per 256-byte block when sending a file\n"
"      --escape CHAR   escape key, a..z (default a = Ctrl-A)\n"
"      --poll          poll the port instead of waiting on a driver event\n"
"                      (Windows: use if a virtual COM driver misbehaves)\n"
"  -L, --list          list available serial devices\n"
"  -q, --quiet         suppress status messages\n"
"  -h, --help          this help\n"
"\n"
"example: %s\n", STERM_VERSION, st_port_example());
    if (n <= 0) return;
    if (to_stderr) st_write_stderr(b, (size_t)n);
    else           st_write_stdout(b, (size_t)n);
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "baud",      required_argument, 0, 'b' },
        { "databits",  required_argument, 0, 'd' },
        { "parity",    required_argument, 0, 'p' },
        { "stopbits",  required_argument, 0, 's' },
        { "flow",      required_argument, 0, 'f' },
        { "echo",      no_argument,       0, 'e' },
        { "hex",       no_argument,       0, 'x' },
        { "timestamp", no_argument,       0, 't' },
        { "log",       required_argument, 0, 'g' },
        { "eol",       required_argument, 0,  1  },
        { "no-crlf",   no_argument,       0,  2  },
        { "tx-delay",  required_argument, 0,  3  },
        { "escape",    required_argument, 0,  4  },
        { "poll",      no_argument,       0,  5  },
        { "list",      no_argument,       0, 'L' },
        { "quiet",     no_argument,       0, 'q' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    char err[512];
    char k;
    int c;

    while ((c = getopt_long(argc, argv, "b:d:p:s:f:extg:Lqh", lo, NULL)) != -1) {
        switch (c) {
            /* the old code used strtoul/atoi with no error check, so
             * "-b abc" silently became 0 baud and hung up the line */
            case 'b': cfg.ser.baud     = parse_uint(optarg, "--baud", 50, 12000000); break;
            case 'd': cfg.ser.databits = (int)parse_uint(optarg, "--databits", 5, 8); break;
            case 's': cfg.ser.stopbits = (int)parse_uint(optarg, "--stopbits", 1, 2); break;
            case 'p':
                cfg.ser.parity = (char)tolower((unsigned char)optarg[0]);
                if (!strchr("neo", cfg.ser.parity) || optarg[1])
                    die("--parity must be n, e or o");
                break;
            case 'f':
                if (optarg[1] || !strchr("nhs", optarg[0]))
                    die("--flow must be n, h or s");
                cfg.ser.rtscts  = optarg[0] == 'h';
                cfg.ser.xonxoff = optarg[0] == 's';
                break;
            case 'e': cfg.echo = true; break;
            case 'x': cfg.hex = true; break;
            case 't': cfg.stamp = true; break;
            case 'g': cfg.logpath = optarg; break;
            case 1:
                if (!strcmp(optarg, "cr")) cfg.eol = EOL_CR;
                else if (!strcmp(optarg, "lf")) cfg.eol = EOL_LF;
                else if (!strcmp(optarg, "crlf")) cfg.eol = EOL_CRLF;
                else die("--eol must be cr, lf or crlf");
                break;
            case 2: cfg.rx_crlf = false; break;
            case 3: cfg.tx_delay = parse_uint(optarg, "--tx-delay", 0, 60000); break;
            case 4:
                if (!isalpha((unsigned char)optarg[0]) || optarg[1])
                    die("--escape needs a single letter a..z");
                cfg.esc = tolower((unsigned char)optarg[0]) - 'a' + 1;
                break;
            case 5: cfg.poll_mode = true; break;
            case 'L': st_list_ports(); return 0;
            case 'q': cfg.quiet = true; break;
            case 'h': usage(false); return 0;
            default: usage(true); return 2;
        }
    }

    if (optind >= argc) { usage(true); return 2; }
    cfg.port = argv[optind];

    st_set_poll_mode(cfg.poll_mode);
    if (!st_serial_open(cfg.port, &cfg.ser, err, sizeof err)) die("%s", err);

    if (cfg.logpath) {
        logfp = fopen(cfg.logpath, "ab");
        if (!logfp) die("cannot open %s", cfg.logpath);
    }

    st_signals_init();
    st_con_raw();

    k = (char)('A' + cfg.esc - 1);
    if (!cfg.quiet)
        msg("sterm %s  %s @ %u %d%c%d - Ctrl-%c h for help, Ctrl-%c q to quit",
            STERM_VERSION, cfg.port, cfg.ser.baud, cfg.ser.databits,
            (char)toupper((unsigned char)cfg.ser.parity), cfg.ser.stopbits, k, k);

    run();

    st_con_cooked();
    st_serial_close();
    if (logfp) fclose(logfp);
    if (!cfg.quiet) st_write_stdout("\r\nsterm: bye\r\n", 13);
    return 0;
}
