// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal JSON DOM: enough for tome.mod.json and tome/state.json.
// No exceptions; parse errors are reported with line and column.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tome::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    Value() = default;
    static Value boolean(bool b);
    static Value number(double n);
    static Value string(std::string s);
    static Value array();
    static Value object();

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return bool_; }
    double as_number() const { return number_; }
    const std::string& as_string() const { return string_; }
    const std::vector<Value>& items() const { return items_; }
    const std::vector<Member>& members() const { return members_; }

    // Object lookup; returns nullptr if missing or if this isn't an object.
    const Value* find(std::string_view key) const;

    // Mutation (builders for writing state.json).
    void push(Value v) { items_.push_back(std::move(v)); }
    // Replaces the member if it exists, keeping insertion order otherwise.
    void set(std::string key, Value v);

    // Same type and contents; object members compare by key, in any order.
    bool operator==(const Value& other) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

struct ParseResult {
    bool ok = false;
    std::string error;  // "line L, column C: message"
};

ParseResult parse(std::string_view text, Value& out);

// Pretty-printed with two-space indentation and a trailing newline.
std::string serialize(const Value& value);

const char* type_name(Value::Type type);

} // namespace tome::json
