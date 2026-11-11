#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "js_engine.h"
#include "../ui/theme.h"

extern "C" {
    void* uefi_malloc(uint64_t size);
    void uefi_free(void* ptr);
    void* uefi_realloc(void* ptr, uint64_t new_size);
    void* uefi_memcpy(void* dst, const void* src, uint64_t n);
    void* uefi_memset(void* dst, int val, uint64_t n);
    double uefi_floor(double x);
    double uefi_ceil(double x);
    double uefi_sqrt(double x);
    double uefi_pow(double x, double y);
    double uefi_fmod(double x, double y);
    double uefi_cos(double x);
    void play_system_sound(const char* name);
}

// ============================================================================
// Constants & Limits (compact, heap-allocated, step-budget guarded)
// ============================================================================

#define JS_STR_POOL_CAP    49152
#define JS_AST_CAP         1536
#define JS_TOK_CAP         4096
#define JS_OBJ_CAP         256
#define JS_PROP_CAP        1280
#define JS_ARR_CAP         128
#define JS_ENV_CAP         256
#define JS_FUNC_CAP        160
#define JS_TIMER_CAP       24
#define JS_STEP_BUDGET     60000
#define JS_MAX_CALL_DEPTH  44

// ============================================================================
// Freestanding String / Number Utilities (Zero stdlib)
// ============================================================================

static int js_strlen(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

static bool js_streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool js_streq_n(const char* a, const char* b, int n) {
    if (!a || !b) return false;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return false;
    }
    return b[n] == 0;
}

