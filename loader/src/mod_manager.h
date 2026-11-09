// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/assets.h"
#include "core/hash_cache.h"
#include "core/hooks.h"
#include "core/keys.h"
#include "core/metadata.h"
#include "core/mod_list.h"
#include "core/mod_state.h"
#include "core/mod_status.h"
#include "core/resolver.h"
#include "game_build.h"
#include "mod_handle.h"
#include "overlay.h"
#include "paths.h"
#include "wasm_mods.h"

#include <tome/tome.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
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
    // Bootstrap found no game window, so no dispatcher: TOME_EVENT_MODS_CHANGED
    // fires on the thread that changed the list (main_thread::post runs it there).
    void fire_events_without_dispatcher() { events_without_dispatcher_ = true; }

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

    // The mods as list_mods gives them (tome.h, API version 20): status()
    // as of the last refresh, without the lock (mods ask from callbacks that
    // run under it). Null before the first refresh. Any thread.
    std::shared_ptr<const mod_list::Snapshot> mod_list() const;
    // mods_dir() for the API (UTF-8), set by init().
    const std::string& mods_dir_utf8() const { return mods_dir_utf8_; }
    // What "game", "edition" and "tome" are checked against (resolve();
    // check_metadata), set by init().
    ResolveEnvironment environment() const { return {tome_version_, game_version_, build_.edition}; }

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
    // Any thread, also under the lock (mods' callbacks ask: objects.cpp).
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
        // tome.mod.json was read and is valid metadata (meta holds it), even
        // if the mod is invalid for another reason (a missing DLL, ...).
        bool meta_ok = false;
        ModMetadata meta;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        std::wstring dll;  // entry for this build: the DLL, or the .wasm (`wasm`); empty for asset-only mods
        FileStamp dll_stamp;
        bool wasm = false;  // `dll` is a WebAssembly module (tome.mod.json "wasm"), not a DLL
        // A DLL, game scripts among the assets (mod_state::is_script), other
        // assets or an icon: what has to be allowed before it runs.
        mod_state::CodeKind code_kind = mod_state::CodeKind::None;
        bool has_code = false;  // code_kind != None
        // With code: SHA-256 of its files' manifest (core/mod_state.h); ""
        // if one couldn't be read. Empty without code.
        std::string code_hash;
        // The key of its tome.sig if that's valid for these files
        // (core/signature.h, base64); "" if there's none or it isn't (then
        // a warning says why).
        std::string signer;
        std::vector<overlay::AssetFile> assets;
        uint64_t assets_fingerprint = 0;  // 0: no asset files
        settings::Schema settings;
        std::string settings_error;
        json::Value stored_settings;  // the settings file as read; null if there's none
        FileStamp settings_stamp;     // of that file, taken before reading it; zero if there's none
        // Its own mapping file for this build (tome.mod.json "mappings"):
        // null if it has none. Its symbols go by "<id>/<name>".
        std::shared_ptr<const mappings::Build> own_mappings;
        bool mappings_checked = false;  // against the game's code (mod_mappings::verify)
        std::wstring mappings_file;  // that file; empty if none
        FileStamp mappings_stamp;
    };

    struct Runtime {
        tome_mod handle;
        HMODULE module = nullptr;
        // A Wasm mod's instead of a module (wasm_mods.h); the lifecycle
        // function pointers stay null for it.
        std::unique_ptr<wasm_mods::Instance> wasm;
        bool loaded() const { return module || wasm; }
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
        std::string sha256;  // of the bytes parsed (tome.sig signs it); "" if unread
        ModMetadata meta;
        bool meta_ok = false;  // parse_metadata accepted it (valid may still be false: "requires")
        bool valid = false;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };
    struct ParsedMappings {
        std::wstring file;
        FileStamp stamp;  // zero: read it again
        std::shared_ptr<const mappings::Build> build;  // null if it has errors
        bool checked = false;  // build checked against the game's code (its errors are in errors)
        std::vector<std::string> errors;
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
    // a mod whose natives can't be declared becomes invalid (§5.5), and so
    // does what requires its mappings (check_mod_requires).
    void declare_natives(std::vector<Installed>& installed) const;
    // Reads the mod's mapping file for this build into `mod` (discover()).
    void read_mappings(Installed& mod) const;
    // Checks "requires" names from mods' mapping files ("<id>/<name>")
    // against the mod named, once every mod is read (discover()).
    void check_mod_requires(std::vector<Installed>& mods) const;
    // Hands the mapping files of the valid mods that are on and allowed to
    // mod_mappings (refresh); a locked mod keeps the table it runs with.
    void publish_mappings(const mod_state::State& state, const std::vector<Installed>& installed) const;
    // Runs fn under the lock, then tells the mods about settings that
    // changed meanwhile, and posts TOME_EVENT_MODS_CHANGED if the mod list did.
    void locked(const std::function<void()>& fn);
    // Makes mod_list() match status_; under lock_ (refresh_locked).
    void publish_mod_list();
    std::vector<std::string> set_enabled_locked(const std::string& id, bool enabled);
    std::vector<std::string> reload_locked(const std::string& id);
    // `invalid` (optional) is set when state.json exists but can't be
    // parsed; `schema` gets the file's (mod_state::parse; kSchema if
    // there's no file, which has nothing to upgrade).
    mod_state::State read_state(bool* invalid = nullptr, int* schema = nullptr) const;
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
    // The approvals nobody clicks: updates signed by the key the user
    // allowed, and TOME_DEBUG_APPROVE's (tests).
    void implicit_approvals(mod_state::State& state, const std::vector<Installed>& installed, bool log) const;
    // Also records the new file's stamp, so the watcher doesn't take Tome's
    // own write for an edit (disk_fingerprint).
    void write_state(const mod_state::State& state);
    // Installed::signer: checks the mod's tome.sig, if it has one, against
    // its id, code hash and the SHA-256 of the tome.mod.json bytes its
    // metadata was parsed from (ParsedMeta::sha256).
    void check_signature(Installed& mod, const std::string& metadata_sha256) const;
    // Installed::code_hash, cached by folder (in file_hashes_, so across
    // starts too) until a code file's name, size or times change.
    std::string code_hash(const Installed& mod) const;
    // A file's SHA-256 (empty if it can't be read), from the hash cache if
    // it hasn't changed since it was last hashed; save_file_hashes writes
    // the cache (tome/cache/file-hashes.json) if it changed.
    std::string file_sha256(const std::wstring& path) const;
    std::wstring file_hashes_path() const;
    void load_file_hashes() const;
    // Not in DllMain (mount_startup_assets): the first refresh saves.
    void save_file_hashes() const;

    bool load(Runtime& rt, const Installed& mod, std::string& error);
    bool load_wasm(Runtime& rt, const Installed& mod, std::string& error);
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
    // What the watcher compares: the mods' files (hashed), and state.json
    // apart from them, which counts only when someone else wrote it.
    struct DiskPrint {
        uint64_t files = 0;
        uint64_t state = 0;       // state.json's stamp, hashed
        bool state_ours = false;  // ...as Tome itself last wrote it
    };
    DiskPrint disk_fingerprint() const;
    // A "sync": "required" mod's identity in the multiplayer handshake
    // (core/handshake.h): tome.mod.json, the DLL, its mapping file and every
    // asset file, hashed, and the mapping files of the mods it names in
    // "depends" or "requires" (from `installed`); cached until one of them
    // changes.
    std::string sync_hash(const Installed& mod, const std::vector<Installed>& installed);
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
    // A multiplayer game is running (set_game_lock). Written under lock_,
    // read without it by game_locked(): mods' calls (set_property,
    // jass_call) ask from callbacks that already run under lock_.
    std::atomic<bool> game_lock_{false};
    std::set<std::string> locked_;  // the mods it keeps running, by id
    std::vector<keys::Binding> declared_keys_;  // what the last refresh gave keybinds::set_mods
    std::set<std::string> safe_mode_enabled_;  // enabled during this session despite safe mode
    std::map<std::string, std::pair<uint64_t, std::string>> sync_hashes_;  // by folder: files' stamps, hash
    mutable std::map<std::string, ParsedMeta> meta_cache_;
    mutable std::map<std::string, ParsedSettings> settings_cache_;
    mutable std::map<std::string, ParsedMappings> mappings_cache_;
    // The last check of each folder's tome.sig, by folder (check_signature).
    struct CheckedSignature {
        std::string text;  // tome.sig
        std::string metadata_sha256;
        std::string code_hash;
        std::string signer;   // "" if it isn't valid
        std::string warning;  // why it isn't
        bool verified = false;
    };
    mutable std::map<std::string, CheckedSignature> signature_cache_;
    mutable hash_cache::Cache file_hashes_;  // every code file's SHA-256 (file_sha256), each folder's code hash
    mutable bool file_hashes_read_ = false;
    // Each .tome in tome/mods as of its last unpack, by file name.
    struct UnpackedPackage {
        FileStamp stamp;
        std::wstring dir;
        std::string error;
    };
    mutable std::map<std::string, UnpackedPackage> package_cache_;
    DiskPrint last_polled_;  // set in init(), then only touched by the watcher thread
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
    // list_mods' snapshot (mod_list()): its own lock, never held while
    // taking another one.
    mutable SRWLOCK mod_list_lock_ = SRWLOCK_INIT;
    std::shared_ptr<const mod_list::Snapshot> mod_list_;
    bool mod_list_changed_ = false;  // under lock_: publish_mod_list changed it, locked() tells the mods
    std::atomic<bool> events_without_dispatcher_{false};  // fire_events_without_dispatcher
    std::string mods_dir_utf8_;
};

ModManager& mod_manager();

} // namespace tome
