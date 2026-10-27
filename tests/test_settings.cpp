#include "core/settings.h"
#include "test.h"

using namespace tome;
using namespace tome::settings;

namespace {

json::Value parse_json(const char* text) {
    json::Value v;
    auto r = json::parse(text, v);
    if (!r.ok) test::fail(__FILE__, __LINE__, "bad JSON in test: " + r.error);
    return v;
}

const char* kSchema = R"({
  "type": "object",
  "properties": {
    "show":  { "type": "boolean", "title": "Show bars", "default": true },
    "scale": { "type": "number", "minimum": 0.5, "maximum": 2, "step": 0.1, "default": 1 },
    "count": { "type": "integer", "minimum": 0, "maximum": 10, "default": 3, "description": "How many" },
    "mode":  { "type": "string", "enum": ["auto", "always"], "enumTitles": ["Auto", "Always"] },
    "name":  { "type": "string", "maxLength": 4, "default": "ab" }
  }
})";

Schema schema() {
    Schema s;
    std::vector<std::string> errors, warnings;
    if (!parse_schema(parse_json(kSchema), s, errors, warnings)) test::fail(__FILE__, __LINE__, "schema");
    return s;
}

bool has(const std::vector<std::string>& list, const char* text) {
    for (const std::string& e : list) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(settings_parse_schema) {
    Schema s = schema();
    CHECK_EQ(s.settings.size(), size_t(5));
    CHECK_EQ(s.settings[0].key, std::string("show"));  // in the file's order
    CHECK_EQ(s.settings[0].title, std::string("Show bars"));
    CHECK_EQ(s.settings[2].title, std::string("count"));  // defaults to the key
    CHECK(s.settings[0].fallback.as_bool());
    CHECK_EQ(s.settings[1].step, 0.1);
    CHECK_EQ(s.settings[2].step, 1.0);
    CHECK_EQ(s.settings[3].fallback.as_string(), std::string("auto"));  // the first choice
    CHECK_EQ(s.settings[3].choices[1].title, std::string("Always"));
    CHECK(s.find("name"));
    CHECK(!s.find("missing"));
}

TEST(settings_schema_errors) {
    Schema s;
    std::vector<std::string> errors, warnings;
    CHECK(!parse_schema(parse_json(R"({"properties": {"a": {"type": "array"}}})"), s, errors, warnings));
    CHECK(has(errors, "\"type\" must be"));
    errors.clear();
    CHECK(!parse_schema(parse_json(R"({"properties": {"a": {"type": "integer", "minimum": 5, "maximum": 1}}})"), s,
                        errors, warnings));
    CHECK(has(errors, "greater than"));
    errors.clear();
    CHECK(!parse_schema(parse_json(R"({"properties": {"a": {"type": "string", "enum": ["x"], "default": "y"}}})"),
                        s, errors, warnings));
    CHECK(has(errors, "isn't one of the choices"));
    errors.clear();
    CHECK(!parse_schema(parse_json(R"({"properties": {"bad key": {"type": "boolean"}}})"), s, errors, warnings));
    errors.clear();
    CHECK(!parse_schema(parse_json(R"([])"), s, errors, warnings));
    errors.clear();
    CHECK(parse_schema(parse_json(R"({"properties": {"a": {"type": "boolean", "format": "x"}}})"), s, errors,
                       warnings));
    CHECK(has(warnings, "unknown keyword \"format\""));
}

TEST(settings_coerce) {
    Schema s = schema();
    json::Value out;
    std::string error;
    CHECK(coerce(*s.find("count"), json::Value::number(2.6), out, error));
    CHECK_EQ(out.as_number(), 3.0);
    CHECK(coerce(*s.find("count"), json::Value::number(99), out, error));
    CHECK_EQ(out.as_number(), 10.0);
    CHECK(!coerce(*s.find("count"), json::Value::string("3"), out, error));
    CHECK(coerce(*s.find("name"), json::Value::string("abcdef"), out, error));
    CHECK_EQ(out.as_string(), std::string("abcd"));
    CHECK(coerce(*s.find("name"), json::Value::string("ab\xc3\xa9"), out, error));
    CHECK_EQ(out.as_string(), std::string("ab\xc3\xa9"));  // 4 bytes fit
    CHECK(coerce(*s.find("name"), json::Value::string("abc\xc3\xa9"), out, error));
    CHECK_EQ(out.as_string(), std::string("abc"));  // not cut inside a character
    CHECK(!coerce(*s.find("mode"), json::Value::string("never"), out, error));
    CHECK(!coerce(*s.find("show"), json::Value::number(1), out, error));
}

TEST(settings_load_values) {
    Schema s = schema();
    std::vector<std::string> warnings;
    json::Value v = load_values(s, parse_json(R"({"show": false, "count": "x", "future": 7})"), warnings);
    CHECK(!v.find("show")->as_bool());
    CHECK_EQ(v.find("count")->as_number(), 3.0);  // invalid: the default
    CHECK(has(warnings, "\"count\""));
    CHECK_EQ(v.find("future")->as_number(), 7.0);  // unknown keys are kept
    CHECK_EQ(v.find("mode")->as_string(), std::string("auto"));
    json::Value empty = load_values(s, json::Value(), warnings);
    CHECK_EQ(empty.members().size(), size_t(5));
}

TEST(settings_step_from_off_grid) {
    Schema s;
    std::vector<std::string> errors, warnings;
    CHECK(parse_schema(parse_json(R"({"properties": {"n": {"type": "integer", "minimum": 0, "maximum": 20,
                                                            "step": 2, "default": 5}}})"),
                       s, errors, warnings));
    const Setting& n = *s.find("n");
    CHECK_EQ(step(n, json::Value::number(5), 1).as_number(), 6.0);  // the next grid point, not 8
    CHECK_EQ(step(n, json::Value::number(5), -1).as_number(), 4.0);
    CHECK_EQ(step(n, json::Value::number(6), 1).as_number(), 8.0);
    CHECK_EQ(step(n, json::Value::number(6), -1).as_number(), 4.0);
}

