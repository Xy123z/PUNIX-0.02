#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/errno.h"

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char* s) {
    if (!s) return -1;
    write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

int fflush(void* stream) {
    // Current implementation doesn't use libc-side buffering (direct write syscalls)
    // So fflush is a successful no-op.
    return 0;
}

// Minimal vsnprintf for printf/sprintf
int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    char* p = str;
    const char* f = format;
    char* end = (size > 0) ? (str + size - 1) : NULL;
    
    while (*f) {
        if (*f == '%') {
            f++;
            
            // Handle precision '.'
            int precision = -1;
            if (*f == '.') {
                f++;
                precision = 0;
                while (*f >= '0' && *f <= '9') {
                    precision = precision * 10 + (*f - '0');
                    f++;
                }
            }

            // Note: width is ignored for now as it's not needed for kilo's status bar
            // but we skip digits to avoid misinterpreting them.
            while (*f >= '0' && *f <= '9') f++;

            // Skip 'l' modifier (long is same as int on 32-bit)
            if (*f == 'l') f++;

            switch (*f) {
                case 's': {
                    char* s = va_arg(ap, char*);
                    if (!s) s = "(null)";
                    int count = 0;
                    while (*s && (precision == -1 || count < precision)) {
                        if (size == 0 || p < end) *p++ = *s;
                        s++;
                        count++;
                    }
                    break;
                }
                case 'd': {
                    int d = va_arg(ap, int);
                    char buf[12];
                    int_to_str(d, buf);
                    char* s = buf;
                    while (*s) {
                        if (size == 0 || p < end) *p++ = *s;
                        s++;
                    }
                    break;
                }
                case 'x': {
                    uint32_t x = va_arg(ap, uint32_t);
                    char buf[12];
                    int_to_hex(x, buf);
                    char* s = buf;
                    while (*s) {
                        if (size == 0 || p < end) *p++ = *s;
                        s++;
                    }
                    break;
                }
                case 'p': {
                    uint16_t pc = va_arg(ap, uint16_t);
                    char buf[12];
                    int_to_hex(pc, buf);
                    char* s = buf;
                    while (*s) {
                        if (size == 0 || p < end) *p++ = *s;
                        s++;
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(ap, int);
                    if (size == 0 || p < end) *p++ = c;
                    break;
                }
                case '%': {
                    if (size == 0 || p < end) *p++ = '%';
                    break;
                }
                default:
                    if (size == 0 || p < end) *p++ = *f;
                    break;
            }
        } else {
            if (size == 0 || p < end) *p++ = *f;
        }
        f++;
    }
    if (size > 0) *p = '\0';
    else if (str && size == 0) {
        // Special internal case for sprintf/vsprintf which pass size=0
        // Wait, better to just pass a large size.
        // Let's assume size=0 means NO LIMIT for now because that's what I used.
        *p = '\0';
    }
    return p - str;
}

int vsprintf(char* str, const char* format, va_list ap) {
    return vsnprintf(str, 0, format, ap); 
}

int printf(const char* format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    write(1, buf, n);
    return n;
}

int vfprintf(FILE* stream, const char* format, va_list ap) {
    int fd = stream ? (int)(uintptr_t)stream : 1;
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    write(fd, buf, n);
    return n;
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(stream, format, ap);
    va_end(ap);
    return n;
}

int sprintf(char* str, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, 0, format, ap);
    va_end(ap);
    return n;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

FILE* fopen(const char* path, const char* mode) {
    int flags = 0;
    if (strchr(mode, 'w')) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strchr(mode, 'a')) flags = O_WRONLY | O_CREAT | O_APPEND;
    else flags = O_RDONLY;
    
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    return (FILE*)(uintptr_t)fd;
}

int fclose(FILE* stream) {
    return close((int)(uintptr_t)stream);
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (size == 0 || nmemb == 0) return 0;
    int fd = (int)(uintptr_t)stream;
    int bytes_read = read(fd, ptr, size * nmemb);
    if (bytes_read <= 0) return 0;
    return (size_t)(bytes_read / size);
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (size == 0 || nmemb == 0) return 0;
    int fd = (int)(uintptr_t)stream;
    int bytes_written = write(fd, ptr, size * nmemb);
    if (bytes_written <= 0) return 0;
    return (size_t)(bytes_written / size);
}

int fseek(FILE* stream, long offset, int whence) {
    int fd = (int)(uintptr_t)stream;
    if (lseek(fd, (int)offset, whence) == -1) return -1;
    return 0;
}

long ftell(FILE* stream) {
    int fd = (int)(uintptr_t)stream;
    return (long)lseek(fd, 0, SEEK_CUR);
}

void rewind(FILE* stream) {
    fseek(stream, 0, SEEK_SET);
}

