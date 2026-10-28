#include "jass.h"

#include <unordered_map>

namespace tome::jass {
namespace {

std::vector<std::string_view> tokenize(std::string_view text) {
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else if (c == ',') {
            tokens.push_back(text.substr(i, 1));
            ++i;
        } else {
            size_t start = i;
            while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' &&
                   text[i] != '\n' && text[i] != ',') {
                ++i;
            }
            tokens.push_back(text.substr(start, i - start));
        }
    }
    return tokens;
}

bool is_keyword(std::string_view s) {
    static const char* const kKeywords[] = {
        "native", "constant", "takes", "returns", "nothing", "function", "endfunction",
        "globals", "endglobals", "type", "extends", "local", "set", "call", "if", "then",
        "else", "elseif", "endif", "loop", "endloop", "exitwhen", "return", "array",
        "and", "or", "not", "true", "false", "null", "debug",
    };
    for (const char* k : kKeywords) {
        if (s == k) return true;
    }
    return false;
}

char type_code(const std::string& type) {
    if (type == "integer") return 'I';
    if (type == "real") return 'R';
    if (type == "boolean") return 'B';
    if (type == "string") return 'S';
    if (type == "code") return 'C';
    if (type == "nothing") return 'V';
    return 'H';
}

// The leading identifier words of a declaration line: everything before a
// `//` comment or the first `=`, split on whitespace and commas, keeping only
// identifier-shaped words. For `constant unit G = x` this is {constant,unit,G}.
void leading_words(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    size_t end = line.size();
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == '/' && line[i + 1] == '/') { end = i; break; }
    }
    for (size_t i = 0; i < end; ++i) {
        if (line[i] == '=') { end = i; break; }
    }
    size_t i = 0;
    auto word_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    while (i < end) {
        if (!word_char(line[i])) { ++i; continue; }
        size_t start = i;
        while (i < end && word_char(line[i])) ++i;
        out.push_back(line.substr(start, i - start));
    }
}

// name -> type; generated maps declare thousands of globals (gg_*, udg_*).
using GlobalTypes = std::unordered_map<std::string_view, std::string_view>;

// Collects one script's globals into `globals` and, with `findings` set,
// reports its locals that shadow one with a different type.
void scan_script(std::string_view script, GlobalTypes& globals, std::vector<ShadowFinding>* findings) {
    std::vector<std::string_view> w;  // reused for every line
    bool in_globals = false;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t nl = script.find('\n', pos);
        std::string_view line =
            script.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? script.size() + 1 : nl + 1;
        ++line_no;

        leading_words(line, w);
        if (w.empty()) continue;

        if (w[0] == "globals") { in_globals = true; continue; }
        if (w[0] == "endglobals") { in_globals = false; continue; }

        if (in_globals) {
            // [constant] <type> [array] <name>
            size_t t = 0;
            if (w[t] == "constant") ++t;
            if (t >= w.size()) continue;
            std::string_view type = w[t++];
            if (t < w.size() && w[t] == "array") ++t;
            if (t >= w.size()) continue;
            std::string_view name = w[t];
            if (is_valid_identifier(name) && !is_keyword(name)) {
                globals.emplace(name, type);  // keeps the first declaration
            }
            continue;
        }

        if (findings && w[0] == "local") {
            // local <type> [array] <name>
            size_t t = 1;
            if (t >= w.size()) continue;
            std::string_view type = w[t++];
            if (t < w.size() && w[t] == "array") ++t;
            if (t >= w.size()) continue;
            std::string_view name = w[t];
            auto it = globals.find(name);
            if (it != globals.end() && it->second != type) {
                findings->push_back({std::string(name), std::string(it->second), std::string(type), line_no});
            }
        }
    }
}

} // namespace

bool scan_shadow_exploit(std::string_view script, std::vector<ShadowFinding>& findings,
                         const std::vector<std::string_view>& prelude) {
    GlobalTypes globals;
    for (std::string_view p : prelude) scan_script(p, globals, nullptr);
    scan_script(script, globals, &findings);
    return !findings.empty();
}

bool is_valid_identifier(std::string_view name) {
    if (name.empty()) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!alpha(name[0])) return false;
    for (char c : name) {
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '_') return false;
    }
    return name.back() != '_';
}

std::string NativeDecl::signature() const {
    std::string sig = "(";
    for (const std::string& type : param_types) {
        char code = type_code(type);
        if (code == 'H') {
            sig += 'H';
            sig += type;
            sig += ';';
        } else {
            sig += code;
        }
    }
    sig += ')';
    char ret = type_code(return_type);
    if (ret == 'H') {
        sig += 'H';
        sig += return_type;
        sig += ';';
    } else {
        sig += ret;
    }
    return sig;
}

uint32_t NativeDecl::code_mask() const {
    uint32_t mask = 0;
    for (size_t i = 0; i < param_types.size() && i < 32; ++i) {
        if (type_code(param_types[i]) == 'C') mask |= 1u << i;
    }
    return mask;
}

bool parse_native(std::string_view text, NativeDecl& out, std::string& error) {
    auto tokens = tokenize(text);
    size_t i = 0;
    auto peek = [&]() -> std::string_view { return i < tokens.size() ? tokens[i] : std::string_view(); };
    auto expect = [&](std::string_view word) {
        if (peek() != word) {
            error = "expected \"" + std::string(word) + "\"" +
                    (peek().empty() ? std::string(" at end") : " before \"" + std::string(peek()) + "\"");
            return false;
        }
        ++i;
        return true;
    };
    auto identifier = [&](const char* what, std::string& dest) {
        std::string_view tok = peek();
        if (!is_valid_identifier(tok) || is_keyword(tok)) {
            error = std::string("expected ") + what +
                    (tok.empty() ? std::string(" at end") : ", got \"" + std::string(tok) + "\"");
            return false;
        }
        dest = std::string(tok);
        ++i;
        return true;
    };

    NativeDecl decl;
    if (peek() == "constant") {
        decl.is_constant = true;
        ++i;
    }
    if (!expect("native")) return false;
    if (!identifier("a native name", decl.name)) return false;
    if (!expect("takes")) return false;

    if (peek() == "nothing") {
        ++i;
    } else {
        for (;;) {
            std::string type, param;
            if (!identifier("a parameter type", type)) return false;
            if (type_code(type) == 'V') {
                error = "\"nothing\" can't be a parameter type";
                return false;
            }
            if (!identifier("a parameter name", param)) return false;
            decl.param_types.push_back(type);
            if (peek() == ",") {
                ++i;
                continue;
            }
            break;
        }
    }

    if (!expect("returns")) return false;
    if (peek() == "nothing") {
        decl.return_type = "nothing";
        ++i;
    } else if (!identifier("a return type", decl.return_type)) {
        return false;
    }
    if (i != tokens.size()) {
        error = "unexpected \"" + std::string(tokens[i]) + "\" after the return type";
        return false;
    }
    out = std::move(decl);
    return true;
}

} // namespace tome::jass
