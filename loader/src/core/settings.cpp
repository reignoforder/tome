// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tome::settings {
namespace {

const char* const kKnownKeywords[] = {
    "type", "title", "description", "default", "minimum", "maximum", "step", "enum", "enumTitles", "maxLength",
    "input",
};

bool is_known_keyword(const std::string& key) {
    for (const char* known : kKnownKeywords) {
        if (key == known) return true;
    }
    return false;
}

bool is_valid_key(const std::string& key) {
    if (key.empty() || key.size() > kMaxKeyLength) return false;
    for (char c : key) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// Cuts at a UTF-8 character boundary.
std::string cut(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

double clamp(const Setting& s, double v) {
    if (s.has_minimum && v < s.minimum) v = s.minimum;
    if (s.has_maximum && v > s.maximum) v = s.maximum;
    return v;
}

std::string format_number(double v) {
    char buf[32];
    // Whole numbers in full (1234567, not 1.23457e+06, which a text box
    // couldn't take back); others to 6 significant digits.
    if (std::isfinite(v) && std::fabs(v) < 1e15 && v == std::trunc(v)) {
        snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        snprintf(buf, sizeof(buf), "%.6g", v);
    }
    return buf;
}

bool parse_setting(const std::string& key, const json::Value& def, Setting& s, std::vector<std::string>& errors,
                   std::vector<std::string>& warnings) {
    const std::string where = "\"properties." + key + "\"";
    if (!is_valid_key(key)) {
        errors.push_back(where + ": keys use 1-64 letters, digits, '-', '_' or '.'");
        return false;
    }
    if (!def.is_object()) {
        errors.push_back(where + " must be an object");
        return false;
    }
    s.key = key;
    s.title = key;

    const json::Value* type = def.find("type");
    std::string type_text = type && type->is_string() ? type->as_string() : "";
    if (type_text == "boolean") s.type = Type::Bool;
    else if (type_text == "integer") s.type = Type::Integer;
    else if (type_text == "number") s.type = Type::Number;
    else if (type_text == "string") s.type = Type::String;
    else {
        errors.push_back(where + ": \"type\" must be \"boolean\", \"integer\", \"number\" or \"string\"");
        return false;
    }

    bool ok = true;
    auto text = [&](const char* name, std::string& out) {
        const json::Value* v = def.find(name);
        if (!v) return;
        if (v->is_string()) out = v->as_string();
        else {
            errors.push_back(where + ": \"" + name + "\" must be a string");
            ok = false;
        }
    };
    auto number = [&](const char* name, bool& has, double& out) {
        const json::Value* v = def.find(name);
        if (!v) return;
        if (v->is_number()) {
            has = true;
            out = v->as_number();
        } else {
            errors.push_back(where + ": \"" + name + "\" must be a number");
            ok = false;
        }
    };
    text("title", s.title);
    text("description", s.description);
    bool has_step = false;
    bool numeric = s.type == Type::Integer || s.type == Type::Number;
    if (numeric) {
        number("minimum", s.has_minimum, s.minimum);
        number("maximum", s.has_maximum, s.maximum);
        number("step", has_step, s.step);
        if (s.has_minimum && s.has_maximum && s.minimum > s.maximum) {
            errors.push_back(where + ": \"minimum\" is greater than \"maximum\"");
            ok = false;
        }
        if (has_step && !(s.step > 0)) {
            errors.push_back(where + ": \"step\" must be positive");
            ok = false;
        }
        if (s.type == Type::Integer) {
            if (s.has_minimum) s.minimum = std::ceil(s.minimum);
            if (s.has_maximum) s.maximum = std::floor(s.maximum);
            s.step = has_step ? std::max(1.0, std::round(s.step)) : 1;
        } else if (!has_step) {
            s.step = s.has_minimum && s.has_maximum && s.maximum > s.minimum ? (s.maximum - s.minimum) / 20 : 0.1;
        }
    }
    if (const json::Value* input = def.find("input")) {
        const bool text_input = input->is_string() && input->as_string() == "text";
        if (!numeric) {
            warnings.push_back(where + ": \"input\" is only supported for integers and numbers (ignored)");
        } else if (text_input || (input->is_string() && input->as_string() == "buttons")) {
            s.text_input = text_input;
        } else {
            warnings.push_back(where + ": \"input\" must be \"text\" or \"buttons\" (ignored)");
        }
    }
    if (s.type == Type::String) {
        if (const json::Value* max = def.find("maxLength")) {
            if (max->is_number() && max->as_number() >= 1 && max->as_number() <= 4096) {
                s.max_length = static_cast<size_t>(max->as_number());
            } else {
                errors.push_back(where + ": \"maxLength\" must be a number from 1 to 4096");
                ok = false;
            }
        }
        if (const json::Value* list = def.find("enum")) {
            if (!list->is_array() || list->items().empty()) {
                errors.push_back(where + ": \"enum\" must be a non-empty list of strings");
                ok = false;
            } else {
                for (const json::Value& item : list->items()) {
                    if (!item.is_string()) {
                        errors.push_back(where + ": \"enum\" must contain only strings");
                        ok = false;
                        break;
                    }
                    s.choices.push_back({item.as_string(), item.as_string()});
                }
            }
            const json::Value* titles = def.find("enumTitles");
            if (titles && titles->is_array() && titles->items().size() == s.choices.size()) {
                for (size_t i = 0; i < s.choices.size(); ++i) {
                    if (titles->items()[i].is_string()) s.choices[i].title = titles->items()[i].as_string();
                }
            } else if (titles) {
                warnings.push_back(where + ": \"enumTitles\" must list one string per \"enum\" entry (ignored)");
            }
        }
    } else if (def.find("enum")) {
        warnings.push_back(where + ": \"enum\" is only supported for strings (ignored)");
    }
    for (const auto& member : def.members()) {
        if (!is_known_keyword(member.first)) {
            warnings.push_back(where + ": unknown keyword \"" + member.first + "\" (ignored)");
        }
    }
    if (!ok) return false;

    switch (s.type) {
    case Type::Bool: s.fallback = json::Value::boolean(false); break;
    case Type::Integer:
    case Type::Number: s.fallback = json::Value::number(clamp(s, 0)); break;
    case Type::String:
        s.fallback = json::Value::string(s.choices.empty() ? std::string() : s.choices.front().value);
        break;
    }
    if (const json::Value* def_value = def.find("default")) {
        json::Value value;
        std::string error;
        if (!coerce(s, *def_value, value, error)) {
            errors.push_back(where + ": \"default\": " + error);
            return false;
        }
        s.fallback = value;
    }
    return true;
}

} // namespace

const Setting* Schema::find(std::string_view key) const {
    for (const Setting& s : settings) {
        if (s.key == key) return &s;
    }
    return nullptr;
}

const char* type_name(Type type) {
    switch (type) {
    case Type::Bool: return "boolean";
    case Type::Integer: return "integer";
    case Type::Number: return "number";
    case Type::String: return "string";
    }
    return "?";
}

bool parse_schema(const json::Value& root, Schema& out, std::vector<std::string>& errors,
                  std::vector<std::string>& warnings) {
    size_t errors_before = errors.size();
    if (!root.is_object()) {
        errors.push_back("the settings schema must be a JSON object");
        return false;
    }
    const json::Value* type = root.find("type");
    if (type && !(type->is_string() && type->as_string() == "object")) {
        errors.push_back("the settings schema's \"type\" must be \"object\"");
    }
    const json::Value* properties = root.find("properties");
    if (!properties || !properties->is_object()) {
        errors.push_back("the settings schema needs a \"properties\" object");
        return false;
    }
    if (properties->members().size() > kMaxSettings) {
        errors.push_back("the settings schema has more than " + std::to_string(kMaxSettings) + " settings");
        return false;
    }
    Schema schema;
    for (const auto& [key, def] : properties->members()) {
        Setting s;
        if (parse_setting(key, def, s, errors, warnings)) schema.settings.push_back(std::move(s));
    }
    if (errors.size() != errors_before) return false;
    out = std::move(schema);
    return true;
}

bool coerce(const Setting& s, const json::Value& value, json::Value& out, std::string& error) {
    switch (s.type) {
    case Type::Bool:
        if (!value.is_bool()) {
            error = "expected true or false";
            return false;
        }
        out = value;
        return true;
    case Type::Integer:
    case Type::Number: {
        if (!value.is_number() || !std::isfinite(value.as_number())) {
            error = std::string("expected a number");
            return false;
        }
        double v = value.as_number();
        if (s.type == Type::Integer) v = std::round(v);
        out = json::Value::number(clamp(s, v));
        return true;
    }
    case Type::String: {
        if (!value.is_string()) {
            error = "expected a string";
            return false;
        }
        if (!s.choices.empty()) {
            for (const Choice& c : s.choices) {
                if (c.value == value.as_string()) {
                    out = value;
                    return true;
                }
            }
            error = "\"" + value.as_string() + "\" isn't one of the choices";
            return false;
        }
        out = json::Value::string(cut(value.as_string(), s.max_length));
        return true;
    }
    }
    error = "unknown type";
    return false;
}

json::Value load_values(const Schema& schema, const json::Value& stored, std::vector<std::string>& warnings) {
    json::Value values = json::Value::object();
    for (const Setting& s : schema.settings) {
        json::Value value = s.fallback;
        if (const json::Value* v = stored.is_object() ? stored.find(s.key) : nullptr) {
            std::string error;
            if (!coerce(s, *v, value, error)) {
                warnings.push_back("\"" + s.key + "\": " + error + "; using the default");
                value = s.fallback;
            }
        }
        values.set(s.key, std::move(value));
    }
    if (stored.is_object()) {
        for (const auto& [key, v] : stored.members()) {
            if (!schema.find(key)) values.set(key, v);
        }
    }
    return values;
}

std::string display(const Setting& s, const json::Value& value) {
    switch (s.type) {
    case Type::Bool: return value.is_bool() && value.as_bool() ? "On" : "Off";
    case Type::Integer:
    case Type::Number: return value.is_number() ? format_number(value.as_number()) : "?";
    case Type::String:
        if (!value.is_string()) return "?";
        for (const Choice& c : s.choices) {
            if (c.value == value.as_string()) return c.title;
        }
        return value.as_string();
    }
    return "?";
}

bool parse_typed_number(const std::string& text, json::Value& out) {
    size_t begin = text.find_first_not_of(" \t"), end = text.find_last_not_of(" \t");
    if (begin == std::string::npos) return false;
    const std::string trimmed = text.substr(begin, end - begin + 1);
    // Digits, one optional sign and one optional point: no hex, exponents or "inf".
    bool digit = false, point = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        char c = trimmed[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && !point) point = true;
        else if (!((c == '-' || c == '+') && i == 0)) return false;
    }
    if (!digit || trimmed.size() > 32) return false;
    out = json::Value::number(std::strtod(trimmed.c_str(), nullptr));
    return true;
}

json::Value step(const Setting& s, const json::Value& value, int direction) {
    switch (s.type) {
    case Type::Bool: return json::Value::boolean(!(value.is_bool() && value.as_bool()));
    case Type::Integer:
    case Type::Number: {
        double v = value.is_number() ? value.as_number() : s.fallback.as_number();
        // Stay on the grid of steps from the minimum, so repeated clicks
        // don't collect rounding errors: a value on the grid moves one step,
        // one between two grid points (typed, or an off-grid default) moves
        // to the next one in that direction. The tolerance absorbs rounding.
        double origin = s.has_minimum ? s.minimum : 0;
        double at = (v - origin) / s.step;
        double steps = direction < 0 ? std::ceil(at - 1e-9) - 1 : std::floor(at + 1e-9) + 1;
        double next = origin + steps * s.step;
        if (std::fabs(next) < s.step * 1e-9) next = 0;
        if (s.type == Type::Integer) next = std::round(next);
        return json::Value::number(clamp(s, next));
    }
    case Type::String: {
        if (s.choices.empty()) return value;
        size_t index = 0;
        for (size_t i = 0; i < s.choices.size(); ++i) {
            if (value.is_string() && s.choices[i].value == value.as_string()) index = i;
        }
        size_t n = s.choices.size();
        index = (index + (direction < 0 ? n - 1 : 1)) % n;
        return json::Value::string(s.choices[index].value);
    }
    }
    return value;
}

} // namespace tome::settings
