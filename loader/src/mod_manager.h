#pragma once

#include "core/assets.h"
#include "core/hooks.h"
#include "core/keys.h"
#include "core/metadata.h"
#include "core/mod_state.h"
#include "core/mod_status.h"
#include "core/resolver.h"
#include "game_build.h"
#include "mod_handle.h"
#include "overlay.h"
#include "paths.h"

#include <tome/tome.h>

#include <windows.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace tome {

class ModManager {
public:
    void init(const Paths& paths, const GameBuild& build);

    // Safe mode (docs/research.md §9): no mod starts, whatever state.json
    // says, until it's enabled in this session (mod menu, /tome enable).
    // state.json isn't changed by it. `reason` says what asked for it
    // ("-tomesafe"); empty: normal start. Call before mount_startup_assets.
    void set_safe_mode(const std::string& reason);
    std::string safe_mode() const;

    // The mod menu's Safe Mode / Normal Mode: turns safe mode on (every mod
    // stops) or off (the mods the player had on start again) for this
    // session. Turning it off also removes tome/safe, so the next start is
    // normal too. Main thread.
    void set_safe_mode_now(bool on);

    // Mounts the assets of the mods that will run, before the game loads
    // anything else (DllMain: no DLLs are loaded here, only files read).
    // `report` says what was mounted, for the log once it's open.
    void mount_startup_assets(const Paths& paths, const GameBuild& build, std::string& report);

    // The main menu was just built: applies asset changes of "reload": "map"
    // mods that were waiting for it. Main thread.
    void on_main_menu();

    // A map is starting to load and hasn't read its files yet: applies
    // waiting asset changes of "reload": "map" mods. Main thread.
    void on_map_load();

    // Re-reads mods/ and tome/state.json, resolves, then enables, disables,
    // loads and reloads mods to match. Safe to call repeatedly from any thread.
    void refresh();

    // True if any file refresh() reads (state.json, a tome.mod.json or a mod
    // DLL) changed since the previous call. Only reads file timestamps, so the
    // watcher thread can poll it; the refresh itself runs on the main thread.
    bool poll_changed();

    // True if a mod's tome/mods/config/<id>.json changed since Tome last read or
    // wrote it (an edit by hand). Not part of poll_changed: Tome's own writes
    // would otherwise refresh every mod after each change. Watcher thread.
    bool poll_settings_changed() const;

    // Reads the settings files that changed, applies their valid values
    // and tells the mods. Main thread.
    void reload_settings();

    std::vector<ModStatus> status() const;
    const Paths& paths() const { return paths_; }

    // Saves the mod's enabled flag to tome/state.json and applies it now.
    // Disabling a mod also disables the enabled mods that depend on it
    // (directly or not, unless another enabled mod provides what they need);
    // returns their ids. Call on the main thread (e.g. from the mod menu).
    std::vector<std::string> set_enabled(const std::string& id, bool enabled);

    // Reloads the DLL of the running mod `id` (every running mod with a DLL
    // if `id` is empty), with its dependents, as if it had been rebuilt.
    // Returns the ids that were reloaded. Main thread.
    std::vector<std::string> reload(const std::string& id);

    // A multiplayer game or replay started (on) or ended (off); main thread
    // (core/game_lock.h, docs/research.md §5.7). While it's on, the running
    // "sync": "required" mods and what they depend on keep running unchanged
    // (no toggles, hot reloads or asset changes; only a crash stops one) and
    // other required mods don't start. Toggles still go into state.json and
    // apply when the lock goes off. Refreshes later, from the message loop.
    void set_game_lock(bool on);
    bool game_locked() const;

    // Settings (core/settings.h), saved in tome/mods/config/<id>.json.
    // The current value of `key`, or a null value if the mod or setting
    // doesn't exist. Any thread.
    json::Value setting(const std::string& id, const std::string& key) const;
    // Validates (core/settings.h, coerce), saves and applies a value, then
    // tells the mod (settings_sink). TOME_ERR_INVALID_ARGUMENT for an unknown
    // setting or a value of the wrong type; `error` says why.
    tome_result set_setting(const std::string& id, const std::string& key, const json::Value& value,
                            std::string& error);
    // Every setting of `id` back to its default.
    void reset_settings(const std::string& id);
    // Called (on the thread that changed them, outside the lock) with the
    // keys whose values changed, from set_setting or from its settings file
    // changing on disk.
    using SettingsSink = void (*)(const std::string& id, const std::vector<std::string>& keys);
    void set_settings_sink(SettingsSink sink) { settings_sink_ = sink; }

    // A guarded call into `mod` crashed (any thread). The mod is disabled,
    // with its dependents, from the main thread; it stays off until its DLL
    // changes or it's disabled and enabled again.
    void on_fault(tome_mod* mod);

