// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace tome::crash_report {
namespace {

std::string hex(uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(value));
    return buf;
}

bool ends_with(std::string_view s, std::string_view end) {
    return s.size() >= end.size() && s.substr(s.size() - end.size()) == end;
}

// "crash-YYYYMMDD-HHMMSS" exactly.
bool is_stem(std::string_view s) {
    if (s.size() != 21 || s.substr(0, 6) != "crash-" || s[14] != '-') return false;
    for (size_t i = 6; i < s.size(); ++i) {
        if (i != 14 && (s[i] < '0' || s[i] > '9')) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

// --- crash loops ---------------------------------------------------------------

Starts parse_starts(std::string_view file) {
    Starts starts;
    size_t end = file.find('\n');
    std::string_view first = trim(file.substr(0, end));
    if (first.empty() || first.size() > 6) return starts;
    int count = 0;
    for (char c : first) {
        if (c < '0' || c > '9') return starts;
        count = count * 10 + (c - '0');
    }
    starts.count = count;
    if (end != std::string_view::npos) {
        std::string_view second = trim(file.substr(end + 1));
        second = second.substr(0, second.find('\n'));
        second = trim(second);
        if (is_stem(second)) starts.last_report = std::string(second);
    }
    return starts;
}

std::string format_starts(const Starts& starts) {
    return std::to_string(std::max(0, starts.count)) + "\n" + starts.last_report + "\n";
}

std::string safe_mode_reason(int starts) {
    return "the last " + std::to_string(starts) + " starts didn't reach the main menu";
}

popup::Spec crash_loop_prompt(int starts, std::string_view last_crash) {
    popup::Spec spec;
    spec.title = "Safe Mode";
    spec.text = "Warcraft III closed before the main menu the last " + std::to_string(starts) +
                " times it started, so Tome started it without mods.";
    if (!last_crash.empty()) {
        spec.text += "\n\nLast crash: " + std::string(last_crash) + " (report in tome\\crashes)";
    }
    spec.text += "\n\nYou can turn mods on one at a time in the Mods menu. Keep safe mode for the next "
                 "starts too, until you turn it off there?";
    spec.options = {"Keep Safe Mode", "This Start Only"};
    spec.default_option = kThisStart;
    spec.cancel_option = kThisStart;
    return spec;
}

// --- reports -------------------------------------------------------------------

const char* exception_name(uint32_t code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000096: return "privileged instruction";
    case 0xC0000094: return "integer division by zero";
    case 0xC0000095: return "integer overflow";
    case 0xC00000FD: return "stack overflow";
    case 0x80000003: return "breakpoint";
    case 0x80000002: return "misaligned data";
    case 0xC000008C: return "array bounds exceeded";
    case 0xC0000006: return "in-page error";
    case 0xC000008E: return "floating-point division by zero";
    case 0xC0000090: return "invalid floating-point operation";
    case 0xC0000091: return "floating-point overflow";
    case 0xC0000093: return "floating-point underflow";
    case 0xC0000092: return "floating-point stack check";
    case 0xC0000409: return "stack buffer overrun";
    case 0xC0000374: return "heap corruption";
    case 0xE06D7363: return "uncaught C++ exception";
    default: return nullptr;
    }
}

std::string describe_exception(uint32_t code, const uint32_t* info, uint32_t info_count) {
    if (code == 0xC0000005 && info && info_count >= 2) {
        const char* access = info[0] == 1 ? "writing" : info[0] == 8 ? "executing" : "reading";
        return std::string("access violation ") + access + " " + hex(info[1]);
    }
    if (const char* name = exception_name(code)) return name;
    return "exception " + hex(code);
}

std::string where(const Location& at) {
    if (at.module.empty()) return hex(at.address);
    char buf[16];
    snprintf(buf, sizeof(buf), "+0x%lx", static_cast<unsigned long>(at.offset));
    std::string text = at.module + buf;
    if (!at.mod.empty()) text += " (mod " + at.mod + ")";
    return text;
}

std::string summary(const Report& report) {
    return describe_exception(report.code, report.info.data(), static_cast<uint32_t>(report.info.size())) +
           " at " + where(report.at);
}

std::string format(const Report& r) {
    std::string out = "Summary: " + summary(r) + "\n";
    out += "Time: " + r.time + "\n";
    out += "Tome: " + r.tome_version + "\n";
    out += "Game: " + r.game + "\n";
    out += "Exception: " + hex(r.code);
    if (const char* name = exception_name(r.code)) out += std::string(" (") + name + ")";
    out += "\n";
    if (!r.info.empty()) {
        out += "Information:";
        for (uint32_t value : r.info) out += " " + hex(value);
        out += "\n";
    }
    out += "Address: " + hex(r.at.address) + " = " + where(r.at) + "\n";
    out += "Thread: " + std::to_string(r.thread) + (r.main_thread ? " (the game's main thread)" : "") + "\n";

    static const char* const names[10] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip", "efl"};
    out += "Registers:";
    for (int i = 0; i < 10; ++i) {
        out += (i == 5 ? "\n          " : " ") + std::string(names[i]) + "=" + hex(r.registers[i]).substr(2);
    }
    out += "\n";

    out += "\nCalled from (likely, innermost first):\n";
    if (r.callers.empty()) out += "  (nothing found on the stack)\n";
    for (const Location& at : r.callers) out += "  " + where(at) + "\n";

    out += "\nMods:\n";
    if (!r.mods_known) {
        out += "  (unknown)\n";
    } else if (r.mods.empty()) {
        out += "  (none installed)\n";
    }
    for (const ModLine& m : r.mods) {
        std::string line = "  " + m.id;
        line.resize(std::max<size_t>(line.size() + 1, 28), ' ');
        line += m.version;
        line.resize(std::max<size_t>(line.size() + 1, 40), ' ');
        out += line + m.state + "\n";
    }

    out += "\nMinidump: " + (r.dump.empty() ? "not written" + (r.dump_error.empty() ? "" : " (" + r.dump_error + ")")
                                            : r.dump) + "\n";
    out += "\nThe end of tome.log:\n" + r.log_tail;
    if (!r.log_tail.empty() && r.log_tail.back() != '\n') out += "\n";
    return out;
}

std::string summary_of(std::string_view text) {
    constexpr std::string_view kPrefix = "Summary: ";
    if (text.substr(0, kPrefix.size()) != kPrefix) return "";
    text.remove_prefix(kPrefix.size());
    return std::string(trim(text.substr(0, text.find('\n'))));
}

std::string tail(std::string_view log, size_t lines) {
    if (lines == 0) return "";
    size_t end = log.size();
    if (end > 0 && log[end - 1] == '\n') --end;  // the last line's own newline
    size_t start = end;
    for (size_t found = 0; start > 0;) {
        if (log[start - 1] == '\n' && ++found == lines) break;
        --start;
    }
    return std::string(log.substr(start));
}

std::string file_stem(int year, int month, int day, int hour, int minute, int second) {
    char buf[32];
    snprintf(buf, sizeof(buf), "crash-%04d%02d%02d-%02d%02d%02d", year, month, day, hour, minute, second);
    return buf;
}

std::vector<std::string> to_remove(std::vector<std::string> names, size_t keep) {
    std::set<std::string> stems;
    for (const std::string& name : names) {
        if (ends_with(name, ".txt") || ends_with(name, ".dmp")) {
            std::string stem = name.substr(0, name.size() - 4);
            if (is_stem(stem)) stems.insert(stem);
        }
    }
    std::set<std::string> old;
    size_t extra = stems.size() > keep ? stems.size() - keep : 0;
    for (auto it = stems.begin(); extra > 0; ++it, --extra) old.insert(*it);  // oldest first
    std::vector<std::string> out;
    for (const std::string& name : names) {
        if (name.size() > 4 && old.count(name.substr(0, name.size() - 4)) &&
            (ends_with(name, ".txt") || ends_with(name, ".dmp"))) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace tome::crash_report
