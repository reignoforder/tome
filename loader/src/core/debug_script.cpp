#include "debug_script.h"

#include <cstdio>
#include <cstdlib>

namespace tome::debug_script {
namespace {

// "@<ms>" and nothing else.
bool parse_time(std::string_view s, uint32_t& out) {
    if (s.size() < 2 || s.size() > 10 || s[0] != '@') return false;
    uint32_t value = 0;
    for (char c : s.substr(1)) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    out = value;
    return true;
}

bool parse_fraction(std::string_view s, double& out) {
    std::string text(s);
    if (text.empty() || text.find_first_not_of("0123456789.") != std::string::npos) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

bool parse_step(std::string_view item, Step& step) {
    size_t at = item.rfind('@');
    if (at == std::string_view::npos || !parse_time(item.substr(at), step.at_ms)) return false;
    std::string_view head = item.substr(0, at);

    struct Named {
        const char* name;
        Kind kind;
    };
    for (Named n : {Named{"close", Kind::Close}, Named{"minimize", Kind::Minimize}, Named{"restore", Kind::Restore}}) {
        if (head == n.name) {
            step.kind = n.kind;
            return true;
        }
    }
    for (Named n : {Named{"type:", Kind::Type}, Named{"text:", Kind::Text}, Named{"key:", Kind::Key},
                    Named{"wait:", Kind::Wait}}) {
        std::string_view prefix = n.name;
        if (head.substr(0, prefix.size()) == prefix) {
            step.kind = n.kind;
            step.text = std::string(head.substr(prefix.size()));
            return (step.kind != Kind::Key && step.kind != Kind::Wait) || !step.text.empty();
        }
    }
    step.kind = Kind::Click;
    std::string_view move = "move:";
    if (head.substr(0, move.size()) == move) {
        step.kind = Kind::Move;
        head = head.substr(move.size());
    }
    size_t comma = head.find(',');
    if (comma == std::string_view::npos) return false;
    return parse_fraction(head.substr(0, comma), step.x) && parse_fraction(head.substr(comma + 1), step.y);
}

} // namespace

std::vector<Step> parse(std::string_view spec, std::vector<std::string>& bad) {
    std::vector<Step> steps;
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find(';', pos);
        if (end == std::string_view::npos) end = spec.size();
        std::string_view item = spec.substr(pos, end - pos);
        Step step;
        if (parse_step(item, step)) {
            steps.push_back(std::move(step));
        } else if (!item.empty()) {
            bad.emplace_back(item);
        }
        pos = end + 1;
    }
    return steps;
}

} // namespace tome::debug_script