    // The program is exiting (main thread): disables every running mod,
    // dependents first, then calls every loaded mod's tome_mod_unload and
    // revokes what it registered. The DLLs stay loaded, since the game's own
    // teardown still runs. Afterwards nothing refreshes any more.
    void shutdown();

private:
    struct Installed {
        std::string folder;   // in tome/mods: a folder's name, or a package's file name ("hello.tome")
        std::wstring dir;     // where its files are: the folder, or the package unpacked in tome/packages
        bool package = false;
        bool valid = false;
        ModMetadata meta;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        std::wstring dll;  // entry for this build; empty for asset-only mods
        FileStamp dll_stamp;
        // A DLL, or game scripts among the assets (mod_state::is_script):
        // what has to be allowed before it runs.
        bool has_code = false;
        // With code: SHA-256 of its code files' manifest (core/mod_state.h);
        // "" if one couldn't be read. Empty without code.
        std::string code_hash;
        std::vector<overlay::AssetFile> assets;
        uint64_t assets_fingerprint = 0;  // 0: no asset files
        settings::Schema settings;
        std::string settings_error;
        json::Value stored_settings;  // the settings file as read; null if there's none
        FileStamp settings_stamp;     // of that file, taken before reading it; zero if there's none
    };

    struct Runtime {
        tome_mod handle;
        HMODULE module = nullptr;
        std::wstring source_dll;
        FileStamp stamp;
        std::wstring shadow_dll;
        std::string folder;  // Installed::folder it was loaded from
        tome_mod_lifecycle_fn enable_fn = nullptr;
        tome_mod_lifecycle_fn disable_fn = nullptr;
        tome_mod_lifecycle_fn unload_fn = nullptr;
        bool enabled = false;
        // `/tome reload` asked for its DLL to be loaded again (the next
        // refresh treats it as changed; unload() clears it). rt.stamp stays
        // the loaded DLL's, so a crash is still recorded against that DLL.
        bool reload_requested = false;
        // From tome.mod.json as of the last refresh that kept it running,
        // for the game lock; frozen while it's locked.
        bool required = false;
        std::vector<std::string> depends;  // ids
        std::vector<std::string> versioned;  // the depends with a version range
        std::vector<std::string> provides;
        std::vector<Dependency> breaks;
    };

    struct LoadFailure {
        FileStamp stamp;  // retry only when the DLL changes
        std::string reason;
        bool crashed = false;  // a crash, while loading or later; also retried once the user re-enables the mod
        std::string calls;     // a crash: where it was called from, as logged
    };

    // What discover() parsed from a mod folder's files, by folder, kept until
    // a file's stamp changes. Under lock_, like discover().
    struct ParsedMeta {
        bool read = false;
        FileStamp stamp;  // of tome.mod.json; zero: read it again
        ModMetadata meta;
        bool valid = false;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };
    struct ParsedSettings {
        bool schema_read = false;
        std::wstring schema_file;
        FileStamp schema_stamp;
        settings::Schema schema;
        std::string error;
        bool unusable = false;  // the schema couldn't be read or parsed: no settings at all
        std::vector<std::string> schema_warnings;
        bool values_read = false;
        std::wstring values_file;
        FileStamp values_stamp;  // of tome/mods/config/<id>.json; zero if there's none
        json::Value values;
        std::string values_warning;
    };

    std::vector<Installed> discover() const;
    void read_settings_schema(Installed& mod) const;
    // A mod's settings values, tome/mods/config/<id>.json, and its name for messages.
    std::wstring settings_file(const std::string& id) const { return paths_.mod_config_dir + L"\\" + widen(id) + L".json"; }
    static std::string settings_name(const std::string& id) { return "config\\" + id + ".json"; }
    // Declares the natives of every valid mod that targets this game build;
    // a mod whose natives can't be declared becomes invalid (§5.5).
    void declare_natives(std::vector<Installed>& installed) const;
    // Runs fn under the lock, then tells the mods about settings that changed meanwhile.
    void locked(const std::function<void()>& fn);
    std::vector<std::string> set_enabled_locked(const std::string& id, bool enabled);
    std::vector<std::string> reload_locked(const std::string& id);
    // `invalid` (optional) is set when state.json exists but can't be
    // parsed, `legacy` when it's from before approvals (mod_state::parse).
    mod_state::State read_state(bool* invalid = nullptr, bool* legacy = nullptr) const;
    // Whether the user wants `id` to run: state.json (new mods start
    // enabled), unless safe mode is on.
    bool wanted_by_user(const mod_state::State& state, const std::string& id) const;
    // Whether the mod's code has to be allowed (again) before it runs.
    static mod_state::Need approval_need(const mod_state::State& state, const Installed& mod);
    // wanted_by_user, and its code allowed: what the resolver gets.
    bool wanted_and_allowed(const mod_state::State& state, const Installed& mod) const {
        return wanted_by_user(state, mod.meta.id) && approval_need(state, mod) == mod_state::Need::None;
    }
    // Records `mod`'s code as allowed in `state` (nothing for a mod without
    // code), logging `how` unless it's null (in DllMain, before the log opens).
    static void approve(mod_state::State& state, const Installed& mod, const char* how);
    // The approvals nobody clicks: mods a state.json from before approvals
    // lists (they ran without one), and TOME_DEBUG_APPROVE's (tests).
    static void implicit_approvals(mod_state::State& state, const std::vector<Installed>& installed, bool legacy,
                                   bool log);
    // Also records the new file's stamp, so the watcher doesn't take Tome's
    // own write for an edit (disk_fingerprint).
    void write_state(const mod_state::State& state);
    // Installed::code_hash, cached by folder until a code file's name, size or time changes.
    std::string code_hash(const Installed& mod) const;

