#include "../include/vfs.h"
#include "../include/string.h"
#include "../include/console.h"

static vfs_mount_entry_t mounts[VFS_MAX_MOUNTS];

void vfs_init() {
    memset(mounts, 0, sizeof(mounts));
    console_print_colored("VFS: Initialized.\n", COLOR_GREEN_ON_BLACK);
}

// Check if a path falls under a mountpoint. Returns the matching mount table entry
// and sets out_rel_path to the remainder of the path.
// e.g., if path is "/mnt/fs1/dir/file", and mountpoint is "/mnt/fs1",
// out_rel_path will be "/dir/file".
vfs_mount_entry_t* vfs_get_mount_by_path(const char* path, const char** out_rel_path) {
    if (!path) return 0;
    int is_abs = (path[0] == '/');
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            const char* mp = mounts[i].mountpoint;
            if (!is_abs && mp[0] == '/') mp++;
            int len = strlen(mp);
            int match = 1;
            for(int j=0; j<len; j++) { if(path[j] != mp[j] || path[j] == '\0') { match=0; break; } }
            if (match) {
                if (path[len] == '\0' || path[len] == '/') {
                    if (out_rel_path) {
                        *out_rel_path = path + len;
                        if (**out_rel_path == '\0') {
                            *out_rel_path = "/"; // default to root of the mounted fs
                        }
                    }
                    return &mounts[i];
                }
            }
        }
    }
    return 0;
}

vfs_mount_entry_t* vfs_get_mount_by_native_id(uint32_t native_id) {
    if (native_id == 0) return 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && mounts[i].native_mountpoint_id == native_id) {
            return &mounts[i];
        }
    }
    return 0;
}

vfs_mount_entry_t* vfs_get_mount(int slot) {
    if (slot >= 0 && slot < VFS_MAX_MOUNTS) return &mounts[slot];
    return 0;
}

int vfs_mount(const char* device, const char* mountpoint, const char* fs_type) {
    if (!device || !mountpoint || !fs_type) return -1;

    // Find a free mount slot
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        console_print_colored("VFS: Max mounts reached.\n", COLOR_LIGHT_RED);
        return -1;
    }

    // Identify device (e.g. /dev/hdb -> drive 1)
    uint8_t drive_index = 0;
    if (strcmp(device, "/dev/hda") == 0) drive_index = 0;
    else if (strcmp(device, "/dev/hdb") == 0) drive_index = 1;
    else {
        console_print_colored("VFS: Unknown device for mounting.\n", COLOR_LIGHT_RED);
        return -1;
    }

    // Call the FS driver's init/mount function to populate the generic superblock
    if (strcmp(fs_type, "minix") == 0) {
        extern int minix_init_mount(uint8_t drive, vfs_superblock_t* sb);
        if (minix_init_mount(drive_index, &mounts[slot].sb) != 0) {
            console_print_colored("VFS: Failed to mount minix filesystem.\n", COLOR_LIGHT_RED);
            return -1;
        }
    } else if (strcmp(fs_type, "punixfs") == 0) {
        extern int punixfs_init_mount(uint8_t drive, vfs_superblock_t* sb);
        if (punixfs_init_mount(drive_index, &mounts[slot].sb) != 0) {
            console_print_colored("VFS: Failed to mount punixfs filesystem.\n", COLOR_LIGHT_RED);
            return -1;
        }
    } else {
        console_print_colored("VFS: Unknown filesystem type.\n", COLOR_LIGHT_RED);
        return -1;
    }

    strcpy(mounts[slot].mountpoint, mountpoint);
    strcpy(mounts[slot].device, device);
    strcpy(mounts[slot].fs_type, fs_type);
    mounts[slot].fs_type[15] = '\0';
    mounts[slot].sb.mount_index = slot;
    
    // Resolve the native mountpoint ID so we can intercept relative directory traversal natively
    extern fs_node_t* fs_find_node(const char* path, uint32_t start_id);
    extern uint32_t fs_root_id;
    fs_node_t* mp_node = 0;
    if (mountpoint[0] == '/') {
        mp_node = fs_find_node(mountpoint + 1, fs_root_id);
    } else {
        mp_node = fs_find_node(mountpoint, fs_root_id); // fallback
    }
    if (mp_node) mounts[slot].native_mountpoint_id = mp_node->id;
    else mounts[slot].native_mountpoint_id = 0;
    
    mounts[slot].active = 1;

    console_print_colored("VFS: Mounted ", COLOR_GREEN_ON_BLACK);
    console_print_colored((char*)device, COLOR_WHITE);
    console_print_colored(" on ", COLOR_GREEN_ON_BLACK);
    console_print_colored((char*)mountpoint, COLOR_WHITE);
    console_print_colored("\n", COLOR_GREEN_ON_BLACK);

    return 0;
}
