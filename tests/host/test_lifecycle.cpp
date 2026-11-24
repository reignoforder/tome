// SPDX-License-Identifier: GPL-3.0-or-later
// Discovery, metadata problems, state.json, hot reload, packages, safe mode
// and shutdown, with the hello examples (C, C++, Rust) and the e2e fixtures.
#include "harness.h"
#include "test.h"

#include "api.h"
#include "core/json.h"
#include "core/zip.h"
#include "frames.h"
#include "hooks.h"
#include "mappings.h"
#include "mod_handle.h"
#include "mod_manager.h"
#include "patches.h"
#include "paths.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

using host::Home;
using host::Session;
using tome::mod_manager;

namespace {

const tome::ModStatus* find_status(const std::vector<tome::ModStatus>& all, const char* id) {
    for (const tome::ModStatus& s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const tome::ModStatus* find_folder(const std::vector<tome::ModStatus>& all, const char* folder) {
    for (const tome::ModStatus& s : all) {
        if (s.folder == folder) return &s;
    }
    return nullptr;
}

bool any_error_contains(const tome::ModStatus* s, const char* text) {
    if (!s) return false;
    if (s->reason.find(text) != std::string::npos) return true;
    for (const std::string& e : s->errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

int count_logged(const char* text) {
    std::string log = host::log_text();
    int n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++n;
    return n;
}

// What a user editing tome/state.json does (the e2e test's set_enabled).
bool edit_state(Home& home, const char* id, bool enabled) {
    std::string text;
    if (!home.read(L"tome\\state.json", text)) return false;
    tome::json::Value root;
    if (!tome::json::parse(text, root).ok || !root.find("mods") || !root.find("mods")->find(id)) return false;
    tome::json::Value mods = *root.find("mods");
    tome::json::Value mod = *mods.find(id);
    mod.set("enabled", tome::json::Value::boolean(enabled));
    mods.set(id, mod);
    root.set("mods", mods);
    return home.write(L"tome\\state.json", tome::json::serialize(root));
}

// --- a zip writer (stored entries), for changed packages ---

void put16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>(v >> 8));
}

void put32(std::string& out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v & 0xFFFF));
    put16(out, static_cast<uint16_t>(v >> 16));
}

std::string write_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    for (const auto& [name, data] : files) {
        uint32_t crc = tome::zip::crc32(data);
        uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);  // stored
        put16(out, 0);
        put16(out, 0x21);  // 1980-01-01
        put32(out, crc);
        put32(out, static_cast<uint32_t>(data.size()));
        put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint16_t>(name.size()));
        put16(out, 0);
        out += name;
        out += data;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(data.size()));
        put32(central, static_cast<uint32_t>(data.size()));
        put16(central, static_cast<uint16_t>(name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }
    uint32_t central_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint16_t>(files.size()));
    put16(out, static_cast<uint16_t>(files.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, central_offset);
    put16(out, 0);
    return out;
}

// The built hello.tome with tome.mod.json's version changed.
bool hello_package(const char* version, std::string& out) {
    std::string archive;
    if (!tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/packages/hello.tome"), archive)) return false;
    std::vector<tome::zip::Entry> entries;
    std::string error;
    if (!tome::zip::read_directory(archive, entries, error)) return false;
    std::vector<std::pair<std::string, std::string>> files;
    for (const tome::zip::Entry& e : entries) {
        if (e.directory()) continue;
        std::string data;
        if (!tome::zip::extract(archive, e, data, error)) return false;
        if (e.name == "tome.mod.json") {
            size_t at = data.find("\"version\": \"1.0.0\"");
            if (at == std::string::npos) return false;
            data.replace(at, std::strlen("\"version\": \"1.0.0\""), std::string("\"version\": \"") + version + "\"");
        }
        files.emplace_back(e.name, data);
    }
    out = write_zip(files);
    return true;
}

using Native1 = int32_t (*)(int32_t);
using NativeReal = uint32_t (*)(const float*);

float bits_to_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

} // namespace

