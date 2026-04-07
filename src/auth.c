/**
 * src/auth.c - Authentication and Credential Management
 * Stores username and password in /etc/credentials file
 */

#include "../include/auth.h"
#include "../include/fs.h"
#include "../include/string.h"
#include "../include/console.h"

#define CREDENTIALS_FILE "credentials"
#define CREDENTIALS_MAGIC 0xC8ED1234  // Magic number for validation

// Credential structure (stored in /etc/credentials file)
typedef struct {
    uint32_t magic;                    // Validation magic
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    uint8_t padding[400];              // Pad to ~512 bytes
} credentials_t;

// Global variables (volatile snapshots of stored user)
char USER_PASSWORD[MAX_PASSWORD_LEN] = "";
char USER_NAME[MAX_USERNAME_LEN] = "";

/**
 * @brief Checks if credentials file exists
 */
static int credentials_exist() {
    fs_node_t* etc_dir = fs_find_node("etc", fs_root_id);
    if (!etc_dir) return 0;
    uint32_t cred_id = fs_find_node_local_id(etc_dir->id, CREDENTIALS_FILE);
    return (cred_id != 0);
}

/**
 * @brief Loads credentials from disk
 * @return 1 if successful, 0 if not found or invalid
 */
int auth_load_credentials() {
    fs_node_t* etc_dir = fs_find_node("etc", fs_root_id);
    if (!etc_dir) return 0;

    uint32_t cred_id = fs_find_node_local_id(etc_dir->id, CREDENTIALS_FILE);
    if (cred_id == 0) return 0;

    fs_node_t* cred_file = fs_get_node(cred_id);
    if (!cred_file || cred_file->type != FS_TYPE_FILE) return 0;

    credentials_t creds;
    memset(&creds, 0, sizeof(credentials_t));
    
    // Read and validate full structure size
    if (fs_read(cred_file, 0, sizeof(credentials_t), (uint8_t*)&creds) < sizeof(credentials_t)) {
        return 0;
    }

    if (creds.magic != CREDENTIALS_MAGIC) {
        console_print_colored("Warning: Credentials file corrupted.\n", COLOR_YELLOW_ON_BLACK);
        return 0;
    }

    strncpy(USER_NAME, creds.username, MAX_USERNAME_LEN - 1);
    strncpy(USER_PASSWORD, creds.password, MAX_PASSWORD_LEN - 1);
    USER_NAME[MAX_USERNAME_LEN-1] = '\0';
    USER_PASSWORD[MAX_PASSWORD_LEN-1] = '\0';

    return 1;
}

/**
 * @brief Saves credentials to disk
 * @return 1 if successful, 0 if failed
 */
int auth_save_credentials() {
    fs_node_t* etc_dir = fs_find_node("etc", fs_root_id);
    if (!etc_dir) {
        if (!fs_create_node(fs_root_id, "etc", FS_TYPE_DIRECTORY, 0, 0)) return 0;
        etc_dir = fs_find_node("etc", fs_root_id);
        if (!etc_dir) return 0;
    }

    uint32_t cred_id = fs_find_node_local_id(etc_dir->id, CREDENTIALS_FILE);
    fs_node_t* cred_file = 0;

    if (cred_id == 0) {
        if (!fs_create_node(etc_dir->id, CREDENTIALS_FILE, FS_TYPE_FILE, 0, 0)) return 0;
        cred_id = fs_find_node_local_id(etc_dir->id, CREDENTIALS_FILE);
    }
    
    cred_file = fs_get_node(cred_id);
    if (!cred_file) return 0;

    credentials_t creds;
    memset(&creds, 0, sizeof(credentials_t));
    creds.magic = CREDENTIALS_MAGIC;
    strncpy(creds.username, USER_NAME, MAX_USERNAME_LEN - 1);
    strncpy(creds.password, USER_PASSWORD, MAX_PASSWORD_LEN - 1);

    if (fs_write(cred_file, 0, sizeof(credentials_t), (uint8_t*)&creds) < sizeof(credentials_t)) {
        return 0;
    }

    fs_update_node(cred_file);
    return 1;
}

/**
 * @brief Verifies credentials against root or stored user
 */
int auth_verify(const char* username, const char* password) {
    if (!username || !password) return -1;

    // Hardcoded root check
    if (strcmp(username, "root") == 0) {
        return (strcmp(password, "root") == 0) ? 0 : -1;
    }

    // Check against stored user
    if (auth_load_credentials()) {
        if (strcmp(username, USER_NAME) == 0 && strcmp(password, USER_PASSWORD) == 0) {
            return 0;
        }
    }

    return -1;
}

/**
 * @brief Registers a new user account
 */
int auth_register(const char* username, const char* password) {
    if (!username || !password) return 0;
    if (strcmp(username, "root") == 0) return 0; // Cannot register root

    strncpy(USER_NAME, username, MAX_USERNAME_LEN - 1);
    strncpy(USER_PASSWORD, password, MAX_PASSWORD_LEN - 1);
    USER_NAME[MAX_USERNAME_LEN-1] = '\0';
    USER_PASSWORD[MAX_PASSWORD_LEN-1] = '\0';

    return auth_save_credentials();
}

/**
 * @brief Legacy init (mostly unused now as login.c handles first-boot)
 */
void auth_init(void (*read_line_func)(char*, int)) {
    // Just ensure /etc exists
    auth_save_credentials();
}
