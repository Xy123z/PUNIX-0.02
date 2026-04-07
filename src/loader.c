#include "../include/loader.h"
#include "../include/console.h"
#include "../include/fs.h"
#include "../include/memory.h"
#include "../include/paging.h"
#include "../include/string.h"
#include "../include/task.h"

/**
 * @brief Loads a flat binary program from disk sectors into user memory.
 * Addresses architectural faults:
 * 1. Prevents kernel overwrite by specific mapping.
 * 2. Handles multi-page programs safely.
 * 3. Clears memory to prevent data leaks.
 */
/**
 * @brief Loads a flat binary program from disk sectors into user memory.
 * Creates a truly isolated address space with unique page directory.
 */
task_t* load_user_program(task_t* target, const char* path, int argc, char** argv, char** envp) {
    /*console_print("LOADING EXTERNAL PROGRAM: ");
    console_print(path);
    console_print("\n");*/

    // 0. Copy arguments and environment to kernel memory while still in old directory
    char* kargv_strs[32]; // Increased to 32
    if (argc > 32) argc = 32;
    
    for (int i = 0; i < argc; i++) {
        kargv_strs[i] = (char*)kmalloc(128); 
        // Safety check: ensure argv[i] is a valid pointer in the current address space
        if (argv && argv[i]) {
            strncpy(kargv_strs[i], argv[i], 127);
            kargv_strs[i][127] = '\0';
        } else {
            strcpy(kargv_strs[i], "");
        }
    }

    char* kenvp_strs[64]; // Increased to 64
    int envc = 0;
    if (envp) {
        // Safety check: ensure envp is readable
        while (envc < 64) {
            // We need to check if envp[envc] is a valid pointer BEFORE dereferencing
            // In a real OS we'd use a safe copy_from_user function.
            char* user_ptr = envp[envc];
            if (!user_ptr) break;
            
            kenvp_strs[envc] = (char*)kmalloc(256);
            strncpy(kenvp_strs[envc], user_ptr, 255);
            kenvp_strs[envc][255] = '\0';
            envc++;
        }
    }

    // 1. Find the file
    fs_node_t* node = fs_find_node(path, current_task ? current_task->cwd_id : fs_root_id);
    if (!node) {
        console_print_colored("FATAL: File not found!\n", COLOR_LIGHT_RED);
        for (int i = 0; i < argc; i++) kfree(kargv_strs[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp_strs[i]);
        return NULL;
    }

    // Read header to check format
    aout_header_t aout_hdr;
    fs_read(node, 0, sizeof(aout_header_t), (uint8_t*)&aout_hdr);
    
    // Check for ELF (0x464C457F is '\x7FELF')
    bool is_elf = (aout_hdr.a_magic == 0x464C457F);
    bool is_aout = !is_elf && (aout_hdr.a_magic == AOUT_MINIX_V1_OMAGIC || 
                    aout_hdr.a_magic == AOUT_MINIX_V1_NMAGIC ||
                    (aout_hdr.a_magic & 0xFFFF) == 0x0301 ||
                    (aout_hdr.a_magic & 0xFFFF) == 0x0103 ||
                    aout_hdr.a_magic == 0x07010301);

    uint32_t user_virt_base = 0x80000000;
    uint32_t entry_point = user_virt_base;
    uint32_t memory_size = node->size;
    uint32_t map_base = user_virt_base;

    // 2. Create NEW page directory for this process
    page_directory_t* new_dir = paging_create_directory();
    if (!new_dir) {
        console_print_colored("FATAL: Out of memory for page directory!\n", COLOR_LIGHT_RED);
        for (int i = 0; i < argc; i++) kfree(kargv_strs[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp_strs[i]);
        return NULL;
    }

    int load_ok = 0;

    page_directory_t* old_dir = current_page_directory;

    if (is_elf) {
        Elf32_Ehdr ehdr;
        fs_read(node, 0, sizeof(Elf32_Ehdr), (uint8_t*)&ehdr);
        
        if (ehdr.e_machine != EM_386) {
            console_print_colored("FATAL: ELF binary is not for i386!\n", COLOR_LIGHT_RED);
            paging_free_directory(new_dir);
            return NULL;
        }

        console_print("loader: ELF binary detected. entry=0x");
        char eh[16]; int_to_hex(ehdr.e_entry, eh); console_print(eh);
        console_print("\n");

        entry_point = ehdr.e_entry;

        // Load Program Headers
        Elf32_Phdr* phdr = (Elf32_Phdr*)kmalloc(sizeof(Elf32_Phdr) * ehdr.e_phnum);
        fs_read(node, ehdr.e_phoff, sizeof(Elf32_Phdr) * ehdr.e_phnum, (uint8_t*)phdr);

        paging_switch_directory(new_dir);

        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD) {
                // Map pages for this segment
                uint32_t start_vpage = phdr[i].p_vaddr & ~4095;
                uint32_t end_vpage = (phdr[i].p_vaddr + phdr[i].p_memsz + 4095) & ~4095;
                
                for (uint32_t v = start_vpage; v < end_vpage; v += 4096) {
                    if (!paging_get_physical(new_dir, v)) {
                        void* p = pmm_alloc_page();
                        memset(p, 0, 4096);
                        paging_map_page(new_dir, v, (uint32_t)p, 4 | 2 | 1); // User, RW, Present
                    }
                }
                
                // Read from file
                fs_read(node, phdr[i].p_offset, phdr[i].p_filesz, (uint8_t*)phdr[i].p_vaddr);
                // BSS is already zeroed
            }
        }
        
        kfree(phdr);
        load_ok = 1;

    } else if (is_aout) {
        console_print("loader: a.out binary detected\n");
        memory_size = aout_hdr.a_text + aout_hdr.a_data + aout_hdr.a_bss;
        if (aout_hdr.a_entry >= 0x40000000) entry_point = aout_hdr.a_entry;
        else entry_point = user_virt_base + aout_hdr.a_entry;

        // Map and load a.out
        uint32_t pages = (memory_size + 4095) / 4096 + 32;
        for (uint32_t i = 0; i < pages; i++) {
            void* p = pmm_alloc_page();
            memset(p, 0, 4096);
            paging_map_page(new_dir, map_base + (i * 4096), (uint32_t)p, 4 | 2 | 1);
        }

        paging_switch_directory(new_dir);
        fs_read(node, 32, aout_hdr.a_text, (uint8_t*)map_base);
        fs_read(node, 32 + aout_hdr.a_text, aout_hdr.a_data, (uint8_t*)(map_base + aout_hdr.a_text));
        load_ok = 1;

    } else {
        console_print("loader: flat binary (magic=0x");
        char mh[16]; int_to_hex(aout_hdr.a_magic, mh); console_print(mh);
        console_print(") assumed\n");
        
        uint32_t pages = (node->size + 4095) / 4096 + 32;
        for (uint32_t i = 0; i < pages; i++) {
            void* p = pmm_alloc_page();
            memset(p, 0, 4096);
            paging_map_page(new_dir, map_base + (i * 4096), (uint32_t)p, 4 | 2 | 1);
        }

        paging_switch_directory(new_dir);
        fs_read(node, 0, node->size, (uint8_t*)map_base);
        load_ok = 1;
    }
    
    paging_switch_directory(old_dir);

    if (!load_ok) {
        console_print_colored("FATAL: Failed to read complete file!\n", COLOR_LIGHT_RED);
        paging_free_directory(new_dir);
        for (int i = 0; i < argc; i++) kfree(kargv_strs[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp_strs[i]);
        return NULL;
    }

    uint32_t user_stack_base = 0xC0000000;
    for (int i = 0; i < 4; i++) {
        void* phys = pmm_alloc_page();
        if (phys) memset(phys, 0, PAGE_SIZE);
        paging_map_page(new_dir, user_stack_base + (i * PAGE_SIZE),
                        (uint32_t)phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    // 3. TARGETING LOGIC
    task_t* task = target;
    if (!task) {
        // Fallback: If no target, create one if in kernel, or reuse current
        if (!current_task || current_task->id == 0) {
            task = task_create(current_task ? current_task->id : 0, new_dir, -1);
        } else {
            task = current_task;
            if (task->page_directory && task->page_directory != kernel_page_directory) {
                paging_free_directory(task->page_directory);
            }
            task->page_directory = new_dir;
        }
    } else {
        // Use provided target
        if (task->page_directory && task->page_directory != kernel_page_directory) {
            paging_free_directory(task->page_directory);
        }
        task->page_directory = new_dir;
    }

    if (!task) {
        paging_free_directory(new_dir);
        for (int i = 0; i < argc; i++) kfree(kargv_strs[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp_strs[i]);
        return NULL;
    }

    // Reset heap pointer for the new address space
    task->heap_end = 0x81000000;

    // Update task's own environ
    if (task->environ) {
        for (int i = 0; task->environ[i]; i++) kfree(task->environ[i]);
        kfree(task->environ);
    }
    if (envc > 0) {
        task->environ = (char**)kmalloc((envc + 1) * sizeof(char*));
        for (int i = 0; i < envc; i++) {
            task->environ[i] = (char*)kmalloc(strlen(kenvp_strs[i]) + 1);
            strcpy(task->environ[i], kenvp_strs[i]);
        }
        task->environ[envc] = NULL;
    } else {
        task->environ = NULL;
    }

    // Set task name early so it's visible in ps immediately
    const char* task_name = path;
    const char* last_slash = strrchr(path, '/');
    if (last_slash) task_name = last_slash + 1;
    strncpy(task->name, task_name, 31);

    uint32_t stack_virt_top = user_stack_base + (4 * PAGE_SIZE);
    uint32_t* stack_phys_page = (uint32_t*)paging_get_physical(new_dir, stack_virt_top - PAGE_SIZE);
    
    // Note: paging_get_physical returns identity mapped physical address in kernel
    uint32_t offset = PAGE_SIZE;
    uint32_t u_argv_ptrs[16];
    uint32_t u_envp_ptrs[32];

    // Push environment strings
    for (int i = envc - 1; i >= 0; i--) {
        int len = strlen(kenvp_strs[i]) + 1;
        offset -= len;
        memcpy((uint8_t*)stack_phys_page + offset, kenvp_strs[i], len);
        u_envp_ptrs[i] = stack_virt_top - (PAGE_SIZE - offset);
        kfree(kenvp_strs[i]);
    }
    
    // Push argv strings
    for (int i = argc - 1; i >= 0; i--) {
        int len = strlen(kargv_strs[i]) + 1;
        offset -= len;
        memcpy((uint8_t*)stack_phys_page + offset, kargv_strs[i], len);
        u_argv_ptrs[i] = stack_virt_top - (PAGE_SIZE - offset);
        kfree(kargv_strs[i]);
    }
    
    offset &= ~3;

    // Push envp array
    offset -= 4; // NULL term
    *(uint32_t*)((uint8_t*)stack_phys_page + offset) = 0;
    for (int i = envc - 1; i >= 0; i--) {
        offset -= 4;
        *(uint32_t*)((uint8_t*)stack_phys_page + offset) = u_envp_ptrs[i];
    }
    uint32_t u_envp_base = stack_virt_top - (PAGE_SIZE - offset);

    // Push argv array
    offset -= 4; // NULL term for argv array
    *(uint32_t*)((uint8_t*)stack_phys_page + offset) = 0;
    for (int i = argc - 1; i >= 0; i--) {
        offset -= 4;
        *(uint32_t*)((uint8_t*)stack_phys_page + offset) = u_argv_ptrs[i];
    }
    uint32_t u_argv_base = stack_virt_top - (PAGE_SIZE - offset);

    offset -= 12; // argc, argv, envp
    uint32_t* kstack_frame = (uint32_t*)((uint8_t*)stack_phys_page + offset);
    kstack_frame[0] = argc;
    kstack_frame[1] = u_argv_base;
    kstack_frame[2] = u_envp_base; // envp
    
    uint32_t esp = stack_virt_top - (PAGE_SIZE - offset);
    task_replace(task, entry_point, esp);
    
    // Note: Session/TTY focus is now handled by TTY device layer and Alt+F keys
    return task;
}

