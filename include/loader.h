#ifndef LOADER_H
#define LOADER_H

#include "types.h"

#include "task.h"

// ELF 32-bit Header
typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint32_t      e_entry;
    uint32_t      e_phoff;
    uint32_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} Elf32_Ehdr;

// ELF 32-bit Program Header
typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define PT_LOAD 1
#define EM_386  3

// Minix a.out header (32 bytes)
typedef struct {
    uint32_t a_magic;   // 0x01030107 is common
    uint8_t  a_flags;
    uint8_t  a_cpu;
    uint8_t  a_hdrlen;
    uint8_t  a_unused;
    uint32_t a_text;
    uint32_t a_data;
    uint32_t a_bss;
    uint32_t a_entry;
    uint32_t a_total;
    uint32_t a_syms;
} __attribute__((packed)) aout_header_t;

// Minix Header Magics
#define AOUT_MINIX_V1_OMAGIC 0x01030107  // Combined I&D
#define AOUT_MINIX_V1_NMAGIC 0x01030110  // Separate I&D

task_t* load_user_program(task_t* target, const char* path, int argc, char** argv, char** envp);

#endif // LOADER_H
