#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tome::json {

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

Value Value::number(double n) {
    Value v;
    v.type_ = Type::Number;
    v.number_ = n;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const Member& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

bool Value::operator==(const Value& other) const {
    if (type_ != other.type_) {
        return false;
    }
    switch (type_) {
    case Type::Null:
        return true;
    case Type::Bool:
        return bool_ == other.bool_;
    case Type::Number:
        return number_ == other.number_;
    case Type::String:
        return string_ == other.string_;
    case Type::Array:
        return items_ == other.items_;
    case Type::Object:
        if (members_.size() != other.members_.size()) {
            return false;
        }
        for (const Member& member : members_) {
            const Value* theirs = other.find(member.first);
            if (!theirs || !(*theirs == member.second)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

void Value::set(std::string key, Value v) {
    for (Member& member : members_) {
        if (member.first == key) {
            member.second = std::move(v);
            return;
        }
    }
    members_.emplace_back(std::move(key), std::move(v));
}

const char* type_name(Value::Type type) {
    switch (type) {
    case Value::Type::Null:   return "null";
    case Value::Type::Bool:   return "boolean";
    case Value::Type::Number: return "number";
    case Value::Type::String: return "string";
    case Value::Type::Array:  return "array";
    case Value::Type::Object: return "object";
    }
    return "?";
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult run(Value& out) {
        skip_whitespace();
        if (!parse_value(out, 0)) {
            return {false, error_};
        }
        skip_whitespace();
        if (pos_ != text_.size()) {
            fail("unexpected trailing characters");
            return {false, error_};
        }
        return {true, {}};
    }

private:
    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;

    bool fail(const char* message) {
        if (!error_.empty()) {
            return false;
        }
        size_t line = 1, column = 1;
        for (size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "line %zu, column %zu: %s", line, column, message);
        error_ = buf;
        return false;
    }

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return at_end() ? '\0' : text_[pos_]; }

    void skip_whitespace() {
        while (!at_end()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume_literal(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting too deep");
        }
        switch (peek()) {
        case '{': return parse_object(out, depth);
        case '[': return parse_array(out, depth);
        case '"': {
            std::string s;
            if (!parse_string(s)) return false;
            out = Value::string(std::move(s));
            return true;
        }
        case 't':
            if (consume_literal("true")) { out = Value::boolean(true); return true; }
            break;
        case 'f':
            if (consume_literal("false")) { out = Value::boolean(false); return true; }
            break;
        case 'n':
            if (consume_literal("null")) { out = Value(); return true; }
            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                return parse_number(out);
            }
            break;
        }
        if (at_end()) {
            return fail("unexpected end of input");
        }
        return fail("unexpected character");
    }

    bool parse_object(Value& out, int depth) {
        ++pos_;  // {
        out = Value::object();
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            if (peek() != '"') {
                return fail("expected a string key");
            }
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (peek() != ':') {
                return fail("expected ':' after key");
            }
            ++pos_;
            skip_whitespace();
            Value member;
            if (!parse_value(member, depth + 1)) return false;
            if (out.find(key)) {
                return fail("duplicate key");
            }
            out.set(std::move(key), std::move(member));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value& out, int depth) {
        ++pos_;  // [
        out = Value::array();
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_whitespace();
            Value item;
            if (!parse_value(item, depth + 1)) return false;
            out.push(std::move(item));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return fail("invalid \\u escape");
        }
        return true;
    }

    static void append_utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (at_end()) {
                return fail("unterminated string");
            }
            char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_end()) {
                return fail("unterminated escape");
            }
            char e = text_[pos_++];
            switch (e) {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned low;
                    if (!consume_literal("\\u") || !parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("invalid surrogate pair");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return fail("invalid escape");
            }
        }
    }

    bool parse_number(Value& out) {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return fail("invalid number");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        std::string number(text_.substr(start, pos_ - start));
        double value = strtod(number.c_str(), nullptr);
        // JSON has no infinity, so serialize() couldn't write it back.
        if (!std::isfinite(value)) return fail("number out of range");
        out = Value::number(value);
        return true;
    }
};

void escape_into(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void serialize_into(std::string& out, const Value& value, int indent) {
    auto newline = [&](int level) {
        out += '\n';
        out.append(static_cast<size_t>(level) * 2, ' ');
    };
    switch (value.type()) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += value.as_bool() ? "true" : "false";
        break;
    case Value::Type::Number: {
        double n = value.as_number();
        char buf[32];
        if (!std::isfinite(n)) {
            out += "null";  // JSON has no NaN or infinity
            break;
        }
        if (std::floor(n) == n && std::fabs(n) < 1e15) {
            snprintf(buf, sizeof(buf), "%.0f", n);
        } else {
            snprintf(buf, sizeof(buf), "%.17g", n);
        }
        out += buf;
        break;
    }
    case Value::Type::String:
        escape_into(out, value.as_string());
        break;
    case Value::Type::Array:
        if (value.items().empty()) {
            out += "[]";
            break;
        }
        out += '[';
        for (size_t i = 0; i < value.items().size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            serialize_into(out, value.items()[i], indent + 1);
        }
        newline(indent);
        out += ']';
        break;
    case Value::Type::Object:
        if (value.members().empty()) {
            out += "{}";
            break;
        }
        out += '{';
        for (size_t i = 0; i < value.members().size(); ++i) {
            if (i) out += ',';
            newline(indent + 1);
            escape_into(out, value.members()[i].first);
            out += ": ";
            serialize_into(out, value.members()[i].second, indent + 1);
        }
        newline(indent);
        out += '}';
        break;
    }
}

} // namespace

ParseResult parse(std::string_view text, Value& out) {
    // Tolerate a UTF-8 byte order mark; Windows editors like to add one.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    return Parser(text).run(out);
}

std::string serialize(const Value& value) {
    std::string out;
    serialize_into(out, value, 0);
    out += '\n';
    return out;
}

} // namespace tome::json
