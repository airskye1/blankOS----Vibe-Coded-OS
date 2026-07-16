#include <stdint.h>
#include <stdbool.h>

// A hierarchical configuration database (similar to Windows Registry)

typedef struct {
    char key_name[64];
    int value_type; // 0=INT, 1=STRING
    union {
        int int_val;
        char str_val[128];
    };
} blankReg_Key;

// Mock database in memory for now
static blankReg_Key registry[1024];
static int reg_count = 0;

void blankReg_set_int(char* key, int val) {
    // Check if exists, otherwise create
    // In a real OS, this flushes to the disk
}

int blankReg_get_int(char* key, int default_val) {
    // Stub
    return default_val;
}

void blankReg_set_string(char* key, char* val) {
    // Stub
}

char* blankReg_get_string(char* key, char* default_val) {
    // Stub
    return default_val;
}
