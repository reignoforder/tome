// SPDX-License-Identifier: MIT-0
/*
 * C++ helpers for Tome mods (C++17 or later), on top of the C API in
 * tome.h, which documents every function. Header-only.
 *
 * Nothing here wraps one API function in particular, so a function added
 * to tome.h works from C++ without changes to this file:
 *
 *   tome::Mod::call(&tome_api::<function>, args...)
 *
 * calls any function of the table. It checks first that the running Tome's
 * table has that function (an older Tome returns TOME_ERR_UNAVAILABLE, or
 * a zero/NULL result for functions that return something else), passes the
 * mod's tome_mod* where the function takes one, accepts a std::string for a
 * const char*, and takes a lambda (or any callable) wherever the function
 * takes a callback and its `user` pointer. The lambda gets the callback's
 * arguments without `user`, with or without the leading tome_mod*:
 *
 *   mod.call(&tome_api::on_event, TOME_EVENT_GAME_START, [](const tome_event_info* info) {...});
 *   mod.call(&tome_api::register_command, "hi", [mod](const char* args) {...});
 *
 * Lambdas are kept while Tome may call them: until the mod is disabled,
 * until a registration of the same button (frame_on_click) or key
 * (on_keybind) replaces them, until a popup's has run, or not at all if Tome
 * refused the registration. Register them in enable, on the main thread.
 * Hooks run on other threads too: keep their state thread-safe.
 *
 * Exceptions (when the mod is built with them): one that leaves a lambda, a
 * native wrapped in tome::native or a lifecycle function would end the game
 * (MinGW calls std::terminate). These helpers catch it, log what() and hand
 * Tome a crash (TOME_EXCEPTION_MOD_FAILED), so Tome stops the call and
 * disables the mod as for a fault.
 *
 * The lifecycle: write the functions you need and export them with
 *
 *   bool load(tome::Mod mod);          // false: the mod stays unloaded
 *   void enable(tome::Mod mod);        // and disable, unload; or nullptr
 *   TOME_EXPORT_MOD(load, enable, nullptr, nullptr)
 *
 * tome::mod() gives the same handle anywhere (natives, threads) once loaded.
 */
#ifndef TOME_TOME_HPP
#define TOME_TOME_HPP

#include "tome.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/* The same declarations as <windows.h>'s, so the two can be included together. */
extern "C" __declspec(dllimport) void __stdcall RaiseException(unsigned long code, unsigned long flags,
                                                               unsigned long count, const unsigned long* args);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);

namespace tome {

class Mod;

namespace detail {

inline const tome_api*& api_slot() {
    static const tome_api* api = nullptr;
    return api;
}

inline tome_mod*& mod_slot() {
    static tome_mod* mod = nullptr;
    return mod;
}

/* True if `api` is big enough to have the field at `field` (any member of tome_api). */
template <class T>
bool table_has(const tome_api* api, const T* field) {
    auto offset = reinterpret_cast<const char*>(field) - reinterpret_cast<const char*>(api);
    return api && static_cast<size_t>(offset) + sizeof(T) <= api->size;
}

[[noreturn]] inline void fail(const char* what) {
    const tome_api* api = api_slot();
    if (api) {
        char line[512];
        snprintf(line, sizeof(line), "uncaught exception: %s", what ? what : "(unknown)");
        api->log(mod_slot(), TOME_LOG_ERROR, line);
    }
    RaiseException(TOME_EXCEPTION_MOD_FAILED, 1 /* EXCEPTION_NONCONTINUABLE */, 0, nullptr);
    std::terminate();  // only without Tome's guard above: a thread the mod started
}

}  // namespace detail

/* Runs f(), turning an exception that leaves it into a crash Tome reports. */
template <class F>
decltype(auto) guard(F&& f) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    // The crash is raised after the catch block has ended: Tome's guard
    // doesn't return here, so raising from inside it would leave the
    // exception object alive on the runtime's caught-exceptions list.
    char what[480];
    what[0] = '\0';
    bool known = false;
    try {
        return f();
    } catch (const std::exception& e) {
        snprintf(what, sizeof(what), "%s", e.what());
        known = true;
    } catch (...) {
    }
    detail::fail(known ? what : nullptr);
#else
    return f();
#endif
}

