# Writing Tome mods in C++

`#include <tome/tome.hpp>` (C++17 or later, header-only, next to `tome.h`).
It wraps no API function in particular: one generic call reaches every
function of the table, including ones added to `tome.h` later, so the header
needs no update when the API grows.

```cpp
#include <tome/tome.hpp>

static int32_t my_log(tome_jass_integer value) {
    tome::mod().log(TOME_LOG_INFO, "MyLog(%d)", (int)value);
    return 0;
}

static bool load(tome::Mod mod) {
    mod.log(TOME_LOG_INFO, "loaded in Tome %s", mod.api()->tome_version);
    return true;
}

static void enable(tome::Mod mod) {
    mod.call(&tome_api::register_native, "MyLog", tome::native<my_log>);
    mod.call(&tome_api::on_event, TOME_EVENT_GAME_START, [mod](const tome_event_info* info) {
        mod.call(&tome_api::display_text, std::string("Welcome to ") + info->map_name, 5.0f);
    });
}

TOME_EXPORT_MOD(load, enable, nullptr, nullptr)
```

`mod.call(&tome_api::<function>, args...)`:

- checks that the running Tome's table has the function. If it doesn't (an
  older Tome), it returns `TOME_ERR_UNAVAILABLE`, or 0/NULL for functions
  returning something else. `mod.has(&tome_api::popup)` asks first;
- passes the mod's `tome_mod*` where the function takes one;
- takes a lambda wherever the function takes a callback and its `user`
  pointer. The lambda gets the callback's arguments without `user`, with or
  without the leading `tome_mod*`. It's kept while Tome may call it: until
  the mod is disabled, until the same button (`frame_on_click`) or key
  (`on_keybind`) is registered again, until a popup's has run, or not at
  all if Tome refused it. Register callbacks in `enable`;
- takes a `std::string` for a `const char*`.

Also in the header:
- `tome::native<fn>`: a native with exceptions caught.
- `mod.text(&tome_api::setting_string, "key")`: a whole string from the
  snprintf-style getters.
- `tome::sized<tome_popup>()`: a versioned struct with its `size` set.
- `tome::has_field(info, &tome_event_info::amount)`: true if the running
  Tome filled that field.
- `mod.log(level, fmt, ...)`.
- `tome::mod()`: the handle anywhere once the mod is loaded.

An exception that leaves a lambda, a native or a lifecycle function would
end the game, because MinGW calls `std::terminate`. The header catches it,
logs `what()` and reports a crash to Tome (`TOME_EXCEPTION_MOD_FAILED`), so
the mod is disabled as for a fault. Threads a mod starts itself aren't
guarded.

Build with `tome_add_mod()` (`sdk/TomeMod.cmake`) as for C. It links the
runtime statically and keeps MinGW's CRT from changing the game threads'
x87 precision (`sdk/support/fpreset.c`); a mod built some other way needs
both.
