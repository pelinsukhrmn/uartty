/* plat.h - platform layer for sterm.
 *
 * sterm.c contains no platform headers. Everything that differs between
 * POSIX and Windows lives behind this interface, implemented twice:
 *   plat_posix.c   termios + poll
 *   plat_win32.c   DCB + WaitCommEvent + console API
 */

#ifndef STERM_PLAT_H
#define STERM_PLAT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    unsigned baud;
    int      databits;    /* 5..8 */
    int      stopbits;    /* 1 or 2 */
    char     parity;      /* 'n', 'e' or 'o' */
    bool     rtscts;
    bool     xonxoff;
} st_params;

/* modem control lines for st_modem_get / st_modem_set */
enum { ST_DTR = 1, ST_RTS = 2, ST_CTS = 4, ST_DSR = 8, ST_DCD = 16 };

/* bits returned by st_wait() */
enum { ST_EV_SERIAL = 1, ST_EV_INPUT = 2, ST_EV_QUIT = 4 };

/* return codes for st_serial_read() */
#define ST_ERR (-1L)   /* transient or fatal read error, errmsg has detail */
#define ST_HUP (-2L)   /* port went away (cable unplugged, driver removed)  */

/* ---- serial port ---------------------------------------------------- */

/* Opens the port exclusively and applies p. On failure writes a human
 * readable reason into err and returns false. */
bool st_serial_open(const char *port, const st_params *p, char *err, size_t errlen);
void st_serial_close(void);

/* Non-blocking. Returns bytes read (may be 0), ST_ERR or ST_HUP. */
long st_serial_read(void *buf, size_t n);

/* Blocking, writes all n bytes. Returns n, or ST_ERR. */
long st_serial_write(const void *buf, size_t n);

void st_serial_drain(void);          /* block until the TX FIFO is empty */

/* Force the event loop to poll the port instead of waiting on a driver
 * event. No-op on POSIX. On Windows this is the escape hatch for drivers
 * whose WaitCommEvent is broken, and what makes the binary testable under
 * Wine, whose implementation faults. Call before st_serial_open. */
void st_set_poll_mode(bool on);
void st_serial_break(void);          /* send a BREAK condition */
bool st_modem_get(int line);
void st_modem_set(int line, bool on);

/* ---- console --------------------------------------------------------- */

bool st_con_isatty(void);
void st_con_raw(void);               /* no echo, no line buffering, no ISIG */
void st_con_cooked(void);            /* restore whatever we found at startup */
long st_con_read(void *buf, size_t n);   /* 0 = EOF, ST_ERR = error */

/* Unbuffered. sterm.c does its own buffering on top of these, so nothing
 * in the program touches stdio and output can never come out reordered. */
void st_write_stdout(const void *buf, size_t n);
void st_write_stderr(const void *buf, size_t n);

/* ---- event loop ------------------------------------------------------ */

/* Blocks until the serial port or the console has data, or a quit signal
 * arrives. timeout_ms < 0 means wait forever. Returns a bitmask of ST_EV_*. */
int  st_wait(int timeout_ms);

/* ---- misc ------------------------------------------------------------ */

void st_sleep_ms(unsigned ms);
void st_stamp(char *buf, size_t n);  /* "[hh:mm:ss.mmm] " */
void st_signals_init(void);          /* make Ctrl-Break / SIGTERM set ST_EV_QUIT */
void st_list_ports(void);            /* print discoverable serial devices */
const char *st_port_example(void);   /* platform-appropriate usage example */

/* Last platform error as text, for the caller to put in a message. */
const char *st_errmsg(void);

#endif /* STERM_PLAT_H */
