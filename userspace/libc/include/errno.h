#ifndef ERRNO_H
#define ERRNO_H

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define EBADF    9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29

#endif
