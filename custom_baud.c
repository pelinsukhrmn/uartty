/* Arbitrary baud rates. Separate TU because <asm/termbits.h> clashes
 * with <termios.h>. */

#ifdef __linux__

#include <asm/termbits.h>
#include <asm/ioctls.h>
#include <sys/ioctl.h>

int st_set_custom_baud(int fd, unsigned int baud)
{
    struct termios2 t;

    if (ioctl(fd, TCGETS2, &t) < 0) return -1;

    t.c_cflag &= ~CBAUD;
    t.c_cflag |= BOTHER;
    t.c_ispeed = baud;
    t.c_ospeed = baud;

    return ioctl(fd, TCSETS2, &t);
}

#else

#include <errno.h>

int st_set_custom_baud(int fd, unsigned int baud)
{
    (void)fd; (void)baud;
    errno = EINVAL;   /* macOS/BSD: use IOSSIOSPEED */
    return -1;
}

#endif
