#ifndef TERMIOS_H
#define TERMIOS_H

#include <stdint.h>

// c_cc special characters
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VSUSP    5
#define VMIN     6
#define VTIME    7
#define VJOB     8
#define NCCS     9

// c_lflag bits
#define ICANON   0x0001
#define ECHO     0x0002
#define ECHOE    0x0004
#define ISIG     0x0008
#define IEXTEN   0x0010
#define TOSTOP   0x0020

// c_iflag bits
#define ICRNL    0x0001
#define BRKINT   0x0002
#define INPCK    0x0004
#define ISTRIP   0x0008
#define IXON     0x0010

// c_oflag bits
#define ONLCR    0x0001
#define OPOST    0x0002

// c_cflag bits
#define CS8      0x0000 // Dummy value, punix defaults to CS8

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[NCCS];
};

typedef struct termios termios_t;

// Optional actions for tcsetattr
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);

#endif
