/* SPDX-License-Identifier: MIT-0 */
/*
 * The runtime of a WebAssembly mod (tome_wasm.h), compiled into it by
 * tome_add_wasm_mod(): the exports Tome calls back through (tome_buffer,
 * tome_callback, tome_native), the callback table behind the wrappers, and
 * the few libc functions the compiler may emit calls to (a Wasm mod has no
 * libc, and anything it imports from outside "tome" is refused).
 */
#include <tome/tome_wasm.h>

/* --- what clang may call ------------------------------------------------- */

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (; n; --n, ++x, ++y) {
        if (*x != *y) return *x - *y;
    }
    return 0;
}

size_t tome_strlen(const char* s) {
    size_t n = 0;
    if (s) while (s[n]) ++n;
    return n;
}

/* --- buffers ----------------------------------------------------------------
 * Tome copies what it hands the mod into a buffer from tome_buffer(size),
 * then calls back with it; the data is valid until that callback returns.
 * Callbacks can nest (a message the mod publishes to itself arrives while
 * the call that published it runs), so the buffer is a stack: a running
 * callback keeps its part (g_used) until it returns. What doesn't fit gets
 * pages of its own: reused at the outermost level, new ones nested. */

static unsigned char g_small[64 * 1024];
static uint32_t g_used = 0;  /* bytes of g_small held by callbacks running now */
static uint32_t g_depth = 0; /* callbacks running now */
static unsigned char* g_big = 0;
static uint32_t g_big_size = 0;
static uint32_t g_pending = 0; /* the size of the last tome_buffer: the block the next call gets */

static unsigned char* grow(uint32_t need) {
    const uint32_t pages = (need + 65535) / 65536;
    const int32_t old = __builtin_wasm_memory_grow(0, pages);
    if (old < 0) return 0;
    return (unsigned char*)(uintptr_t)((uint32_t)old * 65536u);
}

TOME_WASM_EXPORT(tome_buffer) uint32_t tome_buffer(uint32_t size) {
    const uint32_t need = size + 1; /* room for a NUL after text */
    g_pending = size;
    if (need <= sizeof(g_small) - g_used) return (uint32_t)(uintptr_t)(g_small + g_used);
    if (g_depth > 0) return (uint32_t)(uintptr_t)grow(need); /* 0 if it can't: Tome reports it */
    if (need > g_big_size) {
        unsigned char* p = grow(need);
        if (!p) return 0;
        g_big = p;
        g_big_size = (need + 65535) / 65536 * 65536;
    }
    return (uint32_t)(uintptr_t)g_big;
}

/* Holds the callback's data (at `ptr`, `len` bytes and a NUL) until end(). */
static uint32_t begin(uint32_t ptr, uint32_t len) {
    const uint32_t saved = g_used;
    const uintptr_t small = (uintptr_t)g_small;
    if (ptr >= small && ptr < small + sizeof(g_small)) {
        const uint32_t end = (uint32_t)(ptr - small) + len + 1;
        g_used = (end + 7) & ~7u;
        if (g_used > sizeof(g_small)) g_used = sizeof(g_small);
    }
    ++g_depth;
    return saved;
}

static void end(uint32_t saved) {
    --g_depth;
    g_used = saved;
}

/* --- callbacks --------------------------------------------------------------- */

enum kind { K_TEXT = 1, K_EVENT, K_UI, K_CLICK, K_POPUP, K_MESSAGE, K_NATIVE };

typedef struct entry {
    int kind;
    void* fn;
    void* user;
} entry;

#define MAX_CALLBACKS 1024
static entry g_callbacks[MAX_CALLBACKS];
static uint32_t g_callback_count = 0;

/* An id for (kind, fn, user), from 1; 0 if the table is full. A slot comes
 * back once Tome can't call it any more (release): a popup that called
 * back, a registration Tome refused, every slot once Tome revoked the
 * mod's registrations (tome_callbacks_revoked). */
static uint32_t add(int kind, void* fn, void* user) {
    if (!fn) return 0;
    uint32_t i = 0;
    while (i < g_callback_count && g_callbacks[i].kind != 0) ++i;
    if (i == g_callback_count) {
        if (g_callback_count >= MAX_CALLBACKS) return 0;
        ++g_callback_count;
    }
    g_callbacks[i].kind = kind;
    g_callbacks[i].fn = fn;
    g_callbacks[i].user = user;
    return i + 1;
}

static void release(uint32_t id) {
    if (id != 0 && id <= g_callback_count) g_callbacks[id - 1].kind = 0;
}

/* Tome calls this when the mod is enabled again after a disable, which
 * revoked every registration: no id from before can be called back. */
TOME_WASM_EXPORT(tome_callbacks_revoked) void tome_callbacks_revoked(void) {
    for (uint32_t i = 0; i < g_callback_count; ++i) g_callbacks[i].kind = 0;
    g_callback_count = 0;
}

/* A registration's result: a refused one gives its slot back. */
static int32_t registered(uint32_t id, int32_t result) {
    if (result != TOME_OK) release(id);
    return result;
}

