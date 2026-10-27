#include "mod_manager.h"

#include "api.h"
#include "core/hash.h"
#include "core/json.h"
#include "frames.h"
#include "guard.h"
#include "hooks.h"
#include "keybinds.h"
#include "log.h"
#include "main_thread.h"
#include "mod_menu.h"
#include "natives.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace tome {
namespace {

constexpr int kStateSchema = 1;
constexpr unsigned kReleaseRetryMs = 250;

using Gates = std::vector<std::shared_ptr<hooks::Gate>>;

// Set while this thread is inside refresh_locked or shutdown (holding the
// lock): a crash there is handled at the end of the refresh, not posted.
thread_local bool t_in_refresh = false;

struct InRefresh {
    bool saved = t_in_refresh;
    InRefresh() { t_in_refresh = true; }
    ~InRefresh() { t_in_refresh = saved; }
};

// Frees a mod's DLL once none of its hook handlers runs (they're revoked by
// now, so none starts). If one is still running on another thread, or is on
// this thread's own stack (e.g. the mod menu disabled the mod from a click
// that came through the mod's own OnEvent hook), the DLL stays loaded and the
// main thread tries again every kReleaseRetryMs, so nothing waits for it.
// The shadow copy goes with it.
void release_module(HMODULE module, const std::wstring& shadow, const Gates& gates, const std::string& id,
                    bool retry = false) {
    if (!hooks::idle(gates, 0)) {
        if (!retry) TOME_WARN("%s: a hook handler is still running; freeing the DLL once it returns", id.c_str());
        main_thread::post_delayed(kReleaseRetryMs, [=] { release_module(module, shadow, gates, id, true); });
        return;
    }
    FreeLibrary(module);
    DeleteFileW(shadow.c_str());
    if (retry) TOME_INFO("%s: freed the old DLL", id.c_str());
}

std::wstring to_windows_path(const std::string& relative) {
    std::wstring w = widen(relative);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

// Reads just enough of the PE header to give a clear error for 64-bit or
// non-DLL files, instead of LoadLibrary's generic "not a valid Win32 application".
bool check_pe_i386_dll(const std::wstring& path, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "can't open the DLL";
        return false;
    }
    unsigned char header[1024] = {};
    DWORD read = 0;
    ReadFile(file, header, sizeof(header), &read, nullptr);
    CloseHandle(file);

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(header);
    if (read < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > read) {
        error = "not a Windows DLL";
        return false;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(header + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        error = "not a Windows DLL";
        return false;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        error = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
                    ? "the DLL is 64-bit; Warcraft III needs 32-bit (i686) mods"
                    : "the DLL isn't built for 32-bit x86";
        return false;
    }
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        error = "the file is an executable, not a DLL";
        return false;
    }
    return true;
}

std::string last_error_text(DWORD code) {
    char* buf = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string text = len ? std::string(buf, len) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text + " (" + std::to_string(code) + ")";
}

// LoadLibraryEx with the mod's own folder on the search path, so a mod can
// ship helper DLLs next to its main DLL even though Tome loads a shadow copy.
HMODULE load_with_mod_dir(const std::wstring& dll, const std::wstring& mod_dir) {
    using AddDllDirectoryFn = void*(WINAPI*)(PCWSTR);
    using RemoveDllDirectoryFn = BOOL(WINAPI*)(void*);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto add = reinterpret_cast<AddDllDirectoryFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "AddDllDirectory")));
    auto remove = reinterpret_cast<RemoveDllDirectoryFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "RemoveDllDirectory")));
    if (!add || !remove) {
        return LoadLibraryW(dll.c_str());
    }
    void* cookie = add(mod_dir.c_str());
    // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: application dir, System32 and added dirs.
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, 0x00001000);
    if (cookie) remove(cookie);
    return module;
}

// Marks the mod as starting on this thread for the duration of a lifecycle
// call (tome_mod::starting_on).
struct Starting {
    tome_mod& mod;
    explicit Starting(tome_mod& m) : mod(m) { mod.starting_on.store(GetCurrentThreadId(), std::memory_order_release); }
    ~Starting() { mod.starting_on.store(0, std::memory_order_release); }
};

template <typename Fn>
Fn lookup(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Mixes a file's contents into `hash`, a buffer at a time. False if it can't be read.
bool hash_file(const std::wstring& path, Fnv1a64& hash, std::vector<char>& buffer) {
    HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) && got > 0) {
        hash.mix(buffer.data(), got);
    }
    CloseHandle(in);
    return ok;
}

} // namespace

ModManager& mod_manager() {
    static ModManager instance;
    return instance;
}