static void js_strcpy_max(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    if (src) {
        while (i < max_len - 1 && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void js_strappend_max(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = js_strlen(dst);
    int j = 0;
    while (i < max_len - 1 && src[j]) {
        dst[i++] = src[j++];
    }
    dst[i] = 0;
}

static char js_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool js_strcaseeq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (js_tolower(*a) != js_tolower(*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool js_isnan(double x) {
    return x != x;
}

static int js_dtoa(double v, char* out, int max_len) {
    if (!out || max_len < 4) return 0;
    if (js_isnan(v)) {
        js_strcpy_max(out, "NaN", max_len);
        return js_strlen(out);
    }
    if (v > 1e18) { js_strcpy_max(out, "Infinity", max_len); return js_strlen(out); }
    if (v < -1e18) { js_strcpy_max(out, "-Infinity", max_len); return js_strlen(out); }

    int pos = 0;
    if (v < 0.0) {
        out[pos++] = '-';
        v = -v;
    }

    double rounded_int = (double)((long long)(v + 0.0000001));
    double diff = v - rounded_int;
    if (diff < 0.0) diff = -diff;

    if (diff < 1e-7 && v < 9007199254740991.0) {
        long long iv = (long long)rounded_int;
        char rev[32];
        int rc = 0;
        if (iv == 0) rev[rc++] = '0';
        else {
            while (iv > 0 && rc < 30) {
                rev[rc++] = (char)('0' + (iv % 10));
                iv /= 10;
            }
        }
        while (rc > 0 && pos < max_len - 1) out[pos++] = rev[--rc];
        out[pos] = 0;
        return pos;
    }

    long long int_part = (long long)v;
    double frac = v - (double)int_part;
    char rev[32];
    int rc = 0;
    if (int_part == 0) rev[rc++] = '0';
    else {
        while (int_part > 0 && rc < 30) {
            rev[rc++] = (char)('0' + (int_part % 10));
            int_part /= 10;
        }
    }
    while (rc > 0 && pos < max_len - 1) out[pos++] = rev[--rc];

    if (pos < max_len - 2) {
        out[pos++] = '.';
        int frac_start = pos;
        for (int d = 0; d < 6 && pos < max_len - 1; d++) {
            frac *= 10.0;
            int dig = (int)frac;
            if (dig < 0) dig = 0;
            if (dig > 9) dig = 9;
            out[pos++] = (char)('0' + dig);
            frac -= (double)dig;
        }
        while (pos > frac_start && out[pos - 1] == '0') pos--;
        if (pos == frac_start) pos--;
    }
    out[pos] = 0;
    return pos;
}

static double js_atod(const char* s, bool* ok) {
    if (!s) { if (ok) *ok = false; return 0.0; }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) { if (ok) *ok = true; return 0.0; }

    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') { s++; }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        double acc = 0.0;
        bool any = false;
        while (*s) {
            char c = *s;
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
            else break;
            acc = acc * 16.0 + (double)d;
            any = true;
            s++;
        }
        if (ok) *ok = any;
        return sign * acc;
    }

    bool any_digit = false;
    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (double)(*s - '0');
        any_digit = true;
        s++;
    }
    if (*s == '.') {
        s++;
        double div = 10.0;
        while (*s >= '0' && *s <= '9') {
            val += (double)(*s - '0') / div;
            div *= 10.0;
            any_digit = true;
            s++;
        }
    }
    if ((*s == 'e' || *s == 'E') && any_digit) {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp_v = 0;
        while (*s >= '0' && *s <= '9') {
            exp_v = exp_v * 10 + (*s - '0');
            s++;
        }
        if (exp_v > 60) exp_v = 60;
        for (int i = 0; i < exp_v; i++) {
            if (esign > 0) val *= 10.0;
            else val /= 10.0;
        }
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (ok) *ok = (any_digit && *s == 0);
    return sign * val;
}

static bool js_parse_color(const char* s, uint32_t* out_col) {
    if (!s || !out_col) return false;
    while (*s == ' ') s++;
    if (s[0] == '#') {
        uint32_t hex = 0;
        int len = 0;
        for (int i = 1; s[i] && i <= 7; i++) {
            char c = s[i];
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
            else break;
            hex = (hex << 4) | (uint32_t)d;
            len++;
        }
        if (len == 3) {
            uint32_t r = (hex >> 8) & 0xF, g = (hex >> 4) & 0xF, b = hex & 0xF;
            *out_col = ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
            return true;
        }
        if (len == 6) {
            *out_col = hex;
            return true;
        }
    }
    if (js_strcaseeq(s, "cyan") || js_strcaseeq(s, "aqua")) { *out_col = THEME_ACCENT_CYAN; return true; }
    if (js_strcaseeq(s, "green") || js_strcaseeq(s, "lime")) { *out_col = THEME_ACCENT_GREEN; return true; }
    if (js_strcaseeq(s, "red") || js_strcaseeq(s, "crimson")) { *out_col = THEME_ACCENT_ROSE; return true; }
    if (js_strcaseeq(s, "yellow") || js_strcaseeq(s, "amber") || js_strcaseeq(s, "orange")) { *out_col = THEME_ACCENT_AMBER; return true; }
    if (js_strcaseeq(s, "teal")) { *out_col = THEME_ACCENT_TEAL; return true; }
    if (js_strcaseeq(s, "indigo") || js_strcaseeq(s, "purple")) { *out_col = THEME_ACCENT_IRIS; return true; }
    if (js_strcaseeq(s, "white")) { *out_col = THEME_TEXT_PRIMARY; return true; }
    if (js_strcaseeq(s, "gray") || js_strcaseeq(s, "grey")) { *out_col = THEME_TEXT_SECONDARY; return true; }
    return false;
}

// ============================================================================
// JS Value & Arena Types
// ============================================================================

enum JsValType {
    JVT_UNDEF = 0,
    JVT_NULL,
    JVT_BOOL,
    JVT_NUM,
    JVT_STR,
    JVT_OBJ,
    JVT_ARR,
    JVT_FUNC,
    JVT_DOM
};

typedef struct {
    uint8_t type;
    union {
        bool b;
        double num;
        int32_t handle;
    } u;
} JsVal;

static inline JsVal jv_undef(void) { JsVal v; v.type = JVT_UNDEF; v.u.handle = 0; return v; }
static inline JsVal jv_null(void)  { JsVal v; v.type = JVT_NULL;  v.u.handle = 0; return v; }
static inline JsVal jv_bool(bool b){ JsVal v; v.type = JVT_BOOL;  v.u.b = b; return v; }
static inline JsVal jv_num(double n){ JsVal v; v.type = JVT_NUM;  v.u.num = n; return v; }
static inline JsVal jv_str(int32_t h){ JsVal v; v.type = JVT_STR; v.u.handle = h; return v; }
static inline JsVal jv_obj(int32_t h){ JsVal v; v.type = JVT_OBJ; v.u.handle = h; return v; }
static inline JsVal jv_arr(int32_t h){ JsVal v; v.type = JVT_ARR; v.u.handle = h; return v; }
static inline JsVal jv_fn(int32_t h) { JsVal v; v.type = JVT_FUNC; v.u.handle = h; return v; }
static inline JsVal jv_dom(int32_t h){ JsVal v; v.type = JVT_DOM; v.u.handle = h; return v; }

typedef struct {
    int key_str;
    JsVal val;
    int next_prop;
} JsProp;

typedef struct {
    int first_prop;
    int dom_style_id; // >= 0 if this object is an element.style proxy
} JsObj;

typedef struct {
    JsVal* data;
    int len;
    int cap;
} JsArr;

typedef struct {
    int first_prop;
    int parent_env;
} JsEnv;

typedef JsVal (*JsNativeFn)(JsRuntime* rt, JsVal this_val, const JsVal* args, int argc);

typedef struct {
    bool is_native;
    JsNativeFn native_fn;
    int param_count;
    int params[8]; // str handles
    int body_ast;
    int closure_env;
    int name_str;
} JsFunc;

typedef struct {
    bool active;
    bool is_interval;
    int id;
    int fn_handle;     // >= 0 if callback function
    int code_str;      // >= 0 if code string
    uint64_t interval_ms;
    uint64_t next_fire_ms;
} JsTimer;

// ============================================================================
// Lexer & AST Node Definitions
// ============================================================================

enum JsTokKind {
    JT_EOF = 0,
    JT_NUM,
    JT_STR,
    JT_IDENT,
    JT_TPL_FULL,   // `plain template`
    JT_TPL_HEAD,   // `head ${
    JT_TPL_MID,    // } mid ${
    JT_TPL_TAIL,   // } tail`
    // Punctuation & Operators
    JT_LPAREN, JT_RPAREN, JT_LBRACE, JT_RBRACE, JT_LBRACK, JT_RBRACK,
    JT_DOT, JT_COMMA, JT_COLON, JT_SEMI, JT_QMARK, JT_ARROW,
    JT_PLUS, JT_MINUS, JT_STAR, JT_SLASH, JT_PCT,
    JT_PLUSPLUS, JT_MINUSMINUS,
    JT_EQ, JT_PLUSEQ, JT_MINUSEQ, JT_STAREQ, JT_SLASHEQ, JT_PCTEQ,
    JT_EQEQ, JT_EQEQEQ, JT_NEQ, JT_NEQEQ,
    JT_LT, JT_LE, JT_GT, JT_GE,
    JT_ANDAND, JT_OROR, JT_QQ, JT_BANG,
    JT_BITAND, JT_BITOR, JT_BITXOR, JT_SHL, JT_SHR,
    // Keywords
    JT_LET, JT_CONST, JT_VAR, JT_FUNCTION, JT_RETURN,
    JT_IF, JT_ELSE, JT_WHILE, JT_FOR, JT_DO, JT_BREAK, JT_CONTINUE,
    JT_TRUE, JT_FALSE, JT_NULL, JT_UNDEFINED, JT_TYPEOF, JT_NEW, JT_THIS,
    JT_TRY, JT_CATCH, JT_FINALLY, JT_THROW, JT_IN, JT_OF
};

typedef struct {
    uint8_t kind;
    int32_t str_h;
    double num;
    int line;
} JsTok;

enum JsAstKind {
    JA_NONE = 0,
    JA_LIT,        // val
    JA_IDENT,      // name_str
    JA_THIS,
    JA_ARRAY,      // first_child -> next_sibling items
    JA_OBJECT,     // children are JA_PROP(key_str, left=val_expr)
    JA_PROP,
    JA_FN_EXPR,    // func_tmpl_idx (stored in a)
    JA_MEMBER,     // left.name_str
    JA_INDEX,      // left[right]
    JA_CALL,       // left(first_child -> next_sibling args)
    JA_NEW,        // left(args)
    JA_UNARY,      // op=tok_kind, left
    JA_INCDEC,     // op=++,--, a=prefix?1:0, left
    JA_BINARY,     // op=tok_kind, left, right
    JA_ASSIGN,     // op==,+=,-=..., left, right
    JA_TERNARY,    // cond=left, then=right, else=a
    JA_BLOCK,      // first_child -> next_sibling stmts
    JA_VAR_DECL,   // children are JA_PROP(key=name_str, left=init_expr_or_0)
    JA_EXPR_STMT,  // left
    JA_IF,         // left=cond, right=then_stmt, a=else_stmt
    JA_WHILE,      // left=cond, right=body, a=(1=do-while)
    JA_FOR,        // first_child=(init, cond, step, body) chained via a/b/c or list
    JA_FOR_ITER,   // a=is_of?1:0, str_h=var_name, left=iterable, right=body
    JA_RETURN,     // left
    JA_BREAK,
    JA_CONTINUE,
    JA_THROW,      // left
    JA_TRY,        // left=try_blk, right=catch_blk, a=finally_blk, str_h=catch_var
    JA_FN_DECL     // a=func_tmpl_idx
};

typedef struct {
    uint8_t kind;
    uint8_t op;
    int16_t flags;
    int32_t str_h;
    int32_t a;
    int32_t left;
    int32_t right;
    int32_t next_sibling;
    JsVal lit;
} JsAstNode;

typedef struct {
    int param_count;
    int params[8];
    int body_ast;
    int name_str;
} JsFuncTmpl;

#define JS_FN_TMPL_CAP 128

enum JsCtrlFlow {
    CF_NONE = 0,
    CF_RETURN,
    CF_BREAK,
    CF_CONTINUE,
    CF_THROW
};

struct JsRuntime {
    char* str_pool;
    int str_len;

    JsAstNode* ast;
    int ast_len;

    JsFuncTmpl* fn_tmpls;
    int fn_tmpl_len;

    JsObj* objs;
    int obj_len;

    JsProp* props;
    int prop_len;

    JsArr* arrs;
    int arr_len;

    JsEnv* envs;
    int env_len;
    int global_env;

    JsFunc* funcs;
    int func_len;

    JsTimer timers[JS_TIMER_CAP];
    int next_timer_id;
    uint64_t now_ms;

    JsDomNode nodes[JS_MAX_DOM_NODES];
    int node_len;
    int body_node_id;
    int doc_click_fn;
    bool dom_dirty;

    char url[256];
    char title[128];
    char pending_doc_write[2048];
    bool has_doc_write;
    char pending_alert[256];
    bool has_alert;
    char pending_nav[256];
    bool has_nav;

    JsConsoleLine console_lines[JS_MAX_CONSOLE_LINES];
    int console_len;
    char last_error[192];

    uint32_t steps_left;
    int call_depth;
    uint8_t ctrl;
    JsVal ctrl_val;
    uint32_t rng_state;
};

// ============================================================================
// String Pool & Console Helpers
// ============================================================================

static int rt_intern_str_n(JsRuntime* rt, const char* s, int n) {
    if (!rt || !rt->str_pool) return 0;
    if (!s || n <= 0) return 0; // offset 0 is ""
    if (rt->str_len + n + 1 >= JS_STR_POOL_CAP) {
        return 0;
    }
    int off = rt->str_len;
    for (int i = 0; i < n; i++) rt->str_pool[off + i] = s[i];
    rt->str_pool[off + n] = 0;
    rt->str_len += n + 1;
    return off;
}

static int rt_intern_str(JsRuntime* rt, const char* s) {
    return rt_intern_str_n(rt, s, js_strlen(s));
}

static const char* rt_get_str(JsRuntime* rt, int h) {
    if (!rt || !rt->str_pool || h < 0 || h >= rt->str_len) return "";
    return rt->str_pool + h;
}

static void rt_console_push(JsRuntime* rt, uint8_t level, const char* msg) {
    if (!rt || !msg) return;
    if (rt->console_len >= JS_MAX_CONSOLE_LINES) {
        for (int i = 0; i < JS_MAX_CONSOLE_LINES - 1; i++) {
            rt->console_lines[i] = rt->console_lines[i + 1];
        }
        rt->console_len = JS_MAX_CONSOLE_LINES - 1;
    }
    JsConsoleLine* line = &rt->console_lines[rt->console_len++];
    line->level = level;
    js_strcpy_max(line->text, msg, JS_CONSOLE_TEXT_MAX);
}

static void rt_set_error(JsRuntime* rt, const char* msg) {
    if (!rt || !msg) return;
    js_strcpy_max(rt->last_error, msg, sizeof(rt->last_error));
    rt_console_push(rt, 2, msg);
}

// ============================================================================
// Arena / Object / Array / Environment / Coercion Helpers
// ============================================================================

static int rt_alloc_obj(JsRuntime* rt) {
    if (rt->obj_len >= JS_OBJ_CAP) return -1;
    int id = rt->obj_len++;
    rt->objs[id].first_prop = -1;
    rt->objs[id].dom_style_id = -1;
    return id;
}

static int rt_alloc_arr(JsRuntime* rt, int init_cap) {
    if (rt->arr_len >= JS_ARR_CAP) return -1;
    int id = rt->arr_len++;
    int cap = (init_cap < 8) ? 8 : init_cap;
    rt->arrs[id].data = (JsVal*)uefi_malloc((uint64_t)cap * sizeof(JsVal));
    rt->arrs[id].len = 0;
    rt->arrs[id].cap = rt->arrs[id].data ? cap : 0;
    return id;
}

static void rt_arr_push_raw(JsRuntime* rt, int arr_id, JsVal v) {
    if (arr_id < 0 || arr_id >= rt->arr_len) return;
    JsArr* a = &rt->arrs[arr_id];
    if (a->len >= a->cap) {
        int new_cap = (a->cap < 8) ? 8 : a->cap * 2;
        if (new_cap > 4096) return;
        JsVal* nd = (JsVal*)uefi_realloc(a->data, (uint64_t)new_cap * sizeof(JsVal));
        if (!nd) return;
        a->data = nd;
        a->cap = new_cap;
    }
    a->data[a->len++] = v;
}

static int rt_alloc_env(JsRuntime* rt, int parent) {
    if (rt->env_len >= JS_ENV_CAP) return parent;
    int id = rt->env_len++;
    rt->envs[id].first_prop = -1;
    rt->envs[id].parent_env = parent;
    return id;
}

static void rt_prop_list_set(JsRuntime* rt, int* head_idx, const char* key, JsVal val) {
    int p = *head_idx;
    while (p >= 0) {
        if (js_streq(rt_get_str(rt, rt->props[p].key_str), key)) {
            rt->props[p].val = val;
            return;
        }
        p = rt->props[p].next_prop;
    }
    if (rt->prop_len >= JS_PROP_CAP) return;
    int np = rt->prop_len++;
    rt->props[np].key_str = rt_intern_str(rt, key);
    rt->props[np].val = val;
    rt->props[np].next_prop = *head_idx;
    *head_idx = np;
}

static bool rt_prop_list_get(JsRuntime* rt, int head_idx, const char* key, JsVal* out) {
    int p = head_idx;
    while (p >= 0) {
        if (js_streq(rt_get_str(rt, rt->props[p].key_str), key)) {
            if (out) *out = rt->props[p].val;
            return true;
        }
        p = rt->props[p].next_prop;
    }
    return false;
}

static JsVal rt_env_get(JsRuntime* rt, int env_id, const char* key) {
    int e = env_id;
    int guard = 0;
    while (e >= 0 && guard++ < 64) {
        JsVal v;
        if (rt_prop_list_get(rt, rt->envs[e].first_prop, key, &v)) return v;
        e = rt->envs[e].parent_env;
    }
    return jv_undef();
}

static void rt_env_set_existing_or_global(JsRuntime* rt, int env_id, const char* key, JsVal val) {
    int e = env_id;
    int guard = 0;
    while (e >= 0 && guard++ < 64) {
        int p = rt->envs[e].first_prop;
        while (p >= 0) {
            if (js_streq(rt_get_str(rt, rt->props[p].key_str), key)) {
                rt->props[p].val = val;
                return;
            }
            p = rt->props[p].next_prop;
        }
        e = rt->envs[e].parent_env;
    }
    rt_prop_list_set(rt, &rt->envs[rt->global_env].first_prop, key, val);
}

static void rt_env_define(JsRuntime* rt, int env_id, const char* key, JsVal val) {
    if (env_id < 0 || env_id >= rt->env_len) env_id = rt->global_env;
    rt_prop_list_set(rt, &rt->envs[env_id].first_prop, key, val);
}

static bool js_to_bool(JsRuntime* rt, JsVal v) {
    switch (v.type) {
        case JVT_UNDEF:
        case JVT_NULL: return false;
        case JVT_BOOL: return v.u.b;
        case JVT_NUM:  return !js_isnan(v.u.num) && v.u.num != 0.0;
        case JVT_STR:  return rt_get_str(rt, v.u.handle)[0] != 0;
        default:       return true;
    }
}

static double js_to_num(JsRuntime* rt, JsVal v) {
    switch (v.type) {
        case JVT_UNDEF: return 0.0 / 0.0;
        case JVT_NULL:  return 0.0;
        case JVT_BOOL:  return v.u.b ? 1.0 : 0.0;
        case JVT_NUM:   return v.u.num;
        case JVT_STR: {
            bool ok = false;
            double d = js_atod(rt_get_str(rt, v.u.handle), &ok);
            return ok ? d : (0.0 / 0.0);
        }
        default: return 0.0 / 0.0;
    }
}

static void js_val_to_cstr(JsRuntime* rt, JsVal v, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    switch (v.type) {
        case JVT_UNDEF: js_strcpy_max(out, "undefined", max_len); break;
        case JVT_NULL:  js_strcpy_max(out, "null", max_len); break;
        case JVT_BOOL:  js_strcpy_max(out, v.u.b ? "true" : "false", max_len); break;
        case JVT_NUM:   js_dtoa(v.u.num, out, max_len); break;
        case JVT_STR:   js_strcpy_max(out, rt_get_str(rt, v.u.handle), max_len); break;
        case JVT_FUNC:  js_strcpy_max(out, "[Function]", max_len); break;
        case JVT_DOM: {
            JsDomNode* n = js_dom_get_node(rt, v.u.handle);
            js_strcpy_max(out, n ? "[Element <" : "[Element]", max_len);
            if (n) {
                js_strappend_max(out, n->tag, max_len);
                js_strappend_max(out, ">]", max_len);
            }
            break;
        }
        case JVT_ARR: {
            if (v.u.handle < 0 || v.u.handle >= rt->arr_len) {
                js_strcpy_max(out, "[]", max_len);
                break;
            }
            JsArr* a = &rt->arrs[v.u.handle];
            out[0] = 0;
            for (int i = 0; i < a->len; i++) {
                if (i > 0) js_strappend_max(out, ",", max_len);
                char item[96];
                if (a->data[i].type == JVT_ARR) js_strcpy_max(item, "[Array]", sizeof(item));
                else js_val_to_cstr(rt, a->data[i], item, sizeof(item));
                js_strappend_max(out, item, max_len);
            }
            break;
        }
        case JVT_OBJ:   js_strcpy_max(out, "[object Object]", max_len); break;
        default:        js_strcpy_max(out, "undefined", max_len); break;
    }
}

static int js_to_str_h(JsRuntime* rt, JsVal v) {
    if (v.type == JVT_STR) return v.u.handle;
    char buf[512];
    js_val_to_cstr(rt, v, buf, sizeof(buf));
    return rt_intern_str(rt, buf);
}

static bool js_strict_eq(JsRuntime* rt, JsVal a, JsVal b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JVT_UNDEF:
        case JVT_NULL: return true;
        case JVT_BOOL: return a.u.b == b.u.b;
        case JVT_NUM:  return a.u.num == b.u.num;
        case JVT_STR:  return js_streq(rt_get_str(rt, a.u.handle), rt_get_str(rt, b.u.handle));
        default:       return a.u.handle == b.u.handle;
    }
}

static bool js_loose_eq(JsRuntime* rt, JsVal a, JsVal b) {
    if (a.type == b.type) return js_strict_eq(rt, a, b);
    if ((a.type == JVT_NULL && b.type == JVT_UNDEF) ||
        (a.type == JVT_UNDEF && b.type == JVT_NULL)) return true;
    if (a.type == JVT_NUM || b.type == JVT_NUM || a.type == JVT_BOOL || b.type == JVT_BOOL) {
        return js_to_num(rt, a) == js_to_num(rt, b);
    }
    return false;
}

// Forward declarations for evaluator & method dispatch
static JsVal eval_ast(JsRuntime* rt, int ast_id, int env_id);
static JsVal call_js_value(JsRuntime* rt, JsVal fn_val, JsVal this_val, const JsVal* args, int argc);
static JsVal js_get_prop(JsRuntime* rt, JsVal obj, const char* key);
static void js_set_prop(JsRuntime* rt, JsVal obj, const char* key, JsVal val);

// ============================================================================
// Mini DOM Tree Helpers & innerHTML Parser
// ============================================================================

static void dom_attach_child(JsRuntime* rt, int parent_id, int child_id) {
    if (!rt || parent_id < 0 || parent_id >= rt->node_len || child_id < 0 || child_id >= rt->node_len) return;
    if (parent_id == child_id) return;
    JsDomNode* child = &rt->nodes[child_id];
    child->parent_id = parent_id;
    child->next_sibling = -1;
    JsDomNode* parent = &rt->nodes[parent_id];
    if (parent->first_child < 0) {
        parent->first_child = child_id;
    } else {
        int cur = parent->first_child;
        int guard = 0;
        while (rt->nodes[cur].next_sibling >= 0 && guard++ < JS_MAX_DOM_NODES) {
            cur = rt->nodes[cur].next_sibling;
        }
        rt->nodes[cur].next_sibling = child_id;
    }
    rt->dom_dirty = true;
}

static void dom_set_inner_html(JsRuntime* rt, int node_id, const char* html) {
    JsDomNode* n = js_dom_get_node(rt, node_id);
    if (!n || !html) return;

    // Clear existing child chain cleanly and rebuild simple elements/text from html snippet
    n->first_child = -1;
    n->text[0] = 0;

    char plain[512];
    int p_len = 0;
    const char* s = html;
    while (*s) {
        if (*s == '<') {
            if (p_len > 0) {
                plain[p_len] = 0;
                if (!n->text[0]) js_strcpy_max(n->text, plain, sizeof(n->text));
                p_len = 0;
            }
            s++;
            bool closing = (*s == '/');
            if (closing) s++;
            char tag[16]; int ti = 0;
            while (*s && *s != ' ' && *s != '>' && *s != '/' && ti < 15) {
                tag[ti++] = js_tolower(*s++);
            }
            tag[ti] = 0;

            char id_val[48] = {0}, cls_val[64] = {0}, onclick_val[256] = {0};
            while (*s && *s != '>') {
                while (*s == ' ') s++;
                char aname[24]; int ai = 0;
                while (*s && *s != '=' && *s != ' ' && *s != '>' && ai < 23) aname[ai++] = js_tolower(*s++);
                aname[ai] = 0;
                if (*s == '=') {
                    s++;
                    char q = 0;
                    if (*s == '\'' || *s == '"') q = *s++;
                    char aval[256]; int vi = 0;
                    while (*s && ((q && *s != q) || (!q && *s != ' ' && *s != '>')) && vi < 255) aval[vi++] = *s++;
                    aval[vi] = 0;
                    if (q && *s == q) s++;
                    if (js_streq(aname, "id")) js_strcpy_max(id_val, aval, sizeof(id_val));
                    else if (js_streq(aname, "class")) js_strcpy_max(cls_val, aval, sizeof(cls_val));
                    else if (js_streq(aname, "onclick")) js_strcpy_max(onclick_val, aval, sizeof(onclick_val));
                } else if (*s && *s != '>') s++;
            }
            if (*s == '>') s++;

            if (!closing && tag[0]) {
                if (js_streq(tag, "br")) {
                    if (p_len < 509) plain[p_len++] = '\n';
                } else if (js_streq(tag, "button") || js_streq(tag, "div") || js_streq(tag, "p") ||
                           js_streq(tag, "span") || js_streq(tag, "li") || js_streq(tag, "a") ||
                           js_streq(tag, "h1") || js_streq(tag, "h2") || js_streq(tag, "h3")) {
                    int cid = js_dom_create_element(rt, tag, node_id);
                    if (cid >= 0) {
                        JsDomNode* cn = &rt->nodes[cid];
                        if (id_val[0]) js_strcpy_max(cn->elem_id, id_val, sizeof(cn->elem_id));
                        if (cls_val[0]) js_strcpy_max(cn->class_name, cls_val, sizeof(cn->class_name));
                        if (onclick_val[0]) js_strcpy_max(cn->onclick_src, onclick_val, sizeof(cn->onclick_src));
                        // Capture immediate inner text up to matching closing tag
                        int ti2 = 0;
                        while (*s && *s != '<' && ti2 < (int)sizeof(cn->text) - 1) {
                            cn->text[ti2++] = *s++;
                        }
                        cn->text[ti2] = 0;
                    }
                }
            }
        } else {
            if (p_len < (int)sizeof(plain) - 1) plain[p_len++] = *s;
            s++;
        }
    }
    if (p_len > 0) {
        plain[p_len] = 0;
        if (!n->text[0]) js_strcpy_max(n->text, plain, sizeof(n->text));
    }
    rt->dom_dirty = true;
}

static bool dom_matches_selector(const JsDomNode* n, const char* sel) {
    if (!n || !sel || !sel[0]) return false;
    if (sel[0] == '#') return js_streq(n->elem_id, sel + 1);
    if (sel[0] == '.') {
        const char* cls = n->class_name;
        const char* needle = sel + 1;
        int nl = js_strlen(needle);
        for (int i = 0; cls[i]; i++) {
            if ((i == 0 || cls[i - 1] == ' ') && js_streq_n(cls + i, needle, nl) &&
                (cls[i + nl] == 0 || cls[i + nl] == ' ')) {
                return true;
            }
        }
        return false;
    }
    return js_strcaseeq(n->tag, sel);
}

// ============================================================================
// Lexer (supports comments, floats, hex, escapes, template literals `${}`)
// ============================================================================

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static bool is_ident_part(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static uint8_t match_keyword(const char* s, int len) {
    if (js_streq_n(s, "let", len)) return JT_LET;
    if (js_streq_n(s, "const", len)) return JT_CONST;
    if (js_streq_n(s, "var", len)) return JT_VAR;
    if (js_streq_n(s, "function", len)) return JT_FUNCTION;
    if (js_streq_n(s, "return", len)) return JT_RETURN;
    if (js_streq_n(s, "if", len)) return JT_IF;
    if (js_streq_n(s, "else", len)) return JT_ELSE;
    if (js_streq_n(s, "while", len)) return JT_WHILE;
    if (js_streq_n(s, "for", len)) return JT_FOR;
    if (js_streq_n(s, "do", len)) return JT_DO;
    if (js_streq_n(s, "break", len)) return JT_BREAK;
    if (js_streq_n(s, "continue", len)) return JT_CONTINUE;
    if (js_streq_n(s, "true", len)) return JT_TRUE;
    if (js_streq_n(s, "false", len)) return JT_FALSE;
    if (js_streq_n(s, "null", len)) return JT_NULL;
    if (js_streq_n(s, "undefined", len)) return JT_UNDEFINED;
    if (js_streq_n(s, "typeof", len)) return JT_TYPEOF;
    if (js_streq_n(s, "new", len)) return JT_NEW;
    if (js_streq_n(s, "this", len)) return JT_THIS;
    if (js_streq_n(s, "try", len)) return JT_TRY;
    if (js_streq_n(s, "catch", len)) return JT_CATCH;
    if (js_streq_n(s, "finally", len)) return JT_FINALLY;
    if (js_streq_n(s, "throw", len)) return JT_THROW;
    if (js_streq_n(s, "in", len)) return JT_IN;
    if (js_streq_n(s, "of", len)) return JT_OF;
    return JT_IDENT;
}

static int js_tokenize(JsRuntime* rt, const char* src, JsTok* toks, int max_toks) {
    int count = 0;
    int line = 1;
    int tpl_depth = 0;
    int brace_stack[16];
    for (int i = 0; i < 16; i++) brace_stack[i] = 0;

    auto emit = [&](uint8_t k, int32_t sh, double num) {
        if (count < max_toks - 1) {
            toks[count].kind = k;
            toks[count].str_h = sh;
            toks[count].num = num;
            toks[count].line = line;
            count++;
        }
    };

    const char* p = src ? src : "";
    while (*p && count < max_toks - 1) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
        if (c == '\n') { line++; p++; continue; }

        // Comments
        if (c == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p += 2;
            continue;
        }

        // Numbers
        if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
            char nbuf[64]; int ni = 0;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                nbuf[ni++] = *p++; nbuf[ni++] = *p++;
                while (((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) && ni < 60) {
                    nbuf[ni++] = *p++;
                }
            } else {
                while (*p >= '0' && *p <= '9' && ni < 60) nbuf[ni++] = *p++;
                if (*p == '.' && ni < 60) {
                    nbuf[ni++] = *p++;
                    while (*p >= '0' && *p <= '9' && ni < 60) nbuf[ni++] = *p++;
                }
                if ((*p == 'e' || *p == 'E') && ni < 56) {
                    nbuf[ni++] = *p++;
                    if (*p == '+' || *p == '-') nbuf[ni++] = *p++;
                    while (*p >= '0' && *p <= '9' && ni < 60) nbuf[ni++] = *p++;
                }
            }
            nbuf[ni] = 0;
            bool ok = false;
            double dv = js_atod(nbuf, &ok);
            emit(JT_NUM, 0, dv);
            continue;
        }

        // Strings '...' or "..."
        if (c == '\'' || c == '"') {
            char q = *p++;
            char sbuf[512]; int si = 0;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    p++;
                    char ec = *p++;
                    if (ec == 'n') ec = '\n';
                    else if (ec == 't') ec = '\t';
                    else if (ec == 'r') ec = '\r';
                    if (si < 510) sbuf[si++] = ec;
                } else {
                    if (*p == '\n') line++;
                    if (si < 510) sbuf[si++] = *p++;
                    else p++;
                }
            }
            if (*p == q) p++;
            sbuf[si] = 0;
            emit(JT_STR, rt_intern_str_n(rt, sbuf, si), 0.0);
            continue;
        }

        // Template string start `...` or continuation } ... `
        bool is_tpl_cont = (c == '}' && tpl_depth > 0 && brace_stack[tpl_depth - 1] == 0);
        if (c == '`' || is_tpl_cont) {
            bool started = (c == '`');
            p++;
            char sbuf[512]; int si = 0;
            bool hit_interp = false;
            while (*p && *p != '`') {
                if (p[0] == '$' && p[1] == '{') {
                    hit_interp = true;
                    p += 2;
                    break;
                }
                if (*p == '\\' && p[1]) {
                    p++;
                    char ec = *p++;
                    if (ec == 'n') ec = '\n';
                    else if (ec == 't') ec = '\t';
                    if (si < 510) sbuf[si++] = ec;
                } else {
                    if (*p == '\n') line++;
                    if (si < 510) sbuf[si++] = *p++;
                    else p++;
                }
            }
            if (!hit_interp && *p == '`') p++;
            sbuf[si] = 0;
            int sh = rt_intern_str_n(rt, sbuf, si);
            if (started) {
                if (hit_interp) {
                    if (tpl_depth < 15) brace_stack[tpl_depth++] = 0;
                    emit(JT_TPL_HEAD, sh, 0.0);
                } else {
                    emit(JT_TPL_FULL, sh, 0.0);
                }
            } else {
                if (hit_interp) {
                    emit(JT_TPL_MID, sh, 0.0);
                } else {
                    tpl_depth--;
                    emit(JT_TPL_TAIL, sh, 0.0);
                }
            }
            continue;
        }

        // Identifiers / Keywords
        if (is_ident_start(c)) {
            const char* start = p;
            p++;
            while (is_ident_part(*p)) p++;
            int len = (int)(p - start);
            uint8_t kw = match_keyword(start, len);
            int32_t sh = (kw == JT_IDENT) ? rt_intern_str_n(rt, start, len) : 0;
            emit(kw, sh, 0.0);
            continue;
        }

        // Multi-char operators
        if (c == '=' && p[1] == '=' && p[2] == '=') { p += 3; emit(JT_EQEQEQ, 0, 0); continue; }
        if (c == '!' && p[1] == '=' && p[2] == '=') { p += 3; emit(JT_NEQEQ, 0, 0); continue; }
        if (c == '=' && p[1] == '=') { p += 2; emit(JT_EQEQ, 0, 0); continue; }
        if (c == '!' && p[1] == '=') { p += 2; emit(JT_NEQ, 0, 0); continue; }
        if (c == '=' && p[1] == '>') { p += 2; emit(JT_ARROW, 0, 0); continue; }
        if (c == '<' && p[1] == '<') { p += 2; emit(JT_SHL, 0, 0); continue; }
        if (c == '>' && p[1] == '>') { p += 2; emit(JT_SHR, 0, 0); continue; }
        if (c == '<' && p[1] == '=') { p += 2; emit(JT_LE, 0, 0); continue; }
        if (c == '>' && p[1] == '=') { p += 2; emit(JT_GE, 0, 0); continue; }
        if (c == '&' && p[1] == '&') { p += 2; emit(JT_ANDAND, 0, 0); continue; }
        if (c == '|' && p[1] == '|') { p += 2; emit(JT_OROR, 0, 0); continue; }
        if (c == '?' && p[1] == '?') { p += 2; emit(JT_QQ, 0, 0); continue; }
        if (c == '+' && p[1] == '+') { p += 2; emit(JT_PLUSPLUS, 0, 0); continue; }
        if (c == '-' && p[1] == '-') { p += 2; emit(JT_MINUSMINUS, 0, 0); continue; }
        if (c == '+' && p[1] == '=') { p += 2; emit(JT_PLUSEQ, 0, 0); continue; }
        if (c == '-' && p[1] == '=') { p += 2; emit(JT_MINUSEQ, 0, 0); continue; }
        if (c == '*' && p[1] == '=') { p += 2; emit(JT_STAREQ, 0, 0); continue; }
        if (c == '/' && p[1] == '=') { p += 2; emit(JT_SLASHEQ, 0, 0); continue; }
        if (c == '%' && p[1] == '=') { p += 2; emit(JT_PCTEQ, 0, 0); continue; }

        // Single-char punctuation
        p++;
        switch (c) {
            case '(': emit(JT_LPAREN, 0, 0); break;
            case ')': emit(JT_RPAREN, 0, 0); break;
            case '{': if (tpl_depth > 0) brace_stack[tpl_depth - 1]++; emit(JT_LBRACE, 0, 0); break;
            case '}': if (tpl_depth > 0 && brace_stack[tpl_depth - 1] > 0) brace_stack[tpl_depth - 1]--; emit(JT_RBRACE, 0, 0); break;
            case '[': emit(JT_LBRACK, 0, 0); break;
            case ']': emit(JT_RBRACK, 0, 0); break;
            case '.': emit(JT_DOT, 0, 0); break;
            case ',': emit(JT_COMMA, 0, 0); break;
            case ':': emit(JT_COLON, 0, 0); break;
            case ';': emit(JT_SEMI, 0, 0); break;
            case '?': emit(JT_QMARK, 0, 0); break;
            case '+': emit(JT_PLUS, 0, 0); break;
            case '-': emit(JT_MINUS, 0, 0); break;
            case '*': emit(JT_STAR, 0, 0); break;
            case '/': emit(JT_SLASH, 0, 0); break;
            case '%': emit(JT_PCT, 0, 0); break;
            case '<': emit(JT_LT, 0, 0); break;
            case '>': emit(JT_GT, 0, 0); break;
            case '=': emit(JT_EQ, 0, 0); break;
            case '!': emit(JT_BANG, 0, 0); break;
            case '&': emit(JT_BITAND, 0, 0); break;
            case '|': emit(JT_BITOR, 0, 0); break;
            case '^': emit(JT_BITXOR, 0, 0); break;
            default: break;
        }
    }
    emit(JT_EOF, 0, 0);
    return count;
}