TEST(settings_display_and_step) {
    Schema s = schema();
    const Setting& scale = *s.find("scale");
    CHECK_EQ(display(*s.find("show"), json::Value::boolean(true)), std::string("On"));
    CHECK_EQ(display(*s.find("mode"), json::Value::string("always")), std::string("Always"));
    CHECK_EQ(display(scale, json::Value::number(1.5)), std::string("1.5"));
    CHECK_EQ(display(scale, json::Value::number(1234567)), std::string("1234567"));  // not 1.23457e+06

    json::Value v = json::Value::number(1);
    for (int i = 0; i < 7; ++i) v = step(scale, v, 1);
    CHECK_EQ(display(scale, v), std::string("1.7"));  // no rounding errors piling up
    for (int i = 0; i < 20; ++i) v = step(scale, v, 1);
    CHECK_EQ(v.as_number(), 2.0);
    for (int i = 0; i < 30; ++i) v = step(scale, v, -1);
    CHECK_EQ(v.as_number(), 0.5);

    CHECK(!step(*s.find("show"), json::Value::boolean(true), 1).as_bool());
    CHECK(step(*s.find("mode"), json::Value::string("always"), 1).as_string() == std::string("auto"));
    CHECK(step(*s.find("mode"), json::Value::string("auto"), -1).as_string() == std::string("always"));
    CHECK_EQ(step(*s.find("count"), json::Value::number(10), 1).as_number(), 10.0);
    CHECK(step(*s.find("name"), json::Value::string("ab"), 1).as_string() == std::string("ab"));
}

TEST(settings_typed_numbers) {
    Schema s;
    std::vector<std::string> errors, warnings;
    CHECK(parse_schema(parse_json(R"({"properties": {
        "limit": {"type": "integer", "minimum": 30, "maximum": 1000, "input": "text"},
        "scale": {"type": "number", "input": "buttons"},
        "odd":   {"type": "number", "input": "slider"},
        "flag":  {"type": "boolean", "input": "text"}}})"),
                       s, errors, warnings));
    CHECK(s.settings[0].text_input);
    CHECK(!s.settings[1].text_input);
    CHECK(!s.settings[2].text_input);
    CHECK(has(warnings, "properties.odd\": \"input\" must be \"text\" or \"buttons\""));
    CHECK(has(warnings, "properties.flag\": \"input\" is only supported for integers and numbers"));
    CHECK(!has(warnings, "unknown keyword \"input\""));

    json::Value v;
    CHECK(parse_typed_number("250", v) && v.as_number() == 250.0);
    CHECK(parse_typed_number("  -1.5 ", v) && v.as_number() == -1.5);
    CHECK(parse_typed_number("+7", v) && v.as_number() == 7.0);
    CHECK(parse_typed_number(".5", v) && v.as_number() == 0.5);
    CHECK(!parse_typed_number("", v));
    CHECK(!parse_typed_number("   ", v));
    CHECK(!parse_typed_number("fast", v));
    CHECK(!parse_typed_number("1e9", v));
    CHECK(!parse_typed_number("0x10", v));
    CHECK(!parse_typed_number("1.2.3", v));
    CHECK(!parse_typed_number("-", v));
    CHECK(!parse_typed_number("12 fps", v));

    // What the menu saves goes through coerce: rounded and clamped.
    json::Value out;
    std::string error;
    CHECK(parse_typed_number("144.6", v) && coerce(s.settings[0], v, out, error) && out.as_number() == 145.0);
    CHECK(parse_typed_number("5000", v) && coerce(s.settings[0], v, out, error) && out.as_number() == 1000.0);
}