TEST(host_metadata_problems_reported) {
    Home home;
    CHECK(home.install_mod("broken-json"));
    CHECK(home.install_mod("needs-ghost"));
    CHECK(home.install_mod("needs-unmapped"));
    Session session;
    CHECK(session.start(home));
    auto all = mod_manager().status();
    const tome::ModStatus* broken = find_folder(all, "broken-json");
    CHECK(broken && broken->state == "error");
    CHECK(any_error_contains(broken, "tome.mod.json: line 2"));
    const tome::ModStatus* ghost = find_status(all, "needs-ghost");
    CHECK(any_error_contains(ghost, "needs \"ghost\" ^1, which isn't installed"));
    const tome::ModStatus* unmapped = find_status(all, "needs-unmapped");
    CHECK(any_error_contains(unmapped, "needs \"CUnit.wings\", which isn't mapped for 1.27b"));
    // Properties (core/objects.h): "mana" is mapped, "wings" isn't a property at all.
    CHECK(any_error_contains(unmapped, "needs the property \"wings\", which this version of Tome doesn't have"));
    CHECK(!any_error_contains(unmapped, "\"mana\""));
    CHECK_LOG("Mods: 3 installed, 0 active");
}

namespace {

// A mod folder in tome/mods with these files ('/' between folders).
bool write_mod(Home& home, const char* id, std::initializer_list<std::pair<const char*, std::string>> files) {
    const std::wstring dir = L"tome\\mods\\" + tome::widen(id);
    CreateDirectoryW((home.paths().home_dir + L"\\" + dir).c_str(), nullptr);
    for (const auto& [name, text] : files) {
        std::wstring relative = tome::widen(name);
        for (wchar_t& c : relative) c = c == L'/' ? L'\\' : c;
        const size_t slash = relative.rfind(L'\\');
        if (slash != std::wstring::npos) {
            CreateDirectoryW((home.paths().home_dir + L"\\" + dir + L"\\" + relative.substr(0, slash)).c_str(), nullptr);
        }
        if (!home.write(dir + L"\\" + relative, text)) return false;
    }
    return true;
}

std::string meta(const char* id, const char* extra = "") {
    return std::string(R"({"schema": 1, "id": ")") + id +
           R"(", "version": "1.0.0", "game": "*", "tome": "*", "sync": "client")" + extra + "}";
}

// cdecl int(int x): x + 5, at an RVA no Tome symbol is near (the fake
// image is zero there until the mod's mapping is published).
constexpr uint32_t kAddFiveRva = 0x00590000;
const char* const kLibMappings = R"j({"schema": 2, "symbols": {
    "AddFive": {"kind": "function", "rva": "0x00590000", "conv": "cdecl", "sig": "int(int x)",
                "bytes": "55 8b ec 8b 45 08 83 c0 05 5d c3"},
    "Thing.size": {"kind": "offset", "value": "0x48"}
}})j";

void TOME_CALL add_100(tome_mod*, tome_hook_call* call, void*) {
    call->args[0] += 100;
}

} // namespace