// ============================================================================
// Recursive-Descent Parser
// ============================================================================

typedef struct {
    JsRuntime* rt;
    JsTok* toks;
    int count;
    int pos;
    bool had_error;
} JsParser;

static int ast_new(JsParser* p, uint8_t kind) {
    JsRuntime* rt = p->rt;
    if (rt->ast_len >= JS_AST_CAP) {
        p->had_error = true;
        return 0;
    }
    int id = rt->ast_len++;
    JsAstNode* n = &rt->ast[id];
    uefi_memset(n, 0, sizeof(JsAstNode));
    n->kind = kind;
    n->a = -1;
    n->left = -1;
    n->right = -1;
    n->next_sibling = -1;
    return id;
}

static inline JsTok* p_peek(JsParser* p, int off) {
    int idx = p->pos + off;
    if (idx < 0 || idx >= p->count) return &p->toks[p->count];
    return &p->toks[idx];
}
static inline uint8_t p_kind(JsParser* p) { return p_peek(p, 0)->kind; }
static inline bool p_match(JsParser* p, uint8_t k) {
    if (p_kind(p) == k) { p->pos++; return true; }
    return false;
}
static void p_syntax_err(JsParser* p, const char* msg) {
    if (!p->had_error) {
        p->had_error = true;
        rt_set_error(p->rt, msg);
    }
}

static int parse_expr(JsParser* p);
static int parse_assign(JsParser* p);
static int parse_stmt(JsParser* p);
static int parse_block(JsParser* p);

static bool looks_like_arrow_params(JsParser* p) {
    if (p_kind(p) == JT_IDENT && p_peek(p, 1)->kind == JT_ARROW) return true;
    if (p_kind(p) != JT_LPAREN) return false;
    int off = 1;
    int depth = 1;
    while (p->pos + off < p->count && depth > 0) {
        uint8_t k = p_peek(p, off)->kind;
        if (k == JT_LPAREN) depth++;
        else if (k == JT_RPAREN) depth--;
        off++;
    }
    return (depth == 0 && p_peek(p, off)->kind == JT_ARROW);
}

static int parse_arrow_fn(JsParser* p) {
    JsRuntime* rt = p->rt;
    if (rt->fn_tmpl_len >= JS_FN_TMPL_CAP) { p_syntax_err(p, "[JS Syntax] Too many functions"); return -1; }
    int tidx = rt->fn_tmpl_len++;
    JsFuncTmpl* tmpl = &rt->fn_tmpls[tidx];
    tmpl->param_count = 0;
    tmpl->name_str = 0;

    if (p_kind(p) == JT_IDENT) {
        tmpl->params[tmpl->param_count++] = p_peek(p, 0)->str_h;
        p->pos++;
    } else {
        p_match(p, JT_LPAREN);
        while (p_kind(p) != JT_RPAREN && p_kind(p) != JT_EOF) {
            if (p_kind(p) == JT_IDENT && tmpl->param_count < 8) {
                tmpl->params[tmpl->param_count++] = p_peek(p, 0)->str_h;
            }
            p->pos++;
            p_match(p, JT_COMMA);
        }
        p_match(p, JT_RPAREN);
    }
    p_match(p, JT_ARROW);

    if (p_kind(p) == JT_LBRACE) {
        tmpl->body_ast = parse_block(p);
    } else {
        int ret = ast_new(p, JA_RETURN);
        rt->ast[ret].left = parse_assign(p);
        int blk = ast_new(p, JA_BLOCK);
        rt->ast[blk].left = ret;
        tmpl->body_ast = blk;
    }

    int n = ast_new(p, JA_FN_EXPR);
    rt->ast[n].a = tidx;
    return n;
}

static int parse_func_body_node(JsParser* p, int name_str, bool is_decl) {
    JsRuntime* rt = p->rt;
    if (rt->fn_tmpl_len >= JS_FN_TMPL_CAP) { p_syntax_err(p, "[JS Syntax] Too many functions"); return -1; }
    int tidx = rt->fn_tmpl_len++;
    JsFuncTmpl* tmpl = &rt->fn_tmpls[tidx];
    tmpl->param_count = 0;
    tmpl->name_str = name_str;

    p_match(p, JT_LPAREN);
    while (p_kind(p) != JT_RPAREN && p_kind(p) != JT_EOF) {
        if (p_kind(p) == JT_IDENT && tmpl->param_count < 8) {
            tmpl->params[tmpl->param_count++] = p_peek(p, 0)->str_h;
        }
        p->pos++;
        p_match(p, JT_COMMA);
    }
    p_match(p, JT_RPAREN);
    tmpl->body_ast = parse_block(p);

    int n = ast_new(p, is_decl ? JA_FN_DECL : JA_FN_EXPR);
    rt->ast[n].a = tidx;
    rt->ast[n].str_h = name_str;
    return n;
}

