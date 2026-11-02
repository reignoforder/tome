// tome-check: checks a built mod the way Tome would load it, without the
// game (sdk/README.md, "Testing a mod"). Runs the real loader
// against the fake game of testing/fake_game.h: discovers the mod, reads
// tome.mod.json, its settings schema and keybinds, allows it, loads and
// turns it on, binds its natives as a map load does, then turns it off and
// unloads it. Reports every problem Tome found, and every warning or error
// it logged on the way (the mod's own lines too).
//
//   tome-check [--strict] <mod folder or .tome> [<the mods it needs>...]
//
// Exit code: 0 fine (warnings are printed), 1 problems (or warnings with
// --strict), 2 usage or a broken environment.

#include "core/blp.h"
#include "core/mod_state.h"
#include "log.h"
#include "mod_manager.h"
#include "natives.h"
#include "paths.h"
#include "testing/fake_game.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tome;

namespace {

struct Report {
    std::vector<std::string> errors, warnings;
    void error(std::string text) { add(errors, std::move(text)); }
    void warning(std::string text) { add(warnings, std::move(text)); }

    static void add(std::vector<std::string>& list, std::string text) {
        for (const std::string& seen : list) {
            if (seen.find(text) != std::string::npos || text.find(seen) != std::string::npos) return;
        }
        list.push_back(std::move(text));
    }
};

bool find(const std::string& folder, ModStatus& out) {
    for (ModStatus& s : mod_manager().status()) {
        if (s.folder == folder) {
            out = std::move(s);
            return true;
        }
    }
    return false;
}

std::string status_text(const ModStatus& s) {
    return s.reason.empty() ? s.state : s.state + ": " + s.reason;
}

// What Tome says of a mod it hasn't been allowed to run yet, as every
// freshly installed one is: tome-check allows it itself.
bool is_approval_notice(const std::string& text) {
    using mod_state::CodeKind;
    for (CodeKind kind : {CodeKind::Assets, CodeKind::Wasm, CodeKind::Scripts, CodeKind::Native}) {
        if (text.find(mod_state::describe(mod_state::Need::New, kind)) != std::string::npos) return true;
    }
    return false;
}

// "[12:00:00.000] [ 1234] [WARN ] text\r\n" -> level and text.
void read_log_problems(const std::wstring& path, Report& report) {
    std::string text;
    if (!read_file(path, text)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t warn = line.find("] [WARN ] "), error = line.find("] [ERROR] ");
        if (warn != std::string::npos && !is_approval_notice(line)) report.warning(line.substr(warn + 10));
        if (error != std::string::npos) report.error(line.substr(error + 10));
    }
}

// tome.mod.json "icon": the Mods menu shows it through the game's texture
// loader, which refuses an unsafe BLP (core/blp.h) as the game would crash on it.
void check_icon(const ModStatus& mod, const std::string& icon, Report& report) {
    if (icon.empty()) return;
    std::string bytes;
    if (!read_file(widen(mod.dir) + L"\\" + widen(icon), bytes)) {
        report.warning("icon " + icon + " isn't in the mod folder: the Mods menu shows none");
        return;
    }
    bool blp = icon.size() > 4 && _stricmp(icon.c_str() + icon.size() - 4, ".blp") == 0;
    if (!blp) return;
    if (bytes.compare(0, 4, "BLP1") != 0) {
        report.warning("icon " + icon + " isn't a BLP1 texture (Warcraft III's): the Mods menu shows none");
    } else if (const char* reason = blp::reject_reason(bytes)) {
        report.error("icon " + icon + " is refused as unsafe to decode (" + reason + ")");
    }
}

int usage() {
    std::fprintf(stderr, "usage: tome-check [--strict] <mod folder or .tome> [<the mods it needs>...]\n");
    return 2;
}

} // namespace

