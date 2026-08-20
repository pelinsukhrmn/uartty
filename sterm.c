/* sterm - lightweight serial terminal (POSIX termios). Ctrl-A h for help. */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define STERM_VERSION "1.0"
#define RXBUF 4096

int st_set_custom_baud(int fd, unsigned int baud);   /* custom_baud.c */

enum eol { EOL_CR, EOL_LF, EOL_CRLF };

static struct {
    const char *port;
    unsigned baud;
    int databits;
    int stopbits;
    char parity;
    bool rtscts;
    bool xonxoff;
    bool echo;
    bool hex;
    bool stamp;
    bool rx_crlf;
    enum eol eol;
    int esc;
    unsigned tx_delay;
    const char *logpath;
    bool quiet;
} cfg = {
    .baud = 115200, .databits = 8, .stopbits = 1, .parity = 'n',
    .eol = EOL_CR, .esc = 0x01, .rx_crlf = true,
};

static int serfd = -1;
static FILE *logfp = NULL;
static bool stdin_is_tty = false;
static bool raw_active = false;
static struct termios tty_saved, ser_saved;
static bool ser_saved_ok = false;
static volatile sig_atomic_t stop_flag = 0;

static void msg(const char *fmt, ...)
{
    va_list ap;
    if (cfg.quiet) return;
    fputs("\r\n", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputs("\r\n", stdout);
    fflush(stdout);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sterm: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static ssize_t write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                poll(&pf, 1, 1000);
                continue;
            }
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
}

static const struct { unsigned val; speed_t code; } baudtab[] = {
    {   1200, B1200   }, {   2400, B2400   }, {   4800, B4800   },
    {   9600, B9600   }, {  19200, B19200  }, {  38400, B38400  },
    {  57600, B57600  }, { 115200, B115200 }, { 230400, B230400 },
#ifdef B460800
    { 460800, B460800 },
#endif
#ifdef B500000
    { 500000, B500000 },
#endif
#ifdef B921600
    { 921600, B921600 },
#endif
#ifdef B1000000
    {1000000, B1000000},
#endif
#ifdef B1500000
    {1500000, B1500000},
#endif
#ifdef B2000000
    {2000000, B2000000},
#endif
#ifdef B3000000
    {3000000, B3000000},
#endif
};

static bool baud_lookup(unsigned v, speed_t *out)
{
    for (size_t i = 0; i < sizeof baudtab / sizeof baudtab[0]; i++)
        if (baudtab[i].val == v) { *out = baudtab[i].code; return true; }
    return false;
}

