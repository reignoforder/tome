// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/json.h"
#include "test.h"

using tome::json::parse;
using tome::json::serialize;
using tome::json::Value;

TEST(json_parses_nested_document) {
    Value v;
    auto r = parse(R"({"a": [1, 2.5, -3e2], "b": {"c": true, "d": null}, "e": "x\"yé"})", v);
    CHECK(r.ok);
    CHECK(v.is_object());
    const Value* a = v.find("a");
    CHECK(a && a->is_array() && a->items().size() == 3);
    CHECK_EQ(a->items()[1].as_number(), 2.5);
    CHECK_EQ(a->items()[2].as_number(), -300.0);
    CHECK(v.find("b")->find("c")->as_bool());
    CHECK(v.find("b")->find("d")->is_null());
    CHECK_EQ(v.find("e")->as_string(), std::string("x\"y\xC3\xA9"));
    CHECK(v.find("missing") == nullptr);
}

TEST(json_decodes_surrogate_pairs) {
    Value v;
    CHECK(parse(R"("😀")", v).ok);
    CHECK_EQ(v.as_string(), std::string("\xF0\x9F\x98\x80"));
    CHECK(!parse(R"("\ude00")", v).ok);
}

TEST(json_accepts_utf8_bom) {
    Value v;
    CHECK(parse("\xEF\xBB\xBF{}", v).ok);
}

TEST(json_reports_error_position) {
    Value v;
    auto r = parse("{\n  \"a\": 1,\n  \"b\" 2\n}", v);
    CHECK(!r.ok);
    CHECK(r.error.find("line 3") != std::string::npos);
}

TEST(json_rejects_invalid_input) {
    Value v;
    CHECK(!parse("", v).ok);
    CHECK(!parse("{", v).ok);
    CHECK(!parse("[1,]", v).ok);
    CHECK(!parse("{\"a\":1,}", v).ok);
    CHECK(!parse("01", v).ok);
    CHECK(!parse("{\"a\":1} x", v).ok);
    CHECK(!parse("{\"a\":1,\"a\":2}", v).ok);
    CHECK(!parse("\"tab\there\"", v).ok);
    CHECK(!parse("// comment\n{}", v).ok);
}

TEST(json_rejects_deep_nesting) {
    Value v;
    CHECK(!parse(std::string(100, '[') + std::string(100, ']'), v).ok);
}

TEST(json_round_trips) {
    Value root = Value::object();
    root.set("schema", Value::number(1));
    Value mods = Value::object();
    Value entry = Value::object();
    entry.set("enabled", Value::boolean(false));
    mods.set("hello", entry);
    root.set("mods", mods);
    root.set("text", Value::string("line\nbreak"));

    std::string text = serialize(root);
    Value back;
    CHECK(parse(text, back).ok);
    CHECK_EQ(back.find("schema")->as_number(), 1.0);
    CHECK(!back.find("mods")->find("hello")->find("enabled")->as_bool());
    CHECK_EQ(back.find("text")->as_string(), std::string("line\nbreak"));
    CHECK(text.find("\"schema\": 1") != std::string::npos);
}

TEST(json_values_compare_by_contents) {
    Value a, b, c;
    CHECK(parse(R"({"x": [1, "two", null], "y": {"z": true}})", a).ok);
    CHECK(parse(R"({"y": {"z": true}, "x": [1, "two", null]})", b).ok);
    CHECK(parse(R"({"x": [1, "two", null], "y": {"z": false}})", c).ok);
    CHECK(a == b);
    CHECK(!(a == c));
    CHECK(Value::number(1) == Value::number(1.0));
    CHECK(!(Value::number(0) == Value::boolean(false)));
    CHECK(!(Value::string("") == Value()));
    CHECK(Value() == Value());
    Value one = Value::object(), two = Value::object();
    one.set("k", Value::number(1));
    CHECK(!(one == two));
}
