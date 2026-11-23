// SPDX-License-Identifier: GPL-3.0-or-later
#include "wasm_hooks.h"

#include <algorithm>
#include <cctype>

namespace tome::wasm {
namespace {

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

// "const char* text" -> "const char*"; "int" and "unsigned int" stay as they are.
std::string without_name(const std::string& param) {
    size_t end = param.size();
    size_t start = end;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(param[start - 1])) || param[start - 1] == '_')) --start;
    const std::string word = param.substr(start, end - start);
    const std::string rest = trim(std::string_view(param).substr(0, start));
    if (word.empty() || rest.empty()) return param;
    // A type's own last word ("unsigned int"), not a name.
    if (word == "int" || word == "long" || word == "char" || word == "short") return param;
    return rest;
}

// Collapses runs of spaces and puts "*" against the type: "CUnit *" -> "CUnit*".
std::string normal(const std::string& type) {
    std::string out;
    for (char c : type) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ' && out.back() != '*') out += ' ';
        } else {
            if (c == '*' && !out.empty() && out.back() == ' ') out.pop_back();
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    // tools/gen-game-header.py drops a leading "const" the same way, for numbers too.
    if (out.rfind("const ", 0) == 0) out.erase(0, 6);
    return out;
}

// Functions a module never hooks (see wasm_hooks.h).
bool denied(std::string_view name) {
    for (std::string_view prefix : {"Native.", "__", "CrashHandler::"}) {
        if (name.substr(0, prefix.size()) == prefix) return true;
    }
    return name == "GameShutdown" || name == "InitNatives";
}

// Functions whose int arguments are plain values (flags, stat values): a
// BEFORE handler may change them. tools/gen-game-header.py has the same list.
bool writable_ints(std::string_view name) {
    for (std::string_view n : {"CUnit::GetHeroAgi", "CUnit::GetHeroInt", "CUnit::GetHeroStr", "CUnit::SetHeroAgi",
                               "CUnit::SetHeroInt", "CUnit::SetHeroStr", "CUnit::SetHealthBarShown",
                               "HealthBars::Consider"}) {
        if (name == n) return true;
    }
    return false;
}

bool is_int(const std::string& t) {
    return t == "int" || t == "int32_t" || t == "uint32_t" || t == "unsigned" || t == "unsigned int";
}

bool argument(const std::string& raw, HookArg& out, std::string& why) {
    const std::string shown = trim(without_name(raw));
    const std::string t = normal(shown);
    if (is_int(t)) out = HookArg::Int;
    else if (t == "float") out = HookArg::Float;
    else if (t == "CUnit*") out = HookArg::Unit;
    else if (t == "CItem*") out = HookArg::Item;
    else if (t == "CPlayerWar3*") out = HookArg::Player;
    else {
        why = "the argument type \"" + shown + "\" isn't one a module can take (numbers, CUnit*, CItem*, CPlayerWar3*)";
        return false;
    }
    return true;
}

} // namespace

bool hook_point(std::string_view name, std::string_view signature, HookPoint& out, std::string& why) {
    out = HookPoint();
    if (denied(name)) {
        why = "the game's own scripting, start-up, shutdown and crash handling can't be hooked by a module";
        return false;
    }
    const size_t open = signature.find('(');
    const size_t close = signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        why = "the signature isn't of the form \"result(params)\"";
        return false;
    }
    const std::string result = normal(trim(signature.substr(0, open)));
    if (result == "void") out.result = HookResult::None;
    else if (is_int(result)) out.result = HookResult::Int;
    else if (result == "float") out.result = HookResult::Float;
    else {
        why = "the result type \"" + result + "\" isn't one a module can return (void, an integer, float)";
        return false;
    }
    const std::string body = trim(signature.substr(open + 1, close - open - 1));
    if (body.empty() || body == "void") return true;
    size_t at = 0;
    while (at <= body.size()) {
        size_t comma = body.find(',', at);
        if (comma == std::string::npos) comma = body.size();
        const std::string p = trim(std::string_view(body).substr(at, comma - at));
        if (p == "...") {
            why = "variadic functions can't be hooked by a module";
            return false;
        }
        HookArg arg{};
        if (!argument(p, arg, why)) return false;
        if (arg == HookArg::Int && !writable_ints(name)) arg = HookArg::Index;
        out.args.push_back(arg);
        if (out.args.size() > kMaxHookArgs) {
            why = "more than " + std::to_string(kMaxHookArgs) + " arguments";
            return false;
        }
        at = comma + 1;
    }
    return true;
}

} // namespace tome::wasm
