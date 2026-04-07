#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>
#include <stdint.h>

void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nelem, size_t elsize);

int atexit(void (*func)(void));

int atoi(const char* str);
char* itoa(int value, char* str, int base);

void exit(int status);
void abort(void);

float strtof(const char* nptr, char** endptr);
long double strtold(const char* nptr, char** endptr);

long strtol(const char* nptr, char** endptr, int base);
long long strtoll(const char* nptr, char** endptr, int base);
unsigned long long strtoull(const char* nptr, char** endptr, int base);
char* getenv(const char* name);
int setenv(const char* name, const char* value, int overwrite);
int putenv(char* string);

#endif