static int parse_primary(JsParser* p) {
    JsRuntime* rt = p->rt;
    if (looks_like_arrow_params(p)) return parse_arrow_fn(p);

    uint8_t k = p_kind(p);
    if (k == JT_NUM) {
        int n = ast_new(p, JA_LIT);
        rt->ast[n].lit = jv_num(p_peek(p, 0)->num);
        p->pos++;
        return n;
    }
    if (k == JT_STR || k == JT_TPL_FULL) {
        int n = ast_new(p, JA_LIT);
        rt->ast[n].lit = jv_str(p_peek(p, 0)->str_h);
        p->pos++;
        return n;
    }
    if (k == JT_TPL_HEAD) {
        int acc = ast_new(p, JA_LIT);
        rt->ast[acc].lit = jv_str(p_peek(p, 0)->str_h);
        p->pos++;
        for (;;) {
            int e = parse_expr(p);
            int add1 = ast_new(p, JA_BINARY);
            rt->ast[add1].op = JT_PLUS;
            rt->ast[add1].left = acc;
            rt->ast[add1].right = e;
            acc = add1;

            if (p_kind(p) == JT_TPL_MID) {
                int s = ast_new(p, JA_LIT);
                rt->ast[s].lit = jv_str(p_peek(p, 0)->str_h);
                p->pos++;
                int add2 = ast_new(p, JA_BINARY);
                rt->ast[add2].op = JT_PLUS;
                rt->ast[add2].left = acc;
                rt->ast[add2].right = s;
                acc = add2;
            } else if (p_kind(p) == JT_TPL_TAIL) {
                int s = ast_new(p, JA_LIT);
                rt->ast[s].lit = jv_str(p_peek(p, 0)->str_h);
                p->pos++;
                int add2 = ast_new(p, JA_BINARY);
                rt->ast[add2].op = JT_PLUS;
                rt->ast[add2].left = acc;
                rt->ast[add2].right = s;
                acc = add2;
                break;
            } else {
                p_syntax_err(p, "[JS Syntax] Unterminated template literal");
                break;
            }
        }
        return acc;
    }
    if (k == JT_TRUE || k == JT_FALSE) {
        int n = ast_new(p, JA_LIT);
        rt->ast[n].lit = jv_bool(k == JT_TRUE);
        p->pos++;
        return n;
    }
    if (k == JT_NULL) {
        int n = ast_new(p, JA_LIT);
        rt->ast[n].lit = jv_null();
        p->pos++;
        return n;
    }
    if (k == JT_UNDEFINED) {
        int n = ast_new(p, JA_LIT);
        rt->ast[n].lit = jv_undef();
        p->pos++;
        return n;
    }
    if (k == JT_THIS) {
        p->pos++;
        return ast_new(p, JA_THIS);
    }
    if (k == JT_IDENT) {
        int n = ast_new(p, JA_IDENT);
        rt->ast[n].str_h = p_peek(p, 0)->str_h;
        p->pos++;
        return n;
    }
    if (k == JT_FUNCTION) {
        p->pos++;
        int nm = 0;
        if (p_kind(p) == JT_IDENT) {
            nm = p_peek(p, 0)->str_h;
            p->pos++;
        }
        return parse_func_body_node(p, nm, false);
    }
    if (k == JT_LPAREN) {
        p->pos++;
        int inner = parse_expr(p);
        p_match(p, JT_RPAREN);
        return inner;
    }
    if (k == JT_LBRACK) {
        p->pos++;
        int arr = ast_new(p, JA_ARRAY);
        int head = -1, tail = -1;
        while (p_kind(p) != JT_RBRACK && p_kind(p) != JT_EOF) {
            int el = parse_assign(p);
            if (head < 0) head = tail = el;
            else { rt->ast[tail].next_sibling = el; tail = el; }
            if (!p_match(p, JT_COMMA)) break;
        }
        p_match(p, JT_RBRACK);
        rt->ast[arr].left = head;
        return arr;
    }
    if (k == JT_LBRACE) {
        p->pos++;
        int obj = ast_new(p, JA_OBJECT);
        int head = -1, tail = -1;
        while (p_kind(p) != JT_RBRACE && p_kind(p) != JT_EOF) {
            int key_h = 0;
            if (p_kind(p) == JT_IDENT || p_kind(p) == JT_STR) {
                key_h = p_peek(p, 0)->str_h;
                p->pos++;
            } else if (p_kind(p) == JT_NUM) {
                char nb[32]; js_dtoa(p_peek(p, 0)->num, nb, sizeof(nb));
                key_h = rt_intern_str(rt, nb);
                p->pos++;
            } else break;

            int val_expr = -1;
            if (p_match(p, JT_COLON)) {
                val_expr = parse_assign(p);
            } else if (p_kind(p) == JT_LPAREN) {
                val_expr = parse_func_body_node(p, key_h, false);
            } else {
                // ES6 shorthand { x } -> { x: x }
                val_expr = ast_new(p, JA_IDENT);
                rt->ast[val_expr].str_h = key_h;
            }
            int prop = ast_new(p, JA_PROP);
            rt->ast[prop].str_h = key_h;
            rt->ast[prop].left = val_expr;
            if (head < 0) head = tail = prop;
            else { rt->ast[tail].next_sibling = prop; tail = prop; }
            if (!p_match(p, JT_COMMA)) break;
        }
        p_match(p, JT_RBRACE);
        rt->ast[obj].left = head;
        return obj;
    }

    p_syntax_err(p, "[JS Syntax] Unexpected token");
    p->pos++;
    return ast_new(p, JA_LIT);
}

static int parse_postfix(JsParser* p) {
    JsRuntime* rt = p->rt;
    bool is_new = p_match(p, JT_NEW);
    int expr = parse_primary(p);

    for (;;) {
        if (p_match(p, JT_DOT)) {
            int mem = ast_new(p, JA_MEMBER);
            rt->ast[mem].left = expr;
            if (p_kind(p) == JT_IDENT || p_kind(p) >= JT_LET) {
                if (p_peek(p, 0)->str_h > 0) rt->ast[mem].str_h = p_peek(p, 0)->str_h;
                p->pos++;
            }
            expr = mem;
        } else if (p_match(p, JT_LBRACK)) {
            int idx = ast_new(p, JA_INDEX);
            rt->ast[idx].left = expr;
            rt->ast[idx].right = parse_expr(p);
            p_match(p, JT_RBRACK);
            expr = idx;
        } else if (p_match(p, JT_LPAREN)) {
            int call = ast_new(p, is_new ? JA_NEW : JA_CALL);
            is_new = false;
            rt->ast[call].left = expr;
            int head = -1, tail = -1;
            while (p_kind(p) != JT_RPAREN && p_kind(p) != JT_EOF) {
                int arg = parse_assign(p);
                if (head < 0) head = tail = arg;
                else { rt->ast[tail].next_sibling = arg; tail = arg; }
                if (!p_match(p, JT_COMMA)) break;
            }
            p_match(p, JT_RPAREN);
            rt->ast[call].right = head;
            expr = call;
        } else if (p_kind(p) == JT_PLUSPLUS || p_kind(p) == JT_MINUSMINUS) {
            uint8_t op = p_kind(p);
            p->pos++;
            int inc = ast_new(p, JA_INCDEC);
            rt->ast[inc].op = op;
            rt->ast[inc].a = 0; // postfix
            rt->ast[inc].left = expr;
            expr = inc;
        } else {
            break;
        }
    }
    if (is_new) {
        int call = ast_new(p, JA_NEW);
        rt->ast[call].left = expr;
        expr = call;
    }
    return expr;
}

static int parse_unary(JsParser* p) {
    JsRuntime* rt = p->rt;
    uint8_t k = p_kind(p);
    if (k == JT_BANG || k == JT_MINUS || k == JT_PLUS || k == JT_TYPEOF) {
        p->pos++;
        int u = ast_new(p, JA_UNARY);
        rt->ast[u].op = k;
        rt->ast[u].left = parse_unary(p);
        return u;
    }
    if (k == JT_PLUSPLUS || k == JT_MINUSMINUS) {
        p->pos++;
        int inc = ast_new(p, JA_INCDEC);
        rt->ast[inc].op = k;
        rt->ast[inc].a = 1; // prefix
        rt->ast[inc].left = parse_unary(p);
        return inc;
    }
    return parse_postfix(p);
}

static int bin_prec(uint8_t k) {
    switch (k) {
        case JT_QQ:     return 1;
        case JT_OROR:   return 2;
        case JT_ANDAND: return 3;
        case JT_BITOR:  return 4;
        case JT_BITXOR: return 5;
        case JT_BITAND: return 6;
        case JT_EQEQ: case JT_EQEQEQ: case JT_NEQ: case JT_NEQEQ: return 7;
        case JT_LT: case JT_LE: case JT_GT: case JT_GE: case JT_IN: return 8;
        case JT_SHL: case JT_SHR: return 9;
        case JT_PLUS: case JT_MINUS: return 10;
        case JT_STAR: case JT_SLASH: case JT_PCT: return 11;
        default: return 0;
    }
}

static int parse_binary(JsParser* p, int min_prec) {
    JsRuntime* rt = p->rt;
    int left = parse_unary(p);
    for (;;) {
        uint8_t op = p_kind(p);
        int prec = bin_prec(op);
        if (prec < min_prec || prec == 0) break;
        p->pos++;
        int right = parse_binary(p, prec + 1);
        int b = ast_new(p, JA_BINARY);
        rt->ast[b].op = op;
        rt->ast[b].left = left;
        rt->ast[b].right = right;
        left = b;
    }
    return left;
}

static int parse_ternary(JsParser* p) {
    JsRuntime* rt = p->rt;
    int cond = parse_binary(p, 1);
    if (p_match(p, JT_QMARK)) {
        int t_node = ast_new(p, JA_TERNARY);
        rt->ast[t_node].left = cond;
        rt->ast[t_node].right = parse_assign(p);
        p_match(p, JT_COLON);
        rt->ast[t_node].a = parse_assign(p);
        return t_node;
    }
    return cond;
}

static int parse_assign(JsParser* p) {
    JsRuntime* rt = p->rt;
    if (looks_like_arrow_params(p)) return parse_arrow_fn(p);
    int left = parse_ternary(p);
    uint8_t k = p_kind(p);
    if (k == JT_EQ || k == JT_PLUSEQ || k == JT_MINUSEQ || k == JT_STAREQ || k == JT_SLASHEQ || k == JT_PCTEQ) {
        p->pos++;
        int a = ast_new(p, JA_ASSIGN);
        rt->ast[a].op = k;
        rt->ast[a].left = left;
        rt->ast[a].right = parse_assign(p);
        return a;
    }
    return left;
}

static int parse_expr(JsParser* p) {
    return parse_assign(p);
}

static int parse_var_decl(JsParser* p) {
    JsRuntime* rt = p->rt;
    p->pos++; // skip let/const/var
    int decl = ast_new(p, JA_VAR_DECL);
    int head = -1, tail = -1;
    for (;;) {
        if (p_kind(p) != JT_IDENT) break;
        int name_h = p_peek(p, 0)->str_h;
        p->pos++;
        int init = -1;
        if (p_match(p, JT_EQ)) {
            init = parse_assign(p);
        }
        int item = ast_new(p, JA_PROP);
        rt->ast[item].str_h = name_h;
        rt->ast[item].left = init;
        if (head < 0) head = tail = item;
        else { rt->ast[tail].next_sibling = item; tail = item; }
        if (!p_match(p, JT_COMMA)) break;
    }
    rt->ast[decl].left = head;
    p_match(p, JT_SEMI);
    return decl;
}

static int parse_block(JsParser* p) {
    JsRuntime* rt = p->rt;
    p_match(p, JT_LBRACE);
    int blk = ast_new(p, JA_BLOCK);
    int head = -1, tail = -1;
    while (p_kind(p) != JT_RBRACE && p_kind(p) != JT_EOF && !p->had_error) {
        int s = parse_stmt(p);
        if (head < 0) head = tail = s;
        else { rt->ast[tail].next_sibling = s; tail = s; }
    }
    p_match(p, JT_RBRACE);
    rt->ast[blk].left = head;
    return blk;
}

static int parse_stmt(JsParser* p) {
    JsRuntime* rt = p->rt;
    while (p_match(p, JT_SEMI)) {}
    uint8_t k = p_kind(p);
    if (k == JT_LBRACE) return parse_block(p);
    if (k == JT_LET || k == JT_CONST || k == JT_VAR) return parse_var_decl(p);
    if (k == JT_FUNCTION) {
        p->pos++;
        int nm = 0;
        if (p_kind(p) == JT_IDENT) { nm = p_peek(p, 0)->str_h; p->pos++; }
        return parse_func_body_node(p, nm, true);
    }
    if (k == JT_IF) {
        p->pos++;
        p_match(p, JT_LPAREN);
        int cond = parse_expr(p);
        p_match(p, JT_RPAREN);
        int then_s = parse_stmt(p);
        int else_s = -1;
        if (p_match(p, JT_ELSE)) else_s = parse_stmt(p);
        int n = ast_new(p, JA_IF);
        rt->ast[n].left = cond;
        rt->ast[n].right = then_s;
        rt->ast[n].a = else_s;
        return n;
    }
    if (k == JT_WHILE) {
        p->pos++;
        p_match(p, JT_LPAREN);
        int cond = parse_expr(p);
        p_match(p, JT_RPAREN);
        int body = parse_stmt(p);
        int n = ast_new(p, JA_WHILE);
        rt->ast[n].left = cond;
        rt->ast[n].right = body;
        rt->ast[n].a = 0;
        return n;
    }
    if (k == JT_FOR) {
        p->pos++;
        p_match(p, JT_LPAREN);
        // Check for..of / for..in: for ([let|const|var] id of/in expr)
        int look = 0;
        if (p_peek(p, 0)->kind == JT_LET || p_peek(p, 0)->kind == JT_CONST || p_peek(p, 0)->kind == JT_VAR) look = 1;
        if (p_peek(p, look)->kind == JT_IDENT &&
            (p_peek(p, look + 1)->kind == JT_OF || p_peek(p, look + 1)->kind == JT_IN)) {
            if (look == 1) p->pos++;
            int var_h = p_peek(p, 0)->str_h; p->pos++;
            bool is_of = (p_kind(p) == JT_OF); p->pos++;
            int iter = parse_expr(p);
            p_match(p, JT_RPAREN);
            int body = parse_stmt(p);
            int n = ast_new(p, JA_FOR_ITER);
            rt->ast[n].a = is_of ? 1 : 0;
            rt->ast[n].str_h = var_h;
            rt->ast[n].left = iter;
            rt->ast[n].right = body;
            return n;
        }
        int init = -1;
        if (p_kind(p) == JT_LET || p_kind(p) == JT_CONST || p_kind(p) == JT_VAR) init = parse_var_decl(p);
        else {
            if (p_kind(p) != JT_SEMI) init = parse_expr(p);
            p_match(p, JT_SEMI);
        }
        int cond = -1;
        if (p_kind(p) != JT_SEMI) cond = parse_expr(p);
        p_match(p, JT_SEMI);
        int step = -1;
        if (p_kind(p) != JT_RPAREN) step = parse_expr(p);
        p_match(p, JT_RPAREN);
        int body = parse_stmt(p);
        int n = ast_new(p, JA_FOR);
        rt->ast[n].left = init;
        rt->ast[n].right = cond;
        rt->ast[n].a = step;
        rt->ast[n].str_h = body; // reuse str_h slot cleanly as body ast idx
        return n;
    }
    if (k == JT_RETURN) {
        p->pos++;
        int val = -1;
        if (p_kind(p) != JT_SEMI && p_kind(p) != JT_RBRACE && p_kind(p) != JT_EOF) {
            val = parse_expr(p);
        }
        p_match(p, JT_SEMI);
        int n = ast_new(p, JA_RETURN);
        rt->ast[n].left = val;
        return n;
    }
    if (k == JT_BREAK) { p->pos++; p_match(p, JT_SEMI); return ast_new(p, JA_BREAK); }
    if (k == JT_CONTINUE) { p->pos++; p_match(p, JT_SEMI); return ast_new(p, JA_CONTINUE); }
    if (k == JT_THROW) {
        p->pos++;
        int v = parse_expr(p);
        p_match(p, JT_SEMI);
        int n = ast_new(p, JA_THROW);
        rt->ast[n].left = v;
        return n;
    }
    if (k == JT_TRY) {
        p->pos++;
        int try_b = parse_block(p);
        int catch_b = -1, fin_b = -1, catch_var = 0;
        if (p_match(p, JT_CATCH)) {
            if (p_match(p, JT_LPAREN)) {
                if (p_kind(p) == JT_IDENT) { catch_var = p_peek(p, 0)->str_h; p->pos++; }
                p_match(p, JT_RPAREN);
            }
            catch_b = parse_block(p);
        }
        if (p_match(p, JT_FINALLY)) fin_b = parse_block(p);
        int n = ast_new(p, JA_TRY);
        rt->ast[n].left = try_b;
        rt->ast[n].right = catch_b;
        rt->ast[n].a = fin_b;
        rt->ast[n].str_h = catch_var;
        return n;
    }

    int e = parse_expr(p);
    p_match(p, JT_SEMI);
    int es = ast_new(p, JA_EXPR_STMT);
    rt->ast[es].left = e;
    return es;
}