namespace detail {

// --- callbacks: a lambda behind a (C function, user) pair ---------------------

struct Kept {
    virtual ~Kept() = default;
};

template <class F>
struct KeptFn : Kept {
    explicit KeptFn(F f) : fn(std::move(f)) {}
    F fn;
};

/*
 * The lambdas registered with Tome, and those it no longer calls: refused
 * (the registration failed), replaced (frame_on_click and on_keybind
 * replace a button's or key's callback) or finished (a popup's runs once).
 * Dead ones are freed once none of the mod's callbacks is running on the
 * main thread, where they're all registered and where Tome calls all of
 * them but hooks. Tome revokes them all after disable: those retire, and
 * are freed at the next enable or at unload.
 */
struct Entry {
    std::unique_ptr<Kept> holder;
    void* user;
    std::string key;  // what a later registration replaces it by; "" for none
};

struct Callbacks {
    std::vector<Entry> live;
    std::vector<std::unique_ptr<Kept>> dead;
    std::vector<std::unique_ptr<Kept>> retired;
    unsigned long main_thread = 0;  // where tome_mod_load ran
    int depth = 0;                  // the mod's callbacks running on the main thread now
};

inline Callbacks& callbacks() {
    static Callbacks c;
    return c;
}

inline bool on_main_thread() {
    return callbacks().main_thread == GetCurrentThreadId();
}

inline void bury(size_t index) {
    auto& c = callbacks();
    c.dead.push_back(std::move(c.live[index].holder));
    c.live.erase(c.live.begin() + static_cast<std::ptrdiff_t>(index));
}

inline void collect() {
    auto& c = callbacks();
    if (c.depth > 0 || c.dead.empty() || !on_main_thread()) return;
    std::vector<std::unique_ptr<Kept>> dead;
    dead.swap(c.dead);  // freed outside the list, in case a destructor registers again
}

/* After disable: Tome has revoked everything. */
inline void retire() {
    auto& c = callbacks();
    for (Entry& e : c.live) c.retired.push_back(std::move(e.holder));
    c.live.clear();
}

/* At enable and unload, when none can be running (a crash may have left depth up). */
inline void free_retired() {
    auto& c = callbacks();
    c.depth = 0;
    collect();
    std::vector<std::unique_ptr<Kept>> retired;
    retired.swap(c.retired);
}

/* A callback that runs once (a popup's) is done. */
inline void finished(void* user) {
    auto& live = callbacks().live;
    for (size_t i = 0; i < live.size(); ++i) {
        if (live[i].user == user) return bury(i);
    }
}

/* What Tome did with the callbacks one call registered, from live[first] on. */
inline void settle(size_t first, bool registered, const std::string& key) {
    auto& live = callbacks().live;
    if (!registered) {
        while (live.size() > first) bury(live.size() - 1);
    } else if (!key.empty()) {
        for (size_t i = first; i < live.size(); ++i) live[i].key = key;
        for (size_t i = first; i-- > 0;) {
            if (live[i].key == key) bury(i);  // older ones only: indices below i stay put
        }
    }
    collect();
}

template <class F>
void* keep(F&& f) {
    auto holder = std::make_unique<KeptFn<std::decay_t<F>>>(std::forward<F>(f));
    void* user = &holder->fn;
    callbacks().live.push_back(Entry{std::move(holder), user, std::string()});
    return user;
}

/* Counts the mod's callbacks running on the main thread, and frees dead ones after the last. */
struct Running {
    bool main = on_main_thread();
    Running() {
        if (main) ++callbacks().depth;
    }
    ~Running() {
        if (main && --callbacks().depth == 0) collect();
    }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
};

/* Calls f with the arguments in `args` at indices I..., or without the first
 * one (the tome_mod*) if f doesn't take it. */
template <class F, class Tuple, size_t First, size_t... I>
decltype(auto) invoke_from(F& f, Tuple& args, std::index_sequence<First, I...>) {
    if constexpr (std::is_invocable_v<F&, std::tuple_element_t<First, Tuple>, std::tuple_element_t<I, Tuple>...>) {
        return f(std::get<First>(args), std::get<I>(args)...);
    } else {
        static_assert(std::is_invocable_v<F&, std::tuple_element_t<I, Tuple>...>,
                      "the lambda doesn't take this callback's arguments (without the last, `user`)");
        return f(std::get<I>(args)...);
    }
}

template <class F, class Tuple>
decltype(auto) invoke_from(F& f, Tuple&, std::index_sequence<>) {
    return f();
}

/* The C function Tome calls for a callback of type Fn (its last parameter is `user`). */
template <class Fn>
struct Thunk;

template <class R, class... P>
struct Thunk<R (*)(P...)> {
    template <class F>
    static R TOME_CALL call(P... p) {
        auto args = std::tuple<P...>(p...);
        void* user = std::get<sizeof...(P) - 1>(args);
        F& f = *static_cast<F*>(user);
        Running running;
        // A popup's callback runs once: dead afterwards (freed once nothing runs).
        struct Once {
            void* user;
            ~Once() {
                if (std::is_same_v<R (*)(P...), tome_popup_fn> && callbacks().main_thread) finished(user);
            }
        } once{user};
        return guard([&]() -> R {
            if constexpr (std::is_void_v<R>) {
                invoke_from(f, args, std::make_index_sequence<sizeof...(P) - 1>());
            } else {
                return static_cast<R>(invoke_from(f, args, std::make_index_sequence<sizeof...(P) - 1>()));
            }
        });
    }
};

template <class T>
struct is_callback : std::false_type {};

template <class R, class... P>
struct is_callback<R (*)(P...)>
    : std::bool_constant<(sizeof...(P) > 0) &&
                         std::is_same_v<std::tuple_element_t<sizeof...(P) - 1, std::tuple<P...>>, void*>> {};

// --- arguments: what the caller passes to what the C function takes -----------

template <class... P>
struct Params {};

inline auto bind(tome_mod*, Params<>) {
    return std::tuple<>();
}

template <class P0, class... Ps, class... A>
auto bind(tome_mod* mod, Params<P0, Ps...>, A&&... a);

template <class P0, class... Ps, class A0, class... A>
auto bind_first(tome_mod* mod, A0&& a0, A&&... a);

template <class P0, class P1, class... Ps, class A0, class... A>
auto bind_callback(tome_mod* mod, Params<P0, P1, Ps...>, A0&& a0, A&&... a) {
    static_assert(std::is_same_v<P1, void*>, "a callback must be followed by its user pointer");
    using F = std::decay_t<A0>;
    void* user = keep(std::forward<A0>(a0));
    P0 fn = &Thunk<P0>::template call<F>;
    return std::tuple_cat(std::make_tuple(fn, user), bind(mod, Params<Ps...>(), std::forward<A>(a)...));
}

template <class P0, class... Ps, class... A>
auto bind(tome_mod* mod, Params<P0, Ps...>, A&&... a) {
    if constexpr (std::is_same_v<P0, tome_mod*> &&
                  (sizeof...(A) == 0 || !std::is_convertible_v<std::tuple_element_t<0, std::tuple<A..., int>>, tome_mod*>)) {
        // The calling mod, filled in.
        return std::tuple_cat(std::make_tuple(mod), bind(mod, Params<Ps...>(), std::forward<A>(a)...));
    } else {
        static_assert(sizeof...(A) > 0, "too few arguments for this tome_api function");
        return bind_first<P0, Ps...>(mod, std::forward<A>(a)...);
    }
}

template <class P0, class... Ps, class A0, class... A>
auto bind_first(tome_mod* mod, A0&& a0, A&&... a) {
    using Arg = std::decay_t<A0>;
    if constexpr (is_callback<P0>::value && !std::is_convertible_v<A0, P0> && !std::is_null_pointer_v<Arg>) {
        return bind_callback(mod, Params<P0, Ps...>(), std::forward<A0>(a0), std::forward<A>(a)...);
    } else if constexpr (std::is_same_v<P0, const char*> && std::is_same_v<Arg, std::string>) {
        return std::tuple_cat(std::make_tuple(a0.c_str()), bind(mod, Params<Ps...>(), std::forward<A>(a)...));
    } else if constexpr (std::is_same_v<P0, void*> && std::is_pointer_v<Arg> &&
                         std::is_function_v<std::remove_pointer_t<Arg>>) {
        // A function as void* (register_native).
        return std::tuple_cat(std::make_tuple(reinterpret_cast<void*>(a0)),
                              bind(mod, Params<Ps...>(), std::forward<A>(a)...));
    } else {
        return std::tuple_cat(std::make_tuple(static_cast<P0>(std::forward<A0>(a0))),
                              bind(mod, Params<Ps...>(), std::forward<A>(a)...));
    }
}

template <class R>
R unavailable() {
    if constexpr (std::is_same_v<R, tome_result>) {
        return TOME_ERR_UNAVAILABLE;
    } else if constexpr (!std::is_void_v<R>) {
        return R{};
    }
}

}  // namespace detail

