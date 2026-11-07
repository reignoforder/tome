// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_manager.h"

#include "api.h"
#include "core/game_lock.h"
#include "core/hash.h"
#include "core/hash_cache.h"
#include "core/json.h"
#include "crash_handler.h"
#include "frames.h"
#include "guard.h"
#include "hooks.h"
#include "keybinds.h"
#include "log.h"
#include "main_thread.h"
#include "mod_mappings.h"
#include "mod_menu.h"
#include "natives.h"
#include "packages.h"
#include "core/objects.h"
#include "core/package.h"
#include "core/signature.h"
#include "sha256.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>

namespace tome {
namespace {

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

// A file's stamp for the hash cache (core/hash_cache.h); false if it can't
// be opened.
bool hash_stamp(const std::wstring& path, hash_cache::Stamp& out) {
    HANDLE in = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    const bool ok = GetFileInformationByHandle(in, &info);
    CloseHandle(in);
    if (!ok) return false;
    auto wide = [](DWORD high, DWORD low) { return (uint64_t(high) << 32) | low; };
    out.size = wide(info.nFileSizeHigh, info.nFileSizeLow);
    out.written = wide(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
    out.created = wide(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime);
    out.file_id = wide(info.nFileIndexHigh, info.nFileIndexLow);
    out.volume = info.dwVolumeSerialNumber;
    return true;
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
    CreateDirectoryW(paths_.mod_config_dir.c_str(), nullptr);
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
    const std::wstring values_file = settings_file(mod.meta.id);
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
            // Read it again next time: a file that exists but couldn't be read
            // (held open by another program) may be readable then without a
            // new stamp. A missing one keeps the zero stamp, so it's read once it appears.
            cached.schema_stamp = {};
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

    if (!cached.values_read || cached.values_file != values_file || !(cached.values_stamp == values_stamp)) {
        cached.values_read = true;
        cached.values_file = values_file;
        cached.values_stamp = values_stamp;
        cached.values = json::Value();
        cached.values_warning.clear();
        std::string text;
        if (!values_exist || !read_file(values_file, text)) {
            cached.values_stamp = {};
        } else if (json::ParseResult parsed = json::parse(text, cached.values); !parsed.ok) {
            cached.values_warning = settings_name(mod.meta.id) + ": " + parsed.error + "; using the defaults";
            cached.values = json::Value();
        }
    }
    mod.settings_stamp = cached.values_stamp;
    mod.stored_settings = cached.values;
    if (!cached.values_warning.empty()) mod.warnings.push_back(cached.values_warning);
}

void ModManager::declare_natives(std::vector<Installed>& installed) const {
    for (Installed& mod : installed) {
        if (!mod.valid || !mod.meta.game.matches(game_version_)) continue;
        if (!edition_matches(mod.meta.edition, build_.edition)) continue;
        for (const std::string& decl : mod.meta.natives) {
            std::string error;
            if (!natives::declare(mod.meta.id, decl, error)) {
                mod.errors.push_back(error);
                mod.valid = false;
            }
        }
    }
    // A mod that failed here takes its mappings along: what requires them fails too.
    check_mod_requires(installed);
}

void ModManager::read_mappings(Installed& mod) const {
    if (!build_.known) return;
    const mappings::Build& running = *build_.known->data;
    const std::string folder = mod.meta.mappings.empty() ? "mappings" : mod.meta.mappings;
    const std::wstring folder_path = mod.dir + L"\\" + to_windows_path(folder);
    const std::string shown = folder + "/" + running.name + ".json";
    const std::wstring file = folder_path + L"\\" + widen(running.name) + L".json";
    FileStamp stamp;
    if (!file_stamp(file, stamp)) {
        // No file for this build: the mod maps nothing here, and a "requires"
        // naming its symbols says so. A folder that isn't there at all is
        // worth a warning only if tome.mod.json named it.
        if (!mod.meta.mappings.empty() && GetFileAttributesW(folder_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            mod.warnings.push_back("\"mappings\" folder \"" + mod.meta.mappings + "\" not found");
        }
        mappings_cache_.erase(mod.folder);
        return;
    }
    ParsedMappings& cached = mappings_cache_[mod.folder];
    if (cached.file != file || !(cached.stamp == stamp)) {
        cached = {};
        cached.file = file;
        cached.stamp = stamp;
        std::string text;
        json::Value root;
        json::ParseResult parsed;
        if (!read_file(file, text)) {
            cached.stamp = {};  // read again next time
            cached.errors.push_back(shown + ": can't be read");
        } else if (parsed = json::parse(text, root); !parsed.ok) {
            cached.errors.push_back(shown + ": " + parsed.error);
        } else {
            auto build = std::make_shared<mappings::Build>();
            mappings::ParseResult result = mappings::parse_mod_build(root, running, *build);
            for (const std::string& e : result.errors) cached.errors.push_back(shown + ": " + e);
            if (result.ok) cached.build = std::move(build);
        }
    }
    // Against the game's code, as Tome's own file at startup: what resolve
    // hands out and what's hooked is what the file says. Once it can be
    // (not in DllMain's scan for the startup assets, before init).
    if (cached.build && !cached.checked && mod_mappings::ready()) {
        for (const std::string& e : mod_mappings::verify(*cached.build)) {
            cached.errors.push_back(shown + ": " + e + " (a file for another build?)");
        }
        cached.checked = true;
    }
    if (!cached.errors.empty()) {
        mod.errors.insert(mod.errors.end(), cached.errors.begin(), cached.errors.end());
        mod.valid = false;
        return;
    }
    mod.own_mappings = cached.build;
    mod.mappings_checked = cached.checked;
    mod.mappings_file = file;
    mod.mappings_stamp = stamp;
}

void ModManager::check_mod_requires(std::vector<Installed>& mods) const {
    if (!build_.known) return;
    std::set<std::string> installed;
    for (const Installed& mod : mods) installed.insert(mod.meta.id);
    const std::string& build_name = build_.known->data->name;
    // Against the valid mods at the start of each pass, so the order doesn't
    // matter; again until none fails, since a mod that fails here takes its
    // mappings along (only valid mods' are published).
    for (bool failed = true; failed;) {
        failed = false;
        std::map<std::string, const mappings::Build*> mapped;  // valid mods by id; null: no file
        for (const Installed& mod : mods) {
            if (mod.valid) mapped[mod.meta.id] = mod.own_mappings.get();
        }
        for (Installed& mod : mods) {
            if (!mod.valid) continue;
            for (const std::string& name : mod.meta.requires_) {
                if (name.find('/') == std::string::npos) continue;
                std::string_view id, local;
                std::string error;
                if (!mappings::split_mod_symbol(name, id, local)) {
                    error = "\"requires\" entry \"" + name + "\": a mod's mapping is \"<mod id>/<name>\"";
                } else if (auto it = mapped.find(std::string(id)); it == mapped.end()) {
                    error = "needs \"" + name + "\" from the mod " + std::string(id) +
                            (installed.count(std::string(id)) ? ", which has errors" : ", which isn't installed");
                } else if (!it->second || !it->second->find(local)) {
                    error = "needs \"" + name + "\", which " + std::string(id) + "'s mappings don't have for " +
                            build_name;
                }
                if (!error.empty()) {
                    mod.errors.push_back(error);
                    mod.valid = false;
                    failed = true;
                }
            }
        }
    }
}

void ModManager::publish_mappings(const mod_state::State& state, const std::vector<Installed>& installed) const {
    std::set<std::string> kept;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        if (game_lock_ && locked_.count(mod.meta.id)) {  // runs with the table it had
            kept.insert(mod.meta.id);
            continue;
        }
        // A mapping file moves other mods' hooks: it counts as the mod's
        // code (approval), and a mod that's off maps nothing.
        if (!mod.own_mappings) {
            mod_mappings::publish(mod.meta.id, nullptr);
        } else if (!mod.mappings_checked) {
            mod_mappings::publish(mod.meta.id, nullptr, "its mapping file wasn't checked yet");
        } else if (approval_need(state, mod) != mod_state::Need::None) {
            mod_mappings::publish(mod.meta.id, nullptr, "the mod " + mod.meta.id + " isn't allowed yet");
        } else if (!wanted_by_user(state, mod.meta.id)) {
            mod_mappings::publish(mod.meta.id, nullptr, "the mod " + mod.meta.id + " is off");
        } else {
            kept.insert(mod.meta.id);
            mod_mappings::publish(mod.meta.id, mod.own_mappings);
        }
    }
    for (const std::string& id : mod_mappings::ids()) {
        if (!kept.count(id) && !(game_lock_ && locked_.count(id))) mod_mappings::publish(id, nullptr);
    }
}

std::vector<ModManager::Installed> ModManager::discover() const {
    std::vector<Installed> mods;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths_.mods_dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return mods;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || name[0] == L'.') continue;
        const bool is_package = !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (is_package && package::stem(narrow(name)).empty()) continue;
        if (_wcsicmp(name.c_str(), L"config") == 0) continue;  // the mods' settings, not a mod

        Installed mod;
        mod.folder = narrow(name);
        mod.dir = paths_.mods_dir + L"\\" + name;
        mod.package = is_package;
        if (is_package) {
            // Unpacked again only when the file's stamp changes.
            FileStamp stamp;
            file_stamp(mod.dir, stamp);
            UnpackedPackage& unpacked = package_cache_[mod.folder];
            if ((unpacked.dir.empty() && unpacked.error.empty()) || !(unpacked.stamp == stamp)) {
                packages::Unpacked result = packages::unpack(paths_, mod.dir);
                // The metadata cache compares stamps only, and a new copy has new files.
                if (result.dir != unpacked.dir) meta_cache_.erase(mod.folder);
                unpacked = {stamp, result.dir, result.error};
                if (!result.error.empty()) TOME_WARN("%s: %s", mod.folder.c_str(), result.error.c_str());
            }
            if (!unpacked.error.empty()) {
                mod.errors.push_back(unpacked.error);
                mods.push_back(std::move(mod));
                continue;
            }
            mod.dir = unpacked.dir;
        }

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
            } else if (cached.sha256 = sha256_bytes(text), parsed = json::parse(text, root); !parsed.ok) {
                cached.errors.push_back("tome.mod.json: " + parsed.error);
            } else {
                MetadataResult meta = parse_metadata(root, cached.meta);
                cached.errors = std::move(meta.errors);
                cached.warnings = std::move(meta.warnings);
                cached.valid = meta.ok;
                if (cached.valid && build_.known) {
                    // Names with a single ':' are capabilities ("property:mana", core/objects.h);
                    // only "property:" ones are checked so far. "Class::Method" is a mapping.
                    // "<id>/<name>" is a mod's own mapping (check_mod_requires).
                    const mappings::Build* data = build_.known->data;
                    for (const std::string& name : cached.meta.requires_) {
                        if (name.find('/') != std::string::npos) continue;
                        const bool capability =
                            name.find(':') != std::string::npos && name.find("::") == std::string::npos;
                        std::string error;
                        if (capability && !objects::capability_met(
                                              name, [&](const char* s) { return data->find(s) != nullptr; }, error)) {
                            cached.errors.push_back(error);
                            cached.valid = false;
                        } else if (!capability && !data->find(name)) {
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
            if (!mod.meta.wasm.empty()) {
                mod.dll = mod.dir + L"\\" + to_windows_path(mod.meta.wasm);
                mod.wasm = true;
                if (!file_stamp(mod.dll, mod.dll_stamp)) {
                    mod.errors.push_back("\"wasm\" module \"" + mod.meta.wasm + "\" not found");
                    mod.valid = false;
                }
            }
            if (!mod.meta.config.empty()) read_settings_schema(mod);
            read_mappings(mod);
            std::string assets = mod.meta.assets.empty() ? "assets" : mod.meta.assets;
            std::wstring assets_dir = mod.dir + L"\\" + to_windows_path(assets);
            DWORD attributes = GetFileAttributesW(assets_dir.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                overlay::scan_assets(assets_dir, mod.assets, mod.assets_fingerprint, mod.warnings);
            } else if (!mod.meta.assets.empty()) {
                mod.warnings.push_back("\"assets\" folder \"" + mod.meta.assets + "\" not found");
            }
            using mod_state::CodeKind;
            if (!mod.dll.empty() && !mod.wasm) mod.code_kind = CodeKind::Native;
            else if (std::any_of(mod.assets.begin(), mod.assets.end(),
                                 [](const auto& f) { return mod_state::is_script(f.name); }))
                mod.code_kind = CodeKind::Scripts;
            else if (mod.wasm) mod.code_kind = CodeKind::Wasm;
            // A mapping file only: other mods' hooks go where it says.
            else if (!mod.assets.empty() || !mod.meta.icon.empty() || mod.own_mappings) mod.code_kind = CodeKind::Assets;
            mod.has_code = mod.valid && mod.code_kind != CodeKind::None;
            if (mod.has_code) mod.code_hash = code_hash(mod);
            if (mod.has_code && !mod.code_hash.empty()) check_signature(mod, cached.sha256);
        }
        mods.push_back(std::move(mod));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(mods.begin(), mods.end(), [](const Installed& a, const Installed& b) { return a.folder < b.folder; });
    // A folder and a package with the same id: the folder is used, so a mod
    // can be worked on while it's also installed as a package.
    for (Installed& pkg : mods) {
        if (!pkg.package || !pkg.valid) continue;
        for (const Installed& folder : mods) {
            if (!folder.package && folder.valid && folder.meta.id == pkg.meta.id) {
                pkg.valid = false;
                pkg.errors.push_back("the folder " + folder.folder + " has the same id and is used instead");
            }
        }
    }
    check_mod_requires(mods);
    // Forget folders that are gone.
    std::set<std::string> folders;
    for (const Installed& mod : mods) folders.insert(mod.folder);
    auto gone = [&](const auto& entry) { return !folders.count(entry.first); };
    std::erase_if(meta_cache_, gone);
    std::erase_if(settings_cache_, gone);
    std::erase_if(mappings_cache_, gone);
    std::erase_if(package_cache_, gone);
    std::erase_if(signature_cache_, gone);
    return mods;
}

mod_state::State ModManager::read_state(bool* invalid, int* schema) const {
    if (invalid) *invalid = false;
    if (schema) *schema = mod_state::kSchema;
    std::string text;
    if (!read_file(paths_.tome_dir + L"\\state.json", text)) return {};
    json::Value root;
    json::ParseResult parsed = json::parse(text, root);
    if (!parsed.ok) {
        // Mods with code still need their approval, which can't be read now.
        TOME_WARN("tome/state.json is invalid (%s); treating every mod as enabled, but none as allowed",
                  parsed.error.c_str());
        if (invalid) *invalid = true;
        return {};
    }
    int file_schema = 0;
    mod_state::State state = mod_state::parse(root, file_schema);
    if (schema) *schema = file_schema;
    return state;
}

bool ModManager::wanted_by_user(const mod_state::State& state, const std::string& id) const {
    if (!safe_mode_.empty()) return safe_mode_enabled_.count(id) > 0;
    auto saved = state.find(id);
    return saved == state.end() || saved->second.enabled;
}

mod_state::Need ModManager::approval_need(const mod_state::State& state, const Installed& mod) {
    auto saved = state.find(mod.meta.id);
    return mod_state::need(saved == state.end() ? nullptr : &saved->second, mod.has_code, mod.package, mod.folder,
                           mod.code_hash, mod.signer);
}

void ModManager::implicit_approvals(mod_state::State& state, const std::vector<Installed>& installed,
                                    bool log) const {
    // Release builds ignore TOME_DEBUG_APPROVE: anything that sets the game's
    // environment could otherwise allow a mod's code.
    char env[1024] = {};
    if (TOME_DEV) GetEnvironmentVariableA("TOME_DEBUG_APPROVE", env, sizeof(env));
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        auto saved = state.find(mod.meta.id);
        // An update signed by the key the user allowed (mod_state::need lets
        // it run): recorded as what's allowed now.
        if (saved != state.end() && !mod.signer.empty() && saved->second.approved.key == mod.signer &&
            !mod.code_hash.empty() && saved->second.approved.hash != mod.code_hash &&
            approval_need(state, mod) == mod_state::Need::None) {
            approve(state, mod, log ? "signed by the same key as before" : nullptr);
        }
        if (TOME_DEV && approval_need(state, mod) != mod_state::Need::None &&
            mod_state::debug_approves(env, mod.meta.id)) {
            approve(state, mod, log ? "TOME_DEBUG_APPROVE" : nullptr);
        }
    }
}

void ModManager::approve(mod_state::State& state, const Installed& mod, const char* how) {
    if (!mod.has_code) return;
    if (mod.code_hash.empty()) {
        if (how) TOME_WARN("%s: some of its files can't be read, so its code can't be allowed", mod.meta.id.c_str());
        return;
    }
    mod_state::Approval& approved = state[mod.meta.id].approved;
    if (approved.hash == mod.code_hash && approved.folder == mod.folder && approved.key == mod.signer) return;
    approved = {mod.code_hash, mod.folder, mod.signer};
    if (how) {
        TOME_INFO("%s: code allowed (%s), sha256 %s%s%s", mod.meta.id.c_str(), how, mod.code_hash.c_str(),
                  mod.signer.empty() ? "" : ", signed by ", mod.signer.c_str());
    }
}

void ModManager::write_state(const mod_state::State& state) {
    json::Value root = mod_state::serialize(state);
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

bool ModManager::load_wasm(Runtime& rt, const Installed& mod, std::string& error) {
    std::unique_ptr<wasm_mods::Instance> instance = wasm_mods::Instance::load(&rt.handle, mod.dll, error);
    if (!instance) return false;
    rt.wasm = std::move(instance);
    rt.source_dll = mod.dll;
    rt.stamp = mod.dll_stamp;
    rt.handle.clear_fault();

    int32_t rc = 0;
    Starting starting(rt.handle);
    wasm_mods::Instance* wasm = rt.wasm.get();
    const bool returned = guard::run(&rt.handle, "tome_mod_load", nullptr, [&] { rc = wasm->call_load(); });
    if (!returned || rc != 0) {
        error = !returned ? rt.handle.fault_text() : "tome_mod_load returned " + std::to_string(rc);
        // Nothing of a module outlives it (no threads, no patches): free it.
        rt.handle.revoke_all();
        rt.wasm.reset();
        return false;
    }
    TOME_INFO("Loaded %s %s (WebAssembly)", rt.handle.id.c_str(), rt.handle.version.c_str());
    return true;
}

bool ModManager::load(Runtime& rt, const Installed& mod, std::string& error) {
    if (mod.wasm) return load_wasm(rt, mod, error);
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
    crash_handler::add_module(reinterpret_cast<uintptr_t>(module), mod.meta.id);
    rt.source_dll = mod.dll;
    rt.stamp = mod.dll_stamp;
    rt.enable_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_enable");
    rt.disable_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_disable");
    rt.unload_fn = lookup<tome_mod_lifecycle_fn>(module, "tome_mod_unload");

    rt.handle.clear_fault();  // a new DLL starts with a clean record
    int rc = 0;
    // Until it's set up or undone (below): a load that returns an error
    // revokes what it registered, and nothing on another thread may run it
    // in between (a crash marks the mod faulted, which skips it too).
    Starting starting(rt.handle);
    const bool returned =
        guard::run(&rt.handle, "tome_mod_load", nullptr, [&] { rc = load_fn(api_table(), &rt.handle); });
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
            if (!rt->loaded()) continue;
            // Not after a trap, as in unload(): the module's state is
            // whatever the trap left, and it has nothing to undo.
            if (rt->wasm && !rt->handle.faulted && rt->wasm->has("tome_mod_unload")) {
                wasm_mods::Instance* wasm = rt->wasm.get();
                guard::run(&rt->handle, "tome_mod_unload", nullptr, [&] { wasm->call("tome_mod_unload"); });
            }
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
    if (rt.wasm) {
        // A module's crash leaves nothing behind (no threads or patches of
        // its own), so it's always freed.
        wasm_mods::Instance* wasm = rt.wasm.get();
        if (!rt.handle.faulted && wasm->has("tome_mod_unload")) {
            guard::run(&rt.handle, "tome_mod_unload", nullptr, [&] { wasm->call("tome_mod_unload"); });
        }
        rt.handle.revoke_all();
        rt.wasm.reset();
        rt.reload_requested = false;
        TOME_INFO("Unloaded %s", rt.handle.id.c_str());
        return;
    }
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
    rt.reload_requested = false;
}

void ModManager::enable(Runtime& rt) {
    if (rt.enabled) return;
    // A crash leaves it enabled (with what it registered so far); the end of
    // the refresh disables it.
    if (rt.enable_fn) {
        Starting starting(rt.handle);
        guard::run(&rt.handle, "tome_mod_enable", nullptr, [&] { rt.enable_fn(&rt.handle); });
    } else if (rt.wasm) {
        Starting starting(rt.handle);
        wasm_mods::Instance* wasm = rt.wasm.get();
        guard::run(&rt.handle, "tome_mod_enable", nullptr, [&] { wasm->enable(); });
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
    } else if (rt.wasm && !rt.handle.faulted && rt.wasm->has("tome_mod_disable")) {
        // Not after a trap: the module's state is whatever the trap left.
        wasm_mods::Instance* wasm = rt.wasm.get();
        guard::run(&rt.handle, "tome_mod_disable", nullptr, [&] { wasm->call("tome_mod_disable"); });
    }
    rt.handle.revoke_all();
    if (rt.wasm) rt.wasm->revoked();
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
    mod_state::State state = read_state();
    // Both recorded by the first refresh: nothing is written in DllMain.
    implicit_approvals(state, installed, false);
    std::vector<ResolveInput> inputs;
    std::vector<const Installed*> input_mods;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        inputs.push_back({&mod.meta, wanted_and_allowed(state, mod)});
        input_mods.push_back(&mod);
    }
    ResolveResult resolved = resolve(inputs, {tome_version_, game_version_, build_.edition});
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
        if (!rt->enabled || !rt->loaded() || (!id.empty() && rt_id != id)) continue;
        // refresh_locked sees a changed DLL. Not by zeroing rt->stamp: a locked
        // mod's reload waits for the game's end, and a crash meanwhile must
        // still be recorded against the DLL that crashed.
        rt->reload_requested = true;
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
    assets::Plan plan = assets::plan(mounted_assets_, wanted, moment, locked_);
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
        TOME_WARN("%s: couldn't write %s", id.c_str(), settings_name(id).c_str());
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
                    TOME_WARN("%s: %s: %s; keeping the current settings", id.c_str(), settings_name(id).c_str(),
                              parsed.error.c_str());
                    continue;
                }
            }
            std::vector<std::string> warnings;
            const settings::Schema& schema = settings_schemas_[id];
            json::Value values = settings::load_values(schema, stored, warnings);
            for (const std::string& w : warnings) {
                TOME_WARN("%s: %s: %s", id.c_str(), settings_name(id).c_str(), w.c_str());
            }
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
            TOME_INFO("%s: %s changed on disk (%zu setting%s)", id.c_str(), settings_name(id).c_str(), keys.size(),
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

std::vector<std::string> ModManager::set_enabled(const std::string& id, bool enabled) {
    std::vector<std::string> dependents;
    locked([&] { dependents = set_enabled_locked(id, enabled); });
    return dependents;
}

std::vector<std::string> ModManager::set_enabled_locked(const std::string& id, bool enabled) {
    mod_state::State state = read_state();
    std::vector<std::string> ids{id};
    // Only a mod that goes off now takes its dependents along; disabling one
    // that's already off leaves the rest as they are.
    std::vector<Installed> installed = discover();
    // As the refresh will: a mod whose natives can't be declared doesn't run,
    // so it neither needs the target nor stands in for it.
    declare_natives(installed);
    if (!enabled && wanted_by_user(state, id)) {
        std::vector<ResolveInput> inputs;
        size_t target = SIZE_MAX;
        for (const Installed& mod : installed) {
            if (!mod.valid) continue;
            if (mod.meta.id == id) target = inputs.size();
            inputs.push_back({&mod.meta, wanted_and_allowed(state, mod)});
        }
        if (target != SIZE_MAX) {
            for (size_t i : dependents_of(inputs, {tome_version_, game_version_, build_.edition}, target)) {
                ids.push_back(inputs[i].meta->id);
                TOME_INFO("%s: disabled, since it depends on %s", ids.back().c_str(), id.c_str());
            }
        }
    }
    // Turning a mod on is how the user allows its code: the code installed
    // now (the valid mod with that id; a package loses to a folder).
    if (enabled) {
        for (const Installed& mod : installed) {
            if (mod.valid && mod.meta.id == id) approve(state, mod, "turned on");
        }
    }
    for (const std::string& each : ids) {
        state[each].enabled = enabled;
        if (!safe_mode_.empty()) {
            if (enabled) {
                safe_mode_enabled_.insert(each);
            } else {
                safe_mode_enabled_.erase(each);
            }
        }
    }
    write_state(state);
    // Asking for a crashed mod to run again retries it.
    auto failure = failures_.find(id);
    if (enabled && failure != failures_.end() && failure->second.crashed) failures_.erase(failure);
    refresh_locked(std::move(installed));
    ids.erase(ids.begin());
    return ids;
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

void ModManager::set_game_lock(bool on) {
    {
        ExclusiveLock hold(lock_);
        if (on == game_lock_) return;
        game_lock_ = on;
        locked_.clear();
        if (on) {
            std::vector<game_lock::RunningMod> running;
            for (const auto& [id, rt] : runtime_) {
                if (rt->enabled) running.push_back({id, rt->required, rt->depends, rt->provides, rt->versioned});
            }
            locked_ = game_lock::locked_mods(running);
            std::string list;
            for (const std::string& id : locked_) list += (list.empty() ? "" : ", ") + id;
            TOME_INFO("Multiplayer game: required mods are locked until it ends (%s)",
                      list.empty() ? "none running" : list.c_str());
        } else {
            TOME_INFO("Multiplayer game ended: required mods are unlocked");
        }
    }
    // Brings the mod list's notes up to date, and on the way out applies
    // what waited for the game; from the message loop, outside the game's
    // own calls (the mods being changed may have handlers on them).
    main_thread::post([] {
        mod_manager().refresh();
        mod_menu::on_mods_changed();
    });
}

bool ModManager::game_locked() const {
    // Not under lock_: SRW locks aren't reentrant, and mods call set_property
    // and jass_call from tome_mod_enable, which runs with lock_ held.
    return game_lock_.load();
}

bool ModManager::poll_changed() {
    DiskPrint now = disk_fingerprint();
    // state.json as Tome itself wrote it is no change, whatever it was
    // before: the refresh that wrote it already ran. (Taking only its stamp
    // as the baseline missed that: the first refresh's write of a state.json
    // someone else had written looked like an edit, and ran another.)
    const bool changed = now.files != last_polled_.files || (!now.state_ours && now.state != last_polled_.state);
    last_polled_ = now;
    return changed;
}

std::string ModManager::sync_hash(const Installed& mod, const std::vector<Installed>& installed) {
    const std::wstring meta_path = mod.dir + L"\\tome.mod.json";
    FileStamp meta_stamp;
    file_stamp(meta_path, meta_stamp);
    // The mapping files of the mods it names ("depends", "<id>/<name>" in
    // "requires"): its hooks on their names go where those files say, even
    // if those mods aren't compared themselves ("sync": "client").
    std::set<std::string> named;
    for (const Dependency& dep : mod.meta.depends) named.insert(dep.id);
    for (const std::string& name : mod.meta.requires_) {
        std::string_view id, local;
        if (mappings::split_mod_symbol(name, id, local)) named.insert(std::string(id));
    }
    named.erase(mod.meta.id);
    std::vector<const Installed*> libraries;  // by id (named is sorted)
    for (const std::string& id : named) {
        for (const Installed& other : installed) {
            if (other.valid && other.meta.id == id && !other.mappings_file.empty()) libraries.push_back(&other);
        }
    }
    Fnv1a64 key;
    key.mix(&meta_stamp, sizeof(meta_stamp));
    key.mix(mod.dll.data(), mod.dll.size() * sizeof(wchar_t));
    key.mix(&mod.dll_stamp, sizeof(mod.dll_stamp));
    key.mix(&mod.assets_fingerprint, sizeof(mod.assets_fingerprint));
    key.mix(mod.mappings_file.data(), mod.mappings_file.size() * sizeof(wchar_t));
    key.mix(&mod.mappings_stamp, sizeof(mod.mappings_stamp));
    for (const Installed* lib : libraries) {
        key.mix(lib->mappings_file.data(), lib->mappings_file.size() * sizeof(wchar_t));
        key.mix(&lib->mappings_stamp, sizeof(lib->mappings_stamp));
    }
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
    // What it patches: the same DLL with another mapping file hooks other code.
    if (!mod.mappings_file.empty()) {
        mix_name("mappings");
        readable = hash_file(mod.mappings_file, hash, buffer) && readable;
    }
    for (const Installed* lib : libraries) {
        mix_name("mappings of " + lib->meta.id);
        readable = hash_file(lib->mappings_file, hash, buffer) && readable;
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

void ModManager::check_signature(Installed& mod, const std::string& metadata_sha256) const {
    const std::wstring path = mod.dir + L"\\" + widen(std::string(signature::kFile));
    FileStamp stamp;
    if (!file_stamp(path, stamp)) {
        signature_cache_.erase(mod.folder);
        return;
    }
    CheckedSignature checked;
    if (stamp.size > signature::kMaxFileSize) {
        checked.warning = "tome.sig is larger than " + std::to_string(signature::kMaxFileSize) +
                          " bytes; treated as not signed";
    } else if (!read_file(path, checked.text)) {
        checked.warning = "tome.sig can't be read; treated as not signed";
    }
    if (!checked.warning.empty()) {
        signature_cache_.erase(mod.folder);
        mod.warnings.push_back(checked.warning);
        return;
    }
    checked.metadata_sha256 = metadata_sha256;
    checked.code_hash = mod.code_hash;
    // Verified again only when tome.sig, tome.mod.json (the bytes that were
    // parsed) or the code changes.
    CheckedSignature& cached = signature_cache_[mod.folder];
    if (!cached.verified || cached.text != checked.text || cached.metadata_sha256 != checked.metadata_sha256 ||
        cached.code_hash != checked.code_hash) {
        signature::Signature sig;
        std::string error;
        if (!signature::parse(checked.text, sig, error)) {
            checked.warning = "tome.sig is invalid (" + error + "); treated as not signed";
        } else if (metadata_sha256.empty() ||
                   !signature::verify(sig, signature::message(mod.meta.id, mod.code_hash, metadata_sha256))) {
            checked.warning = "tome.sig doesn't match the mod's files (changed after signing?); treated as not signed";
        } else {
            checked.signer = signature::encode_key(sig.key);
        }
        checked.verified = true;
        cached = std::move(checked);
    }
    if (!cached.warning.empty()) mod.warnings.push_back(cached.warning);
    mod.signer = cached.signer;
}

std::string ModManager::code_hash(const Installed& mod) const {
    // The files mod_state::is_code_file picks (every file but the top tome.mod.json and
    // tome.sig), by their path from the mod folder; the stamps first, to skip hashing them
    // again.
    std::vector<std::pair<std::string, std::wstring>> files;  // relative path ('/'), full path
    Fnv1a64 stamps;
    constexpr int kMaxDepth = 32;
    bool complete = true;
    auto walk = [&](const std::wstring& dir, const std::string& prefix, int depth, auto& self) -> void {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) {
            complete = false;
            return;
        }
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::string relative = prefix + narrow(name);
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Into the assets folder too, for its scripts.
                if (depth == 0) {
                    complete = false;  // deeper than anything is hashed: never allowed
                    continue;
                }
                self(path, relative + "/", depth - 1, self);
            } else if (mod_state::is_code_file(relative)) {
                stamps.mix(relative.c_str(), relative.size() + 1);
                stamps.mix(&fd.nFileSizeLow, sizeof(fd.nFileSizeLow));
                stamps.mix(&fd.nFileSizeHigh, sizeof(fd.nFileSizeHigh));
                stamps.mix(&fd.ftLastWriteTime, sizeof(fd.ftLastWriteTime));
                stamps.mix(&fd.ftCreationTime, sizeof(fd.ftCreationTime));
                files.emplace_back(std::move(relative), std::move(path));
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    };
    walk(mod.dir, "", kMaxDepth, walk);
    if (!complete) return {};
    load_file_hashes();
    const std::string dir = narrow(mod.dir);
    if (const std::string* cached = file_hashes_.find_folder(dir, stamps.value)) return *cached;

    std::vector<mod_state::CodeFile> hashed;
    for (const auto& [relative, path] : files) {
        std::string sha = file_sha256(path);
        if (sha.empty()) return {};  // not cached: try again next refresh
        hashed.push_back({relative, std::move(sha)});
    }
    std::string hash = sha256_bytes(mod_state::manifest(std::move(hashed)));
    if (!hash.empty()) file_hashes_.put_folder(dir, stamps.value, hash);
    return hash;
}

std::wstring ModManager::file_hashes_path() const {
    return paths_.tome_dir + L"\\cache\\file-hashes.json";
}

void ModManager::load_file_hashes() const {
    if (file_hashes_read_) return;
    file_hashes_read_ = true;
    std::string text;
    if (read_file(file_hashes_path(), text) && !file_hashes_.parse(text)) {
        TOME_WARN("tome/cache/file-hashes.json isn't valid; every mod's files are hashed again");
    }
}

std::string ModManager::file_sha256(const std::wstring& path) const {
    load_file_hashes();
    hash_cache::Stamp before, after;
    if (!hash_stamp(path, before)) return {};
    const std::string key = narrow(path);
    if (const std::string* sha = file_hashes_.find(key, before)) return *sha;
    std::string sha = sha256_file(path);
    // Only if nothing changed the file while it was read.
    if (!sha.empty() && hash_stamp(path, after) && after == before) file_hashes_.put(key, before, sha);
    return sha;
}

void ModManager::save_file_hashes() const {
    if (!file_hashes_read_ || !file_hashes_.dirty()) return;
    CreateDirectoryW((paths_.tome_dir + L"\\cache").c_str(), nullptr);
    if (write_file_atomic(file_hashes_path(), file_hashes_.serialize())) file_hashes_.mark_saved();
    else TOME_WARN("Couldn't write tome/cache/file-hashes.json");
}

// FNV-1a over the names and timestamps of every file refresh() depends on:
// state.json, every file in the mod folders (DLLs, tome.mod.json, assets)
// and every package.
// Not tome/mods/config (settings files have poll_settings_changed).
ModManager::DiskPrint ModManager::disk_fingerprint() const {
    DiskPrint out;
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
                if (depth > 0) self(path, depth - 1, self);
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
        // Apart from the files (poll_changed): Tome's own write isn't a change.
        const std::wstring path = paths_.tome_dir + L"\\state.json";
        FileStamp stamp;
        bool exists = file_stamp(path, stamp);
        {
            SharedLock hold(state_stamp_lock_);
            out.state_ours = exists && stamp == written_state_stamp_;
        }
        Fnv1a64 state;
        state.mix(&exists, sizeof(exists));
        state.mix(&stamp, sizeof(stamp));
        out.state = state.value;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((paths_.mods_dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!package::stem(narrow(name)).empty()) mix_file(paths_.mods_dir + L"\\" + name);
                continue;
            }
            if (_wcsicmp(name.c_str(), L"config") == 0) continue;
            std::wstring dir = paths_.mods_dir + L"\\" + name;
            mix_files(dir, kTopDepth, mix_files);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    out.files = hash.value;
    return out;
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
    if (shut_down_) return;
    refresh_locked(discover(), moment_hint);
}

void ModManager::refresh_locked(std::vector<Installed> installed, assets::Moment moment_hint) {
    if (shut_down_) return;  // the watcher may still notice changes while the game exits
    TOME_DEBUG("Refreshing mods");
    InRefresh in_refresh;
    save_file_hashes();  // what discover() hashed, here and not in DllMain

    // Declare natives for every installed mod that targets this game build,
    // enabled or not: a map that declares them must still load (§5.5).
    declare_natives(installed);

    // New mods start enabled, but code waits until the user allows it
    // (core/mod_state.h). Entries for mods that aren't installed right now
    // are kept, so removing and re-adding a mod remembers its setting.
    // An unparseable state.json is left alone (the user may be fixing it by
    // hand); rewriting it here would drop every disabled mod for good.
    bool state_invalid = false;
    int state_schema = mod_state::kSchema;
    mod_state::State state = read_state(&state_invalid, &state_schema);
    const mod_state::State state_before = state;
    const bool state_old = state_schema < mod_state::kSchema;
    implicit_approvals(state, installed, true);
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        state.try_emplace(mod.meta.id);
        if (approval_need(state, mod) == mod_state::Need::None || !wanted_by_user(state, mod.meta.id)) continue;
        if (!std::any_of(status_.begin(), status_.end(), [&](const ModStatus& s) {
                return s.folder == mod.folder && s.state == "approval";
            })) {  // once, not on every refresh
            TOME_WARN("%s: %s (in the mod menu, or /tome enable %s)", mod.meta.id.c_str(),
                      mod_state::describe(approval_need(state, mod), mod.code_kind), mod.meta.id.c_str());
        }
    }
    if (!state_invalid && (state_old || !(state_before == state))) write_state(state);
    publish_mappings(state, installed);

    std::vector<ResolveInput> inputs;
    std::vector<const Installed*> input_mods;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        inputs.push_back({&mod.meta, wanted_and_allowed(state, mod)});
        input_mods.push_back(&mod);
    }
    // During a multiplayer game, mods that can't start before it ends wait
    // (game_lock::waits): they're left out of resolve() for this refresh (not
    // in state.json), so they can neither turn running mods off nor stand in
    // for a dependency they won't meet; what needs them, with nothing that
    // runs to meet that need, waits too (waiting_dependents).
    std::set<std::string> waiting;
    std::vector<size_t> waiting_inputs;
    if (game_lock_) {
        std::set<std::string> locked_needs;
        std::set<std::wstring> locked_dirs;
        std::vector<const std::vector<Dependency>*> locked_breaks;
        for (const std::string& id : locked_) {
            auto rt = runtime_.find(id);
            // A locked mod that crashed stops in this refresh (below).
            if (rt == runtime_.end() || !rt->second->enabled || (rt->second->loaded() && rt->second->handle.faulted)) {
                continue;
            }
            locked_needs.insert(rt->second->depends.begin(), rt->second->depends.end());
            locked_dirs.insert(rt->second->handle.dir_w);
            locked_breaks.push_back(&rt->second->breaks);
        }
        for (size_t k = 0; k < inputs.size(); ++k) {
            const Installed& mod = *input_mods[k];
            const ModMetadata& m = mod.meta;
            auto rt = runtime_.find(m.id);
            if (!inputs[k].user_enabled || locked_.count(m.id) || (rt != runtime_.end() && rt->second->enabled)) continue;
            // A DLL known to fail shows its error instead.
            auto failure = failures_.find(m.id);
            bool loaded = rt != runtime_.end() && rt->second->loaded();
            if (!mod.dll.empty() && !loaded && failure != failures_.end() && failure->second.stamp == mod.dll_stamp) {
                continue;
            }
            game_lock::Newcomer n;
            n.id = m.id;
            n.required = m.sync == SyncMode::Required;
            n.provides = m.provides;
            n.blocked = locked_dirs.count(mod.dir) > 0;
            for (const std::vector<Dependency>* breaks : locked_breaks) {
                for (const Dependency& dep : *breaks) {
                    // As resolve() matches it: by id and version, or by "provides" for any version.
                    if ((m.id == dep.id && dep.range.matches(m.version)) ||
                        (dep.range.any() && std::find(m.provides.begin(), m.provides.end(), dep.id) != m.provides.end())) {
                        n.broken = true;
                    }
                }
            }
            if (!game_lock::waits(n, locked_needs)) continue;
            waiting.insert(m.id);
            waiting_inputs.push_back(k);
            inputs[k].user_enabled = false;
        }
    }
    ResolveResult resolved = resolve(inputs, {tome_version_, game_version_, build_.edition});
    if (!waiting_inputs.empty()) {
        for (size_t k : waiting_dependents(inputs, resolved, waiting_inputs)) waiting.insert(inputs[k].meta->id);
    }

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
    std::set<std::string> stopping, reloading, deferred;
    for (auto& [id, rt] : runtime_) {
        auto it = by_id.find(id);
        bool removed = it == by_id.end();
        bool code_changed = !removed && (rt->source_dll != it->second->dll ||
                                         (rt->loaded() && (!(rt->stamp == it->second->dll_stamp) || rt->reload_requested)));
        bool crashed = rt->loaded() && rt->handle.faulted;
        if (crashed) {
            failures_[id] = {rt->stamp, rt->handle.fault_text(), true, rt->handle.fault_calls_text()};
            TOME_WARN("%s crashed; it stays off until its DLL changes, or until it's disabled and enabled again",
                      id.c_str());
        }
        // Locked for a multiplayer game: whatever changed waits for its end.
        // Nothing can keep a crashed mod running, though.
        if (locked_.count(id) && !crashed) {
            if (removed || code_changed) deferred.insert(id);
            continue;
        }
        if (removed || code_changed || crashed) reloading.insert(id);
        if (rt->enabled && (!wanted.count(id) || code_changed || crashed)) stopping.insert(id);
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& [id, rt] : runtime_) {
            if (!rt->enabled || stopping.count(id)) continue;
            // A locked mod runs the code it was enabled with: its own
            // dependencies count, not what its tome.mod.json says now.
            std::vector<std::string> depends = rt->depends;
            if (!locked_.count(id)) {
                auto it = by_id.find(id);
                if (it == by_id.end()) continue;
                depends.clear();
                for (const Dependency& dep : it->second->meta.depends) depends.push_back(dep.id);
            }
            // What a locked mod needs is met by locked mods only (locked_mods
            // took in whatever met it, and plan_start holds new providers), so
            // only one of those stopping (a crash) takes it down, not another
            // mod that happens to provide the same name.
            const bool frozen = locked_.count(id) > 0;
            for (const std::string& dep : depends) {
                bool dep_stopping = false;
                for (const std::string& s : stopping) {
                    if (frozen && !locked_.count(s)) continue;
                    if (s == dep) dep_stopping = true;
                    auto other = by_id.find(s);
                    if (!frozen && other != by_id.end()) {
                        const auto& p = other->second->meta.provides;
                        if (std::find(p.begin(), p.end(), dep) != p.end()) dep_stopping = true;
                    }
                    const auto& p = runtime_[s]->provides;  // what it was enabled with
                    if (std::find(p.begin(), p.end(), dep) != p.end()) dep_stopping = true;
                }
                if (dep_stopping) {
                    stopping.insert(id);
                    grew = true;
                    break;
                }
            }
        }
    }

    // A crash (its own or a dependency's) stops even a locked mod; from
    // then on it's like any required mod that isn't running.
    for (const std::string& id : stopping) {
        if (locked_.erase(id)) TOME_WARN("%s stops during a multiplayer game; the game may desync", id.c_str());
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
        // Settings: the values from the settings file (or the defaults), and which
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
        // A locked mod keeps running as it started even if its folder went
        // away or its files broke meanwhile, and so do its settings (the
        // loop below updates them only from a usable schema).
        for (const std::string& id : locked_) {
            if (settings_schemas_.count(id)) with_settings.insert(id);
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
            for (const std::string& w : warnings) mod.warnings.push_back(settings_name(mod.meta.id) + ": " + w);
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
            settings_files_[mod.meta.id] = settings_file(mod.meta.id);
            settings_schemas_[mod.meta.id] = mod.settings;
        }
    }

    // Keybinds too, so tome_mod_enable can register its callbacks.
    std::vector<keys::Binding> declared_keys;
    std::set<std::string> declaring;
    for (const Installed& mod : installed) {
        if (!mod.valid) continue;
        declaring.insert(mod.meta.id);
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
    // A locked mod whose folder went away or whose tome.mod.json broke keeps
    // running, and keeps the keybinds it had.
    for (const keys::Binding& b : declared_keys_) {
        if (locked_.count(b.mod) && !declaring.count(b.mod)) declared_keys.push_back(b);
    }
    declared_keys_ = declared_keys;
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
    // During a multiplayer game, required mods that weren't running when it
    // started, and what depends on them, are held (core/game_lock.h; what
    // waits was left out of resolve() above, so this is a backstop).
    std::vector<game_lock::Candidate> candidates;
    for (size_t i : resolved.load_order) {
        const Installed& mod = *input_mods[i];
        auto rt = runtime_.find(mod.meta.id);
        auto failure = failures_.find(mod.meta.id);
        bool loaded = rt != runtime_.end() && rt->second->loaded();
        game_lock::Candidate c;
        c.id = mod.meta.id;
        c.required = mod.meta.sync == SyncMode::Required;
        for (const Dependency& dep : mod.meta.depends) {
            c.depends.push_back(dep.id);
            if (!dep.range.any()) c.versioned.push_back(dep.id);
        }
        c.provides = mod.meta.provides;
        c.fails = !mod.dll.empty() && !loaded && failure != failures_.end() && failure->second.stamp == mod.dll_stamp;
        c.running = rt != runtime_.end() && rt->second->enabled;
        candidates.push_back(std::move(c));
    }
    game_lock::StartPlan plan = game_lock::plan_start(candidates, game_lock_, locked_);
    std::set<std::string> held = plan.held;
    held.insert(waiting.begin(), waiting.end());
    std::vector<const Installed*> running;
    for (size_t k : plan.start) running.push_back(input_mods[resolved.load_order[k]]);
    apply_assets(running, moment);

    // Start everything wanted, dependencies first. A mod whose DLL fails to
    // load takes its dependents down with it.
    std::set<std::string> failed_now;
    for (size_t i : resolved.load_order) {
        const Installed& mod = *input_mods[i];
        const std::string& id = mod.meta.id;
        if (locked_.count(id)) continue;  // running, and stays as it is until the game ends
        if (held.count(id)) continue;

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
        // Only while none of its DLL is loaded: mods may keep the pointers
        // mod_version(), mod_dir() and config_dir() return (valid until
        // tome_mod_unload), and assigning a new string frees the old buffer.
        // A new version in tome.mod.json alone doesn't reload the DLL, so it
        // shows here once it does; a new folder always means a new DLL path,
        // which unloads the old one first.
        if (!rt.loaded()) {
            rt.handle.version = mod.meta.version.to_string();
            rt.handle.dir_w = mod.dir;
            rt.handle.dir = narrow(mod.dir);
            rt.folder = mod.folder;
            rt.handle.config_dir_w = paths_.mod_config_dir + L"\\" + widen(id);
            rt.handle.config_dir = narrow(rt.handle.config_dir_w);
        }

        if (!mod.dll.empty() && !rt.loaded()) {
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
            if (!rt.loaded()) {
                resolved.mods[i].state = ModState::DependencyInactive;  // reported as "error" below
                resolved.mods[i].reason = crashed ? error : "failed to load: " + error;
                failed_now.insert(id);
                for (const std::string& p : mod.meta.provides) failed_now.insert(p);
                continue;
            }
        }
        rt.required = mod.meta.sync == SyncMode::Required;
        rt.handle.sync_required = rt.required;
        rt.depends.clear();
        rt.versioned.clear();
        for (const Dependency& dep : mod.meta.depends) {
            rt.depends.push_back(dep.id);
            if (!dep.range.any()) rt.versioned.push_back(dep.id);
        }
        rt.provides = mod.meta.provides;
        rt.breaks = mod.meta.breaks;
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
        if (rt->loaded() && rt->handle.faulted) {
            refresh_locked(moment_hint);
            return;
        }
    }

    // Build the status list.
    std::vector<ModStatus> status;
    for (const Installed& mod : installed) {
        ModStatus s;
        s.folder = mod.folder;
        s.dir = narrow(mod.dir);
        s.package = mod.package;
        s.description = mod.meta.description;
        // The game decodes the icon, so only once the mod's files are allowed.
        if (mod.valid && approval_need(state, mod) == mod_state::Need::None) s.icon = mod.meta.icon;
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
        bool required = mod.meta.sync == SyncMode::Required;
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
        s.user_enabled = wanted_and_allowed(state, mod);
        s.code_hash = mod.code_hash;
        s.signer = mod.signer;
        s.code_kind = mod.has_code ? mod_state::kind_name(mod.code_kind) : "";
        const mod_state::Need need = approval_need(state, mod);
        auto rt = runtime_.find(mod.meta.id);
        s.running = rt != runtime_.end() && rt->second->enabled;
        bool locked = s.running && locked_.count(mod.meta.id) > 0;
        bool held_off = !s.running && held.count(mod.meta.id) > 0;
        bool changed = false;
        if (locked) {
            // What the game runs: the version and the handshake identity it
            // started with, while newer files wait.
            const std::string& running_version = rt->second->handle.version;
            changed = deferred.count(mod.meta.id) > 0 || (!running_version.empty() && running_version != s.version);
            if (!running_version.empty()) s.version = running_version;
            auto mounted = std::find_if(mounted_assets_.begin(), mounted_assets_.end(),
                                        [&](const assets::ModAssets& m) { return m.id == mod.meta.id; });
            uint64_t mounted_fingerprint = mounted == mounted_assets_.end() ? 0 : mounted->fingerprint;
            changed |= mounted_fingerprint != mod.assets_fingerprint;
            // The handshake counts it as what it was enabled as, whatever
            // tome.mod.json says now.
            required = rt->second->required;
            s.sync = required ? "required" : "client";
            for (const ModStatus& before : status_) {
                if (before.id == mod.meta.id && !before.sync_hash.empty()) s.sync_hash = before.sync_hash;
            }
        }
        // Only running mods take part in the handshake; hashing reads every file.
        if (s.running && required && s.sync_hash.empty()) s.sync_hash = sync_hash(mod, installed);
        s.locked = game_lock::note(locked, held_off, s.user_enabled, changed);
        s.state = s.running ? "active" : held_off ? "waiting" : (r.state == ModState::DisabledByUser ? "disabled" : "error");
        // A held mod may be "disabled" to the resolver (what waits is left out of resolve()).
        s.reason = s.running ? "" : held_off ? s.locked : r.state == ModState::DisabledByUser ? "" : r.reason;
        if (r.state == ModState::DisabledByUser && !safe_mode_.empty() && state[mod.meta.id].enabled) {
            s.reason = "safe mode";
        }
        // Off only because its code isn't allowed (yet, or again).
        if (!s.running && !held_off && need != mod_state::Need::None && wanted_by_user(state, mod.meta.id)) {
            s.state = "approval";
            s.reason = mod_state::describe(need, mod.code_kind);
            s.approval = need == mod_state::Need::New       ? "new"
                         : need == mod_state::Need::Changed ? "changed"
                                                            : "signer";
        }
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

    // A locked mod whose folder went away or whose tome.mod.json can't be
    // read (or now names another id) keeps running until the game ends: it
    // stays in the list, and in the handshake, as it was when it started,
    // in place of what its folder holds now.
    for (const std::string& id : locked_) {
        auto rt = runtime_.find(id);
        if (rt == runtime_.end() || !rt->second->enabled) continue;
        if (std::any_of(status.begin(), status.end(), [&](const ModStatus& s) { return s.id == id && s.running; })) {
            continue;
        }
        ModStatus s;
        auto before = std::find_if(status_.begin(), status_.end(), [&](const ModStatus& b) { return b.id == id; });
        if (before != status_.end()) {
            s = *before;
        } else {
            s.folder = rt->second->folder;
            s.dir = rt->second->handle.dir;
            s.package = !package::stem(s.folder).empty();
            s.id = s.name = id;
            s.version = rt->second->handle.version;
        }
        s.sync = rt->second->required ? "required" : "client";
        s.user_enabled = wanted_by_user(state, id);
        s.running = true;
        s.state = "active";
        s.reason.clear();
        s.locked = game_lock::note(true, false, s.user_enabled, true);
        // Never in place of a mod that runs: that one must stay listed (to be
        // seen and toggled), and blocked ids from this folder don't start.
        auto same_folder = std::find_if(status.begin(), status.end(), [&](const ModStatus& other) {
            return other.folder == s.folder && !other.running;
        });
        if (same_folder != status.end()) {
            *same_folder = std::move(s);
        } else {
            status.push_back(std::move(s));
        }
    }

    // Log only when something changed, so idle refreshes stay quiet.
    auto same = [](const std::vector<ModStatus>& a, const std::vector<ModStatus>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].folder != b[i].folder || a[i].state != b[i].state || a[i].reason != b[i].reason ||
                a[i].version != b[i].version || a[i].warnings != b[i].warnings ||
                a[i].assets_pending != b[i].assets_pending || a[i].locked != b[i].locked) {
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
            if (s.running && !s.locked.empty()) TOME_INFO("    %s: %s", s.folder.c_str(), s.locked.c_str());
            for (const std::string& w : s.warnings) TOME_WARN("    %s: %s", s.folder.c_str(), w.c_str());
        }
    }
    status_ = std::move(status);

    // For crash reports (crash_handler.h): what ran, and whose DLL is whose.
    std::vector<crash_report::ModLine> lines;
    for (const ModStatus& s : status_) lines.push_back({s.id.empty() ? s.folder : s.id, s.version, s.state});
    std::vector<std::pair<uintptr_t, std::string>> modules;
    for (const auto& [id, rt] : runtime_) {
        if (rt->module) modules.emplace_back(reinterpret_cast<uintptr_t>(rt->module), id);
    }
    crash_handler::set_mods(std::move(lines), std::move(modules));

    // Unpacked packages nothing uses any more: not installed now, and not
    // what a loaded mod (a locked one, say) was loaded from.
    std::set<std::wstring> keep;
    for (const Installed& mod : installed) {
        if (mod.package) keep.insert(mod.dir);
    }
    for (const auto& [id, rt] : runtime_) keep.insert(rt->handle.dir_w);
    packages::remove_unused(paths_, keep);

    // From now on asset changes follow the mods' reload modes. Until here
    // (DllMain and the first refresh, both part of startup) a mod whose DLL
    // fails to load loses its assets at once, whatever its reload mode.
    assets_started_ = true;
}

} // namespace tome