// ============================================================================
// Built-in Methods & DOM Property Bridge
// ============================================================================

static JsVal nb_arr_push(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || tv.u.handle < 0 || tv.u.handle >= rt->arr_len) return jv_num(0);
    for (int i = 0; i < argc; i++) rt_arr_push_raw(rt, tv.u.handle, args[i]);
    return jv_num((double)rt->arrs[tv.u.handle].len);
}
static JsVal nb_arr_pop(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)args; (void)argc;
    if (tv.type != JVT_ARR || tv.u.handle < 0 || tv.u.handle >= rt->arr_len) return jv_undef();
    JsArr* a = &rt->arrs[tv.u.handle];
    if (a->len <= 0) return jv_undef();
    return a->data[--a->len];
}
static JsVal nb_arr_join(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || tv.u.handle < 0 || tv.u.handle >= rt->arr_len) return jv_str(0);
    char sep[16] = ",";
    if (argc > 0 && args[0].type != JVT_UNDEF) js_val_to_cstr(rt, args[0], sep, sizeof(sep));
    JsArr* a = &rt->arrs[tv.u.handle];
    char out[512]; out[0] = 0;
    for (int i = 0; i < a->len; i++) {
        if (i > 0) js_strappend_max(out, sep, sizeof(out));
        char tmp[128];
        js_val_to_cstr(rt, a->data[i], tmp, sizeof(tmp));
        js_strappend_max(out, tmp, sizeof(out));
    }
    return jv_str(rt_intern_str(rt, out));
}
static JsVal nb_arr_includes(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || argc < 1 || tv.u.handle < 0 || tv.u.handle >= rt->arr_len) return jv_bool(false);
    JsArr* a = &rt->arrs[tv.u.handle];
    for (int i = 0; i < a->len; i++) if (js_strict_eq(rt, a->data[i], args[0])) return jv_bool(true);
    return jv_bool(false);
}
static JsVal nb_arr_foreach(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || argc < 1 || args[0].type != JVT_FUNC) return jv_undef();
    JsArr* a = &rt->arrs[tv.u.handle];
    for (int i = 0; i < a->len && rt->ctrl == CF_NONE; i++) {
        JsVal cargs[3] = { a->data[i], jv_num((double)i), tv };
        call_js_value(rt, args[0], jv_undef(), cargs, 3);
    }
    return jv_undef();
}
static JsVal nb_arr_map(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || argc < 1 || args[0].type != JVT_FUNC) return jv_undef();
    JsArr* a = &rt->arrs[tv.u.handle];
    int out_id = rt_alloc_arr(rt, a->len);
    for (int i = 0; i < a->len && rt->ctrl == CF_NONE; i++) {
        JsVal cargs[3] = { a->data[i], jv_num((double)i), tv };
        rt_arr_push_raw(rt, out_id, call_js_value(rt, args[0], jv_undef(), cargs, 3));
    }
    return jv_arr(out_id);
}
static JsVal nb_arr_filter(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_ARR || argc < 1 || args[0].type != JVT_FUNC) return jv_undef();
    JsArr* a = &rt->arrs[tv.u.handle];
    int out_id = rt_alloc_arr(rt, a->len);
    for (int i = 0; i < a->len && rt->ctrl == CF_NONE; i++) {
        JsVal cargs[3] = { a->data[i], jv_num((double)i), tv };
        if (js_to_bool(rt, call_js_value(rt, args[0], jv_undef(), cargs, 3))) {
            rt_arr_push_raw(rt, out_id, a->data[i]);
        }
    }
    return jv_arr(out_id);
}

static JsVal nb_str_upper(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)args; (void)argc;
    char buf[512]; js_val_to_cstr(rt, tv, buf, sizeof(buf));
    for (int i = 0; buf[i]; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    return jv_str(rt_intern_str(rt, buf));
}
static JsVal nb_str_lower(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)args; (void)argc;
    char buf[512]; js_val_to_cstr(rt, tv, buf, sizeof(buf));
    for (int i = 0; buf[i]; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;
    return jv_str(rt_intern_str(rt, buf));
}
static JsVal nb_str_trim(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)args; (void)argc;
    const char* s = rt_get_str(rt, tv.u.handle);
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char buf[512]; js_strcpy_max(buf, s, sizeof(buf));
    int n = js_strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return jv_str(rt_intern_str(rt, buf));
}
static JsVal nb_str_includes(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (argc < 1) return jv_bool(false);
    const char* hay = rt_get_str(rt, tv.u.handle);
    char nd[128]; js_val_to_cstr(rt, args[0], nd, sizeof(nd));
    if (!nd[0]) return jv_bool(true);
    int nl = js_strlen(nd);
    for (int i = 0; hay[i]; i++) {
        bool m = true;
        for (int j = 0; j < nl; j++) if (hay[i + j] != nd[j]) { m = false; break; }
        if (m) return jv_bool(true);
    }
    return jv_bool(false);
}
static JsVal nb_str_split(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    const char* s = rt_get_str(rt, tv.u.handle);
    char sep[32] = "";
    if (argc > 0) js_val_to_cstr(rt, args[0], sep, sizeof(sep));
    int arr_id = rt_alloc_arr(rt, 8);
    if (!sep[0]) {
        for (int i = 0; s[i]; i++) rt_arr_push_raw(rt, arr_id, jv_str(rt_intern_str_n(rt, s + i, 1)));
        return jv_arr(arr_id);
    }
    int sl = js_strlen(sep);
    const char* seg = s;
    while (*s) {
        if (js_streq_n(s, sep, sl)) {
            rt_arr_push_raw(rt, arr_id, jv_str(rt_intern_str_n(rt, seg, (int)(s - seg))));
            s += sl;
            seg = s;
        } else s++;
    }
    rt_arr_push_raw(rt, arr_id, jv_str(rt_intern_str(rt, seg)));
    return jv_arr(arr_id);
}
static JsVal nb_str_slice(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    const char* s = rt_get_str(rt, tv.u.handle);
    int len = js_strlen(s);
    int st = (argc > 0) ? (int)js_to_num(rt, args[0]) : 0;
    int en = (argc > 1) ? (int)js_to_num(rt, args[1]) : len;
    if (st < 0) st += len;
    if (en < 0) en += len;
    if (st < 0) st = 0;
    if (en > len) en = len;
    if (en <= st) return jv_str(0);
    return jv_str(rt_intern_str_n(rt, s + st, en - st));
}

static JsVal nb_dom_append(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_DOM || argc < 1 || args[0].type != JVT_DOM) return jv_undef();
    dom_attach_child(rt, tv.u.handle, args[0].u.handle);
    return args[0];
}
static JsVal nb_dom_set_attr(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_DOM || argc < 2) return jv_undef();
    JsDomNode* n = js_dom_get_node(rt, tv.u.handle);
    if (!n) return jv_undef();
    char k[32], v[256];
    js_val_to_cstr(rt, args[0], k, sizeof(k));
    js_val_to_cstr(rt, args[1], v, sizeof(v));
    if (js_streq(k, "id")) js_strcpy_max(n->elem_id, v, sizeof(n->elem_id));
    else if (js_streq(k, "class")) js_strcpy_max(n->class_name, v, sizeof(n->class_name));
    else if (js_streq(k, "href")) js_strcpy_max(n->href, v, sizeof(n->href));
    else if (js_streq(k, "value")) js_strcpy_max(n->text, v, sizeof(n->text));
    else if (js_streq(k, "onclick")) js_strcpy_max(n->onclick_src, v, sizeof(n->onclick_src));
    rt->dom_dirty = true;
    return jv_undef();
}
static JsVal nb_dom_add_listener(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    if (tv.type != JVT_DOM || argc < 2) return jv_undef();
    char ev[24]; js_val_to_cstr(rt, args[0], ev, sizeof(ev));
    JsDomNode* n = js_dom_get_node(rt, tv.u.handle);
    if (n && js_streq(ev, "click") && args[1].type == JVT_FUNC) {
        n->click_fn_handle = args[1].u.handle;
    }
    return jv_undef();
}

static int rt_reg_native(JsRuntime* rt, JsNativeFn fn) {
    if (rt->func_len >= JS_FUNC_CAP) return -1;
    int id = rt->func_len++;
    uefi_memset(&rt->funcs[id], 0, sizeof(JsFunc));
    rt->funcs[id].is_native = true;
    rt->funcs[id].native_fn = fn;
    return id;
}

static JsVal js_get_prop(JsRuntime* rt, JsVal obj, const char* key) {
    if (obj.type == JVT_ARR) {
        if (obj.u.handle < 0 || obj.u.handle >= rt->arr_len) return jv_undef();
        JsArr* a = &rt->arrs[obj.u.handle];
        if (js_streq(key, "length")) return jv_num((double)a->len);
        if (js_streq(key, "push")) return jv_fn(rt_reg_native(rt, nb_arr_push));
        if (js_streq(key, "pop")) return jv_fn(rt_reg_native(rt, nb_arr_pop));
        if (js_streq(key, "join")) return jv_fn(rt_reg_native(rt, nb_arr_join));
        if (js_streq(key, "includes")) return jv_fn(rt_reg_native(rt, nb_arr_includes));
        if (js_streq(key, "forEach")) return jv_fn(rt_reg_native(rt, nb_arr_foreach));
        if (js_streq(key, "map")) return jv_fn(rt_reg_native(rt, nb_arr_map));
        if (js_streq(key, "filter")) return jv_fn(rt_reg_native(rt, nb_arr_filter));
        bool ok = false;
        double d = js_atod(key, &ok);
        if (ok) {
            int idx = (int)d;
            if (idx >= 0 && idx < a->len) return a->data[idx];
        }
        return jv_undef();
    }
    if (obj.type == JVT_STR) {
        const char* s = rt_get_str(rt, obj.u.handle);
        if (js_streq(key, "length")) return jv_num((double)js_strlen(s));
        if (js_streq(key, "toUpperCase")) return jv_fn(rt_reg_native(rt, nb_str_upper));
        if (js_streq(key, "toLowerCase")) return jv_fn(rt_reg_native(rt, nb_str_lower));
        if (js_streq(key, "trim")) return jv_fn(rt_reg_native(rt, nb_str_trim));
        if (js_streq(key, "includes")) return jv_fn(rt_reg_native(rt, nb_str_includes));
        if (js_streq(key, "split")) return jv_fn(rt_reg_native(rt, nb_str_split));
        if (js_streq(key, "slice") || js_streq(key, "substring")) return jv_fn(rt_reg_native(rt, nb_str_slice));
        bool ok = false;
        double d = js_atod(key, &ok);
        if (ok) {
            int idx = (int)d;
            if (idx >= 0 && idx < js_strlen(s)) return jv_str(rt_intern_str_n(rt, s + idx, 1));
        }
        return jv_undef();
    }
    if (obj.type == JVT_DOM) {
        JsDomNode* n = js_dom_get_node(rt, obj.u.handle);
        if (!n) return jv_undef();
        if (js_streq(key, "id")) return jv_str(rt_intern_str(rt, n->elem_id));
        if (js_streq(key, "tagName")) return jv_str(rt_intern_str(rt, n->tag));
        if (js_streq(key, "className")) return jv_str(rt_intern_str(rt, n->class_name));
        if (js_streq(key, "href")) return jv_str(rt_intern_str(rt, n->href));
        if (js_streq(key, "textContent") || js_streq(key, "innerText") ||
            js_streq(key, "innerHTML") || js_streq(key, "value")) {
            return jv_str(rt_intern_str(rt, n->text));
        }
        if (js_streq(key, "hidden")) return jv_bool(n->hidden);
        if (js_streq(key, "style")) {
            int sobj = rt_alloc_obj(rt);
            if (sobj >= 0) rt->objs[sobj].dom_style_id = n->id;
            return jv_obj(sobj);
        }
        if (js_streq(key, "appendChild")) return jv_fn(rt_reg_native(rt, nb_dom_append));
        if (js_streq(key, "setAttribute")) return jv_fn(rt_reg_native(rt, nb_dom_set_attr));
        if (js_streq(key, "addEventListener")) return jv_fn(rt_reg_native(rt, nb_dom_add_listener));
        if (n->js_obj_handle >= 0) {
            JsVal v;
            if (rt_prop_list_get(rt, rt->objs[n->js_obj_handle].first_prop, key, &v)) return v;
        }
        return jv_undef();
    }
    if (obj.type == JVT_OBJ && obj.u.handle >= 0 && obj.u.handle < rt->obj_len) {
        if (js_streq(key, "title") && js_streq(rt_get_str(rt, 0), "")) {}
        JsVal v;
        if (rt_prop_list_get(rt, rt->objs[obj.u.handle].first_prop, key, &v)) return v;
    }
    return jv_undef();
}

static void js_set_prop(JsRuntime* rt, JsVal obj, const char* key, JsVal val) {
    if (obj.type == JVT_DOM) {
        JsDomNode* n = js_dom_get_node(rt, obj.u.handle);
        if (!n) return;
        if (js_streq(key, "id")) js_strcpy_max(n->elem_id, rt_get_str(rt, js_to_str_h(rt, val)), sizeof(n->elem_id));
        else if (js_streq(key, "className")) js_strcpy_max(n->class_name, rt_get_str(rt, js_to_str_h(rt, val)), sizeof(n->class_name));
        else if (js_streq(key, "href")) js_strcpy_max(n->href, rt_get_str(rt, js_to_str_h(rt, val)), sizeof(n->href));
        else if (js_streq(key, "textContent") || js_streq(key, "innerText") || js_streq(key, "value")) {
            char buf[512]; js_val_to_cstr(rt, val, buf, sizeof(buf));
            js_strcpy_max(n->text, buf, sizeof(n->text));
            n->first_child = -1;
        } else if (js_streq(key, "innerHTML")) {
            char buf[512]; js_val_to_cstr(rt, val, buf, sizeof(buf));
            dom_set_inner_html(rt, n->id, buf);
        } else if (js_streq(key, "hidden")) {
            n->hidden = js_to_bool(rt, val);
        } else if (js_streq(key, "onclick")) {
            if (val.type == JVT_FUNC) n->click_fn_handle = val.u.handle;
            else js_val_to_cstr(rt, val, n->onclick_src, sizeof(n->onclick_src));
        } else {
            if (n->js_obj_handle < 0) n->js_obj_handle = rt_alloc_obj(rt);
            if (n->js_obj_handle >= 0) rt_prop_list_set(rt, &rt->objs[n->js_obj_handle].first_prop, key, val);
        }
        rt->dom_dirty = true;
        return;
    }
    if (obj.type == JVT_ARR && obj.u.handle >= 0 && obj.u.handle < rt->arr_len) {
        bool ok = false;
        double d = js_atod(key, &ok);
        if (ok && d >= 0) {
            int idx = (int)d;
            JsArr* a = &rt->arrs[obj.u.handle];
            while (a->len <= idx) rt_arr_push_raw(rt, obj.u.handle, jv_undef());
            if (idx < a->len) a->data[idx] = val;
        }
        return;
    }
    if (obj.type == JVT_OBJ && obj.u.handle >= 0 && obj.u.handle < rt->obj_len) {
        JsObj* o = &rt->objs[obj.u.handle];
        if (o->dom_style_id >= 0) {
            JsDomNode* dn = js_dom_get_node(rt, o->dom_style_id);
            if (dn) {
                char sbuf[64]; js_val_to_cstr(rt, val, sbuf, sizeof(sbuf));
                if (js_streq(key, "color")) js_parse_color(sbuf, &dn->color);
                else if (js_streq(key, "backgroundColor") || js_streq(key, "background")) js_parse_color(sbuf, &dn->bg_color);
                else if (js_streq(key, "display")) dn->hidden = js_streq(sbuf, "none");
                rt->dom_dirty = true;
            }
            return;
        }
        // Handle document.title assignment cleanly
        rt_prop_list_set(rt, &o->first_prop, key, val);
        JsVal doc_v = rt_env_get(rt, rt->global_env, "document");
        if (doc_v.type == JVT_OBJ && doc_v.u.handle == obj.u.handle && js_streq(key, "title")) {
            js_val_to_cstr(rt, val, rt->title, sizeof(rt->title));
            rt->dom_dirty = true;
        }
    }
}