void perror(const char* s) {
    const char* errmsg;
    switch (errno) {
        case EPERM:   errmsg = "Operation not permitted"; break;
        case ENOENT:  errmsg = "No such file or directory"; break;
        case EINTR:   errmsg = "Interrupted system call"; break;
        case EIO:     errmsg = "Input/output error"; break;
        case ENXIO:   errmsg = "No such device or address"; break;
        case EBADF:   errmsg = "Bad file descriptor"; break;
        case EAGAIN:  errmsg = "Resource temporarily unavailable"; break;
        case ENOMEM:  errmsg = "Out of memory"; break;
        case EACCES:  errmsg = "Permission denied"; break;
        case EEXIST:  errmsg = "File exists"; break;
        case ENOTDIR: errmsg = "Not a directory"; break;
        case EISDIR:  errmsg = "Is a directory"; break;
        case EINVAL:  errmsg = "Invalid argument"; break;
        case ENFILE:  errmsg = "Too many open files in system"; break;
        case EMFILE:  errmsg = "Too many open files"; break;
        case ENOTTY:  errmsg = "Inappropriate ioctl for device"; break;
        case ESPIPE:  errmsg = "Illegal seek"; break;
        default:      errmsg = "Unknown error"; break;
    }
    if (s && *s) {
        print(s);
        print(": ");
    }
    print(errmsg);
    print("\n");
}

ssize_t getline(char** lineptr, size_t* n, FILE* stream) {
    if (!lineptr || !n || !stream) return -1;
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    int fd = (int)(uintptr_t)stream;
    size_t i = 0;
    while (1) {
        char c;
        if (read(fd, &c, 1) <= 0) {
            if (i == 0) return -1;
            break;
        }
        if (i + 1 >= *n) {
            *n *= 2;
            char *newptr = realloc(*lineptr, *n);
            if (!newptr) return -1;
            *lineptr = newptr;
        }
        (*lineptr)[i++] = c;
        if (c == '\n') break;
    }
    (*lineptr)[i] = '\0';
    return (ssize_t)i;
}

// System specific helpers
extern void sys_print(const char* str);
extern void sys_print_colored(const char* str, uint8_t color);

void print(const char* str) {
    if (str) write(1, str, strlen(str));
}

void print_colored(const char* str, uint8_t color) {
    // Note: color support currently relies on SYS_PRINT_COLORED or ANSI
    // We can use ANSI if we want to be fully compliant, 
    // or keep sys_print_colored if kernel still supports it.
    // Let's use ANSI for maximum compliance if possible, 
    // but sys_print_colored is fine for internal kernel colors.
    sys_print_colored(str, color);
}

int getchar(void) {
    char c;
    if (read(0, &c, 1) < 1) return EOF;
    return (int)c;
}

char* fgets(char* s, int size, void* stream) {
    if (!s || size <= 0) return NULL;
    int fd = (int)(uintptr_t)stream;
    int i = 0;
    while (i < size - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}
static int isspace_libc(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r');
}

int vsscanf(const char* str, const char* format, va_list ap) {
    const char* p = str;
    const char* f = format;
    int count = 0;

    while (*f) {
        if (isspace_libc(*f)) {
            while (isspace_libc(*p)) p++;
            f++;
            continue;
        }

        if (*f == '%') {
            f++;
            if (*f == '%') {
                if (*p == '%') {
                    p++;
                    f++;
                } else {
                    return count;
                }
                continue;
            }

            // Handle assignments
            while (isspace_libc(*p)) p++;

            switch (*f) {
                case 'd': {
                    char* endp;
                    int* val = va_arg(ap, int*);
                    *val = (int)strtol(p, &endp, 10);
                    if (p == endp) return count;
                    p = endp;
                    count++;
                    break;
                }
                case 'x': {
                    char* endp;
                    unsigned int* val = va_arg(ap, unsigned int*);
                    *val = (unsigned int)strtol(p, &endp, 16);
                    if (p == endp) return count;
                    p = endp;
                    count++;
                    break;
                }
                case 's': {
                    char* val = va_arg(ap, char*);
                    if (!*p) return count;
                    while (*p && !isspace_libc(*p)) {
                        *val++ = *p++;
                    }
                    *val = '\0';
                    count++;
                    break;
                }
                case 'c': {
                    char* val = va_arg(ap, char*);
                    if (!*p) return count;
                    *val = *p++;
                    count++;
                    break;
                }
                default:
                    // Unsupported format
                    return count;
            }
        } else {
            if (*p == *f) {
                p++;
            } else {
                return count;
            }
        }
        f++;
    }
    return count;
}

int vfscanf(void* stream, const char* format, va_list ap) {
    // Basic implementation: read into a buffer and use vsscanf
    // Since we don't have ungetc or sophisticated buffering yet,
    // we use a large enough temporary buffer for now.
    char buf[1024];
    if (fgets(buf, sizeof(buf), stream)) {
        return vsscanf(buf, format, ap);
    }
    return EOF;
}

int scanf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfscanf(stdin, format, ap);
    va_end(ap);
    return res;
}

int sscanf(const char* str, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsscanf(str, format, ap);
    va_end(ap);
    return res;
}

int fscanf(void* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfscanf(stream, format, ap);
    va_end(ap);
    return res;
}