// --- the mod's handle ---------------------------------------------------------------

class Mod {
public:
    Mod() = default;
    Mod(const tome_api* api, tome_mod* mod) : api_(api), mod_(mod) {}

    const tome_api* api() const { return api_; }
    tome_mod* handle() const { return mod_; }
    explicit operator bool() const { return api_ && mod_; }

    /* True if the running Tome has this function: has(&tome_api::popup). */
    template <class F>
    bool has(F tome_api::*field) const {
        return api_ && detail::table_has(api_, &(api_->*field)) && api_->*field;
    }

    /* Calls any function of the table: see the top of this file. */
    template <class R, class... P, class... A>
    R call(R (*tome_api::*field)(P...), A&&... args) const {
        if (!has(field)) return detail::unavailable<R>();
        size_t first = detail::callbacks().live.size();
        auto bound = detail::bind(mod_, detail::Params<P...>(), std::forward<A>(args)...);
        if constexpr (std::is_void_v<R>) {
            std::apply(api_->*field, bound);
        } else {
            R result = std::apply(api_->*field, bound);
            if (detail::callbacks().live.size() > first) {
                // Tome keeps no callback it refused, and replaces a button's or a key's.
                bool registered = true;
                if constexpr (std::is_same_v<R, tome_result>) registered = result == TOME_OK;
                detail::settle(first, registered, replace_key(field, bound));
            }
            return result;
        }
    }