static int serial_open(void)
{
    struct termios t;
    speed_t code;
    int fd = open(cfg.port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) die("open %s: %s", cfg.port, strerror(errno));

    if (!isatty(fd)) die("%s is not a tty", cfg.port);

    ioctl(fd, TIOCEXCL);   /* keep a second terminal from stealing the port */

    if (tcgetattr(fd, &t) < 0) die("tcgetattr: %s", strerror(errno));
    ser_saved = t;
    ser_saved_ok = true;

    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;

    t.c_cflag &= ~CSIZE;
    switch (cfg.databits) {
        case 5: t.c_cflag |= CS5; break;
        case 6: t.c_cflag |= CS6; break;
        case 7: t.c_cflag |= CS7; break;
        default: t.c_cflag |= CS8; break;
    }

    t.c_cflag &= ~(PARENB | PARODD);
    if (cfg.parity == 'e') t.c_cflag |= PARENB;
    else if (cfg.parity == 'o') t.c_cflag |= PARENB | PARODD;

    if (cfg.stopbits == 2) t.c_cflag |= CSTOPB;
    else t.c_cflag &= ~CSTOPB;

#ifdef CRTSCTS
    if (cfg.rtscts) t.c_cflag |= CRTSCTS; else t.c_cflag &= ~CRTSCTS;
#endif
    if (cfg.xonxoff) t.c_iflag |= IXON | IXOFF; else t.c_iflag &= ~(IXON | IXOFF | IXANY);

    t.c_cc[VMIN] = 0;      /* poll() drives the loop, never block in read() */
    t.c_cc[VTIME] = 0;

    if (baud_lookup(cfg.baud, &code)) {
        cfsetispeed(&t, code);
        cfsetospeed(&t, code);
        if (tcsetattr(fd, TCSANOW, &t) < 0) die("tcsetattr: %s", strerror(errno));
    } else {
        /* non-standard rate: park at B38400, then set it via TCSETS2 */
        cfsetispeed(&t, B38400);
        cfsetospeed(&t, B38400);
        if (tcsetattr(fd, TCSANOW, &t) < 0) die("tcsetattr: %s", strerror(errno));
        if (st_set_custom_baud(fd, cfg.baud) < 0)
            die("baud %u not supported: %s", cfg.baud, strerror(errno));
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void serial_restore(void)
{
    if (serfd >= 0 && ser_saved_ok)
        tcsetattr(serfd, TCSAFLUSH, &ser_saved);
}

static bool modem_get(int bit)
{
    int st = 0;
    if (ioctl(serfd, TIOCMGET, &st) < 0) return false;
    return (st & bit) != 0;
}

static void modem_set(int bit, bool on)
{
    int b = bit;
    ioctl(serfd, on ? TIOCMBIS : TIOCMBIC, &b);
}

static void tty_raw(void)
{
    struct termios t;
    if (!stdin_is_tty || raw_active) return;
    t = tty_saved;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    raw_active = true;
}

static void tty_cooked(void)
{
    if (!stdin_is_tty || !raw_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tty_saved);
    raw_active = false;
}

static void cleanup(void)
{
    tty_cooked();
    serial_restore();
    if (logfp) { fclose(logfp); logfp = NULL; }
}

static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static unsigned char hexbuf[16];
static size_t hexlen = 0;
static unsigned long hexoff = 0;
static bool at_line_start = true;

static void put(const char *s) { write_all(STDOUT_FILENO, s, strlen(s)); }

static void hex_flush(void)
{
    char line[128];
    int n = 0;
    if (!hexlen) return;
    n += snprintf(line + n, sizeof line - n, "%08lx  ", hexoff);
    for (size_t i = 0; i < 16; i++) {
        if (i < hexlen) n += snprintf(line + n, sizeof line - n, "%02x ", hexbuf[i]);
        else            n += snprintf(line + n, sizeof line - n, "   ");
        if (i == 7) n += snprintf(line + n, sizeof line - n, " ");
    }
    n += snprintf(line + n, sizeof line - n, " |");
    for (size_t i = 0; i < hexlen; i++)
        n += snprintf(line + n, sizeof line - n, "%c",
                      isprint(hexbuf[i]) ? hexbuf[i] : '.');
    n += snprintf(line + n, sizeof line - n, "|\r\n");
    write_all(STDOUT_FILENO, line, (size_t)n);
    hexoff += hexlen;
    hexlen = 0;
}

static void stamp_print(void)
{
    struct timespec ts;
    struct tm tm;
    char buf[32];
    int n;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03ld] ",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    write_all(STDOUT_FILENO, buf, (size_t)n);
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
    for (size_t i = 0; i < n; i++) {
        if (cfg.stamp && at_line_start && b[i] != '\n' && b[i] != '\r') {
            stamp_print();
            at_line_start = false;
        }
        write_all(STDOUT_FILENO, &b[i], 1);
        if (b[i] == '\r') {
            if (cfg.rx_crlf && !(i + 1 < n && b[i + 1] == '\n')) put("\n");
            at_line_start = true;
        } else if (b[i] == '\n') {
            at_line_start = true;
        }
    }
}

static void show_help(void)
{
    char k = (char)('A' + cfg.esc - 1);
    msg("sterm %s  %s @ %u %d%c%d%s",
        STERM_VERSION, cfg.port, cfg.baud, cfg.databits,
        (char)toupper((unsigned char)cfg.parity), cfg.stopbits,
        cfg.rtscts ? " rtscts" : "");
    printf("  Ctrl-%c q   quit\r\n", k);
    printf("  Ctrl-%c h   this help\r\n", k);
    printf("  Ctrl-%c c   clear screen\r\n", k);
    printf("  Ctrl-%c e   local echo      (now %s)\r\n", k, cfg.echo ? "on" : "off");
    printf("  Ctrl-%c x   hex dump        (now %s)\r\n", k, cfg.hex ? "on" : "off");
    printf("  Ctrl-%c t   timestamps      (now %s)\r\n", k, cfg.stamp ? "on" : "off");
    printf("  Ctrl-%c b   send BREAK\r\n", k);
    printf("  Ctrl-%c d   toggle DTR      (now %s)\r\n", k, modem_get(TIOCM_DTR) ? "1" : "0");
    printf("  Ctrl-%c r   toggle RTS      (now %s)\r\n", k, modem_get(TIOCM_RTS) ? "1" : "0");
    printf("  Ctrl-%c s   send a file\r\n", k);
    printf("  Ctrl-%c l   logging         (now %s)\r\n", k, logfp ? "on" : "off");
    printf("  Ctrl-%c Ctrl-%c  send a literal 0x%02x\r\n", k, k, cfg.esc);
    fflush(stdout);
}

static bool prompt_line(const char *label, char *buf, size_t n)
{
    ssize_t r;
    tty_cooked();
    printf("\r\n%s", label);
    fflush(stdout);
    r = read(STDIN_FILENO, buf, n - 1);
    tty_raw();
    if (r <= 0) return false;
    buf[r] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] != '\0';
}

