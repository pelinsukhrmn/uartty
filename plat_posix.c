/* plat_posix.c - POSIX termios backend for sterm. */

#ifndef _WIN32

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "plat.h"

int st_set_custom_baud(int fd, unsigned int baud);   /* custom_baud.c */

static int serfd = -1;
static struct termios ser_saved, tty_saved;
static bool ser_saved_ok = false, tty_saved_ok = false;
static bool stdin_tty = false, raw_active = false;
static volatile sig_atomic_t quit_flag = 0;
static int wakefd[2] = { -1, -1 };   /* self-pipe: closes the signal race */

const char *st_errmsg(void) { return strerror(errno); }

/* ---- serial ---------------------------------------------------------- */

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

bool st_serial_open(const char *port, const st_params *p, char *err, size_t errlen)
{
    struct termios t;
    speed_t code;
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) {
        if (errno == EACCES)
            snprintf(err, errlen, "%s: permission denied - "
                     "sudo usermod -aG dialout $USER, then log out and back in", port);
        else
            snprintf(err, errlen, "open %s: %s", port, strerror(errno));
        return false;
    }
    if (!isatty(fd)) {
        snprintf(err, errlen, "%s is not a tty", port);
        close(fd);
        return false;
    }

    ioctl(fd, TIOCEXCL);   /* keep a second terminal from stealing the port */

    if (tcgetattr(fd, &t) < 0) {
        snprintf(err, errlen, "tcgetattr: %s", strerror(errno));
        close(fd);
        return false;
    }
    ser_saved = t;
    ser_saved_ok = true;

    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;

    t.c_cflag &= ~CSIZE;
    switch (p->databits) {
        case 5: t.c_cflag |= CS5; break;
        case 6: t.c_cflag |= CS6; break;
        case 7: t.c_cflag |= CS7; break;
        default: t.c_cflag |= CS8; break;
    }

    t.c_cflag &= ~(PARENB | PARODD);
    if (p->parity == 'e') t.c_cflag |= PARENB;
    else if (p->parity == 'o') t.c_cflag |= PARENB | PARODD;

    if (p->stopbits == 2) t.c_cflag |= CSTOPB; else t.c_cflag &= ~CSTOPB;

#ifdef CRTSCTS
    if (p->rtscts) t.c_cflag |= CRTSCTS; else t.c_cflag &= ~CRTSCTS;
#endif
    if (p->xonxoff) t.c_iflag |= IXON | IXOFF;
    else            t.c_iflag &= ~(IXON | IXOFF | IXANY);

    t.c_cc[VMIN] = 0;      /* the event loop blocks, never read() */
    t.c_cc[VTIME] = 0;

    if (baud_lookup(p->baud, &code)) {
        cfsetispeed(&t, code);
        cfsetospeed(&t, code);
        if (tcsetattr(fd, TCSANOW, &t) < 0) {
            snprintf(err, errlen, "tcsetattr: %s", strerror(errno));
            close(fd);
            return false;
        }
    } else {
        cfsetispeed(&t, B38400);
        cfsetospeed(&t, B38400);
        if (tcsetattr(fd, TCSANOW, &t) < 0) {
            snprintf(err, errlen, "tcsetattr: %s", strerror(errno));
            close(fd);
            return false;
        }
        if (st_set_custom_baud(fd, p->baud) < 0) {
            snprintf(err, errlen, "baud %u not supported here: %s",
                     p->baud, strerror(errno));
            close(fd);
            return false;
        }
    }

    tcflush(fd, TCIOFLUSH);
    serfd = fd;
    return true;
}

void st_serial_close(void)
{
    if (serfd < 0) return;
    if (ser_saved_ok) tcsetattr(serfd, TCSAFLUSH, &ser_saved);
    close(serfd);
    serfd = -1;
}

long st_serial_read(void *buf, size_t n)
{
    ssize_t r;
    if (serfd < 0) return ST_HUP;
    r = read(serfd, buf, n);
    if (r >= 0) return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    if (errno == EIO || errno == ENXIO || errno == ENODEV) return ST_HUP;
    return ST_ERR;
}

long st_serial_write(const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t left = n;
    if (serfd < 0) return ST_ERR;
    while (left) {
        ssize_t w = write(serfd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { .fd = serfd, .events = POLLOUT };
                poll(&pf, 1, 1000);
                continue;
            }
            return ST_ERR;
        }
        p += w;
        left -= (size_t)w;
    }
    return (long)n;
}

void st_serial_drain(void) { if (serfd >= 0) tcdrain(serfd); }
void st_set_poll_mode(bool on) { (void)on; }   /* poll() already does this */
void st_serial_break(void) { if (serfd >= 0) tcsendbreak(serfd, 0); }

static int line_bit(int line)
{
    switch (line) {
        case ST_DTR: return TIOCM_DTR;
        case ST_RTS: return TIOCM_RTS;
        case ST_CTS: return TIOCM_CTS;
        case ST_DSR: return TIOCM_DSR;
        case ST_DCD: return TIOCM_CD;
        default: return 0;
    }
}