// ============================================================================
// AST Tree-Walking Evaluator
// ============================================================================

static JsVal call_js_value(JsRuntime* rt, JsVal fn_val, JsVal this_val, const JsVal* args, int argc) {
    if (fn_val.type != JVT_FUNC || fn_val.u.handle < 0 || fn_val.u.handle >= rt->func_len) {
        rt_set_error(rt, "[JS TypeError] Value is not a function");
        return jv_undef();
    }
    if (rt->call_depth >= JS_MAX_CALL_DEPTH) {
        rt_set_error(rt, "[JS RangeError] Maximum call stack depth exceeded");
        rt->ctrl = CF_THROW;
        return jv_undef();
    }
    JsFunc* fn = &rt->funcs[fn_val.u.handle];
    if (fn->is_native) {
        return fn->native_fn(rt, this_val, args, argc);
    }

    int call_env = rt_alloc_env(rt, fn->closure_env);
    rt_env_define(rt, call_env, "this", this_val);
    for (int i = 0; i < fn->param_count; i++) {
        JsVal pv = (i < argc) ? args[i] : jv_undef();
        rt_env_define(rt, call_env, rt_get_str(rt, fn->params[i]), pv);
    }

    rt->call_depth++;
    eval_ast(rt, fn->body_ast, call_env);
    rt->call_depth--;

    if (rt->ctrl == CF_RETURN) {
        JsVal res = rt->ctrl_val;
        rt->ctrl = CF_NONE;
        rt->ctrl_val = jv_undef();
        return res;
    }
    return jv_undef();
}

static JsVal apply_assign_op(JsRuntime* rt, uint8_t op, JsVal cur, JsVal rhs) {
    if (op == JT_EQ) return rhs;
    if (op == JT_PLUSEQ) {
        if (cur.type == JVT_STR || rhs.type == JVT_STR) {
            char a[512], b[256];
            js_val_to_cstr(rt, cur, a, sizeof(a));
            js_val_to_cstr(rt, rhs, b, sizeof(b));
            js_strappend_max(a, b, sizeof(a));
            return jv_str(rt_intern_str(rt, a));
        }
        return jv_num(js_to_num(rt, cur) + js_to_num(rt, rhs));
    }
    double a = js_to_num(rt, cur), b = js_to_num(rt, rhs);
    if (op == JT_MINUSEQ) return jv_num(a - b);
    if (op == JT_STAREQ)  return jv_num(a * b);
    if (op == JT_SLASHEQ) return jv_num(a / b);
    if (op == JT_PCTEQ)   return jv_num(uefi_fmod(a, b));
    return rhs;
}

static JsVal assign_lvalue(JsRuntime* rt, int target_ast, int env_id, uint8_t op, JsVal rhs) {
    if (target_ast < 0 || target_ast >= rt->ast_len) return jv_undef();
    JsAstNode* t = &rt->ast[target_ast];
    if (t->kind == JA_IDENT) {
        const char* nm = rt_get_str(rt, t->str_h);
        JsVal next = (op == JT_EQ) ? rhs : apply_assign_op(rt, op, rt_env_get(rt, env_id, nm), rhs);
        rt_env_set_existing_or_global(rt, env_id, nm, next);
        return next;
    }
    if (t->kind == JA_MEMBER) {
        JsVal obj = eval_ast(rt, t->left, env_id);
        const char* prop = rt_get_str(rt, t->str_h);
        JsVal next = (op == JT_EQ) ? rhs : apply_assign_op(rt, op, js_get_prop(rt, obj, prop), rhs);
        js_set_prop(rt, obj, prop, next);
        return next;
    }
    if (t->kind == JA_INDEX) {
        JsVal obj = eval_ast(rt, t->left, env_id);
        char idx[64];
        js_val_to_cstr(rt, eval_ast(rt, t->right, env_id), idx, sizeof(idx));
        JsVal next = (op == JT_EQ) ? rhs : apply_assign_op(rt, op, js_get_prop(rt, obj, idx), rhs);
        js_set_prop(rt, obj, idx, next);
        return next;
    }
    return rhs;
}

static JsVal eval_ast(JsRuntime* rt, int ast_id, int env_id) {
    if (!rt || ast_id < 0 || ast_id >= rt->ast_len || rt->ctrl != CF_NONE) return jv_undef();
    if (rt->steps_left == 0) {
        rt_set_error(rt, "[JS Timeout] Execution step budget exceeded");
        rt->ctrl = CF_THROW;
        return jv_undef();
    }
    rt->steps_left--;

    JsAstNode* n = &rt->ast[ast_id];
    switch (n->kind) {
        case JA_LIT:   return n->lit;
        case JA_THIS:  return rt_env_get(rt, env_id, "this");
        case JA_IDENT: return rt_env_get(rt, env_id, rt_get_str(rt, n->str_h));
        case JA_EXPR_STMT: return eval_ast(rt, n->left, env_id);
        case JA_BLOCK: {
            int sub_env = rt_alloc_env(rt, env_id);
            JsVal last = jv_undef();
            int s = n->left;
            while (s >= 0 && rt->ctrl == CF_NONE) {
                last = eval_ast(rt, s, sub_env);
                s = rt->ast[s].next_sibling;
            }
            return last;
        }
        case JA_VAR_DECL: {
            int item = n->left;
            while (item >= 0 && rt->ctrl == CF_NONE) {
                JsVal val = (rt->ast[item].left >= 0) ? eval_ast(rt, rt->ast[item].left, env_id) : jv_undef();
                rt_env_define(rt, env_id, rt_get_str(rt, rt->ast[item].str_h), val);
                item = rt->ast[item].next_sibling;
            }
            return jv_undef();
        }
        case JA_FN_DECL:
        case JA_FN_EXPR: {
            int tidx = n->a;
            if (tidx < 0 || tidx >= rt->fn_tmpl_len || rt->func_len >= JS_FUNC_CAP) return jv_undef();
            int fid = rt->func_len++;
            JsFuncTmpl* tmpl = &rt->fn_tmpls[tidx];
            JsFunc* f = &rt->funcs[fid];
            f->is_native = false;
            f->param_count = tmpl->param_count;
            for (int i = 0; i < tmpl->param_count; i++) f->params[i] = tmpl->params[i];
            f->body_ast = tmpl->body_ast;
            f->closure_env = env_id;
            f->name_str = tmpl->name_str;
            JsVal fv = jv_fn(fid);
            if (n->kind == JA_FN_DECL && tmpl->name_str > 0) {
                rt_env_define(rt, env_id, rt_get_str(rt, tmpl->name_str), fv);
            }
            return fv;
        }
        case JA_ARRAY: {
            int id = rt_alloc_arr(rt, 8);
            int cur = n->left;
            while (cur >= 0 && rt->ctrl == CF_NONE) {
                rt_arr_push_raw(rt, id, eval_ast(rt, cur, env_id));
                cur = rt->ast[cur].next_sibling;
            }
            return jv_arr(id);
        }
        case JA_OBJECT: {
            int id = rt_alloc_obj(rt);
            JsVal ov = jv_obj(id);
            int cur = n->left;
            while (cur >= 0 && rt->ctrl == CF_NONE) {
                JsVal v = eval_ast(rt, rt->ast[cur].left, env_id);
                js_set_prop(rt, ov, rt_get_str(rt, rt->ast[cur].str_h), v);
                cur = rt->ast[cur].next_sibling;
            }
            return ov;
        }
        case JA_MEMBER: {
            JsVal obj = eval_ast(rt, n->left, env_id);
            return js_get_prop(rt, obj, rt_get_str(rt, n->str_h));
        }
        case JA_INDEX: {
            JsVal obj = eval_ast(rt, n->left, env_id);
            char idx[64];
            js_val_to_cstr(rt, eval_ast(rt, n->right, env_id), idx, sizeof(idx));
            return js_get_prop(rt, obj, idx);
        }
        case JA_NEW:
        case JA_CALL: {
            JsVal this_val = rt_env_get(rt, env_id, "this");
            JsVal callee = jv_undef();
            if (rt->ast[n->left].kind == JA_MEMBER) {
                this_val = eval_ast(rt, rt->ast[n->left].left, env_id);
                callee = js_get_prop(rt, this_val, rt_get_str(rt, rt->ast[n->left].str_h));
            } else if (rt->ast[n->left].kind == JA_INDEX) {
                this_val = eval_ast(rt, rt->ast[n->left].left, env_id);
                char idx[64];
                js_val_to_cstr(rt, eval_ast(rt, rt->ast[n->left].right, env_id), idx, sizeof(idx));
                callee = js_get_prop(rt, this_val, idx);
            } else {
                callee = eval_ast(rt, n->left, env_id);
            }
            JsVal args[12];
            int argc = 0;
            int arg_ast = n->right;
            while (arg_ast >= 0 && argc < 12 && rt->ctrl == CF_NONE) {
                args[argc++] = eval_ast(rt, arg_ast, env_id);
                arg_ast = rt->ast[arg_ast].next_sibling;
            }
            return call_js_value(rt, callee, this_val, args, argc);
        }
        case JA_ASSIGN: {
            JsVal rhs = eval_ast(rt, n->right, env_id);
            return assign_lvalue(rt, n->left, env_id, n->op, rhs);
        }
        case JA_INCDEC: {
            JsVal cur = eval_ast(rt, n->left, env_id);
            double old_v = js_to_num(rt, cur);
            double new_v = (n->op == JT_PLUSPLUS) ? (old_v + 1.0) : (old_v - 1.0);
            assign_lvalue(rt, n->left, env_id, JT_EQ, jv_num(new_v));
            return jv_num(n->a ? new_v : old_v);
        }
        case JA_UNARY: {
            JsVal v = eval_ast(rt, n->left, env_id);
            if (n->op == JT_BANG) return jv_bool(!js_to_bool(rt, v));
            if (n->op == JT_MINUS) return jv_num(-js_to_num(rt, v));
            if (n->op == JT_PLUS)  return jv_num(js_to_num(rt, v));
            if (n->op == JT_TYPEOF) {
                const char* tn = "undefined";
                if (v.type == JVT_NULL) tn = "object";
                else if (v.type == JVT_BOOL) tn = "boolean";
                else if (v.type == JVT_NUM) tn = "number";
                else if (v.type == JVT_STR) tn = "string";
                else if (v.type == JVT_FUNC) tn = "function";
                else if (v.type == JVT_OBJ || v.type == JVT_ARR || v.type == JVT_DOM) tn = "object";
                return jv_str(rt_intern_str(rt, tn));
            }
            return v;
        }
        case JA_BINARY: {
            JsVal lv = eval_ast(rt, n->left, env_id);
            if (n->op == JT_ANDAND) return js_to_bool(rt, lv) ? eval_ast(rt, n->right, env_id) : lv;
            if (n->op == JT_OROR)   return js_to_bool(rt, lv) ? lv : eval_ast(rt, n->right, env_id);
            if (n->op == JT_QQ)     return (lv.type == JVT_NULL || lv.type == JVT_UNDEF) ? eval_ast(rt, n->right, env_id) : lv;
            JsVal rv = eval_ast(rt, n->right, env_id);
            switch (n->op) {
                case JT_PLUS:
                    if (lv.type == JVT_STR || rv.type == JVT_STR) {
                        char a[512], b[256];
                        js_val_to_cstr(rt, lv, a, sizeof(a));
                        js_val_to_cstr(rt, rv, b, sizeof(b));
                        js_strappend_max(a, b, sizeof(a));
                        return jv_str(rt_intern_str(rt, a));
                    }
                    return jv_num(js_to_num(rt, lv) + js_to_num(rt, rv));
                case JT_MINUS:  return jv_num(js_to_num(rt, lv) - js_to_num(rt, rv));
                case JT_STAR:   return jv_num(js_to_num(rt, lv) * js_to_num(rt, rv));
                case JT_SLASH:  return jv_num(js_to_num(rt, lv) / js_to_num(rt, rv));
                case JT_PCT:    return jv_num(uefi_fmod(js_to_num(rt, lv), js_to_num(rt, rv)));
                case JT_LT:     return jv_bool(js_to_num(rt, lv) <  js_to_num(rt, rv));
                case JT_LE:     return jv_bool(js_to_num(rt, lv) <= js_to_num(rt, rv));
                case JT_GT:     return jv_bool(js_to_num(rt, lv) >  js_to_num(rt, rv));
                case JT_GE:     return jv_bool(js_to_num(rt, lv) >= js_to_num(rt, rv));
                case JT_EQEQEQ: return jv_bool(js_strict_eq(rt, lv, rv));
                case JT_NEQEQ:  return jv_bool(!js_strict_eq(rt, lv, rv));
                case JT_EQEQ:   return jv_bool(js_loose_eq(rt, lv, rv));
                case JT_NEQ:    return jv_bool(!js_loose_eq(rt, lv, rv));
                case JT_BITAND: return jv_num((double)((int32_t)js_to_num(rt, lv) & (int32_t)js_to_num(rt, rv)));
                case JT_BITOR:  return jv_num((double)((int32_t)js_to_num(rt, lv) | (int32_t)js_to_num(rt, rv)));
                case JT_BITXOR: return jv_num((double)((int32_t)js_to_num(rt, lv) ^ (int32_t)js_to_num(rt, rv)));
                case JT_SHL:    return jv_num((double)((int32_t)js_to_num(rt, lv) << ((int32_t)js_to_num(rt, rv) & 31)));
                case JT_SHR:    return jv_num((double)((int32_t)js_to_num(rt, lv) >> ((int32_t)js_to_num(rt, rv) & 31)));
                default: return jv_undef();
            }
        }
        case JA_TERNARY:
            return js_to_bool(rt, eval_ast(rt, n->left, env_id))
                ? eval_ast(rt, n->right, env_id)
                : eval_ast(rt, n->a, env_id);
        case JA_IF:
            if (js_to_bool(rt, eval_ast(rt, n->left, env_id))) return eval_ast(rt, n->right, env_id);
            else if (n->a >= 0) return eval_ast(rt, n->a, env_id);
            return jv_undef();
        case JA_WHILE: {
            while (rt->ctrl == CF_NONE && rt->steps_left > 0 && js_to_bool(rt, eval_ast(rt, n->left, env_id))) {
                eval_ast(rt, n->right, env_id);
                if (rt->ctrl == CF_BREAK) { rt->ctrl = CF_NONE; break; }
                if (rt->ctrl == CF_CONTINUE) { rt->ctrl = CF_NONE; continue; }
            }
            return jv_undef();
        }
        case JA_FOR: {
            int fenv = rt_alloc_env(rt, env_id);
            if (n->left >= 0) eval_ast(rt, n->left, fenv);
            while (rt->ctrl == CF_NONE && rt->steps_left > 0) {
                if (n->right >= 0 && !js_to_bool(rt, eval_ast(rt, n->right, fenv))) break;
                eval_ast(rt, n->str_h, fenv);
                if (rt->ctrl == CF_BREAK) { rt->ctrl = CF_NONE; break; }
                if (rt->ctrl == CF_CONTINUE) rt->ctrl = CF_NONE;
                if (n->a >= 0 && rt->ctrl == CF_NONE) eval_ast(rt, n->a, fenv);
            }
            return jv_undef();
        }
        case JA_FOR_ITER: {
            JsVal col = eval_ast(rt, n->left, env_id);
            const char* vn = rt_get_str(rt, n->str_h);
            int fenv = rt_alloc_env(rt, env_id);
            if (n->a == 1 && col.type == JVT_ARR && col.u.handle >= 0 && col.u.handle < rt->arr_len) {
                JsArr* a = &rt->arrs[col.u.handle];
                for (int i = 0; i < a->len && rt->ctrl == CF_NONE && rt->steps_left > 0; i++) {
                    rt_env_define(rt, fenv, vn, a->data[i]);
                    eval_ast(rt, n->right, fenv);
                    if (rt->ctrl == CF_BREAK) { rt->ctrl = CF_NONE; break; }
                    if (rt->ctrl == CF_CONTINUE) rt->ctrl = CF_NONE;
                }
            } else if (n->a == 0 && col.type == JVT_OBJ && col.u.handle >= 0 && col.u.handle < rt->obj_len) {
                int p = rt->objs[col.u.handle].first_prop;
                while (p >= 0 && rt->ctrl == CF_NONE && rt->steps_left > 0) {
                    rt_env_define(rt, fenv, vn, jv_str(rt->props[p].key_str));
                    eval_ast(rt, n->right, fenv);
                    if (rt->ctrl == CF_BREAK) { rt->ctrl = CF_NONE; break; }
                    if (rt->ctrl == CF_CONTINUE) rt->ctrl = CF_NONE;
                    p = rt->props[p].next_prop;
                }
            }
            return jv_undef();
        }
        case JA_RETURN:
            rt->ctrl_val = (n->left >= 0) ? eval_ast(rt, n->left, env_id) : jv_undef();
            rt->ctrl = CF_RETURN;
            return rt->ctrl_val;
        case JA_BREAK:    rt->ctrl = CF_BREAK; return jv_undef();
        case JA_CONTINUE: rt->ctrl = CF_CONTINUE; return jv_undef();
        case JA_THROW:
            rt->ctrl_val = eval_ast(rt, n->left, env_id);
            rt->ctrl = CF_THROW;
            return jv_undef();
        case JA_TRY: {
            eval_ast(rt, n->left, env_id);
            if (rt->ctrl == CF_THROW && n->right >= 0) {
                JsVal err = rt->ctrl_val;
                rt->ctrl = CF_NONE;
                rt->ctrl_val = jv_undef();
                int cenv = rt_alloc_env(rt, env_id);
                if (n->str_h > 0) rt_env_define(rt, cenv, rt_get_str(rt, n->str_h), err);
                eval_ast(rt, n->right, cenv);
            }
            if (n->a >= 0) {
                uint8_t saved_ctrl = rt->ctrl;
                JsVal saved_val = rt->ctrl_val;
                rt->ctrl = CF_NONE;
                eval_ast(rt, n->a, env_id);
                if (rt->ctrl == CF_NONE) { rt->ctrl = saved_ctrl; rt->ctrl_val = saved_val; }
            }
            return jv_undef();
        }
        default: return jv_undef();
    }
}