// A mod's own mapping file (tome.mod.json "mappings", by default the
// mappings/ folder): its symbols resolve and hook as "<id>/<name>", and
// "requires" names them, its own or another mod's. A file for another build,
// a missing name or mod, and a "mappings" folder that isn't there are
// reported.
TEST(host_mods_own_mappings) {
    Home home;
    CHECK(write_mod(home, "maplib", {{"tome.mod.json", meta("maplib", R"(, "requires": ["maplib/AddFive"])")},
                                     {"mappings/1.27b.json", kLibMappings},
                                     {"mappings/1.26a.json", "not even JSON: another build's, never read"}}));
    CHECK(write_mod(home, "mapuser",
                    {{"tome.mod.json", meta("mapuser", R"(, "requires": ["maplib/AddFive", "maplib/Thing.size"])")}}));
    CHECK(write_mod(home, "mapmissing",
                    {{"tome.mod.json", meta("mapmissing", R"(, "requires": ["maplib/Gone", "ghost/X", "a/b/c"])")}}));
    CHECK(write_mod(home, "mapbad", {{"tome.mod.json", meta("mapbad")},
                                     {"mappings/1.27b.json", R"({"schema": 2, "sha256": "00", "symbols": {}})"}}));
    CHECK(write_mod(home, "mapnamed",
                    {{"tome.mod.json", meta("mapnamed", R"(, "mappings": "maps", "requires": ["mapnamed/X"])")}}));
    // A library that can't load takes its mappings along, whatever the order.
    CHECK(write_mod(home, "mapchain", {{"tome.mod.json", meta("mapchain", R"(, "requires": ["ghost/Y"])")},
                                       {"mappings/1.27b.json", R"({"schema": 2, "symbols": {
                                           "Thing.size": {"kind": "offset", "value": "0x48"}}})"}}));
    CHECK(write_mod(home, "mapchainuser",
                    {{"tome.mod.json", meta("mapchainuser", R"(, "requires": ["mapchain/Thing.size"])")}}));
    // Bytes that aren't the game's (at a function of Tome's mapping, which
    // the fake game has): refused when read, not when hooked.
    const tome::BuildMappings* known = tome::find_build_by_name("1.27b");
    CHECK(known && known->data->find("RCString::GetText"));
    if (!known || !known->data->find("RCString::GetText")) return;
    char get_text_rva[16];
    std::snprintf(get_text_rva, sizeof(get_text_rva), "0x%08x",
                  static_cast<unsigned>(known->data->find("RCString::GetText")->value));
    CHECK(write_mod(home, "mapwrong", {{"tome.mod.json", meta("mapwrong")},
                                       {"mappings/1.27b.json", R"({"schema": 2, "symbols": {
                                           "GetText": {"kind": "function", "rva": ")" +
                                           std::string(get_text_rva) + R"j(", "conv": "thiscall",
                                                    "sig": "const char*(void* this)", "bytes": "90 90 90 90 90"}}})j"}}));
    Session session;
    CHECK(session.start(home));
    tome::frames::set_main_thread(GetCurrentThreadId());
    const tome_api* api = tome::api_table();

    // A mapping file moves other mods' hooks: it waits for approval like
    // code, and isn't published until then.
    CHECK_EQ(find_status(mod_manager().status(), "maplib")->state, std::string("approval"));
    CHECK(api->resolve("maplib/AddFive") == nullptr);
    {
        auto early = new tome_mod();  // lives for the test's process
        early->id = "mapuser";
        early->version = "1.0.0";
        CHECK_EQ(api->hook(early, "maplib/AddFive", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_ERR_UNAVAILABLE);
        CHECK_LOG("[mapuser] hook(maplib/AddFive): not mapped now: the mod maplib isn't allowed yet");
    }
    mod_manager().set_enabled("maplib", true);

    auto all = mod_manager().status();
    const tome::ModStatus* lib = find_status(all, "maplib");
    CHECK(lib && lib->errors.empty());
    const tome::ModStatus* user = find_status(all, "mapuser");
    CHECK(user && user->errors.empty());
    const tome::ModStatus* missing = find_status(all, "mapmissing");
    CHECK(any_error_contains(missing, "needs \"maplib/Gone\", which maplib's mappings don't have for 1.27b"));
    CHECK(any_error_contains(missing, "needs \"ghost/X\" from the mod ghost, which isn't installed"));
    CHECK(any_error_contains(missing, "\"requires\" entry \"a/b/c\": a mod's mapping is \"<mod id>/<name>\""));
    const tome::ModStatus* bad = find_status(all, "mapbad");
    CHECK(any_error_contains(bad, "mappings/1.27b.json: build: \"sha256\" isn't this build's"));
    const tome::ModStatus* named = find_status(all, "mapnamed");
    CHECK(any_error_contains(named, "needs \"mapnamed/X\", which mapnamed's mappings don't have for 1.27b"));
    CHECK(named && std::find(named->warnings.begin(), named->warnings.end(),
                             "\"mappings\" folder \"maps\" not found") != named->warnings.end());
    CHECK(any_error_contains(find_status(all, "mapchainuser"),
                             "needs \"mapchain/Thing.size\" from the mod mapchain, which has errors"));
    CHECK(any_error_contains(find_status(all, "mapwrong"), "mappings/1.27b.json: \"GetText\" at 0x"));
    CHECK(any_error_contains(find_status(all, "mapwrong"), "(a file for another build?)"));
    CHECK(api->resolve("mapwrong/GetText") == nullptr);

    const uintptr_t base = session.game.base();
    const tome_symbol* fn = api->resolve("maplib/AddFive");
    CHECK(fn != nullptr);
    if (!fn) return;
    CHECK_EQ(std::string(fn->name), std::string("maplib/AddFive"));
    CHECK_EQ(fn->kind, int32_t(TOME_SYMBOL_FUNCTION));
    CHECK_EQ(fn->rva, kAddFiveRva);
    CHECK_EQ(fn->value, base + kAddFiveRva);
    CHECK_EQ(std::string(fn->convention), std::string("cdecl"));
    const tome_symbol* size = api->resolve("maplib/Thing.size");
    CHECK(size && size->kind == TOME_SYMBOL_OFFSET && size->value == 0x48 && size->rva == 0);
    CHECK(api->resolve("maplib/Gone") == nullptr);
    CHECK(api->resolve("mapbad/AddFive") == nullptr);
    CHECK(api->resolve("mapchain/Thing.size") == nullptr);

    // The fake game wrote the function's bytes when the mapping was
    // published, so it runs; any mod can hook it by the library's name.
    auto add_five = reinterpret_cast<int (*)(int)>(fn->value);
    CHECK_EQ(add_five(1), 6);
    auto mod = new tome_mod();  // lives for the test's process
    mod->id = "mapuser";
    mod->version = "1.0.0";
    CHECK_EQ(api->hook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_OK);
    CHECK_LOG("Hooked maplib/AddFive at ");
    CHECK_EQ(add_five(1), 106);
    CHECK_EQ(api->unhook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, add_100, nullptr), TOME_OK);
    CHECK_EQ(add_five(1), 6);
    CHECK_EQ(api->hook(mod, "maplib/Gone", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_ERR_UNAVAILABLE);
    CHECK_LOG("[mapuser] hook(maplib/Gone): not in that mod's mapping file for this build");

    // A changed file: the new symbols, and the old pointer still reads.
    std::string changed = kLibMappings;
    changed.replace(changed.find("0x48"), 4, "0x50");
    CHECK(home.write(L"tome\\mods\\maplib\\mappings\\1.27b.json", changed));
    CHECK(home.touch(L"tome\\mods\\maplib\\mappings\\1.27b.json"));
    mod_manager().refresh();
    const tome_symbol* size2 = api->resolve("maplib/Thing.size");
    CHECK(size2 && size2->value == 0x50);
    CHECK(size->value == 0x48);
    // New bytes at the same place: the fake game writes them over its old ones.
    changed.replace(changed.find("83 c0 05"), 8, "83 c0 07");
    CHECK(home.write(L"tome\\mods\\maplib\\mappings\\1.27b.json", changed));
    CHECK(home.touch(L"tome\\mods\\maplib\\mappings\\1.27b.json"));
    mod_manager().refresh();
    CHECK(find_status(mod_manager().status(), "maplib")->errors.empty());
    CHECK_EQ(add_five(1), 8);

    // Moved to another address while hooked: the new hook goes there, the
    // old one stays, and unhook by the name reaches both.
    CHECK_EQ(api->hook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_OK);
    std::string moved = changed;
    moved.replace(moved.find("0x00590000"), 10, "0x00591000");
    CHECK(home.write(L"tome\\mods\\maplib\\mappings\\1.27b.json", moved));
    CHECK(home.touch(L"tome\\mods\\maplib\\mappings\\1.27b.json"));
    mod_manager().refresh();
    const tome_symbol* moved_fn = api->resolve("maplib/AddFive");
    CHECK(moved_fn && moved_fn->rva == 0x00591000);
    if (!moved_fn) return;
    auto add_five_moved = reinterpret_cast<int (*)(int)>(moved_fn->value);
    CHECK_EQ(api->hook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_OK);
    CHECK_EQ(add_five(1), 108);
    CHECK_EQ(add_five_moved(1), 108);
    CHECK_EQ(api->unhook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, add_100, nullptr), TOME_OK);
    CHECK_EQ(add_five(1), 8);
    CHECK_EQ(add_five_moved(1), 8);
    CHECK_EQ(api->unhook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, add_100, nullptr), TOME_ERR_INVALID_ARGUMENT);

    // Turned off: its file is withdrawn, and hooks by its names say why.
    mod_manager().set_enabled("maplib", false);
    CHECK(api->resolve("maplib/AddFive") == nullptr);
    CHECK_EQ(api->hook(mod, "maplib/AddFive", TOME_HOOK_BEFORE, 0, add_100, nullptr), TOME_ERR_UNAVAILABLE);
    CHECK_LOG("[mapuser] hook(maplib/AddFive): not mapped now: the mod maplib is off");
    mod_manager().set_enabled("maplib", true);
    CHECK(api->resolve("maplib/AddFive") != nullptr);

    // No file for this build: maplib's own "requires" fails, and with it its
    // symbols (and so mapuser).
    CHECK(home.remove(L"tome\\mods\\maplib\\mappings\\1.27b.json"));
    mod_manager().refresh();
    all = mod_manager().status();
    CHECK(any_error_contains(find_status(all, "maplib"), "needs \"maplib/AddFive\", which maplib's mappings don't have"));
    CHECK(any_error_contains(find_status(all, "mapuser"), "needs \"maplib/AddFive\""));
    CHECK(api->resolve("maplib/AddFive") == nullptr);
}

// A "sync": "required" mod's handshake hash covers the mapping files of the
// mods it names: players whose library files differ are told, even when the
// library itself isn't compared ("sync": "client").
TEST(host_sync_hash_covers_named_mappings) {
    Home home;
    CHECK(write_mod(home, "synclib", {{"tome.mod.json", meta("synclib")}, {"mappings/1.27b.json", kLibMappings}}));
    CHECK(write_mod(home, "syncuser",
                    {{"tome.mod.json", R"({"schema": 1, "id": "syncuser", "version": "1.0.0", "game": "*",
                        "tome": "*", "sync": "required", "requires": ["synclib/Thing.size"]})"}}));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("synclib", true);
    const tome::ModStatus* user = find_status(mod_manager().status(), "syncuser");
    CHECK(user && user->state == "active");
    const std::string before = user ? user->sync_hash : "";
    CHECK(!before.empty());

    std::string changed = kLibMappings;
    changed.replace(changed.find("0x48"), 4, "0x50");
    CHECK(home.write(L"tome\\mods\\synclib\\mappings\\1.27b.json", changed));
    CHECK(home.touch(L"tome\\mods\\synclib\\mappings\\1.27b.json"));
    mod_manager().set_enabled("synclib", true);  // the changed files, allowed again (a folder mod needn't be)
    user = find_status(mod_manager().status(), "syncuser");
    CHECK(user && !user->sync_hash.empty() && user->sync_hash != before);
}

namespace {

// A symbol of Tome's mapping as a mod's file would write it, under `name`.
std::string as_mod_symbol(const tome::mappings::Symbol& sym, const char* name, const char* sig = nullptr) {
    std::string bytes;
    for (int16_t b : sym.bytes) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), b < 0 ? "??" : "%02x", b);
        bytes += (bytes.empty() ? "" : " ") + std::string(hex);
    }
    char rva[16];
    std::snprintf(rva, sizeof(rva), "0x%08x", static_cast<unsigned>(sym.value));
    return std::string("\"") + name + R"(": {"kind": "function", "rva": ")" + rva + R"(", "conv": ")" +
           tome::mappings::to_string(sym.convention) + R"(", "sig": ")" + (sig ? sig : sym.signature.c_str()) +
           R"(", "bytes": ")" + bytes + "\"}";
}

} // namespace

// A mod's name for a function Tome maps too shares Tome's patch (each
// handler sees the name it hooked by); one for a function a built-in patch
// reserved is refused like Tome's name for it; one that passes the
// arguments another way is refused.
TEST(host_mods_own_mappings_of_tomes_functions) {
    Home home;
    Session session;
    // The fake game's build, to copy symbols from before the mod is installed.
    const tome::BuildMappings* known = tome::find_build_by_name("1.27b");
    CHECK(known != nullptr);
    if (!known) return;
    const tome::mappings::Build& data = *known->data;
    const tome::mappings::Symbol* get_text = data.find("RCString::GetText");
    const tome::mappings::Symbol* compile = data.find("Jass::compile");
    CHECK(get_text && compile);
    if (!get_text || !compile) return;
    const std::string file = "{\"schema\": 2, \"symbols\": {" + as_mod_symbol(*get_text, "GetText") + ", " +
                             as_mod_symbol(*compile, "Compile") + ", " +
                             as_mod_symbol(*get_text, "GetTextTwoArgs", "const char*(void* a, void* b)") + "}}";
    CHECK(write_mod(home, "aliases", {{"tome.mod.json", meta("aliases")}, {"mappings/1.27b.json", file}}));
    CHECK(session.start(home));
    mod_manager().set_enabled("aliases", true);
    tome::frames::set_main_thread(GetCurrentThreadId());
    const tome_api* api = tome::api_table();
    std::string report;
    tome::patches::install(session.game.base(), *session.game.build().known, report);

    auto mod = new tome_mod();  // lives for the test's process
    mod->id = "aliases";
    mod->version = "1.0.0";
    auto handler = [](tome_mod*, tome_hook_call*, void*) {};
    CHECK_EQ(api->hook(mod, "aliases/Compile", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_ERR_NOT_OWNER);
    CHECK_LOG("[aliases] hook(aliases/Compile): reserved by a built-in Tome patch");
    CHECK_EQ(api->unhook(mod, "aliases/Compile", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_NOT_OWNER);

    CHECK_EQ(api->hook(mod, "RCString::GetText", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_OK);
    CHECK_EQ(api->hook(mod, "aliases/GetText", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_OK);
    CHECK_LOG("Hooked aliases/GetText: the same function as RCString::GetText");
    CHECK(tome::hooks::find("aliases/GetText") == tome::hooks::find("RCString::GetText"));
    CHECK_EQ(api->hook(mod, "aliases/GetTextTwoArgs", TOME_HOOK_BEFORE, 0, handler, nullptr), TOME_ERR_UNAVAILABLE);
    CHECK_LOG("which this mapping says takes its arguments another way (thiscall \"const char*(void* a, void* b)\")");
    // Unhooking by one name leaves what was hooked by the other.
    CHECK_EQ(api->unhook(mod, "aliases/GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_OK);
    CHECK_EQ(api->unhook(mod, "aliases/GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_ERR_INVALID_ARGUMENT);
    CHECK_EQ(api->unhook(mod, "RCString::GetText", TOME_HOOK_BEFORE, handler, nullptr), TOME_OK);
}

TEST(host_64_bit_dll_rejected) {
    Home home;
    CHECK(home.install_mod("wrong-arch"));
    // Just the headers a 64-bit DLL starts with: Tome reads those before LoadLibrary.
    std::string dll(1024, '\0');
    dll[0] = 'M';
    dll[1] = 'Z';
    dll[0x3C] = static_cast<char>(0x80);
    std::memcpy(&dll[0x80], "PE\0\0", 4);
    dll[0x84] = static_cast<char>(0x64);  // IMAGE_FILE_MACHINE_AMD64 (0x8664)
    dll[0x85] = static_cast<char>(0x86);
    dll[0x96] = 0x22;  // executable image, large address aware
    dll[0x97] = 0x20;  // IMAGE_FILE_DLL
    CHECK(home.write(L"tome\\mods\\wrong-arch\\wrong-arch.dll", dll));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("wrong-arch", true);
    CHECK_LOG("wrong-arch: the DLL is 64-bit");
    CHECK(!find_status(mod_manager().status(), "wrong-arch")->running);
}

TEST(host_resolve_a_mapped_name) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_LOG("[hello] CTextFrame::SetText is a thiscall function at 0x");
    CHECK_LOG("hello: code allowed (turned on), sha256 ");
}

TEST(host_state_json_toggles_live) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_LOG("[hello] enabled (1 time");

    CHECK(edit_state(home, "hello", false));
    CHECK(mod_manager().poll_changed());  // what the folder watcher asks
    mod_manager().refresh();
    CHECK_LOG("[hello] disabled");
    CHECK(!find_status(mod_manager().status(), "hello")->running);

    CHECK(edit_state(home, "hello", true));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_LOG("[hello] enabled (2 times");
}

// Tome's own write of a state.json someone else wrote (the first refresh's)
// isn't an edit: no second refresh. Someone else's edit after it still is.
TEST(host_own_state_write_is_no_change) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.write(L"tome\\state.json", "{\"mods\": {}}"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);  // Tome writes state.json
    std::string state;
    CHECK(home.read(L"tome\\state.json", state));
    CHECK(state.find("\"hello\"") != std::string::npos);
    CHECK(!mod_manager().poll_changed());
    CHECK(!mod_manager().poll_changed());

    CHECK(edit_state(home, "hello", false));
    CHECK(mod_manager().poll_changed());
    CHECK(!mod_manager().poll_changed());
}

TEST(host_hot_reload_loads_the_new_dll) {
    Home home;
    CHECK(home.install_mod("hello"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    CHECK_EQ(count_logged("[hello] loaded (Tome"), 1);

    CHECK(home.touch(L"tome\\mods\\hello\\hello.dll"));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_LOG("Unloaded hello");
    CHECK_EQ(count_logged("[hello] loaded (Tome"), 2);
    CHECK_EQ(count_logged("[hello] enabled (1 time"), 2);  // the new DLL starts fresh
}

TEST(host_removed_mod_leaves_the_list) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("needs-ghost"));
    Session session;
    CHECK(session.start(home));
    CHECK_EQ(mod_manager().status().size(), size_t(2));
    CHECK(home.remove(L"tome\\mods\\needs-ghost"));
    CHECK(mod_manager().poll_changed());
    mod_manager().refresh();
    CHECK_EQ(mod_manager().status().size(), size_t(1));
    CHECK(find_status(mod_manager().status(), "needs-ghost") == nullptr);
}

TEST(host_packages) {
    Home home;
    CHECK(home.install_mod("hello"));
    std::string package;
    CHECK(tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/packages/hello.tome"), package));
    CHECK(home.write(L"tome\\mods\\hello-pkg.tome", package));
    Session session;
    CHECK(session.start(home));
    CHECK_LOG("Unpacked hello-pkg.tome");
    auto all = mod_manager().status();
    CHECK(any_error_contains(find_folder(all, "hello-pkg.tome"), "the folder hello has the same id and is used instead"));

    // Without the folder, the package's copy runs.
    CHECK(home.remove(L"tome\\mods\\hello"));
    mod_manager().refresh();
    mod_manager().set_enabled("hello", true);
    const tome::ModStatus* s = find_status(mod_manager().status(), "hello");
    CHECK(s && s->running && s->package);

    // Updated in place: a new version in the same file reloads from a new copy.
    std::string updated;
    CHECK(hello_package("1.0.1", updated));
    CHECK(home.write(L"tome\\mods\\hello-pkg.tome", updated));
    CHECK(home.touch(L"tome\\mods\\hello-pkg.tome"));
    mod_manager().refresh();
    CHECK_LOG("Loaded hello 1.0.1");
    CHECK_LOG("Removed the unused unpacked package hello-pkg.");

    // Removed: the mod stops and its unpacked copy goes too.
    CHECK(home.remove(L"tome\\mods\\hello-pkg.tome"));
    mod_manager().refresh();
    CHECK(find_status(mod_manager().status(), "hello") == nullptr);
    CHECK_EQ(count_logged("Unloaded hello"), 2);
}

TEST(host_shutdown_stops_every_mod) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("crash", true);
    mod_manager().shutdown();
    CHECK_LOG("[crash] disabled");
    CHECK_LOG("[crash] unloading");
    CHECK_LOG("Exiting: disabled and unloaded 2 mods");
}

// Safe mode asked for at startup (-tomesafe, Shift, tome/safe): nothing
// starts, and the mod menu can still start one mod for the session.
TEST(host_safe_mode_from_startup) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    session.safe_mode = "-tomesafe";
    CHECK(session.start(home));
    auto all = mod_manager().status();
    CHECK_EQ(find_status(all, "hello")->reason, std::string("safe mode"));
    CHECK(!find_status(all, "hello")->running);
    CHECK_NO_LOG("[hello] loaded");

    mod_manager().set_enabled("hello", true);
    all = mod_manager().status();
    CHECK(find_status(all, "hello")->running);
    CHECK(!find_status(all, "crash")->running);
}

// Safe mode turned on in the mod menu: every running mod stops.
TEST(host_safe_mode_turned_on_live) {
    Home home;
    CHECK(home.install_mod("hello"));
    CHECK(home.install_mod("crash"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello", true);
    mod_manager().set_enabled("crash", true);
    mod_manager().set_safe_mode_now(true);
    CHECK_LOG("Safe mode turned on in the mod menu");
    auto all = mod_manager().status();
    CHECK(!find_status(all, "hello")->running);
    CHECK(!find_status(all, "crash")->running);
    CHECK_LOG("[crash] disabled");

    mod_manager().set_safe_mode_now(false);
    all = mod_manager().status();
    CHECK(find_status(all, "hello")->running);
    CHECK(find_status(all, "crash")->running);
}

TEST(host_cpp_mod_natives) {
    Home home;
    CHECK(home.install_mod("hello-cpp"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-cpp", true);
    CHECK(find_status(mod_manager().status(), "hello-cpp")->running);
    session.game.load_map();
    auto log = reinterpret_cast<Native1>(session.game.native("HelloCppLog"));
    auto twice = reinterpret_cast<NativeReal>(session.game.native("HelloCppDouble"));
    CHECK(log && twice);
    if (!log || !twice) return;
    log(7);
    CHECK_LOG("HelloCppLog(7) called from JASS");
    float x = 1.25f;
    CHECK_EQ(bits_to_float(twice(&x)), 2.5f);  // real arguments are float*, results float bits in eax
}

TEST(host_rust_mod_natives) {
    std::string dll;
    if (!tome::read_file(host::win_path(TOME_TEST_BUILD_DIR "/mods/hello-rs/hello_rs.dll"), dll)) {
        std::printf("  skipped: the Rust example isn't built\n");
        return;
    }
    Home home;
    CHECK(home.install_mod("hello-rs"));
    Session session;
    CHECK(session.start(home));
    mod_manager().set_enabled("hello-rs", true);
    CHECK(find_status(mod_manager().status(), "hello-rs")->running);
    session.game.load_map();
    auto log = reinterpret_cast<Native1>(session.game.native("HelloRsLog"));
    auto twice = reinterpret_cast<NativeReal>(session.game.native("HelloRsDouble"));
    CHECK(log && twice);
    if (!log || !twice) return;
    log(9);
    CHECK_LOG("HelloRsLog(9)");
    float x = -3.0f;
    CHECK_EQ(bits_to_float(twice(&x)), -6.0f);
}
