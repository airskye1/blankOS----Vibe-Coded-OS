#include <stdbool.h>
#include <stddef.h>

extern "C" {
extern void blankReg_write_string(char* key, char* value);
extern char* blankReg_read_string(char* key);

typedef struct {
    char domain[64];
    char name[64];
    char value[256];
    bool secure;
    bool http_only;
} HttpCookie;

// Called by the OS networking stack when an HTTP response contains "Set-Cookie:"
void cookie_jar_store(char* domain, char* raw_cookie_string) {
    (void)domain;
    (void)raw_cookie_string;
}

// Called by the OS networking stack when making an HTTP request to append "Cookie:" headers
const char* cookie_jar_retrieve(char* domain) {
    (void)domain;
    return "session=example_token; theme=dark";
}
}