// ============================================================================
// Standard Library & Web API Natives (console, Math, JSON, DOM, Timers)
// ============================================================================

static JsVal nb_console_out(JsRuntime* rt, uint8_t lvl, const JsVal* args, int argc) {
    char line[JS_CONSOLE_TEXT_MAX]; line[0] = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0) js_strappend_max(line, " ", sizeof(line));
        char seg[128]; js_val_to_cstr(rt, args[i], seg, sizeof(seg));
        js_strappend_max(line, seg, sizeof(line));
    }
    rt_console_push(rt, lvl, line);
    return jv_undef();
}
static JsVal nb_console_log(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return nb_console_out(rt, 0, a, c); }
static JsVal nb_console_warn(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return nb_console_out(rt, 1, a, c); }
static JsVal nb_console_err(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return nb_console_out(rt, 2, a, c); }
static JsVal nb_console_clear(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; (void)a; (void)c; rt->console_len = 0; return jv_undef(); }

static JsVal nb_alert(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv;
    char msg[256] = "";
    if (argc > 0) js_val_to_cstr(rt, args[0], msg, sizeof(msg));
    js_strcpy_max(rt->pending_alert, msg, sizeof(rt->pending_alert));
    rt->has_alert = true;
    rt_console_push(rt, 3, msg);
    play_system_sound((char*)"notify");
    return jv_undef();
}
static JsVal nb_play_sound(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv;
    char cue[32] = "tool";
    if (argc > 0) js_val_to_cstr(rt, args[0], cue, sizeof(cue));
    play_system_sound(cue);
    return jv_undef();
}

static JsVal nb_parse_int(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; if (argc < 1) return jv_num(0.0 / 0.0);
    char s[64]; js_val_to_cstr(rt, args[0], s, sizeof(s));
    bool ok = false; double d = js_atod(s, &ok);
    return ok ? jv_num((double)(long long)d) : jv_num(0.0 / 0.0);
}
static JsVal nb_parse_float(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; if (argc < 1) return jv_num(0.0 / 0.0);
    return jv_num(js_to_num(rt, args[0]));
}
static JsVal nb_isnan(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; if (argc < 1) return jv_bool(true);
    return jv_bool(js_isnan(js_to_num(rt, args[0])));
}
static JsVal nb_number(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; return (argc > 0) ? jv_num(js_to_num(rt, args[0])) : jv_num(0);
}
static JsVal nb_string(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; return (argc > 0) ? jv_str(js_to_str_h(rt, args[0])) : jv_str(0);
}
static JsVal nb_boolean(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; return (argc > 0) ? jv_bool(js_to_bool(rt, args[0])) : jv_bool(false);
}

// Math
static JsVal nb_math_abs(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; double v = c ? js_to_num(rt, a[0]) : 0.0; return jv_num(v < 0 ? -v : v); }
static JsVal nb_math_floor(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return jv_num(c ? uefi_floor(js_to_num(rt, a[0])) : 0.0); }
static JsVal nb_math_ceil(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return jv_num(c ? uefi_ceil(js_to_num(rt, a[0])) : 0.0); }
static JsVal nb_math_round(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; double v = c ? js_to_num(rt, a[0]) : 0.0; return jv_num(uefi_floor(v + 0.5)); }
static JsVal nb_math_sqrt(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; double v = c ? js_to_num(rt, a[0]) : 0.0; return jv_num(v >= 0 ? uefi_sqrt(v) : (0.0 / 0.0)); }
static JsVal nb_math_pow(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return jv_num(c >= 2 ? uefi_pow(js_to_num(rt, a[0]), js_to_num(rt, a[1])) : 0.0); }
static JsVal nb_math_min(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; if (c == 0) return jv_num(1e18);
    double m = js_to_num(rt, a[0]);
    for (int i = 1; i < c; i++) { double v = js_to_num(rt, a[i]); if (v < m) m = v; }
    return jv_num(m);
}
static JsVal nb_math_max(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; if (c == 0) return jv_num(-1e18);
    double m = js_to_num(rt, a[0]);
    for (int i = 1; i < c; i++) { double v = js_to_num(rt, a[i]); if (v > m) m = v; }
    return jv_num(m);
}
static JsVal nb_math_random(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; (void)a; (void)c;
    rt->rng_state = rt->rng_state * 1664525U + 1013904223U;
    return jv_num((double)(rt->rng_state & 0xFFFFFFU) / (double)0x1000000U);
}

static JsVal nb_date_now(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; (void)a; (void)c;
    return jv_num((double)rt->now_ms);
}

static JsVal nb_json_stringify(JsRuntime* rt, JsVal tv, const JsVal* args, int argc) {
    (void)tv; if (argc < 1) return jv_undef();
    JsVal v = args[0];
    if (v.type == JVT_STR) {
        char out[512] = "\"";
        js_strappend_max(out, rt_get_str(rt, v.u.handle), 509);
        js_strappend_max(out, "\"", sizeof(out));
        return jv_str(rt_intern_str(rt, out));
    }
    if (v.type == JVT_ARR && v.u.handle >= 0 && v.u.handle < rt->arr_len) {
        JsArr* a = &rt->arrs[v.u.handle];
        char out[512] = "[";
        for (int i = 0; i < a->len; i++) {
            if (i > 0) js_strappend_max(out, ",", sizeof(out));
            JsVal sub = nb_json_stringify(rt, tv, &a->data[i], 1);
            js_strappend_max(out, rt_get_str(rt, js_to_str_h(rt, sub)), sizeof(out));
        }
        js_strappend_max(out, "]", sizeof(out));
        return jv_str(rt_intern_str(rt, out));
    }
    if (v.type == JVT_OBJ && v.u.handle >= 0 && v.u.handle < rt->obj_len) {
        char out[512] = "{";
        int p = rt->objs[v.u.handle].first_prop;
        bool first = true;
        while (p >= 0) {
            if (!first) js_strappend_max(out, ",", sizeof(out));
            first = false;
            js_strappend_max(out, "\"", sizeof(out));
            js_strappend_max(out, rt_get_str(rt, rt->props[p].key_str), sizeof(out));
            js_strappend_max(out, "\":", sizeof(out));
            JsVal sub = nb_json_stringify(rt, tv, &rt->props[p].val, 1);
            js_strappend_max(out, rt_get_str(rt, js_to_str_h(rt, sub)), sizeof(out));
            p = rt->props[p].next_prop;
        }
        js_strappend_max(out, "}", sizeof(out));
        return jv_str(rt_intern_str(rt, out));
    }
    return jv_str(js_to_str_h(rt, v));
}

// Timers
static JsVal schedule_timer(JsRuntime* rt, const JsVal* args, int argc, bool interval) {
    if (argc < 1) return jv_num(0);
    int slot = -1;
    for (int i = 0; i < JS_TIMER_CAP; i++) if (!rt->timers[i].active) { slot = i; break; }
    if (slot < 0) return jv_num(0);
    uint64_t ms = (argc > 1) ? (uint64_t)((long long)js_to_num(rt, args[1])) : 16ULL;
    if (ms < 16ULL) ms = 16ULL;

    JsTimer* tm = &rt->timers[slot];
    tm->active = true;
    tm->is_interval = interval;
    tm->id = rt->next_timer_id++;
    tm->fn_handle = (args[0].type == JVT_FUNC) ? args[0].u.handle : -1;
    tm->code_str = (args[0].type == JVT_STR) ? args[0].u.handle : -1;
    tm->interval_ms = ms;
    tm->next_fire_ms = rt->now_ms + ms;
    return jv_num((double)tm->id);
}
static JsVal nb_set_timeout(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return schedule_timer(rt, a, c, false); }
static JsVal nb_set_interval(JsRuntime* rt, JsVal tv, const JsVal* a, int c) { (void)tv; return schedule_timer(rt, a, c, true); }
static JsVal nb_clear_timer(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; if (c < 1) return jv_undef();
    int id = (int)js_to_num(rt, a[0]);
    for (int i = 0; i < JS_TIMER_CAP; i++) if (rt->timers[i].active && rt->timers[i].id == id) rt->timers[i].active = false;
    return jv_undef();
}

// Document methods
static JsVal nb_doc_by_id(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; if (c < 1) return jv_null();
    char id[48]; js_val_to_cstr(rt, a[0], id, sizeof(id));
    for (int i = 0; i < rt->node_len; i++) {
        if (!rt->nodes[i].is_text_node && js_streq(rt->nodes[i].elem_id, id)) return jv_dom(i);
    }
    return jv_null();
}
static JsVal nb_doc_qs(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; if (c < 1) return jv_null();
    char sel[64]; js_val_to_cstr(rt, a[0], sel, sizeof(sel));
    for (int i = 0; i < rt->node_len; i++) {
        if (!rt->nodes[i].is_text_node && dom_matches_selector(&rt->nodes[i], sel)) return jv_dom(i);
    }
    return jv_null();
}
static JsVal nb_doc_qsa(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; int arr = rt_alloc_arr(rt, 8);
    if (c < 1) return jv_arr(arr);
    char sel[64]; js_val_to_cstr(rt, a[0], sel, sizeof(sel));
    for (int i = 0; i < rt->node_len; i++) {
        if (!rt->nodes[i].is_text_node && dom_matches_selector(&rt->nodes[i], sel)) rt_arr_push_raw(rt, arr, jv_dom(i));
    }
    return jv_arr(arr);
}
static JsVal nb_doc_create_el(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; char tag[16] = "div";
    if (c > 0) js_val_to_cstr(rt, a[0], tag, sizeof(tag));
    int nid = js_dom_create_element(rt, tag, -1);
    return (nid >= 0) ? jv_dom(nid) : jv_null();
}
static JsVal nb_doc_create_text(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv; char txt[256] = "";
    if (c > 0) js_val_to_cstr(rt, a[0], txt, sizeof(txt));
    int nid = js_dom_create_text(rt, txt, -1);
    return (nid >= 0) ? jv_dom(nid) : jv_null();
}
static JsVal nb_doc_write(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv;
    for (int i = 0; i < c; i++) {
        char buf[512]; js_val_to_cstr(rt, a[i], buf, sizeof(buf));
        js_strappend_max(rt->pending_doc_write, buf, sizeof(rt->pending_doc_write));
        rt->has_doc_write = true;
    }
    return jv_undef();
}
static JsVal nb_doc_add_listener(JsRuntime* rt, JsVal tv, const JsVal* a, int c) {
    (void)tv;
    if (c >= 2 && a[1].type == JVT_FUNC) {
        char ev[24]; js_val_to_cstr(rt, a[0], ev, sizeof(ev));
        if (js_streq(ev, "DOMContentLoaded")) {
            call_js_value(rt, a[1], jv_undef(), NULL, 0);
        } else if (js_streq(ev, "click")) {
            rt->doc_click_fn = a[1].u.handle;
        }
    }
    return jv_undef();
}

