// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_mappings.h"

#include "core/verify.h"
#include "patch.h"
#include "unwind.h"

#include <windows.h>

#include <map>

namespace tome::mod_mappings {
namespace {

struct Table {
    std::string id;
    std::shared_ptr<const mappings::Build> build;
    std::vector<std::string> names;  // "<id>/<name>", for api[i].name
    std::vector<tome_symbol> api;    // in build->symbols' order
};

SRWLOCK g_lock = SRWLOCK_INIT;
uintptr_t g_base = 0;
std::vector<std::unique_ptr<Table>> g_tables;          // every table published: never freed
std::map<std::string, const Table*, std::less<>> g_current;  // by mod id
std::map<std::string, std::string, std::less<>> g_withheld;  // by mod id: why its file isn't published
std::function<std::vector<std::string>(const mappings::Build&)> g_verifier;

const Table* current(std::string_view name, std::string_view& local) {
    std::string_view id;
    if (!mappings::split_mod_symbol(name, id, local)) return nullptr;
    auto it = g_current.find(id);
    return it != g_current.end() ? it->second : nullptr;
}

} // namespace

tome_symbol to_api_symbol(const mappings::Symbol& sym, const char* name, uintptr_t base) {
    tome_symbol out = {};
    out.size = sizeof(out);
    out.name = name;
    switch (sym.kind) {
    case mappings::Kind::Function: out.kind = TOME_SYMBOL_FUNCTION; break;
    case mappings::Kind::Data:     out.kind = TOME_SYMBOL_DATA; break;
    case mappings::Kind::Call:     out.kind = TOME_SYMBOL_CALL; break;
    case mappings::Kind::Offset:   out.kind = TOME_SYMBOL_OFFSET; break;
    case mappings::Kind::Constant: out.kind = TOME_SYMBOL_CONSTANT; break;
    case mappings::Kind::Import:   out.kind = TOME_SYMBOL_IMPORT; break;
    }
    out.rva = mappings::has_rva(sym.kind) ? sym.value : 0;
    out.value = mappings::has_rva(sym.kind) ? base + sym.value : sym.value;
    out.convention = mappings::to_string(sym.convention);
    out.signature = sym.signature.c_str();
    return out;
}

void init(uintptr_t game_base) {
    ExclusiveLock hold(g_lock);
    g_base = game_base;
}

void publish(const std::string& id, std::shared_ptr<const mappings::Build> build, std::string withheld) {
    ExclusiveLock hold(g_lock);
    auto it = g_current.find(id);
    if (!build) {
        if (it != g_current.end()) g_current.erase(it);
        if (withheld.empty()) {
            g_withheld.erase(id);
        } else {
            g_withheld[id] = std::move(withheld);
        }
        return;
    }
    g_withheld.erase(id);
    if (it != g_current.end() && it->second->build == build) return;
    // Published before (the mod was invalid for a while, or two folders
    // share the id): that table again, not another one that's never freed.
    for (const auto& table : g_tables) {
        if (table->id == id && table->build == build) {
            g_current[id] = table.get();
            return;
        }
    }
    auto table = std::make_unique<Table>();
    table->id = id;
    table->build = build;
    table->names.reserve(build->symbols.size());
    for (const mappings::Symbol& sym : build->symbols) table->names.push_back(id + "/" + sym.name);
    table->api.reserve(build->symbols.size());
    for (size_t i = 0; i < build->symbols.size(); ++i) {
        table->api.push_back(to_api_symbol(build->symbols[i], table->names[i].c_str(), g_base));
    }
    g_current[id] = table.get();
    g_tables.push_back(std::move(table));
}

std::string missing_reason(std::string_view name) {
    SharedLock hold(g_lock);
    std::string_view id, local;
    if (!mappings::split_mod_symbol(name, id, local)) return {};
    auto it = g_withheld.find(id);
    return it != g_withheld.end() ? it->second : std::string();
}

bool ready() {
    SharedLock hold(g_lock);
    return g_base != 0 || g_verifier;
}

std::vector<std::string> verify(const mappings::Build& build) {
    std::function<std::vector<std::string>(const mappings::Build&)> verifier;
    {
        SharedLock hold(g_lock);
        verifier = g_verifier;
    }
    return verifier ? verifier(build) : check_code(build);
}

std::vector<std::string> check_code(const mappings::Build& build) {
    uintptr_t base = 0;
    {
        SharedLock hold(g_lock);
        base = g_base;
    }
    if (!base) return {};
    std::map<uint32_t, uint8_t> originals;
    for (const auto& [address, byte] : original_bytes()) {
        if (address >= base && address - base < build.size_of_image) originals[static_cast<uint32_t>(address - base)] = byte;
    }
    return mappings::verify_code(build, reinterpret_cast<const uint8_t*>(base), build.size_of_image, &originals);
}

std::vector<std::string> ids() {
    SharedLock hold(g_lock);
    std::vector<std::string> out;
    for (const auto& [id, table] : g_current) out.push_back(id);
    return out;
}

const mappings::Symbol* find(std::string_view name) {
    SharedLock hold(g_lock);
    std::string_view local;
    const Table* table = current(name, local);
    return table ? table->build->find(local) : nullptr;
}

const tome_symbol* resolve(std::string_view name) {
    SharedLock hold(g_lock);
    std::string_view local;
    const Table* table = current(name, local);
    if (!table) return nullptr;
    for (size_t i = 0; i < table->build->symbols.size(); ++i) {
        if (table->build->symbols[i].name == local) return &table->api[i];
    }
    return nullptr;
}

void set_verifier(std::function<std::vector<std::string>(const mappings::Build&)> fn) {
    ExclusiveLock hold(g_lock);
    g_verifier = std::move(fn);
}

} // namespace tome::mod_mappings