    /*
     * A string from one of the snprintf-style getters (setting_string,
     * keybind_name), whole: text(&tome_api::setting_string, "greeting").
     * "" if there's none or the running Tome lacks the function.
     */
    std::string text(size_t (*tome_api::*field)(tome_mod*, const char*, char*, size_t), const std::string& key) const {
        if (!has(field)) return std::string();
        size_t len = (api_->*field)(mod_, key.c_str(), nullptr, 0);
        std::string out(len, '\0');
        if (len) (api_->*field)(mod_, key.c_str(), &out[0], len + 1);
        return out;
    }

    /* printf-style line in tome.log, tagged with the mod's id. */
#ifdef __MINGW_PRINTF_FORMAT
    __attribute__((format(__MINGW_PRINTF_FORMAT, 3, 4)))
#endif
    void log(tome_log_level level, const char* fmt, ...) const {
        if (!api_) return;
        char line[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        api_->log(mod_, level, line);
    }

private:
    // frame_on_click and on_keybind replace the callback of the same button or
    // key: what identifies it (their first argument after the mod).
    template <class F, class Tuple>
    std::string replace_key(F tome_api::*field, const Tuple& bound) const {
        auto offset = reinterpret_cast<const char*>(&(api_->*field)) - reinterpret_cast<const char*>(api_);
        std::string kind = offset == static_cast<std::ptrdiff_t>(offsetof(tome_api, frame_on_click)) ? "click:"
                           : offset == static_cast<std::ptrdiff_t>(offsetof(tome_api, on_keybind))  ? "key:"
                                                                                                    : "";
        if constexpr (std::tuple_size_v<Tuple> > 1) {
            using T = std::tuple_element_t<1, Tuple>;
            if (kind.empty()) return kind;
            if constexpr (std::is_same_v<T, const char*>) {
                return kind + (std::get<1>(bound) ? std::get<1>(bound) : "");
            } else if constexpr (std::is_pointer_v<T> && !std::is_function_v<std::remove_pointer_t<T>>) {
                char text[24];
                snprintf(text, sizeof(text), "%p", static_cast<const void*>(std::get<1>(bound)));
                return kind + text;
            }
        }
        return std::string();
    }

    const tome_api* api_ = nullptr;
    tome_mod* mod_ = nullptr;
};

/* The mod's handle, from anywhere once tome_mod_load has run (natives, threads). */
inline Mod mod() {
    return Mod(detail::api_slot(), detail::mod_slot());
}

/*
 * True if Tome filled `field` of a versioned struct (one whose first member
 * is `size`): has_field(info, &tome_event_info::amount).
 */
template <class S, class T>
bool has_field(const S* s, T S::*field) {
    if (!s) return false;
    auto offset = reinterpret_cast<const char*>(&(s->*field)) - reinterpret_cast<const char*>(s);
    return static_cast<size_t>(offset) + sizeof(T) <= s->size;
}

/* A versioned struct for Tome to read, zeroed with `size` set: sized<tome_popup>(). */
template <class S>
S sized() {
    S s;
    std::memset(&s, 0, sizeof(s));
    s.size = static_cast<uint32_t>(sizeof(S));
    return s;
}

/*
 * A native in the engine's ABI (tome.h, "JASS native ABI") with exceptions
 * turned into crashes Tome reports:
 *
 *   static int32_t my_log(tome_jass_integer value) {...}
 *   mod.call(&tome_api::register_native, "MyLog", tome::native<my_log>);
 */
namespace detail {
template <auto F>
struct Native;

template <class R, class... P, R (*F)(P...)>
struct Native<F> {
    static R TOME_CALL call(P... p) {
        return guard([&]() -> R { return F(p...); });
    }
};
}  // namespace detail

template <auto F>
constexpr auto native = &detail::Native<F>::call;

// --- lifecycle -------------------------------------------------------------------

namespace detail {

inline int load(const tome_api* api, tome_mod* mod, bool (*fn)(Mod)) {
    if (!api || !mod) return 1;
    api_slot() = api;
    mod_slot() = mod;
    callbacks().main_thread = GetCurrentThreadId();
    return guard([&] { return fn(Mod(api, mod)); }) ? 0 : 1;
}

inline void enable(tome_mod* mod, void (*fn)(Mod)) {
    free_retired();
    if (fn) guard([&] { fn(Mod(api_slot(), mod)); });
}

inline void disable(tome_mod* mod, void (*fn)(Mod)) {
    retire();  // first: Tome revokes everything even if fn throws
    if (fn) guard([&] { fn(Mod(api_slot(), mod)); });
}

inline void unload(tome_mod* mod, void (*fn)(Mod)) {
    if (fn) guard([&] { fn(Mod(api_slot(), mod)); });
    retire();
    free_retired();
}

}  // namespace detail
}  // namespace tome

/* Exports the lifecycle functions; pass nullptr for those the mod doesn't need. */
#define TOME_EXPORT_MOD(load_fn, enable_fn, disable_fn, unload_fn)                                    \
    TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* api, tome_mod* mod) {                     \
        return ::tome::detail::load(api, mod, load_fn);                                               \
    }                                                                                                 \
    TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) { ::tome::detail::enable(mod, enable_fn); } \
    TOME_EXPORT void TOME_CALL tome_mod_disable(tome_mod* mod) { ::tome::detail::disable(mod, disable_fn); } \
    TOME_EXPORT void TOME_CALL tome_mod_unload(tome_mod* mod) { ::tome::detail::unload(mod, unload_fn); }

#endif /* TOME_TOME_HPP */