    bool load(Runtime& rt, const Installed& mod, std::string& error);
    void unload(Runtime& rt);
    void enable(Runtime& rt);
    void disable(Runtime& rt);

    // `moment` says when this runs if it's known to be a better time for
    // asset changes than an ordinary refresh (the main menu, a map load).
    void refresh_locked(assets::Moment moment = assets::Moment::Refresh);
    // The same, with what discover() just returned.
    void refresh_locked(std::vector<Installed> installed, assets::Moment moment = assets::Moment::Refresh);
    // Brings the assets archive in line with `running` (load order), as far
    // as each mod's reload mode allows at `moment`. Returns the archive's new
    // summary, or "" if it didn't change.
    std::string apply_assets(const std::vector<const Installed*>& running, assets::Moment moment);
    uint64_t disk_fingerprint() const;
    // A "sync": "required" mod's identity in the multiplayer handshake
    // (core/handshake.h): tome.mod.json, the DLL and every asset file, hashed;
    // cached until one of them changes.
    std::string sync_hash(const Installed& mod);
    bool write_settings(const std::string& id);  // also records the new stamp
    // Sends what changed to settings_sink_; call without the lock held.
    void notify_settings(std::vector<std::pair<std::string, std::vector<std::string>>> changes) const;

    Paths paths_;
    GameBuild build_;
    SemVer tome_version_;
    GameVersion game_version_;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::map<std::string, std::unique_ptr<Runtime>> runtime_;
    std::vector<std::string> enable_order_;  // ids, in the order they were enabled
    std::map<std::string, LoadFailure> failures_;
    std::vector<ModStatus> status_;
    unsigned shadow_counter_ = 0;
    std::vector<assets::ModAssets> mounted_assets_;  // what the assets archive holds
    std::vector<assets::Pending> pending_assets_;
    bool assets_started_ = false;  // the first refresh finished; before that, asset changes count as startup
    bool shut_down_ = false;       // shutdown() ran: no more refreshes
    std::string safe_mode_;        // why safe mode is on; empty if it isn't
    bool game_lock_ = false;       // a multiplayer game is running (set_game_lock)
    std::set<std::string> locked_;  // the mods it keeps running, by id
    std::vector<keys::Binding> declared_keys_;  // what the last refresh gave keybinds::set_mods
    std::set<std::string> safe_mode_enabled_;  // enabled during this session despite safe mode
    std::map<std::string, std::pair<uint64_t, std::string>> sync_hashes_;  // by folder: files' stamps, hash
    mutable std::map<std::string, ParsedMeta> meta_cache_;
    mutable std::map<std::string, ParsedSettings> settings_cache_;
    mutable std::map<std::string, std::pair<uint64_t, std::string>> code_hash_cache_;  // by folder: stamps, hash
    // Each .tome in tome/mods as of its last unpack, by file name.
    struct UnpackedPackage {
        FileStamp stamp;
        std::wstring dir;
        std::string error;
    };
    mutable std::map<std::string, UnpackedPackage> package_cache_;
    uint64_t last_polled_ = 0;  // set in init(), then only touched by the watcher thread
    // tome/state.json as Tome last wrote it. Its own lock: written under
    // lock_ on the main thread, read by the watcher thread without lock_.
    mutable SRWLOCK state_stamp_lock_ = SRWLOCK_INIT;
    FileStamp written_state_stamp_;
    // Settings have a lock of their own, taken after lock_ if both are: mods
    // read and change them from callbacks that run while lock_ is held.
    mutable SRWLOCK settings_lock_ = SRWLOCK_INIT;
    std::map<std::string, json::Value> settings_values_;  // by mod id: every setting's current value
    std::map<std::string, settings::Schema> settings_schemas_;  // by mod id
    std::map<std::string, std::wstring> settings_files_;  // by mod id: tome/mods/config/<id>.json
    std::map<std::string, FileStamp> settings_stamps_;    // by mod id: that file as Tome last read or wrote it
    std::vector<std::pair<std::string, std::vector<std::string>>> settings_changed_;  // for notify_settings
    SettingsSink settings_sink_ = nullptr;
};

ModManager& mod_manager();

} // namespace tome