int main() {
    // The wide command line: char** argv is in the ANSI code page, which
    // can't hold every path (win_path takes UTF-8).
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return usage();
    bool strict = false;
    std::vector<std::wstring> paths;
    for (int i = 1; i < argc; ++i) {
        std::string arg = narrow(argv[i]);
        if (arg == "--strict") {
            strict = true;
        } else if (arg.empty() || arg[0] == '-') {
            return usage();
        } else {
            paths.push_back(testing::win_path(arg));
        }
    }
    LocalFree(argv);
    if (paths.empty()) return usage();

    testing::Home::remove_leftovers(L"tome-check");
    testing::Home home(L"tome-check");
    std::vector<std::string> folders;
    for (const std::wstring& from : paths) {
        std::wstring name;
        if (!testing::install(home, from, &name)) {
            std::fprintf(stderr, "tome-check: can't read %s\n", narrow(from).c_str());
            home.remove();
            return 2;
        }
        folders.push_back(narrow(name));
    }

    testing::FakeGame game;
    testing::StartOptions options;
    options.events = true;
    options.fake_storm = true;
    if (!game.create() || !testing::start(home, game.build(), options)) {
        std::fprintf(stderr, "tome-check: Tome didn't start (log: %s)\n", narrow(testing::log_path(home)).c_str());
        return 2;
    }
    const std::string& folder = folders[0];
    Report report;
    ModStatus mod;
    std::string summary;
    if (!find(folder, mod)) {
        report.error("Tome doesn't see a mod in " + folder + " (no tome.mod.json at its top?)");
    } else {
        for (const std::string& e : mod.errors) {
            report.error(e.compare(0, 13, "tome.mod.json") == 0 ? e : "tome.mod.json: " + e);
        }
        if (!mod.settings_error.empty()) report.error("settings: " + mod.settings_error);
        for (const std::string& w : mod.warnings) {
            if (!is_approval_notice(w)) report.warning(w);
        }
    }
    if (report.errors.empty() && !mod.id.empty()) {
        // Allowed and turned on, as a player would (the mods it needs first).
        for (size_t i = 1; i < folders.size(); ++i) {
            ModStatus dep;
            if (find(folders[i], dep) && !dep.id.empty()) mod_manager().set_enabled(dep.id, true);
        }
        mod_manager().set_enabled(mod.id, true);
        find(folder, mod);
        if (mod.state != "active") {
            report.error("doesn't start: " + status_text(mod));
        } else {
            game.load_map();
            unsigned declared = 0;
            for (const natives::NativeInfo& n : natives::list()) {
                if (n.owner != mod.id) continue;
                ++declared;
                if (!n.attached) {
                    report.warning("native " + n.name + " is in tome.mod.json, but the mod didn't register it "
                                   "(register_native in tome_mod_enable): it returns 0");
                }
            }
            check_icon(mod, mod.icon, report);
            mod_manager().set_enabled(mod.id, false);
            find(folder, mod);
            if (mod.state != "disabled") report.error("doesn't turn off: " + status_text(mod));
            char text[160];
            std::snprintf(text, sizeof(text), "%s, %u native%s", mod.code_kind.empty() ? "no code" : mod.code_kind.c_str(),
                          declared, declared == 1 ? "" : "s");
            summary = text;
        }
    }
    mod_manager().shutdown();
    read_log_problems(testing::log_path(home), report);
    log::close();

    std::string name = mod.id.empty() ? folder : mod.version.empty() ? mod.id : mod.id + " " + mod.version;
    for (const std::string& e : report.errors) std::printf("%s: error: %s\n", name.c_str(), e.c_str());
    for (const std::string& w : report.warnings) std::printf("%s: warning: %s\n", name.c_str(), w.c_str());
    bool failed = !report.errors.empty() || (strict && !report.warnings.empty());
    if (failed) {
        std::printf("%s: check failed; Tome's log: %s\n", name.c_str(), narrow(testing::log_path(home)).c_str());
    } else {
        std::printf("%s: ok (%s)\n", name.c_str(), summary.empty() ? "checked" : summary.c_str());
        home.remove();
    }
    return failed ? 1 : 0;
}