static void send_file(void)
{
    char path[512];
    unsigned char chunk[256];
    unsigned long total = 0;
    FILE *f;
    size_t r;

    if (!stdin_is_tty) return;
    if (!prompt_line("file to send: ", path, sizeof path)) { msg("cancelled"); return; }

    f = fopen(path, "rb");
    if (!f) { msg("cannot open %s: %s", path, strerror(errno)); return; }

    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (write_all(serfd, chunk, r) < 0) { msg("write failed: %s", strerror(errno)); break; }
        total += r;
        if (cfg.tx_delay) {
            tcdrain(serfd);
            usleep(cfg.tx_delay * 1000);
        }
    }
    fclose(f);
    tcdrain(serfd);
    msg("sent %lu bytes", total);
}

static void toggle_log(void)
{
    char path[512];
    if (logfp) { fclose(logfp); logfp = NULL; msg("logging off"); return; }
    if (!stdin_is_tty) return;
    if (!prompt_line("log file: ", path, sizeof path)) { msg("cancelled"); return; }
    logfp = fopen(path, "ab");
    if (!logfp) msg("cannot open %s: %s", path, strerror(errno));
    else msg("logging to %s", path);
}

static bool esc_command(unsigned char c)
{
    if (c == (unsigned char)cfg.esc) {
        write_all(serfd, &c, 1);
        return true;
    }
    switch (c) {
        case 'q': case 'Q': return false;
        case 'h': case 'H': case '?': show_help(); break;
        case 'c': case 'C': put("\033[2J\033[H"); break;
        case 'e': cfg.echo = !cfg.echo;   msg("local echo %s", cfg.echo ? "on" : "off"); break;
        case 'x': hex_flush(); cfg.hex = !cfg.hex;
                  msg("hex dump %s", cfg.hex ? "on" : "off"); break;
        case 't': cfg.stamp = !cfg.stamp; msg("timestamps %s", cfg.stamp ? "on" : "off"); break;
        case 'b': tcsendbreak(serfd, 0);  msg("BREAK sent"); break;
        case 'd': { bool v = !modem_get(TIOCM_DTR);
                    modem_set(TIOCM_DTR, v); msg("DTR = %d", v); break; }
        case 'r': { bool v = !modem_get(TIOCM_RTS);
                    modem_set(TIOCM_RTS, v); msg("RTS = %d", v); break; }
        case 's': send_file(); break;
        case 'l': toggle_log(); break;
        default: break;
    }
    return true;
}

static bool handle_stdin(void)
{
    static bool esc_pending = false;
    unsigned char buf[512];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);

    if (n == 0) return false;
    if (n < 0) return errno == EAGAIN || errno == EINTR;

    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];

        if (esc_pending) {
            esc_pending = false;
            if (!esc_command(c)) return false;
            continue;
        }
        if (stdin_is_tty && c == (unsigned char)cfg.esc) { esc_pending = true; continue; }

        if (c == '\r' || c == '\n') {
            static const char cr[] = "\r", lf[] = "\n", crlf[] = "\r\n";
            const char *s = cfg.eol == EOL_CR ? cr : cfg.eol == EOL_LF ? lf : crlf;
            write_all(serfd, s, strlen(s));
            if (cfg.echo) put("\r\n");
        } else {
            write_all(serfd, &c, 1);
            if (cfg.echo) write_all(STDOUT_FILENO, &c, 1);
        }
    }
    return true;
}

static bool handle_serial(void)
{
    unsigned char buf[RXBUF];
    ssize_t n = read(serfd, buf, sizeof buf);

    if (n == 0) return true;
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return true;
        msg("read error: %s", strerror(errno));
        return false;
    }
    rx_render(buf, (size_t)n);
    if (logfp) { fwrite(buf, 1, (size_t)n, logfp); fflush(logfp); }
    return true;
}

