#ifndef BLANKOS_JS_ENGINE_H
#define BLANKOS_JS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BlankJS: Freestanding ECMAScript Subset & Live DOM Engine for Horizon Web
// Zero stdlib dependencies, bounded execution step budget, closures, arrays,
// objects, template literals, timers, and DOM bindings.
// ============================================================================

#define JS_MAX_DOM_NODES      256
#define JS_MAX_CONSOLE_LINES   64
#define JS_CONSOLE_TEXT_MAX   140

typedef struct {
    int id;
    int parent_id;
    int first_child;
    int next_sibling;
    char tag[16];          // "div", "p", "h1", "h2", "button", "input", "span", "a", "li", "body", "#text"
    char elem_id[48];
    char class_name[64];
    char href[192];
    char text[512];        // textContent / button label / input value
    char onclick_src[256]; // inline onclick="..." attribute
    uint32_t color;
    uint32_t bg_color;
    bool hidden;
    bool is_text_node;
    int js_obj_handle;     // cached JS wrapper object index (-1 if none)
    int click_fn_handle;   // bound addEventListener('click', fn) or .onclick closure (-1 if none)
} JsDomNode;

typedef struct {
    uint8_t level; // 0=log, 1=warn, 2=error, 3=info, 4=result
    char text[JS_CONSOLE_TEXT_MAX];
} JsConsoleLine;

typedef struct JsRuntime JsRuntime;

JsRuntime* js_runtime_create(void);
void js_runtime_destroy(JsRuntime* rt);
void js_runtime_reset(JsRuntime* rt, const char* url, const char* title);

// Execute JavaScript code inside runtime. Returns true on success.
// If eval_out is non-NULL, copies a string representation of the completion value.
bool js_eval(JsRuntime* rt, const char* code, char* eval_out, int eval_out_max);

// Advance timers (setTimeout / setInterval) using current uptime ms.
// Returns true if DOM was mutated by timer callbacks.
bool js_tick_timers(JsRuntime* rt, uint64_t now_ms);

// Fire click event on DOM node (runs both inline onclick and addEventListener/onclick closure).
// Returns true if JS mutated the DOM or produced an alert/navigation.
bool js_dispatch_click(JsRuntime* rt, int dom_node_id);

// DOM tree construction / accessors used by browser HTML parser & renderer
int js_dom_get_body_id(JsRuntime* rt);
int js_dom_create_element(JsRuntime* rt, const char* tag, int parent_id);
int js_dom_create_text(JsRuntime* rt, const char* text, int parent_id);
JsDomNode* js_dom_get_node(JsRuntime* rt, int node_id);
int js_dom_get_node_count(JsRuntime* rt);
bool js_dom_is_dirty(JsRuntime* rt);
void js_dom_clear_dirty(JsRuntime* rt);
const char* js_dom_get_title(JsRuntime* rt);
void js_dom_set_title(JsRuntime* rt, const char* title);

// Pending document.write / alert / navigation hooks
bool js_take_doc_write(JsRuntime* rt, char* out_html, int max_len);
bool js_take_alert(JsRuntime* rt, char* out_msg, int max_len);
bool js_take_navigate(JsRuntime* rt, char* out_url, int max_len);

// DevTools Console accessors
int js_console_count(JsRuntime* rt);
const JsConsoleLine* js_console_get(JsRuntime* rt, int index);
void js_console_clear(JsRuntime* rt);
const char* js_last_error(JsRuntime* rt);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_JS_ENGINE_H