void ModManager::init(const Paths& paths, const GameBuild& build) {
    paths_ = paths;
    build_ = build;
    parse_semver(TOME_VERSION, tome_version_);
    parse_game_version(build.file_version, game_version_);
    init_api(build);
    guard::set_fault_sink([](tome_mod* mod) { mod_manager().on_fault(mod); });

    CreateDirectoryW(paths_.mods_dir.c_str(), nullptr);
    last_polled_ = disk_fingerprint();  // baseline for poll_changed(), set before the watcher starts

    // Shadow copies from earlier sessions. Files still in use (another game
    // instance) fail to delete and are left alone.
    std::wstring cache = paths_.tome_dir + L"\\cache";
    CreateDirectoryW(cache.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((cache + L"\\*.dll").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((cache + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
}

// The mod's settings schema ("config" in tome.mod.json) and its saved values.
// Problems with either are reported but don't stop the mod. Each file is
// parsed again only when its stamp changes.
void ModManager::read_settings_schema(Installed& mod) const {
    const std::wstring schema_file = mod.dir + L"\\" + to_windows_path(mod.meta.config);
    const std::wstring values_file = mod.dir + L"\\config\\settings.json";
    // The stamps first: if a file changes while it's read, the next poll sees a newer one.
    FileStamp schema_stamp, values_stamp;
    bool schema_exists = file_stamp(schema_file, schema_stamp);
    bool values_exist = file_stamp(values_file, values_stamp);

    ParsedSettings& cached = settings_cache_[mod.folder];
    if (!cached.schema_read || cached.schema_file != schema_file || !(cached.schema_stamp == schema_stamp)) {
        cached = {};
        cached.schema_read = true;
        cached.schema_file = schema_file;
        cached.schema_stamp = schema_stamp;
        std::string text;
        json::Value root;
        json::ParseResult parsed;
        if (!schema_exists || !read_file(schema_file, text)) {
            cached.error = "can't read " + mod.meta.config;
            cached.unusable = true;
        } else if (parsed = json::parse(text, root); !parsed.ok) {
            cached.error = mod.meta.config + ": " + parsed.error;
            cached.unusable = true;
        } else {
            std::vector<std::string> errors, warnings;
            if (!settings::parse_schema(root, cached.schema, errors, warnings)) {
                cached.error = mod.meta.config + ": " + errors.front();
            }
            for (const std::string& w : warnings) cached.schema_warnings.push_back(mod.meta.config + ": " + w);
        }
    }
    mod.settings_error = cached.error;
    if (cached.unusable) return;
    mod.settings = cached.schema;
    mod.warnings.insert(mod.warnings.end(), cached.schema_warnings.begin(), cached.schema_warnings.end());

    if (!cached.values_read || !(cached.values_stamp == values_stamp)) {
        cached.values_read = true;
        cached.values_stamp = values_stamp;
        cached.values = json::Value();
        cached.values_warning.clear();
        std::string text;
        if (!values_exist || !read_file(values_file, text)) {
            cached.values_stamp = {};
        } else if (json::ParseResult parsed = json::parse(text, cached.values); !parsed.ok) {
            cached.values_warning = "config\\settings.json: " + parsed.error + "; using the defaults";
            cached.values = json::Value();
        }
    }
    mod.settings_stamp = cached.values_stamp;
    mod.stored_settings = cached.values;
    if (!cached.values_warning.empty()) mod.warnings.push_back(cached.values_warning);
}

std::vector<ModManager::Installed> ModManager::discover() const {
    std::vector<Installed> mods;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths_.mods_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return mods;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || name[0] == L'.') continue;

        Installed mod;
        mod.folder = narrow(name);
        mod.dir = paths_.mods_dir + L"\\" + name;

        // tome.mod.json is parsed again only when its stamp changes.
        const std::wstring meta_file = mod.dir + L"\\tome.mod.json";
        FileStamp meta_stamp;
        bool meta_exists = file_stamp(meta_file, meta_stamp);
        ParsedMeta& cached = meta_cache_[mod.folder];
        if (!cached.read || !(cached.stamp == meta_stamp)) {
            cached = {};
            cached.read = true;
            cached.stamp = meta_stamp;
            std::string text;
            json::Value root;
            json::ParseResult parsed;
            if (!meta_exists || !read_file(meta_file, text)) {
                cached.stamp = {};  // read again next time
                cached.errors.push_back("no readable tome.mod.json");
            } else if (parsed = json::parse(text, root); !parsed.ok) {
                cached.errors.push_back("tome.mod.json: " + parsed.error);
            } else {
                MetadataResult meta = parse_metadata(root, cached.meta);
                cached.errors = std::move(meta.errors);
                cached.warnings = std::move(meta.warnings);
                cached.valid = meta.ok;
                if (cached.valid && build_.known) {
                    // Names with a single ':' are capabilities ("event:frame"), not mappings; not checked
                    // yet. "Class::Method" is a mapping.
                    for (const std::string& name : cached.meta.requires_) {
                        const bool capability =
                            name.find(':') != std::string::npos && name.find("::") == std::string::npos;
                        if (!capability && !build_.known->data->find(name)) {
                            cached.errors.push_back("needs \"" + name + "\", which isn't mapped for " +
                                                    build_.known->data->name);
                            cached.valid = false;
                        }
                    }
                }
            }
        }
        mod.meta = cached.meta;
        mod.errors = cached.errors;
        mod.warnings = cached.warnings;
        mod.valid = cached.valid;
        if (mod.valid) {
            std::string entry = mod.meta.entry_for(game_version_);
            if (!entry.empty()) {
                mod.dll = mod.dir + L"\\" + to_windows_path(entry);
                if (!file_stamp(mod.dll, mod.dll_stamp)) {
                    mod.errors.push_back("entry DLL \"" + entry + "\" not found");
                    mod.valid = false;
                }
            } else if (!mod.meta.entry_by_game.empty()) {
                mod.warnings.push_back("no \"entry\" DLL for this game version; loading as assets-only");
            }
            if (!mod.meta.config.empty()) read_settings_schema(mod);
            std::string assets = mod.meta.assets.empty() ? "assets" : mod.meta.assets;
            std::wstring assets_dir = mod.dir + L"\\" + to_windows_path(assets);
            DWORD attributes = GetFileAttributesW(assets_dir.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                overlay::scan_assets(assets_dir, mod.assets, mod.assets_fingerprint, mod.warnings);
            } else if (!mod.meta.assets.empty()) {
                mod.warnings.push_back("\"assets\" folder \"" + mod.meta.assets + "\" not found");
            }
        }
        mods.push_back(std::move(mod));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(mods.begin(), mods.end(), [](const Installed& a, const Installed& b) { return a.folder < b.folder; });
    // Forget folders that are gone.
    std::set<std::string> folders;
    for (const Installed& mod : mods) folders.insert(mod.folder);
    auto gone = [&](const auto& entry) { return !folders.count(entry.first); };
    std::erase_if(meta_cache_, gone);
    std::erase_if(settings_cache_, gone);
    return mods;
}

std::map<std::string, bool> ModManager::read_state(bool* invalid) const {
    std::map<std::string, bool> state;
    if (invalid) *invalid = false;
    std::string text;
    if (!read_file(paths_.tome_dir + L"\\state.json", text)) return state;
    json::Value root;
    json::ParseResult parsed = json::parse(text, root);
    if (!parsed.ok) {
        TOME_WARN("tome/state.json is invalid (%s); treating every mod as enabled", parsed.error.c_str());
        if (invalid) *invalid = true;
        return state;
    }
    const json::Value* mods = root.find("mods");
    if (!mods || !mods->is_object()) return state;
    for (const auto& [id, entry] : mods->members()) {
        const json::Value* enabled = entry.find("enabled");
        if (enabled && enabled->is_bool()) state[id] = enabled->as_bool();
    }
    return state;
}

bool ModManager::wanted_by_user(const std::map<std::string, bool>& state, const std::string& id) const {
    if (!safe_mode_.empty()) return safe_mode_enabled_.count(id) > 0;
    auto saved = state.find(id);
    return saved == state.end() || saved->second;
}

void ModManager::write_state(const std::map<std::string, bool>& state) {
    json::Value root = json::Value::object();
    root.set("schema", json::Value::number(kStateSchema));
    json::Value mods = json::Value::object();
    for (const auto& [id, enabled] : state) {
        json::Value entry = json::Value::object();
        entry.set("enabled", json::Value::boolean(enabled));
        mods.set(id, std::move(entry));
    }
    root.set("mods", std::move(mods));
    const std::wstring path = paths_.tome_dir + L"\\state.json";
    if (!write_file_atomic(path, json::serialize(root))) {
        TOME_WARN("Couldn't write tome/state.json");
        return;
    }
    FileStamp stamp;
    if (file_stamp(path, stamp)) {
        ExclusiveLock hold(state_stamp_lock_);
        written_state_stamp_ = stamp;
    }
}

bool ModManager::load(Runtime& rt, const Installed& mod, std::string& error) {
    if (!check_pe_i386_dll(mod.dll, error)) return false;

    // Load a copy so the original can be rebuilt while the game runs.
    rt.shadow_dll = paths_.tome_dir + L"\\cache\\" + widen(mod.meta.id) + L"." +
                    std::to_wstring(++shadow_counter_) + L".dll";
    if (!CopyFileW(mod.dll.c_str(), rt.shadow_dll.c_str(), FALSE)) {
        error = "couldn't copy the DLL to tome/cache: " + last_error_text(GetLastError());
        return false;
    }

    HMODULE module = load_with_mod_dir(rt.shadow_dll, mod.dir);
    if (!module) {
        error = "LoadLibrary failed: " + last_error_text(GetLastError());
        DeleteFileW(rt.shadow_dll.c_str());
        return false;
    }

    auto load_fn = lookup<tome_mod_load_fn>(module, "tome_mod_load");
    if (!load_fn) {
        error = "the DLL doesn't export tome_mod_load";
        FreeLibrary(module);
        DeleteFileW(rt.shadow_dll.c_str());
        return false;
    }

    rt.module = module;
    rt.source_dll = mod.dll;
    rt.stamp = mod.dll_stamp;
    rt.enable_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_enable");
    rt.disable_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_disable");
    rt.unload_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_unload");

    rt.handle.clear_fault();  // a new DLL starts with a clean record
    int rc = 0;
    bool returned;
    {
        Starting starting(rt.handle);
        returned = guard::run(&rt.handle, "tome_mod_load", nullptr, [&] { rc = load_fn(api_table(), &rt.handle); });
    }
    if (!returned) {
        error = rt.handle.fault_text();
        // The DLL stays loaded: whatever the crash left behind (a thread, a
        // patch of its own) may still point into it.
        rt.handle.revoke_all();
        rt.handle.take_hook_gates();
        rt.module = nullptr;
        return false;
    }
    if (rc != 0) {
        error = "tome_mod_load returned " + std::to_string(rc);
        // Undo whatever it registered before failing (hooks would otherwise
        // keep calling into the freed DLL).
        rt.handle.revoke_all();
        release_module(module, rt.shadow_dll, rt.handle.take_hook_gates(), rt.handle.id);
        rt.module = nullptr;
        return false;
    }
    TOME_INFO("Loaded %s %s", rt.handle.id.c_str(), rt.handle.version.c_str());
    return true;
}

void ModManager::shutdown() {
    ExclusiveLock hold(lock_);
    InRefresh in_refresh;
    if (!shut_down_) {
        shut_down_ = true;
        std::vector<std::string> order = enable_order_;
        for (auto it = order.rbegin(); it != order.rend(); ++it) disable(*runtime_[*it]);
        // Unload in reverse order of the disables, then any loaded mod that wasn't running.
        std::vector<Runtime*> loaded;
        for (auto it = order.rbegin(); it != order.rend(); ++it) loaded.push_back(runtime_[*it].get());
        for (auto& [id, rt] : runtime_) {
            if (std::find(loaded.begin(), loaded.end(), rt.get()) == loaded.end()) loaded.push_back(rt.get());
        }
        size_t count = 0;
        for (Runtime* rt : loaded) {
            if (!rt->module) continue;
            if (rt->unload_fn) {
                guard::run(&rt->handle, "tome_mod_unload", nullptr, [&] { rt->unload_fn(&rt->handle); },
                           guard::kEvenIfFaulted);
            }
            rt->handle.revoke_all();
            ++count;
        }
        TOME_INFO("Exiting: disabled and unloaded %zu mod%s", count, count == 1 ? "" : "s");
    }
}

void ModManager::unload(Runtime& rt) {
    if (!rt.module) return;
    // A crashed mod still gets its tome_mod_disable and tome_mod_unload, to
    // undo what Tome can't (its own threads or patches).
    if (rt.unload_fn) {
        guard::run(&rt.handle, "tome_mod_unload", nullptr, [&] { rt.unload_fn(&rt.handle); }, guard::kEvenIfFaulted);
    }
    rt.handle.revoke_all();  // anything registered after disable, e.g. from tome_mod_unload
    if (rt.handle.faulted) {
        // Whatever the crash left behind may still point into the DLL.
        rt.handle.take_hook_gates();
        TOME_INFO("Unloaded %s (its DLL stays loaded after the crash)", rt.handle.id.c_str());
    } else {
        release_module(rt.module, rt.shadow_dll, rt.handle.take_hook_gates(), rt.handle.id);
        TOME_INFO("Unloaded %s", rt.handle.id.c_str());
    }
    rt.module = nullptr;
    rt.enable_fn = rt.disable_fn = rt.unload_fn = nullptr;
}

void ModManager::enable(Runtime& rt) {
    if (rt.enabled) return;
    // A crash leaves it enabled (with what it registered so far); the end of
    // the refresh disables it.
    if (rt.enable_fn) {
        Starting starting(rt.handle);
        guard::run(&rt.handle, "tome_mod_enable", nullptr, [&] { rt.enable_fn(&rt.handle); });
    }
    rt.enabled = true;
    enable_order_.push_back(rt.handle.id);
    TOME_INFO("Enabled %s", rt.handle.id.c_str());
}

void ModManager::disable(Runtime& rt) {
    if (!rt.enabled) return;
    if (rt.disable_fn) {
        guard::run(&rt.handle, "tome_mod_disable", nullptr, [&] { rt.disable_fn(&rt.handle); },
                   guard::kEvenIfFaulted);
    }
    rt.handle.revoke_all();
    rt.enabled = false;
    enable_order_.erase(std::remove(enable_order_.begin(), enable_order_.end(), rt.handle.id), enable_order_.end());
    TOME_INFO("Disabled %s", rt.handle.id.c_str());
}

void ModManager::mount_startup_assets(const Paths& paths, const GameBuild& build, std::string& report) {
    ExclusiveLock hold(lock_);
    paths_ = paths;
    build_ = build;
    parse_semver(TOME_VERSION, tome_version_);
    parse_game_version(build.file_version, game_version_);

    // The same choice refresh() makes, minus the DLLs: a mod whose DLL then
    // fails to load loses its assets again in the first refresh.
    std::vector<Installed> installed = discover();
    std::map<std::string, bool> state = read_state();
    std::vector<ResolveInput> inputs;
    std::vector<const Installed*> input_mods;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        inputs.push_back({&mod.meta, wanted_by_user(state, mod.meta.id)});
        input_mods.push_back(&mod);
    }
    ResolveResult resolved = resolve(inputs, {tome_version_, game_version_});
    std::vector<const Installed*> running;
    for (size_t i : resolved.load_order) running.push_back(input_mods[i]);
    report = apply_assets(running, assets::Moment::Startup);
    if (report.empty()) report = "no mod assets";
}

void ModManager::on_main_menu() {
    locked([this] {
        bool waiting = std::any_of(pending_assets_.begin(), pending_assets_.end(),
                                   [](const assets::Pending& p) { return p.reload == ReloadMode::Map; });
        if (waiting) refresh_locked(assets::Moment::MainMenu);
    });
}

void ModManager::on_map_load() {
    locked([this] {
        bool waiting = std::any_of(pending_assets_.begin(), pending_assets_.end(),
                                   [](const assets::Pending& p) { return p.reload == ReloadMode::Map; });
        if (waiting) refresh_locked(assets::Moment::MapLoad);
    });
}

std::vector<std::string> ModManager::reload(const std::string& id) {
    std::vector<std::string> reloaded;
    locked([&] { reloaded = reload_locked(id); });
    return reloaded;
}

std::vector<std::string> ModManager::reload_locked(const std::string& id) {
    std::vector<std::string> reloaded;
    for (auto& [rt_id, rt] : runtime_) {
        if (!rt->enabled || !rt->module || (!id.empty() && rt_id != id)) continue;
        rt->stamp = FileStamp{};  // refresh_locked sees a changed DLL
        failures_.erase(rt_id);
        reloaded.push_back(rt_id);
    }
    if (!reloaded.empty()) refresh_locked();
    return reloaded;
}

std::string ModManager::apply_assets(const std::vector<const Installed*>& running, assets::Moment moment) {
    std::vector<assets::ModAssets> wanted;
    std::map<std::string, const Installed*> by_id;
    for (const Installed* mod : running) {
        if (!mod->assets_fingerprint) continue;
        wanted.push_back({mod->meta.id, mod->meta.reload, mod->assets_fingerprint});
        by_id[mod->meta.id] = mod;
    }
    assets::Plan plan = assets::plan(mounted_assets_, wanted, moment);
    for (const assets::Pending& p : plan.pending) {
        bool known = std::any_of(pending_assets_.begin(), pending_assets_.end(), [&p](const assets::Pending& q) {
            return q.id == p.id && q.reload == p.reload;
        });
        if (!known) {
            TOME_INFO("Assets of %s: changes wait for %s", p.id.c_str(),
                      p.reload == ReloadMode::Map ? "the next map load or the main menu (reload: map)"
                                                  : "a restart (reload: restart)");
        }
    }
    pending_assets_ = std::move(plan.pending);
    if (!plan.changed) {
        mounted_assets_ = std::move(plan.mount);  // reload modes may have changed
        return {};
    }

    std::vector<overlay::ModSource> sources;
    for (const assets::ModAssets& m : plan.mount) {
        auto it = by_id.find(m.id);
        if (it != by_id.end() && it->second->assets_fingerprint == m.fingerprint) {
            sources.push_back({m.id, m.fingerprint, false, it->second->assets});
        } else {
            sources.push_back({m.id, m.fingerprint, true, {}});  // its newer files have to wait
        }
    }
    std::string summary;
    if (!overlay::mount_assets(sources, summary)) {
        TOME_ERROR("Assets: %s; the previous assets stay mounted", summary.c_str());
        return {};
    }
    mounted_assets_ = std::move(plan.mount);
    TOME_INFO("Assets: %s", summary.c_str());
    overlay::log_probe();
    return summary;
}

void ModManager::refresh() {
    locked([this] { refresh_locked(); });
}

void ModManager::locked(const std::function<void()>& fn) {
    std::vector<std::pair<std::string, std::vector<std::string>>> changes;
    {
        ExclusiveLock hold(lock_);
        fn();
        changes.swap(settings_changed_);
    }
    notify_settings(std::move(changes));
}

void ModManager::notify_settings(std::vector<std::pair<std::string, std::vector<std::string>>> changes) const {
    if (!settings_sink_) return;
    for (const auto& [id, keys] : changes) settings_sink_(id, keys);
}

json::Value ModManager::setting(const std::string& id, const std::string& key) const {
    SharedLock hold(settings_lock_);
    auto values = settings_values_.find(id);
    if (values == settings_values_.end()) return {};
    const json::Value* value = values->second.find(key);
    return value ? *value : json::Value();
}

bool ModManager::write_settings(const std::string& id) {
    auto file = settings_files_.find(id);
    auto values = settings_values_.find(id);
    if (file == settings_files_.end() || values == settings_values_.end()) return false;
    CreateDirectoryW(file->second.substr(0, file->second.rfind(L'\\')).c_str(), nullptr);
    if (!write_file_atomic(file->second, json::serialize(values->second))) {
        TOME_WARN("%s: couldn't write config\\settings.json", id.c_str());
        return false;
    }
    // Tome's own write: not a change for poll_settings_changed or the next refresh.
    FileStamp stamp;
    if (file_stamp(file->second, stamp)) settings_stamps_[id] = stamp;
    return true;
}

bool ModManager::poll_settings_changed() const {
    SharedLock hold(settings_lock_);
    for (const auto& [id, file] : settings_files_) {
        FileStamp now;
        if (!file_stamp(file, now)) now = {};
        auto known = settings_stamps_.find(id);
        if (known == settings_stamps_.end() || !(known->second == now)) return true;
    }
    return false;
}

void ModManager::reload_settings() {
    std::vector<std::pair<std::string, std::vector<std::string>>> changes;
    {
        ExclusiveLock hold(settings_lock_);
        for (const auto& [id, file] : settings_files_) {
            FileStamp now;
            std::string text;
            if (!file_stamp(file, now) || !read_file(file, text)) {
                now = {};
                text.clear();
            }
            FileStamp& known = settings_stamps_[id];
            if (known == now) continue;
            known = now;
            json::Value stored;
            if (!text.empty()) {
                json::ParseResult parsed = json::parse(text, stored);
                if (!parsed.ok) {
                    TOME_WARN("%s: config\\settings.json: %s; keeping the current settings", id.c_str(),
                              parsed.error.c_str());
                    continue;
                }
            }
            std::vector<std::string> warnings;
            const settings::Schema& schema = settings_schemas_[id];
            json::Value values = settings::load_values(schema, stored, warnings);
            for (const std::string& w : warnings) TOME_WARN("%s: config\\settings.json: %s", id.c_str(), w.c_str());
            json::Value& current = settings_values_[id];
            std::vector<std::string> keys;
            for (const settings::Setting& setting : schema.settings) {
                const json::Value* before = current.find(setting.key);
                if (!before || *before != *values.find(setting.key)) {
                    keys.push_back(setting.key);
                }
            }
            current = std::move(values);
            if (keys.empty()) continue;
            TOME_INFO("%s: config\\settings.json changed on disk (%zu setting%s)", id.c_str(), keys.size(),
                      keys.size() == 1 ? "" : "s");
            changes.emplace_back(id, std::move(keys));
        }
    }
    notify_settings(std::move(changes));
}

tome_result ModManager::set_setting(const std::string& id, const std::string& key, const json::Value& value,
                                    std::string& error) {
    std::vector<std::pair<std::string, std::vector<std::string>>> changes;
    {
        ExclusiveLock hold(settings_lock_);
        auto schema = settings_schemas_.find(id);
        const settings::Setting* setting = schema == settings_schemas_.end() ? nullptr : schema->second.find(key);
        if (!setting) {
            error = "no setting \"" + key + "\" in " + id + "'s settings schema";
            return TOME_ERR_INVALID_ARGUMENT;
        }
        json::Value coerced;
        if (!settings::coerce(*setting, value, coerced, error)) return TOME_ERR_INVALID_ARGUMENT;
        json::Value& values = settings_values_[id];
        const json::Value* before = values.find(key);
        if (before && *before == coerced) return TOME_OK;
        values.set(key, coerced);
        write_settings(id);
        TOME_INFO("%s: setting %s = %s", id.c_str(), key.c_str(), settings::display(*setting, coerced).c_str());
        changes.push_back({id, {key}});
    }
    notify_settings(std::move(changes));
    return TOME_OK;
}

void ModManager::reset_settings(const std::string& id) {
    std::vector<std::pair<std::string, std::vector<std::string>>> changes;
    {
        ExclusiveLock hold(settings_lock_);
        auto schema = settings_schemas_.find(id);
        if (schema == settings_schemas_.end()) return;
        json::Value& values = settings_values_[id];
        std::vector<std::string> keys;
        for (const settings::Setting& setting : schema->second.settings) {
            const json::Value* before = values.find(setting.key);
            if (before && *before == setting.fallback) continue;
            values.set(setting.key, setting.fallback);
            keys.push_back(setting.key);
        }
        if (keys.empty()) return;
        write_settings(id);
        TOME_INFO("%s: settings reset to their defaults", id.c_str());
        changes.push_back({id, std::move(keys)});
    }
    notify_settings(std::move(changes));
}

void ModManager::set_enabled(const std::string& id, bool enabled) {
    locked([&] { set_enabled_locked(id, enabled); });
}

void ModManager::set_enabled_locked(const std::string& id, bool enabled) {
    std::map<std::string, bool> state = read_state();
    state[id] = enabled;
    write_state(state);
    if (!safe_mode_.empty()) {
        if (enabled) {
            safe_mode_enabled_.insert(id);
        } else {
            safe_mode_enabled_.erase(id);
        }
    }
    // Asking for a crashed mod to run again retries it.
    auto failure = failures_.find(id);
    if (enabled && failure != failures_.end() && failure->second.crashed) failures_.erase(failure);
    refresh_locked();
}

void ModManager::set_safe_mode(const std::string& reason) {
    ExclusiveLock hold(lock_);
    safe_mode_ = reason;
}

std::string ModManager::safe_mode() const {
    SharedLock hold(lock_);
    return safe_mode_;
}

void ModManager::set_safe_mode_now(bool on) {
    locked([&] {
        if (on == !safe_mode_.empty()) return;
        safe_mode_ = on ? "turned on in the mod menu" : "";
        safe_mode_enabled_.clear();
        if (on) {
            TOME_WARN("Safe mode turned on in the mod menu: every mod stops for this session");
        } else {
            TOME_INFO("Safe mode turned off in the mod menu");
            std::wstring file = paths_.tome_dir + L"\\safe";
            if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(file.c_str())) {
                TOME_INFO("Removed tome/safe, so the next start is normal too");
            }
        }
        refresh_locked();
    });
}

void ModManager::on_fault(tome_mod* mod) {
    (void)mod;  // refresh_locked finds every crashed mod
    if (t_in_refresh) return;  // this thread's refresh handles it before it returns
    main_thread::post([] {
        mod_manager().refresh();
        mod_menu::on_mods_changed();
    });
}

bool ModManager::poll_changed() {
    uint64_t fingerprint = disk_fingerprint();
    if (fingerprint == last_polled_) return false;
    last_polled_ = fingerprint;
    return true;
}

std::string ModManager::sync_hash(const Installed& mod) {
    const std::wstring meta_path = mod.dir + L"\\tome.mod.json";
    FileStamp meta_stamp;
    file_stamp(meta_path, meta_stamp);
    Fnv1a64 key;
    key.mix(&meta_stamp, sizeof(meta_stamp));
    key.mix(mod.dll.data(), mod.dll.size() * sizeof(wchar_t));
    key.mix(&mod.dll_stamp, sizeof(mod.dll_stamp));
    key.mix(&mod.assets_fingerprint, sizeof(mod.assets_fingerprint));
    auto cached = sync_hashes_.find(mod.folder);
    if (cached != sync_hashes_.end() && cached->second.first == key.value) return cached->second.second;

    // Names are mixed in too, so moving a file changes the hash; a file that
    // can't be read changes it as well (the other players would differ).
    DWORD started = GetTickCount();
    std::vector<char> buffer(1 << 20);  // shared by every file
    Fnv1a64 hash;
    auto mix_name = [&](const std::string& name) { hash.mix(name.c_str(), name.size() + 1); };
    mix_name("tome.mod.json");
    bool readable = hash_file(meta_path, hash, buffer);
    if (!mod.dll.empty()) {
        mix_name("dll");
        readable = hash_file(mod.dll, hash, buffer) && readable;
    }
    std::vector<const overlay::AssetFile*> files;
    for (const overlay::AssetFile& f : mod.assets) files.push_back(&f);
    std::sort(files.begin(), files.end(), [](auto* a, auto* b) { return a->name < b->name; });
    for (const overlay::AssetFile* f : files) {
        mix_name(f->name);
        readable = hash_file(f->path, hash, buffer) && readable;
    }
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", static_cast<uint32_t>(hash.value ^ (hash.value >> 32)));
    TOME_INFO("%s: sync hash %s (%zu asset files, %lu ms)%s", mod.meta.id.c_str(), text, files.size(),
              GetTickCount() - started, readable ? "" : "; some files couldn't be read");
    sync_hashes_[mod.folder] = {key.value, text};
    return text;
}

// FNV-1a over the names and timestamps of every file refresh() depends on:
// state.json and every file in the mod folders (DLLs, tome.mod.json, assets),
// except the mods' own config folders (settings.json has poll_settings_changed).
uint64_t ModManager::disk_fingerprint() const {
    Fnv1a64 hash;
    auto mix = [&](const void* data, size_t len) { hash.mix(data, len); };
    auto mix_file = [&](const std::wstring& path) {
        FileStamp stamp;
        bool exists = file_stamp(path, stamp);
        mix(path.data(), path.size() * sizeof(wchar_t));
        mix(&exists, sizeof(exists));
        mix(&stamp, sizeof(stamp));
    };
    // From the directory listing, so no file is opened.
    constexpr int kTopDepth = 32;
    auto mix_files = [&](const std::wstring& dir, int depth, auto& self) -> void {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                bool mod_config = depth == kTopDepth && _wcsicmp(name.c_str(), L"config") == 0;
                if (depth > 0 && !mod_config) self(path, depth - 1, self);
            } else {
                mix(path.data(), path.size() * sizeof(wchar_t));
                mix(&fd.nFileSizeLow, sizeof(fd.nFileSizeLow));
                mix(&fd.nFileSizeHigh, sizeof(fd.nFileSizeHigh));
                mix(&fd.ftLastWriteTime, sizeof(fd.ftLastWriteTime));
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    };

    {
        // state.json as Tome itself wrote it counts as unchanged: the refresh
        // that wrote it already ran, so the watcher needn't run another.
        const std::wstring path = paths_.tome_dir + L"\\state.json";
        FileStamp stamp;
        bool exists = file_stamp(path, stamp);
        bool ours;
        {
            SharedLock hold(state_stamp_lock_);
            ours = exists && stamp == written_state_stamp_;
        }
        if (ours) {
            static const char kWrittenByTome[] = "state.json written by Tome";
            mix(kWrittenByTome, sizeof(kWrittenByTome));
        } else {
            mix_file(path);
        }
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths_.mods_dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L"..") continue;
            std::wstring dir = paths_.mods_dir + L"\\" + name;
            mix_files(dir, kTopDepth, mix_files);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return hash.value;
}

std::vector<ModStatus> ModManager::status() const {
    SharedLock hold(lock_);
    std::vector<ModStatus> status = status_;
    SharedLock hold_settings(settings_lock_);
    for (ModStatus& s : status) {
        auto values = settings_values_.find(s.id);
        if (!s.settings.settings.empty() && values != settings_values_.end()) s.settings_values = values->second;
    }
    return status;
}

void ModManager::refresh_locked(assets::Moment moment_hint) {
    if (shut_down_) return;  // the watcher may still notice changes while the game exits
    TOME_DEBUG("Refreshing mods");
    InRefresh in_refresh;
    std::vector<Installed> installed = discover();

    // Declare natives for every installed mod that targets this game build,
    // enabled or not: a map that declares them must still load (§5.5).
    for (Installed& mod : installed) {
        if (!mod.valid || !mod.meta.game.matches(game_version_)) continue;
        for (const std::string& decl : mod.meta.natives) {
            std::string error;
            if (!natives::declare(mod.meta.id, decl, error)) {
                mod.errors.push_back(error);
                mod.valid = false;
            }
        }
    }

    // New mods start enabled. Entries for mods that aren't installed right now
    // are kept, so removing and re-adding a mod remembers its setting.
    // An unparseable state.json is left alone (the user may be fixing it by
    // hand); rewriting it here would drop every disabled mod for good.
    bool state_invalid = false;
    std::map<std::string, bool> state = read_state(&state_invalid);
    bool state_changed = false;
    for (const Installed& mod : installed) {
        if (mod.valid && !state.count(mod.meta.id)) {
            state[mod.meta.id] = true;
            state_changed = true;
        }
    }
    if (state_changed && !state_invalid) write_state(state);

    std::vector<ResolveInput> inputs;
    std::vector<const Installed*> input_mods;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        inputs.push_back({&mod.meta, wanted_by_user(state, mod.meta.id)});
        input_mods.push_back(&mod);
    }
    ResolveResult resolved = resolve(inputs, {tome_version_, game_version_});

    // A crashed mod stays off until its DLL changes (below) or the user
    // turns it off; enabling it again then retries it.
    std::erase_if(failures_, [&](const auto& entry) {
        return entry.second.crashed && !wanted_by_user(state, entry.first);
    });

    std::map<std::string, const Installed*> wanted;  // id -> mod, for mods that should run
    for (size_t i : resolved.load_order) wanted[input_mods[i]->meta.id] = input_mods[i];
    std::map<std::string, const Installed*> by_id;
    for (const Installed* mod : input_mods) by_id[mod->meta.id] = mod;

    // Which running mods have to stop: no longer wanted, or their DLL changed
    // (hot reload) or crashed, or a mod they depend on is stopping.
    std::set<std::string> stopping, reloading;
    for (auto& [id, rt] : runtime_) {
        auto it = by_id.find(id);
        bool removed = it == by_id.end();
        bool code_changed = !removed && (rt->source_dll != it->second->dll ||
                                         (rt->module && !(rt->stamp == it->second->dll_stamp)));
        bool crashed = rt->module && rt->handle.faulted;
        if (crashed) {
            failures_[id] = {rt->stamp, rt->handle.fault_text(), true, rt->handle.fault_calls_text()};
            TOME_WARN("%s crashed; it stays off until its DLL changes, or until it's disabled and enabled again",
                      id.c_str());
        }
        if (removed || code_changed || crashed) reloading.insert(id);
        if (rt->enabled && (!wanted.count(id) || code_changed || crashed)) stopping.insert(id);
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& [id, rt] : runtime_) {
            if (!rt->enabled || stopping.count(id)) continue;
            auto it = by_id.find(id);
            if (it == by_id.end()) continue;
            for (const Dependency& dep : it->second->meta.depends) {
                bool dep_stopping = stopping.count(dep.id) > 0;
                for (const std::string& s : stopping) {
                    auto other = by_id.find(s);
                    if (other != by_id.end()) {
                        const auto& p = other->second->meta.provides;
                        if (std::find(p.begin(), p.end(), dep.id) != p.end()) dep_stopping = true;
                    }
                }
                if (dep_stopping) {
                    stopping.insert(id);
                    grew = true;
                    break;
                }
            }
        }
    }

    // Disable dependents before their dependencies: reverse enable order.
    std::vector<std::string> order = enable_order_;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (stopping.count(*it)) disable(*runtime_[*it]);
    }
    for (const std::string& id : reloading) {
        Runtime& rt = *runtime_[id];
        unload(rt);
        if (!by_id.count(id)) runtime_.erase(id);
    }

    {
        // Settings: the values from settings.json (or the defaults), and which
        // ones changed since the last refresh, to tell the mods.
        // Before any mod starts, so tome_mod_enable reads the current values.
        // Under settings_lock_ only: mods read them from their callbacks while
        // this thread holds lock_.
        ExclusiveLock hold_settings(settings_lock_);
        // Forget mods that are gone or no longer have a usable schema, so
        // the API and set_setting don't keep using an old one.
        std::set<std::string> with_settings;
        for (const Installed& mod : installed) {
            if (mod.valid && !mod.settings.settings.empty()) with_settings.insert(mod.meta.id);
        }
        auto forget = [&](auto& by_id) {
            std::erase_if(by_id, [&](const auto& entry) { return !with_settings.count(entry.first); });
        };
        forget(settings_values_);
        forget(settings_schemas_);
        forget(settings_files_);
        forget(settings_stamps_);
        for (Installed& mod : installed) {
            if (!mod.valid || mod.settings.settings.empty()) continue;
            std::vector<std::string> warnings;
            // The file as Tome last read or wrote it: keep the values in
            // memory, which may be newer than what discover() read (a mod's
            // set_setting from another thread), checked against the schema
            // again in case that changed. Otherwise it was edited: read it.
            auto known = settings_stamps_.find(mod.meta.id);
            auto held = settings_values_.find(mod.meta.id);
            bool unchanged = known != settings_stamps_.end() && known->second == mod.settings_stamp &&
                             held != settings_values_.end() && held->second.is_object();
            json::Value values = settings::load_values(mod.settings, unchanged ? held->second : mod.stored_settings,
                                                       warnings);
            if (!unchanged) settings_stamps_[mod.meta.id] = mod.settings_stamp;
            for (const std::string& w : warnings) mod.warnings.push_back("config\\settings.json: " + w);
            json::Value& current = settings_values_[mod.meta.id];
            if (current.is_object()) {
                std::vector<std::string> changed;
                for (const settings::Setting& setting : mod.settings.settings) {
                    const json::Value* before = current.find(setting.key);
                    if (!before || *before != *values.find(setting.key)) {
                        changed.push_back(setting.key);
                    }
                }
                if (!changed.empty()) settings_changed_.emplace_back(mod.meta.id, std::move(changed));
            }
            current = std::move(values);
            settings_files_[mod.meta.id] = mod.dir + L"\\config\\settings.json";
            settings_schemas_[mod.meta.id] = mod.settings;
        }
    }

    // Keybinds too, so tome_mod_enable can register its callbacks.
    std::vector<keys::Binding> declared_keys;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        for (const keys::Declared& d : mod.meta.keybinds) {
            keys::Binding b;
            b.mod = mod.meta.id;
            b.mod_name = mod.meta.name.empty() ? mod.meta.id : mod.meta.name;
            b.id = d.id;
            b.label = d.label;
            b.fallback = b.key = d.key;
            declared_keys.push_back(std::move(b));
        }
    }
    keybinds::set_mods(std::move(declared_keys));

    // Assets go in before the mods are enabled, so a mod can load its own
    // TOC files from tome_mod_enable.
    assets::Moment moment = !assets_started_                    ? assets::Moment::Startup
                            : moment_hint != assets::Moment::Refresh ? moment_hint
                            : frames::on_main_thread() && frames::main_menu_showing() ? assets::Moment::MainMenu
                                                                                       : assets::Moment::Refresh;
    // A DLL that failed before and hasn't changed won't load below either,
    // and neither will its dependents: leave their assets out from the start
    // rather than mounting them and rebuilding the archive again without them.
    std::set<std::string> known_failed;
    std::vector<const Installed*> running;
    for (size_t i : resolved.load_order) {
        const Installed& mod = *input_mods[i];
        bool fails = std::any_of(mod.meta.depends.begin(), mod.meta.depends.end(),
                                 [&](const Dependency& dep) { return known_failed.count(dep.id) > 0; });
        if (!fails && !mod.dll.empty()) {
            auto failure = failures_.find(mod.meta.id);
            auto rt = runtime_.find(mod.meta.id);
            bool loaded = rt != runtime_.end() && rt->second->module;
            fails = !loaded && failure != failures_.end() && failure->second.stamp == mod.dll_stamp;
        }
        if (fails) {
            known_failed.insert(mod.meta.id);
            for (const std::string& p : mod.meta.provides) known_failed.insert(p);
            continue;
        }
        running.push_back(&mod);
    }
    apply_assets(running, moment);

    // Start everything wanted, dependencies first. A mod whose DLL fails to
    // load takes its dependents down with it.
    std::set<std::string> failed_now;
    for (size_t i : resolved.load_order) {
        const Installed& mod = *input_mods[i];
        const std::string& id = mod.meta.id;

        bool dep_failed = false;
        for (const Dependency& dep : mod.meta.depends) {
            if (failed_now.count(dep.id)) dep_failed = true;
        }
        if (dep_failed) {
            resolved.mods[i].state = ModState::DependencyInactive;
            resolved.mods[i].reason = "a dependency failed to load";
            failed_now.insert(id);
            for (const std::string& p : mod.meta.provides) failed_now.insert(p);
            continue;
        }

        auto& slot = runtime_[id];
        if (!slot) {
            slot = std::make_unique<Runtime>();
            slot->handle.id = id;
        }
        Runtime& rt = *slot;
        // Only when they change: mods may keep the pointers mod_version(),
        // mod_dir() and config_dir() return (valid until tome_mod_unload),
        // and assigning a new string frees the old buffer.
        std::string version = mod.meta.version.to_string();
        if (rt.handle.version != version) rt.handle.version = std::move(version);
        if (rt.handle.dir_w != mod.dir) {
            rt.handle.dir_w = mod.dir;
            rt.handle.dir = narrow(mod.dir);
            rt.handle.config_dir = narrow(mod.dir + L"\\config");
        }

        if (!mod.dll.empty() && !rt.module) {
            auto failure = failures_.find(id);
            std::string error;
            bool crashed = false;
            if (failure != failures_.end() && failure->second.stamp == mod.dll_stamp) {
                error = failure->second.reason;  // same DLL as last time; don't retry
                crashed = failure->second.crashed;
            } else if (!load(rt, mod, error)) {
                crashed = rt.handle.faulted;
                TOME_ERROR("Failed to load %s: %s", id.c_str(), error.c_str());
                failures_[id] = {mod.dll_stamp, error, crashed, crashed ? rt.handle.fault_calls_text() : ""};
            } else {
                failures_.erase(id);
            }
            if (!rt.module) {
                resolved.mods[i].state = ModState::DependencyInactive;  // reported as "error" below
                resolved.mods[i].reason = crashed ? error : "failed to load: " + error;
                failed_now.insert(id);
                for (const std::string& p : mod.meta.provides) failed_now.insert(p);
                continue;
            }
        }
        enable(rt);
    }
    if (!failed_now.empty()) {
        running.erase(std::remove_if(running.begin(), running.end(),
                                     [&](const Installed* mod) { return failed_now.count(mod->meta.id) > 0; }),
                      running.end());
        apply_assets(running, moment);
    }

    // A mod that crashed during this refresh (in tome_mod_enable, or in a
    // callback run meanwhile): another pass stops it and its dependents.
    for (auto& [id, rt] : runtime_) {
        if (rt->module && rt->handle.faulted) {
            refresh_locked(moment_hint);
            return;
        }
    }

    // Build the status list.
    std::vector<ModStatus> status;
    for (const Installed& mod : installed) {
        ModStatus s;
        s.folder = mod.folder;
        s.description = mod.meta.description;
        s.icon = mod.meta.icon;
        s.authors = mod.meta.authors;
        if (!mod.valid) {
            s.id = mod.meta.id;
            s.name = mod.folder;
            s.state = "error";
            s.errors = mod.errors;
            s.warnings = mod.warnings;
            for (const std::string& e : mod.errors) s.reason += (s.reason.empty() ? "" : "; ") + e;
            status.push_back(std::move(s));
            continue;
        }
        s.sync = to_string(mod.meta.sync);
        s.reload = to_string(mod.meta.reload);
        s.settings = mod.settings;
        s.settings_error = mod.settings_error;
        s.keybinds = mod.meta.keybinds;
        auto failure = failures_.find(mod.meta.id);
        if (failure != failures_.end() && failure->second.crashed) s.crash_calls = failure->second.calls;
        size_t idx = static_cast<size_t>(std::find(input_mods.begin(), input_mods.end(), &mod) - input_mods.begin());
        const ResolvedMod& r = resolved.mods[idx];
        s.id = mod.meta.id;
        s.name = mod.meta.name;
        s.version = mod.meta.version.to_string();
        s.user_enabled = wanted_by_user(state, mod.meta.id);
        auto rt = runtime_.find(mod.meta.id);
        s.running = rt != runtime_.end() && rt->second->enabled;
        // Only running mods take part in the handshake; hashing reads every file.
        if (s.running && mod.meta.sync == SyncMode::Required) s.sync_hash = sync_hash(mod);
        s.state = s.running ? "active" : (r.state == ModState::DisabledByUser ? "disabled" : "error");
        s.reason = s.running || r.state == ModState::DisabledByUser ? "" : r.reason;
        if (r.state == ModState::DisabledByUser && !safe_mode_.empty() && state[mod.meta.id]) s.reason = "safe mode";
        s.warnings = mod.warnings;
        s.warnings.insert(s.warnings.end(), r.warnings.begin(), r.warnings.end());
        for (const assets::Pending& p : pending_assets_) {
            if (p.id == s.id) {
                s.assets_pending = p.reload == ReloadMode::Map ? "asset changes apply at the next map or menu"
                                                               : "restart to apply asset changes";
                s.restart_pending = p.reload == ReloadMode::Restart;
            }
        }
        status.push_back(std::move(s));
    }

    // Log only when something changed, so idle refreshes stay quiet.
    auto same = [](const std::vector<ModStatus>& a, const std::vector<ModStatus>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].folder != b[i].folder || a[i].state != b[i].state || a[i].reason != b[i].reason ||
                a[i].version != b[i].version || a[i].warnings != b[i].warnings ||
                a[i].assets_pending != b[i].assets_pending) {
                return false;
            }
        }
        return true;
    };
    if (!same(status, status_) || status_.empty()) {
        size_t active = 0;
        for (const ModStatus& s : status) active += s.running;
        TOME_INFO("Mods: %zu installed, %zu active", status.size(), active);
        for (const ModStatus& s : status) {
            if (s.reason.empty()) {
                TOME_INFO("  %-24s %-10s %s", s.id.empty() ? s.folder.c_str() : s.id.c_str(),
                          s.version.c_str(), s.state.c_str());
            } else {
                TOME_INFO("  %-24s %-10s %s: %s", s.id.empty() ? s.folder.c_str() : s.id.c_str(),
                          s.version.c_str(), s.state.c_str(), s.reason.c_str());
            }
            if (!s.assets_pending.empty()) TOME_INFO("    %s: %s", s.folder.c_str(), s.assets_pending.c_str());
            for (const std::string& w : s.warnings) TOME_WARN("    %s: %s", s.folder.c_str(), w.c_str());
        }
    }
    status_ = std::move(status);
    // From now on asset changes follow the mods' reload modes. Until here
    // (DllMain and the first refresh, both part of startup) a mod whose DLL
    // fails to load loses its assets at once, whatever its reload mode.
    assets_started_ = true;
}

} // namespace tome