static void init_builtins(JsRuntime* rt) {
    int g = rt->global_env;

    // console
    int con = rt_alloc_obj(rt);
    JsVal conv = jv_obj(con);
    js_set_prop(rt, conv, "log", jv_fn(rt_reg_native(rt, nb_console_log)));
    js_set_prop(rt, conv, "info", jv_fn(rt_reg_native(rt, nb_console_log)));
    js_set_prop(rt, conv, "warn", jv_fn(rt_reg_native(rt, nb_console_warn)));
    js_set_prop(rt, conv, "error", jv_fn(rt_reg_native(rt, nb_console_err)));
    js_set_prop(rt, conv, "clear", jv_fn(rt_reg_native(rt, nb_console_clear)));
    rt_env_define(rt, g, "console", conv);

    // Math
    int m = rt_alloc_obj(rt);
    JsVal mv = jv_obj(m);
    js_set_prop(rt, mv, "PI", jv_num(3.141592653589793));
    js_set_prop(rt, mv, "E", jv_num(2.718281828459045));
    js_set_prop(rt, mv, "abs", jv_fn(rt_reg_native(rt, nb_math_abs)));
    js_set_prop(rt, mv, "floor", jv_fn(rt_reg_native(rt, nb_math_floor)));
    js_set_prop(rt, mv, "ceil", jv_fn(rt_reg_native(rt, nb_math_ceil)));
    js_set_prop(rt, mv, "round", jv_fn(rt_reg_native(rt, nb_math_round)));
    js_set_prop(rt, mv, "sqrt", jv_fn(rt_reg_native(rt, nb_math_sqrt)));
    js_set_prop(rt, mv, "pow", jv_fn(rt_reg_native(rt, nb_math_pow)));
    js_set_prop(rt, mv, "min", jv_fn(rt_reg_native(rt, nb_math_min)));
    js_set_prop(rt, mv, "max", jv_fn(rt_reg_native(rt, nb_math_max)));
    js_set_prop(rt, mv, "random", jv_fn(rt_reg_native(rt, nb_math_random)));
    rt_env_define(rt, g, "Math", mv);

    // JSON & Date
    int jso = rt_alloc_obj(rt);
    js_set_prop(rt, jv_obj(jso), "stringify", jv_fn(rt_reg_native(rt, nb_json_stringify)));
    rt_env_define(rt, g, "JSON", jv_obj(jso));

    int dt = rt_alloc_obj(rt);
    js_set_prop(rt, jv_obj(dt), "now", jv_fn(rt_reg_native(rt, nb_date_now)));
    rt_env_define(rt, g, "Date", jv_obj(dt));

    // Global functions
    rt_env_define(rt, g, "alert", jv_fn(rt_reg_native(rt, nb_alert)));
    rt_env_define(rt, g, "playSound", jv_fn(rt_reg_native(rt, nb_play_sound)));
    rt_env_define(rt, g, "parseInt", jv_fn(rt_reg_native(rt, nb_parse_int)));
    rt_env_define(rt, g, "parseFloat", jv_fn(rt_reg_native(rt, nb_parse_float)));
    rt_env_define(rt, g, "isNaN", jv_fn(rt_reg_native(rt, nb_isnan)));
    rt_env_define(rt, g, "Number", jv_fn(rt_reg_native(rt, nb_number)));
    rt_env_define(rt, g, "String", jv_fn(rt_reg_native(rt, nb_string)));
    rt_env_define(rt, g, "Boolean", jv_fn(rt_reg_native(rt, nb_boolean)));
    rt_env_define(rt, g, "setTimeout", jv_fn(rt_reg_native(rt, nb_set_timeout)));
    rt_env_define(rt, g, "setInterval", jv_fn(rt_reg_native(rt, nb_set_interval)));
    rt_env_define(rt, g, "clearTimeout", jv_fn(rt_reg_native(rt, nb_clear_timer)));
    rt_env_define(rt, g, "clearInterval", jv_fn(rt_reg_native(rt, nb_clear_timer)));

    // document
    int doc = rt_alloc_obj(rt);
    JsVal dv = jv_obj(doc);
    js_set_prop(rt, dv, "title", jv_str(rt_intern_str(rt, rt->title)));
    js_set_prop(rt, dv, "URL", jv_str(rt_intern_str(rt, rt->url)));
    js_set_prop(rt, dv, "body", jv_dom(rt->body_node_id));
    js_set_prop(rt, dv, "getElementById", jv_fn(rt_reg_native(rt, nb_doc_by_id)));
    js_set_prop(rt, dv, "querySelector", jv_fn(rt_reg_native(rt, nb_doc_qs)));
    js_set_prop(rt, dv, "querySelectorAll", jv_fn(rt_reg_native(rt, nb_doc_qsa)));
    js_set_prop(rt, dv, "createElement", jv_fn(rt_reg_native(rt, nb_doc_create_el)));
    js_set_prop(rt, dv, "createTextNode", jv_fn(rt_reg_native(rt, nb_doc_create_text)));
    js_set_prop(rt, dv, "write", jv_fn(rt_reg_native(rt, nb_doc_write)));
    js_set_prop(rt, dv, "addEventListener", jv_fn(rt_reg_native(rt, nb_doc_add_listener)));
    rt_env_define(rt, g, "document", dv);
}

// ============================================================================
// Public C Runtime API
// ============================================================================

extern "C" {

JsRuntime* js_runtime_create(void) {
    JsRuntime* rt = (JsRuntime*)uefi_malloc(sizeof(JsRuntime));
    if (!rt) return NULL;
    uefi_memset(rt, 0, sizeof(JsRuntime));

    rt->str_pool = (char*)uefi_malloc(JS_STR_POOL_CAP);
    rt->ast      = (JsAstNode*)uefi_malloc(JS_AST_CAP * sizeof(JsAstNode));
    rt->fn_tmpls = (JsFuncTmpl*)uefi_malloc(JS_FN_TMPL_CAP * sizeof(JsFuncTmpl));
    rt->objs     = (JsObj*)uefi_malloc(JS_OBJ_CAP * sizeof(JsObj));
    rt->props    = (JsProp*)uefi_malloc(JS_PROP_CAP * sizeof(JsProp));
    rt->arrs     = (JsArr*)uefi_malloc(JS_ARR_CAP * sizeof(JsArr));
    rt->envs     = (JsEnv*)uefi_malloc(JS_ENV_CAP * sizeof(JsEnv));
    rt->funcs    = (JsFunc*)uefi_malloc(JS_FUNC_CAP * sizeof(JsFunc));

    if (!rt->str_pool || !rt->ast || !rt->fn_tmpls || !rt->objs ||
        !rt->props || !rt->arrs || !rt->envs || !rt->funcs) {
        js_runtime_destroy(rt);
        return NULL;
    }
    rt->rng_state = 0x5A17C0DE;
    js_runtime_reset(rt, "blank://start", "Horizon Web");
    return rt;
}

static void rt_free_arrays(JsRuntime* rt) {
    if (!rt || !rt->arrs) return;
    for (int i = 0; i < rt->arr_len; i++) {
        if (rt->arrs[i].data) {
            uefi_free(rt->arrs[i].data);
            rt->arrs[i].data = NULL;
        }
    }
}

void js_runtime_destroy(JsRuntime* rt) {
    if (!rt) return;
    rt_free_arrays(rt);
    if (rt->str_pool) uefi_free(rt->str_pool);
    if (rt->ast) uefi_free(rt->ast);
    if (rt->fn_tmpls) uefi_free(rt->fn_tmpls);
    if (rt->objs) uefi_free(rt->objs);
    if (rt->props) uefi_free(rt->props);
    if (rt->arrs) uefi_free(rt->arrs);
    if (rt->envs) uefi_free(rt->envs);
    if (rt->funcs) uefi_free(rt->funcs);
    uefi_free(rt);
}

void js_runtime_reset(JsRuntime* rt, const char* url, const char* title) {
    if (!rt) return;
    rt_free_arrays(rt);

    rt->str_pool[0] = 0;
    rt->str_len = 1;
    rt->ast_len = 1; // 0 reserved
    rt->fn_tmpl_len = 0;
    rt->obj_len = 0;
    rt->prop_len = 0;
    rt->arr_len = 0;
    rt->env_len = 0;
    rt->func_len = 0;
    rt->node_len = 0;
    rt->console_len = 0;
    rt->last_error[0] = 0;
    rt->doc_click_fn = -1;
    rt->next_timer_id = 1;
    rt->dom_dirty = false;
    rt->has_doc_write = false;
    rt->pending_doc_write[0] = 0;
    rt->has_alert = false;
    rt->pending_alert[0] = 0;
    rt->has_nav = false;
    rt->pending_nav[0] = 0;
    for (int i = 0; i < JS_TIMER_CAP; i++) rt->timers[i].active = false;

    js_strcpy_max(rt->url, url ? url : "blank://start", sizeof(rt->url));
    js_strcpy_max(rt->title, title ? title : "Horizon Web", sizeof(rt->title));

    rt->global_env = rt_alloc_env(rt, -1);
    rt->body_node_id = js_dom_create_element(rt, "body", -1);
    rt->dom_dirty = false;
    init_builtins(rt);
}

bool js_eval(JsRuntime* rt, const char* code, char* eval_out, int eval_out_max) {
    if (eval_out && eval_out_max > 0) eval_out[0] = 0;
    if (!rt || !code || !code[0]) return true;

    JsTok* toks = (JsTok*)uefi_malloc(JS_TOK_CAP * sizeof(JsTok));
    if (!toks) return false;

    int tcount = js_tokenize(rt, code, toks, JS_TOK_CAP);
    JsParser parser;
    parser.rt = rt;
    parser.toks = toks;
    parser.count = tcount;
    parser.pos = 0;
    parser.had_error = false;

    int ast_start = rt->ast_len;
    int head = -1, tail = -1;
    while (p_kind(&parser) != JT_EOF && !parser.had_error) {
        int s = parse_stmt(&parser);
        if (head < 0) head = tail = s;
        else { rt->ast[tail].next_sibling = s; tail = s; }
    }
    uefi_free(toks);

    if (parser.had_error) {
        rt->ast_len = ast_start;
        return false;
    }

    rt->steps_left = JS_STEP_BUDGET;
    rt->call_depth = 0;
    rt->ctrl = CF_NONE;
    rt->ctrl_val = jv_undef();

    JsVal last = jv_undef();
    int cur = head;
    while (cur >= 0 && rt->ctrl == CF_NONE) {
        last = eval_ast(rt, cur, rt->global_env);
        cur = rt->ast[cur].next_sibling;
    }

    if (rt->ctrl == CF_THROW) {
        char ebuf[160]; js_val_to_cstr(rt, rt->ctrl_val, ebuf, sizeof(ebuf));
        rt_set_error(rt, ebuf);
        rt->ctrl = CF_NONE;
        return false;
    }
    rt->ctrl = CF_NONE;

    if (eval_out && eval_out_max > 0) {
        js_val_to_cstr(rt, last, eval_out, eval_out_max);
    }
    return true;
}

bool js_tick_timers(JsRuntime* rt, uint64_t now_ms) {
    if (!rt) return false;
    rt->now_ms = now_ms;
    bool prev_dirty = rt->dom_dirty;
    rt->dom_dirty = false;

    for (int i = 0; i < JS_TIMER_CAP; i++) {
        JsTimer* tm = &rt->timers[i];
        if (!tm->active) continue;
        if (now_ms >= tm->next_fire_ms) {
            if (tm->is_interval) {
                tm->next_fire_ms = now_ms + tm->interval_ms;
            } else {
                tm->active = false;
            }
            rt->steps_left = JS_STEP_BUDGET;
            rt->call_depth = 0;
            rt->ctrl = CF_NONE;
            if (tm->fn_handle >= 0) {
                call_js_value(rt, jv_fn(tm->fn_handle), jv_undef(), NULL, 0);
            } else if (tm->code_str >= 0) {
                js_eval(rt, rt_get_str(rt, tm->code_str), NULL, 0);
            }
            rt->ctrl = CF_NONE;
        }
    }
    bool mutated = rt->dom_dirty;
    rt->dom_dirty = prev_dirty || mutated;
    return mutated;
}

bool js_dispatch_click(JsRuntime* rt, int dom_node_id) {
    if (!rt) return false;
    JsDomNode* n = js_dom_get_node(rt, dom_node_id);
    if (!n) return false;

    bool fired = false;
    rt->steps_left = JS_STEP_BUDGET;
    rt->call_depth = 0;
    rt->ctrl = CF_NONE;

    if (n->onclick_src[0]) {
        js_eval(rt, n->onclick_src, NULL, 0);
        fired = true;
    }
    if (n->click_fn_handle >= 0) {
        int ev = rt_alloc_obj(rt);
        JsVal ev_v = jv_obj(ev);
        js_set_prop(rt, ev_v, "type", jv_str(rt_intern_str(rt, "click")));
        js_set_prop(rt, ev_v, "target", jv_dom(dom_node_id));
        call_js_value(rt, jv_fn(n->click_fn_handle), jv_dom(dom_node_id), &ev_v, 1);
        fired = true;
    }
    if (rt->doc_click_fn >= 0) {
        int ev = rt_alloc_obj(rt);
        JsVal ev_v = jv_obj(ev);
        js_set_prop(rt, ev_v, "target", jv_dom(dom_node_id));
        call_js_value(rt, jv_fn(rt->doc_click_fn), jv_dom(dom_node_id), &ev_v, 1);
        fired = true;
    }
    rt->ctrl = CF_NONE;
    return fired || rt->dom_dirty || rt->has_alert || rt->has_nav || rt->has_doc_write;
}

int js_dom_get_body_id(JsRuntime* rt) {
    return rt ? rt->body_node_id : -1;
}

int js_dom_create_element(JsRuntime* rt, const char* tag, int parent_id) {
    if (!rt || rt->node_len >= JS_MAX_DOM_NODES) return -1;
    int id = rt->node_len++;
    JsDomNode* n = &rt->nodes[id];
    uefi_memset(n, 0, sizeof(JsDomNode));
    n->id = id;
    n->parent_id = -1;
    n->first_child = -1;
    n->next_sibling = -1;
    n->js_obj_handle = -1;
    n->click_fn_handle = -1;
    js_strcpy_max(n->tag, tag ? tag : "div", sizeof(n->tag));
    if (parent_id >= 0) dom_attach_child(rt, parent_id, id);
    return id;
}

int js_dom_create_text(JsRuntime* rt, const char* text, int parent_id) {
    int id = js_dom_create_element(rt, "#text", parent_id);
    if (id < 0) return -1;
    rt->nodes[id].is_text_node = true;
    js_strcpy_max(rt->nodes[id].text, text ? text : "", sizeof(rt->nodes[id].text));
    return id;
}

JsDomNode* js_dom_get_node(JsRuntime* rt, int node_id) {
    if (!rt || node_id < 0 || node_id >= rt->node_len) return NULL;
    return &rt->nodes[node_id];
}

int js_dom_get_node_count(JsRuntime* rt) { return rt ? rt->node_len : 0; }
bool js_dom_is_dirty(JsRuntime* rt) { return rt ? rt->dom_dirty : false; }
void js_dom_clear_dirty(JsRuntime* rt) { if (rt) rt->dom_dirty = false; }
const char* js_dom_get_title(JsRuntime* rt) { return rt ? rt->title : ""; }
void js_dom_set_title(JsRuntime* rt, const char* title) {
    if (!rt || !title) return;
    js_strcpy_max(rt->title, title, sizeof(rt->title));
}

bool js_take_doc_write(JsRuntime* rt, char* out_html, int max_len) {
    if (!rt || !rt->has_doc_write) return false;
    js_strcpy_max(out_html, rt->pending_doc_write, max_len);
    rt->has_doc_write = false;
    rt->pending_doc_write[0] = 0;
    return true;
}

bool js_take_alert(JsRuntime* rt, char* out_msg, int max_len) {
    if (!rt || !rt->has_alert) return false;
    js_strcpy_max(out_msg, rt->pending_alert, max_len);
    rt->has_alert = false;
    rt->pending_alert[0] = 0;
    return true;
}

bool js_take_navigate(JsRuntime* rt, char* out_url, int max_len) {
    if (!rt || !rt->has_nav) return false;
    js_strcpy_max(out_url, rt->pending_nav, max_len);
    rt->has_nav = false;
    rt->pending_nav[0] = 0;
    return true;
}

int js_console_count(JsRuntime* rt) { return rt ? rt->console_len : 0; }
const JsConsoleLine* js_console_get(JsRuntime* rt, int index) {
    if (!rt || index < 0 || index >= rt->console_len) return NULL;
    return &rt->console_lines[index];
}
void js_console_clear(JsRuntime* rt) { if (rt) rt->console_len = 0; }
const char* js_last_error(JsRuntime* rt) { return rt ? rt->last_error : ""; }

} // extern "C"