static int run(void)
{
    struct pollfd pfd[2];
    int nfd;

    for (;;) {
        pfd[0].fd = serfd;        pfd[0].events = POLLIN;  pfd[0].revents = 0;
        pfd[1].fd = STDIN_FILENO; pfd[1].events = POLLIN;  pfd[1].revents = 0;
        nfd = 2;

        if (poll(pfd, (nfds_t)nfd, -1) < 0) {
            if (errno == EINTR) { if (stop_flag) break; continue; }
            msg("poll: %s", strerror(errno));
            break;
        }
        if (stop_flag) break;

        if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pfd[0].revents & (POLLHUP | POLLERR)) { msg("port closed"); break; }
            if (!handle_serial()) break;
        }
        if (pfd[1].revents & (POLLIN | POLLHUP)) {
            if (!handle_stdin()) break;
        }
    }
    hex_flush();
    return 0;
}

static void list_ports(void)
{
    static const char *pfx[] = { "ttyUSB", "ttyACM", "ttyS", "ttyAMA", NULL };
    struct dirent *de;
    DIR *d;

    d = opendir("/dev/serial/by-id");
    if (d) {
        printf("/dev/serial/by-id:\n");
        while ((de = readdir(d)))
            if (de->d_name[0] != '.') printf("  %s\n", de->d_name);
        closedir(d);
    }
    d = opendir("/dev");
    if (!d) return;
    printf("/dev:\n");
    while ((de = readdir(d)))
        for (int i = 0; pfx[i]; i++)
            if (strncmp(de->d_name, pfx[i], strlen(pfx[i])) == 0 &&
                isdigit((unsigned char)de->d_name[strlen(pfx[i])])) {
                printf("  /dev/%s\n", de->d_name);
                break;
            }
    closedir(d);
}

static void usage(FILE *out)
{
    fprintf(out,
"sterm %s - lightweight UART terminal (POSIX termios)\n"
"\n"
"usage: sterm [options] <device>\n"
"\n"
"  -b, --baud N        baud rate (default 115200, arbitrary rates on Linux)\n"
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
"  -L, --list          list available serial devices\n"
"  -q, --quiet         suppress status messages\n"
"  -h, --help          this help\n"
"\n"
"example: sterm /dev/ttyUSB1 -b 115200 -t\n", STERM_VERSION);
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
        { "list",      no_argument,       0, 'L' },
        { "quiet",     no_argument,       0, 'q' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;

    while ((c = getopt_long(argc, argv, "b:d:p:s:f:extg:Lqh", lo, NULL)) != -1) {
        switch (c) {
            case 'b': cfg.baud = (unsigned)strtoul(optarg, NULL, 10); break;
            case 'd': cfg.databits = atoi(optarg); break;
            case 'p': cfg.parity = (char)tolower((unsigned char)optarg[0]); break;
            case 's': cfg.stopbits = atoi(optarg); break;
            case 'f':
                cfg.rtscts  = optarg[0] == 'h';
                cfg.xonxoff = optarg[0] == 's';
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
            case 3: cfg.tx_delay = (unsigned)strtoul(optarg, NULL, 10); break;
            case 4:
                if (!isalpha((unsigned char)optarg[0])) die("--escape needs a letter");
                cfg.esc = tolower((unsigned char)optarg[0]) - 'a' + 1;
                break;
            case 'L': list_ports(); return 0;
            case 'q': cfg.quiet = true; break;
            case 'h': usage(stdout); return 0;
            default: usage(stderr); return 2;
        }
    }

    if (optind >= argc) { usage(stderr); return 2; }
    cfg.port = argv[optind];

    if (cfg.databits < 5 || cfg.databits > 8) die("databits must be 5..8");
    if (cfg.stopbits != 1 && cfg.stopbits != 2) die("stopbits must be 1 or 2");
    if (!strchr("neo", cfg.parity)) die("parity must be n, e or o");

    serfd = serial_open();

    if (cfg.logpath) {
        logfp = fopen(cfg.logpath, "ab");
        if (!logfp) die("cannot open %s: %s", cfg.logpath, strerror(errno));
    }

    stdin_is_tty = isatty(STDIN_FILENO);
    if (stdin_is_tty && tcgetattr(STDIN_FILENO, &tty_saved) == 0) tty_raw();

    atexit(cleanup);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!cfg.quiet) {
        msg("sterm %s  %s @ %u %d%c%d - Ctrl-%c h for help, Ctrl-%c q to quit",
            STERM_VERSION, cfg.port, cfg.baud, cfg.databits,
            (char)toupper((unsigned char)cfg.parity), cfg.stopbits,
            'A' + cfg.esc - 1, 'A' + cfg.esc - 1);
    }

    run();

    cleanup();
    if (!cfg.quiet) fputs("\r\nsterm: bye\r\n", stdout);
    return 0;
}
