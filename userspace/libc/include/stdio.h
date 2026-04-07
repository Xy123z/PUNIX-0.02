#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

typedef struct _FILE FILE;
typedef int ssize_t;

#define EOF (-1)

int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int vsprintf(char* str, const char* format, va_list ap);
char* fgets(char* s, int size, void* stream);
int snprintf(char* str, size_t size, const char* format, ...);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);
int vfprintf(FILE* stream, const char* format, va_list ap);
ssize_t getline(char** lineptr, size_t* n, FILE* stream);
void perror(const char* s);

#define stdin ((FILE*)0)
#define stdout ((FILE*)1)
#define stderr ((FILE*)2)

int puts(const char* s);
int fflush(void* stream);
int putchar(int c);
int getchar(void);

int scanf(const char* format, ...);
int sscanf(const char* str, const char* format, ...);
int fscanf(void* stream, const char* format, ...);
int vsscanf(const char* str, const char* format, va_list ap);
int vfscanf(void* stream, const char* format, va_list ap);

// System-specific / Extended I/O
void print(const char* str);
void print_colored(const char* str, uint8_t color);

#endif