static const entry* get(uint32_t id, int kind) {
    if (id == 0 || id > g_callback_count || g_callbacks[id - 1].kind != kind) return 0;
    return &g_callbacks[id - 1];
}

static uint32_t word(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void callback(uint32_t id, uint32_t ptr, uint32_t len);

TOME_WASM_EXPORT(tome_callback) void tome_callback(uint32_t id, uint32_t ptr, uint32_t len) {
    const uint32_t saved = begin(ptr, len);
    callback(id, ptr, len);
    end(saved);
}

static void callback(uint32_t id, uint32_t ptr, uint32_t len) {
    if (id == 0 || id > g_callback_count) return;
    const entry* e = &g_callbacks[id - 1];
    unsigned char* data = (unsigned char*)(uintptr_t)ptr;
    switch (e->kind) {
    case K_TEXT:
        data[len] = 0; /* tome_buffer left room */
        ((tome_text_fn)e->fn)((const char*)data, e->user);
        break;
    case K_EVENT: ((tome_wasm_event_fn)e->fn)((const tome_wasm_event*)data, e->user); break;
    case K_UI:
        if (len >= 4) ((tome_ui_fn)e->fn)((int32_t)word(data), e->user);
        break;
    case K_CLICK:
        if (len >= 4) ((tome_click_fn)e->fn)(word(data), e->user);
        break;
    case K_POPUP: {
        /* A popup calls back once: its slot is free again (and may be
         * taken by a popup fn shows). */
        const tome_popup_fn fn = (tome_popup_fn)e->fn;
        void* const user = e->user;
        release(id);
        if (len >= 8) fn(word(data), (int32_t)word(data + 4), user);
        break;
    }
    case K_MESSAGE: {
        /* [topic length][topic][data]: the topic is NUL-terminated in place
         * by moving it back over its length. */
        if (len < 4) break;
        const uint32_t topic_len = word(data);
        if (topic_len > len - 4) break;
        memmove(data, data + 4, topic_len);
        data[topic_len] = 0;
        ((tome_message_fn)e->fn)((const char*)data, data + 4 + topic_len, len - 4 - topic_len, e->user);
        break;
    }
    }
}

TOME_WASM_EXPORT(tome_native) uint32_t tome_native(uint32_t id, uint32_t args, uint32_t count) {
    const entry* e = get(id, K_NATIVE);
    if (!e) return 0;
    /* The words, and the strings after them: the block tome_buffer sized. */
    const uint32_t held = g_pending > 4 * count ? g_pending : 4 * count;
    const uint32_t saved = begin(args, held);
    const uint32_t result = ((tome_native_fn)e->fn)((const uint32_t*)(uintptr_t)args, e->user);
    end(saved);
    return result;
}

/* --- the wrappers -------------------------------------------------------------- */

int32_t tome_register_command(const char* name, tome_text_fn fn, void* user) {
    const uint32_t id = add(K_TEXT, (void*)fn, user);
    if (!id) return TOME_ERR_INVALID_ARGUMENT;
    return registered(id, tome_raw_register_command(name, (uint32_t)tome_strlen(name), id));
}

int32_t tome_on_setting_changed(tome_text_fn fn, void* user) {
    const uint32_t id = add(K_TEXT, (void*)fn, user);
    return id ? registered(id, tome_raw_on_setting_changed(id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_on_event(int32_t event, tome_wasm_event_fn fn, void* user) {
    const uint32_t id = add(K_EVENT, (void*)fn, user);
    return id ? registered(id, tome_raw_on_event(event, id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_on_ui(int32_t ui_event, tome_ui_fn fn, void* user) {
    const uint32_t id = add(K_UI, (void*)fn, user);
    return id ? registered(id, tome_raw_on_ui(ui_event, id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_frame_on_click(tome_frame_t button, tome_click_fn fn, void* user) {
    const uint32_t id = add(K_CLICK, (void*)fn, user);
    return id ? registered(id, tome_raw_frame_on_click(button, id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_on_keybind(const char* key, tome_text_fn fn, void* user) {
    const uint32_t id = add(K_TEXT, (void*)fn, user);
    return id ? registered(id, tome_raw_on_keybind(key, (uint32_t)tome_strlen(key), id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_subscribe(const char* topic, tome_message_fn fn, void* user) {
    const uint32_t id = add(K_MESSAGE, (void*)fn, user);
    return id ? registered(id, tome_raw_subscribe(topic, (uint32_t)tome_strlen(topic), id)) : TOME_ERR_INVALID_ARGUMENT;
}

int32_t tome_register_native(const char* name, tome_native_fn fn, void* user) {
    const uint32_t id = add(K_NATIVE, (void*)fn, user);
    if (!id) return TOME_ERR_INVALID_ARGUMENT;
    return registered(id, tome_raw_register_native(name, (uint32_t)tome_strlen(name), id));
}

int32_t tome_popup(const char* title, const char* text, const char* const* options, int32_t option_count,
                   int32_t default_option, int32_t cancel_option, tome_popup_fn fn, void* user, uint32_t* id) {
    uint32_t labels[2 * 32];
    if (option_count <= 0 || option_count > 32) return TOME_ERR_INVALID_ARGUMENT;
    for (int32_t i = 0; i < option_count; ++i) {
        labels[2 * i] = (uint32_t)(uintptr_t)options[i];
        labels[2 * i + 1] = (uint32_t)tome_strlen(options[i]);
    }
    const uint32_t spec[9] = {
        (uint32_t)(uintptr_t)title, (uint32_t)tome_strlen(title), (uint32_t)(uintptr_t)text,
        (uint32_t)tome_strlen(text), (uint32_t)(uintptr_t)labels, (uint32_t)option_count,
        (uint32_t)default_option, (uint32_t)cancel_option, 0,
    };
    uint32_t callback = 0;
    if (fn) {
        callback = add(K_POPUP, (void*)fn, user);
        if (!callback) return TOME_ERR_INVALID_ARGUMENT;
    }
    const int32_t result = tome_raw_popup(spec, callback, id);
    if (callback) registered(callback, result);
    return result;
}

/* Copies out (NUL-terminated, cut to fit) what a raw out-parameter call
 * wrote; returns the full length. */
static uint32_t terminate(char* out, uint32_t size, uint32_t full) {
    if (full == UINT32_MAX) full = 0;
    if (size) out[full < size ? full : size - 1] = 0;
    return full;
}

uint32_t tome_mod_id(char* out, uint32_t size) {
    return terminate(out, size, tome_raw_mod_id(out, size ? size - 1 : 0));
}

uint32_t tome_mod_version(char* out, uint32_t size) {
    return terminate(out, size, tome_raw_mod_version(out, size ? size - 1 : 0));
}

uint32_t tome_setting_string(const char* key, char* out, uint32_t size) {
    return terminate(out, size, tome_raw_setting_string(key, (uint32_t)tome_strlen(key), out, size ? size - 1 : 0));
}

uint32_t tome_keybind_name(const char* key, char* out, uint32_t size) {
    return terminate(out, size, tome_raw_keybind_name(key, (uint32_t)tome_strlen(key), out, size ? size - 1 : 0));
}

static unsigned char g_info[256];

const tome_wasm_game_info* tome_game_info(void) {
    const uint32_t n = tome_raw_game_info(g_info, sizeof(g_info));
    if (n == UINT32_MAX || n > sizeof(g_info)) {
        static const tome_wasm_game_info none = {sizeof(tome_wasm_game_info), "", "", -1};
        return &none;
    }
    return (const tome_wasm_game_info*)g_info;
}

/* 12 units and their names. */
static unsigned char g_selection[12 * sizeof(tome_wasm_unit) + 12 * 128];

int32_t tome_selection(tome_wasm_unit* out, int32_t max, int32_t* leader) {
    uint32_t count = 0;
    int32_t active = -1;
    const uint32_t n = tome_raw_selection(g_selection, sizeof(g_selection), &count, &active);
    if (leader) *leader = active;
    if (n == UINT32_MAX || n > sizeof(g_selection)) return 0;
    const tome_wasm_unit* units = (const tome_wasm_unit*)g_selection;
    for (int32_t i = 0; i < max && (uint32_t)i < count; ++i) out[i] = units[i];
    return (int32_t)count;
}

/* One unit and its name. */
static unsigned char g_unit[sizeof(tome_wasm_unit) + 128];

int32_t tome_unit_info(uint32_t unit, tome_wasm_unit* out) {
    const uint32_t n = tome_raw_unit_info(unit, g_unit, sizeof(g_unit));
    if (n == UINT32_MAX || n > sizeof(g_unit)) return TOME_ERR_INVALID_ARGUMENT;
    *out = *(const tome_wasm_unit*)g_unit;
    return TOME_OK;
}

/* A string result of jass_call. */
static char g_jass_text[4096];

int32_t tome_jass_call(const char* name, const tome_jass_value* args, int32_t count, tome_jass_value* result) {
    tome_jass_value ignored;
    return tome_raw_jass_call(name, (uint32_t)tome_strlen(name), args, count, result ? result : &ignored, g_jass_text,
                              sizeof(g_jass_text));
}

char* tome_append(char* buf, uint32_t size, const char* text) {
    uint32_t n = (uint32_t)tome_strlen(buf);
    while (text && *text && n + 1 < size) buf[n++] = *text++;
    if (size) buf[n < size ? n : size - 1] = 0;
    return buf;
}

char* tome_append_int(char* buf, uint32_t size, int64_t value) {
    char digits[24];
    int i = 0;
    uint64_t v = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    do {
        digits[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char text[25];
    int n = 0;
    if (value < 0) text[n++] = '-';
    while (i) text[n++] = digits[--i];
    text[n] = 0;
    return tome_append(buf, size, text);
}
