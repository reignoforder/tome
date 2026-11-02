// SPDX-License-Identifier: GPL-3.0-or-later
#include "hooks.h"

#include <algorithm>

namespace tome::hooks {
namespace {

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

bool is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::vector<std::string> words(const std::string& type) {
    std::vector<std::string> out;
    std::string word;
    for (char c : type + ' ') {
        if (is_ident(c)) {
            word += c;
        } else if (!word.empty()) {
            out.push_back(word);
            word.clear();
        }
    }
    return out;
}

enum class Type { Void, Int32, Float, Double, Wide };

// Classifies one parameter or return type ("const char* text", "float x",
// "int"). Pointers are 32-bit whatever they point to; a named type that
// isn't a known wide one is taken as 32-bit (ints, enums, handles).
Type classify(const std::string& text) {
    if (text.find('*') != std::string::npos || text.find('&') != std::string::npos) return Type::Int32;
    std::vector<std::string> w = words(text);
    int longs = 0;
    for (const std::string& word : w) {
        if (word == "double") return Type::Double;  // also long double: 64-bit in MSVC
        if (word == "int64_t" || word == "uint64_t" || word == "__int64") return Type::Wide;
        if (word == "long") ++longs;
    }
    if (longs >= 2) return Type::Wide;
    for (const std::string& word : w) {
        if (word == "float") return Type::Float;
    }
    if (w.size() == 1 && w[0] == "void") return Type::Void;
    return Type::Int32;
}

// Splits "a, b(c, d), e" at top-level commas.
std::vector<std::string> split_params(const std::string& list) {
    std::vector<std::string> out;
    int depth = 0;
    std::string current;
    for (char c : list) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }
    out.push_back(trim(current));
    return out;
}

bool run_order(const Handler& a, const Handler& b) {
    if (a.phase != b.phase) return a.phase < b.phase;
    if (a.last != b.last) return b.last;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.seq < b.seq;
}

// Gates of the handlers running on this thread, innermost last.
thread_local std::vector<Gate*> t_running;

// The chains running on this thread, innermost last: call_next only
// accepts a tome_hook_call* that is one of theirs.
thread_local std::vector<Dispatch*> t_dispatches;

// Calls one handler unless it was revoked. busy goes up before revoked is
// read (both sequentially consistent), so whoever revokes and then waits
// for busy to drop either sees this call or this call sees the revocation.
bool call_handler(const Handler& h, Dispatch& d) {
    Gate* gate = h.gate.get();
    if (gate) {
        gate->busy.fetch_add(1);
        if (gate->revoked.load()) {
            gate->busy.fetch_sub(1);
            return false;
        }
        t_running.push_back(gate);
    }
    bool finished = true;
    if (d.invoke) {
        finished = d.invoke(h, d.call);
    } else {
        h.fn(h.mod, &d.call, h.user);
    }
    if (gate) {
        t_running.pop_back();
        gate->busy.fetch_sub(1);
    }
    return finished;
}

void call_next_in(Dispatch& d) {
    ++d.handed_on;
    for (size_t i = d.next; i < d.count; ++i) {
        const Handler& h = d.handlers[i];
        if (h.phase != TOME_HOOK_REPLACE) continue;
        const size_t saved = d.next;
        const size_t handed_on = d.handed_on;
        d.next = i + 1;
        const bool finished = call_handler(h, d);
        d.next = saved;
        // Done unless it was skipped, or crashed before calling next.
        if (finished || d.handed_on != handed_on) return;
    }
    d.original(d.context, d.call);
}

} // namespace

int running_on_this_thread(const Gate& gate) {
    return static_cast<int>(std::count(t_running.begin(), t_running.end(), &gate));
}