bool st_modem_get(int line)
{
    int st = 0, bit = line_bit(line);
    if (serfd < 0 || !bit) return false;
    if (ioctl(serfd, TIOCMGET, &st) < 0) return false;
    return (st & bit) != 0;
}

void st_modem_set(int line, bool on)
{
    int bit = line_bit(line);
    if (serfd < 0 || !bit) return;
    ioctl(serfd, on ? TIOCMBIS : TIOCMBIC, &bit);
}

/* ---- console --------------------------------------------------------- */

bool st_con_isatty(void) { return stdin_tty; }

void st_con_raw(void)
{
    struct termios t;
    if (!stdin_tty || !tty_saved_ok || raw_active) return;
    t = tty_saved;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    raw_active = true;
}

void st_con_cooked(void)
{
    if (!stdin_tty || !tty_saved_ok || !raw_active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tty_saved);
    raw_active = false;
}

long st_con_read(void *buf, size_t n)
{
    ssize_t r = read(STDIN_FILENO, buf, n);
    if (r >= 0) return (long)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -3;
    return ST_ERR;
}

static void write_fd(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

void st_write_stdout(const void *buf, size_t n) { write_fd(STDOUT_FILENO, buf, n); }
void st_write_stderr(const void *buf, size_t n) { write_fd(STDERR_FILENO, buf, n); }

/* ---- event loop ------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    quit_flag = 1;
    if (wakefd[1] >= 0) {
        char b = 1;
        ssize_t r = write(wakefd[1], &b, 1);   /* async-signal-safe */
        (void)r;
    }
}

void st_signals_init(void)
{
    struct sigaction sa;

    /* Self-pipe rather than a bare flag: the old code tested stop_flag and
     * then called poll(), so a signal landing in between was lost and the
     * program stayed blocked until the next byte arrived. */
    if (pipe(wakefd) == 0) {
        fcntl(wakefd[0], F_SETFL, O_NONBLOCK);
        fcntl(wakefd[1], F_SETFL, O_NONBLOCK);
        fcntl(wakefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(wakefd[1], F_SETFD, FD_CLOEXEC);
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    stdin_tty = isatty(STDIN_FILENO);
    if (stdin_tty && tcgetattr(STDIN_FILENO, &tty_saved) == 0) tty_saved_ok = true;
}

int st_wait(int timeout_ms)
{
    struct pollfd pfd[3];
    int n = 0, si = -1, ii = -1, wi = -1, r, ev = 0;

    if (quit_flag) return ST_EV_QUIT;

    if (serfd >= 0) {
        pfd[n].fd = serfd; pfd[n].events = POLLIN; pfd[n].revents = 0;
        si = n++;
    }
    pfd[n].fd = STDIN_FILENO; pfd[n].events = POLLIN; pfd[n].revents = 0;
    ii = n++;
    if (wakefd[0] >= 0) {
        pfd[n].fd = wakefd[0]; pfd[n].events = POLLIN; pfd[n].revents = 0;
        wi = n++;
    }

    r = poll(pfd, (nfds_t)n, timeout_ms);
    if (quit_flag) return ST_EV_QUIT;
    if (r < 0) return errno == EINTR ? 0 : 0;
    if (r == 0) return 0;

    if (wi >= 0 && (pfd[wi].revents & POLLIN)) {
        char drain[64];
        while (read(wakefd[0], drain, sizeof drain) > 0) { }
        return ST_EV_QUIT;
    }
    /* POLLHUP together with POLLIN means the peer left but bytes are still
     * queued. Report the data; the caller drains it and sees the hup next
     * time round. The old loop broke out first and dropped the tail. */
    if (si >= 0 && (pfd[si].revents & (POLLIN | POLLHUP | POLLERR)))
        ev |= ST_EV_SERIAL;
    if (pfd[ii].revents & (POLLIN | POLLHUP | POLLERR))
        ev |= ST_EV_INPUT;
    return ev;
}

void st_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void st_stamp(char *buf, size_t n)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, n, "[%02d:%02d:%02d.%03ld] ",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

void st_list_ports(void)
{
    static const char *pfx[] = { "ttyUSB", "ttyACM", "ttyS", "ttyAMA", NULL };
    struct dirent *de;
    DIR *d;
    int found = 0;

    d = opendir("/dev/serial/by-id");
    if (d) {
        printf("/dev/serial/by-id:\n");
        while ((de = readdir(d)))
            if (de->d_name[0] != '.') { printf("  %s\n", de->d_name); found++; }
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
                found++;
                break;
            }
    closedir(d);
    if (!found) printf("no serial ports found\n");
}

const char *st_port_example(void) { return "sterm /dev/ttyUSB1 -b 115200 -t"; }

#endif /* !_WIN32 */
