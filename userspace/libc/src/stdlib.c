#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/punix.h"
#include "../include/errno.h"

// errno: the global error-number variable, set by syscall wrappers.
// Defined here exactly once; all other TUs extern-declare it via errno.h.
int errno = 0;

extern void* sbrk(intptr_t inc);

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
} block_meta;

#define META_SIZE sizeof(struct block_meta)
static void *global_base = NULL;

static block_meta *find_free_block(block_meta **last, size_t size) {
    block_meta *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

static block_meta *request_space(block_meta* last, size_t size) {
    block_meta *block;
    block = sbrk(0);
    void *request = sbrk(size + META_SIZE);
    if (request == (void*) -1) return NULL;
    
    if (last) last->next = block;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    return block;
}

void* malloc(size_t size) {
    if (size <= 0) return NULL;
    size = (size + 3) & ~3;
    block_meta *block;
    if (!global_base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        global_base = block;
    } else {
        block_meta *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) return NULL;
        } else {
            block->free = 0;
        }
    }
    return (block + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    block_meta* block_ptr = (block_meta*)ptr - 1;
    block_ptr->free = 1;
}

void* realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    block_meta* block_ptr = (block_meta*)ptr - 1;
    if (block_ptr->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block_ptr->size);
    free(ptr);
    return new_ptr;
}

void* calloc(size_t nelem, size_t elsize) {
    size_t size = nelem * elsize;
    void *ptr = malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (atexit_count < ATEXIT_MAX) {
        atexit_funcs[atexit_count++] = func;
        return 0;
    }
    return -1;
}

int atoi(const char* str) {
    return str_to_int(str);
}

char* itoa(int value, char* str, int base) {
    if (base == 10) {
        int_to_str(value, str);
    } else if (base == 16) {
        int_to_hex((uint32_t)value, str);
    } else {
        // Fallback or others not implemented
        str[0] = '0';
        str[1] = '\0';
    }
    return str;
}

// exit calls atexit handlers then sys_exit
void exit(int status) {
    while (atexit_count > 0) {
        atexit_funcs[--atexit_count]();
    }
    sys_exit(status);
}

void abort(void) {
    exit(1);
}

float strtof(const char* nptr, char** endptr) {
    return (float)strtol(nptr, endptr, 10);
}

long double strtold(const char* nptr, char** endptr) {
    return (long double)strtol(nptr, endptr, 10);
}

long strtol(const char* nptr, char** endptr, int base) {
    long res = 0;
    int i = 0;
    while (nptr[i] == ' ' || nptr[i] == '\t') i++;
    
    int neg = 0;
    if (nptr[i] == '-') { neg = 1; i++; }
    else if (nptr[i] == '+') { i++; }

    while (nptr[i]) {
        int v = -1;
        if (nptr[i] >= '0' && nptr[i] <= '9') v = nptr[i] - '0';
        else if (nptr[i] >= 'a' && nptr[i] <= 'z') v = nptr[i] - 'a' + 10;
        else if (nptr[i] >= 'A' && nptr[i] <= 'Z') v = nptr[i] - 'A' + 10;
        
        if (v == -1 || v >= base) break;
        res = res * base + v;
        i++;
    }
    
    if (endptr) *endptr = (char*)(nptr + i);
    return neg ? -res : res;
}

long long strtoll(const char* nptr, char** endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    return (unsigned long long)strtol(nptr, endptr, base);
}

char **environ = NULL;

char* getenv(const char* name) {
    if (!name || !environ) return NULL;

    size_t len = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=') {
            return environ[i] + len + 1;
        }
    }
    return NULL;
}

int putenv(char* string) {
    if (!string || !strchr(string, '=')) return -1;
    
    char* name_end = strchr(string, '=');
    size_t len = name_end - string;

    int count = 0;
    if (environ) {
        while (environ[count]) {
            if (strncmp(environ[count], string, len) == 0 && environ[count][len] == '=') {
                environ[count] = string;
                return 0;
            }
            count++;
        }
    }

    // Not found, add new
    char** new_env = (char**)malloc((count + 2) * sizeof(char*));
    if (!new_env) return -1;
    
    if (environ) {
        for (int i = 0; i < count; i++) new_env[i] = environ[i];
        // Note: we don't free the old environ because it might be from the initial stack
    }
    new_env[count] = string;
    new_env[count + 1] = NULL;
    environ = new_env;
    return 0;
}

int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !value) return -1;
    
    char* existing = getenv(name);
    if (existing && !overwrite) return 0;

    size_t nl = strlen(name);
    size_t vl = strlen(value);
    char* buf = (char*)malloc(nl + vl + 2);
    if (!buf) return -1;
    
    strcpy(buf, name);
    strcat(buf, "=");
    strcat(buf, value);
    
    return putenv(buf);
}

#include <sys/mman.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int offset) {
    // TCC uses mmap for tcc_run (executable memory) or for mapping files.
    // For now, we only support anonymous mapping via malloc.
    if (flags & MAP_ANONYMOUS) {
        return malloc(length);
    }
    return MAP_FAILED;
}

int munmap(void *addr, size_t length) {
    free(addr);
    return 0;
}

int mprotect(void *addr, size_t len, int prot) {
    return 0; // Just pretend we changed protections
}