bool layout_for(mappings::Convention convention, const std::string& signature, Layout& out, std::string& error) {
    out = Layout();
    if (convention == mappings::Convention::None) {
        error = "no calling convention";
        return false;
    }
    const size_t open = signature.find('(');
    const size_t close = signature.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        !trim(signature.substr(close + 1)).empty()) {
        error = "signature \"" + signature + "\" isn't of the form \"result(params)\"";
        return false;
    }

    const std::string result = trim(signature.substr(0, open));
    if (result.empty()) {
        error = "signature \"" + signature + "\" has no result type";
        return false;
    }
    switch (classify(result)) {
    case Type::Void: out.result = Result::None; break;
    case Type::Int32: case Type::Wide: out.result = Result::Int; break;
    case Type::Float: out.result = Result::Float; break;
    case Type::Double: out.result = Result::Double; break;
    }

    std::vector<std::string> params = split_params(signature.substr(open + 1, close - open - 1));
    if (params.size() == 1 && (params[0].empty() || classify(params[0]) == Type::Void)) params.clear();
    if (!params.empty() && params.back() == "...") {
        out.variadic = true;
        params.pop_back();
    }

    // Variadic functions are cdecl whatever they're declared as (MSVC turns
    // a variadic thiscall into cdecl with `this` pushed first).
    const mappings::Convention conv = out.variadic ? mappings::Convention::Cdecl : convention;
    if (conv == mappings::Convention::Thiscall && params.empty()) {
        error = "a thiscall function needs `this` as its first parameter";
        return false;
    }
    const int registers = conv == mappings::Convention::Fastcall ? 2 : conv == mappings::Convention::Thiscall ? 1 : 0;
    int registers_used = 0;
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& p = params[i];
        if (p.find("...") != std::string::npos) {
            error = "\"...\" must be the last parameter";
            return false;
        }
        Param param;
        switch (classify(p)) {
        case Type::Void:
            error = "parameter " + std::to_string(i + 1) + " is void";
            return false;
        case Type::Wide: case Type::Double: param.words = 2; break;
        case Type::Float: param.is_float = true; break;
        case Type::Int32: break;
        }
        if (out.arg_words + param.words > kMaxArgWords) {
            error = "more than " + std::to_string(kMaxArgWords) + " words of arguments";
            return false;
        }
        param.index = static_cast<uint8_t>(out.arg_words);
        out.arg_words += param.words;
        // fastcall puts the first two 32-bit non-float arguments in ecx and
        // edx (64-bit ones go on the stack without using a register up);
        // thiscall puts `this` in ecx.
        const bool fits_register = param.words == 1 && !param.is_float;
        if (registers_used < registers && fits_register && (conv != mappings::Convention::Thiscall || i == 0)) {
            param.where = registers_used == 0 ? Where::Ecx : Where::Edx;
            ++registers_used;
        } else {
            param.where = Where::Stack;
            param.slot = static_cast<uint8_t>(out.stack_slots);
            out.stack_slots += param.words;
        }
        out.params.push_back(param);
    }
    out.pop_bytes = conv == mappings::Convention::Cdecl ? 0 : out.stack_slots * 4;
    return true;
}

void insert(std::vector<Handler>& chain, const Handler& h) {
    chain.insert(std::upper_bound(chain.begin(), chain.end(), h, run_order), h);
}

size_t remove(std::vector<Handler>& chain, tome_mod* mod, int32_t phase, tome_hook_fn fn, void* user) {
    const size_t before = chain.size();
    std::erase_if(chain, [&](const Handler& h) {
        return h.mod == mod && h.phase == phase && h.fn == fn && h.user == user;
    });
    return before - chain.size();
}

size_t remove_all(std::vector<Handler>& chain, tome_mod* mod) {
    return std::erase_if(chain, [&](const Handler& h) { return h.mod == mod; });
}

void run(Dispatch& d) {
    t_dispatches.push_back(&d);
    for (size_t i = 0; i < d.count; ++i) {
        if (d.handlers[i].phase == TOME_HOOK_BEFORE) call_handler(d.handlers[i], d);
    }
    if (!d.call.skip_original) {
        d.in_replace = true;
        d.next = 0;
        call_next_in(d);
        d.in_replace = false;
    }
    for (size_t i = 0; i < d.count; ++i) {
        if (d.handlers[i].phase == TOME_HOOK_AFTER) call_handler(d.handlers[i], d);
    }
    t_dispatches.pop_back();
}

ThreadState thread_state() {
    return {t_running.size(), t_dispatches.size()};
}

void restore_thread_state(const ThreadState& state) {
    while (t_running.size() > state.running) {
        t_running.back()->busy.fetch_sub(1);
        t_running.pop_back();
    }
    if (t_dispatches.size() > state.dispatches) t_dispatches.resize(state.dispatches);
}

void call_next(tome_hook_call* call) {
    if (!call) return;
    for (auto it = t_dispatches.rbegin(); it != t_dispatches.rend(); ++it) {
        if (&(*it)->call == call) {
            if ((*it)->in_replace) call_next_in(**it);
            return;
        }
    }
}

} // namespace tome::hooks
