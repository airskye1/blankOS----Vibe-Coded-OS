#include <stdbool.h>
#include <stddef.h>

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
    // 1. Parse the raw string into name=value; Secure; HttpOnly
    // 2. Generate a blankReg key: "network/cookies/<domain>/<name>"
    // 3. Store securely in blankReg so it persists across reboots
    
    // Stub implementation
    // blankReg_write_string("network/cookies/discord.com/session", "xxxx-yyyy-zzzz");
}

// Called by the OS networking stack when making an HTTP request to append "Cookie:" headers
char* cookie_jar_retrieve(char* domain) {
    // 1. Query blankReg for all keys matching "network/cookies/<domain>/*"
    // 2. Construct the "Cookie: name1=value1; name2=value2" string
    // 3. Return the string to be injected into the TCP packet
    return "session=example_token; theme=dark";
}
