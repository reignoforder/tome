#include "core/embedded_mappings.h"
#include "core/mappings.h"
#include "test.h"

using namespace tome;
using mappings::Kind;

namespace {

mappings::ParseResult parse_text(const std::string& text, mappings::Build& out) {
    json::Value root;
    auto r = json::parse(text, root);
    if (!r.ok) {
        test::fail(__FILE__, __LINE__, "bad JSON in test: " + r.error);
        return {};
    }
    return mappings::parse_build(root, out);
}

// A build header followed by the given symbols.
std::string with_symbols(const std::string& symbols) {
    return R"({"schema": 2, "name": "test", "file_version": "1.2.3.4",
               "pe_timestamp": "0x584a499e", "size_of_image": "0x1000",
               "sha256": "d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236",
               "symbols": {)" + symbols + "}}";
}

bool has_error_containing(const mappings::ParseResult& r, const char* text) {
    for (const std::string& e : r.errors) {
        if (e.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(mappings_parse_every_kind) {
    mappings::Build b;
    auto r = parse_text(with_symbols(R"j(
        "Foo::Bar": {"kind": "function", "rva": "0x0100", "conv": "thiscall", "sig": "int(Foo* this)",
                     "bytes": "55 8b ec ?? ?? 00", "doc": "d"},
        "Foo.vtable": {"kind": "data", "rva": "0x200"},
        "Foo::Bar.call.startup": {"kind": "call", "rva": "0x300", "target": "Foo::Bar"},
        "Foo.field": {"kind": "offset", "value": "0xB4"},
        "Tag.stand": {"kind": "constant", "value": 23},
        "Import.Sleep": {"kind": "import", "rva": "0x400", "conv": "stdcall", "sig": "void(uint32_t ms)",
                         "target": "kernel32.dll!Sleep"}
    )j"), b);
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK_EQ(b.name, std::string("test"));
    CHECK_EQ(b.pe_timestamp, 0x584a499eu);
    CHECK_EQ(b.size_of_image, 0x1000u);
    CHECK_EQ(b.symbols.size(), size_t(6));

    const mappings::Symbol* fn = b.find("Foo::Bar");
    CHECK(fn && fn->kind == Kind::Function && fn->value == 0x100);
    CHECK(fn && fn->convention == mappings::Convention::Thiscall);
    CHECK(fn && fn->signature == "int(Foo* this)" && fn->doc == "d");
    CHECK(fn && fn->bytes == std::vector<int16_t>({0x55, 0x8b, 0xec, -1, -1, 0x00}));
    const mappings::Symbol* call = b.find("Foo::Bar.call.startup");
    CHECK(call && call->kind == Kind::Call && call->target == "Foo::Bar");
    const mappings::Symbol* offset = b.find("Foo.field");
    CHECK(offset && offset->kind == Kind::Offset && offset->value == 0xb4);
    const mappings::Symbol* constant = b.find("Tag.stand");
    CHECK(constant && constant->kind == Kind::Constant && constant->value == 23);
    const mappings::Symbol* import = b.find("Import.Sleep");
    CHECK(import && import->kind == Kind::Import && import->value == 0x400);
    CHECK(import && import->convention == mappings::Convention::Stdcall && import->signature == "void(uint32_t ms)");
    CHECK(import && import->target == "kernel32.dll!Sleep" && import->bytes.empty());
    CHECK(b.find("Missing") == nullptr);
}

TEST(mappings_reject_bad_symbols) {
    mappings::Build b;
    auto r = parse_text(with_symbols(R"j(
        "NoConv": {"kind": "function", "rva": "0x100", "sig": "void(void)", "bytes": "55 8b ec 83 ec"},
        "BadConv": {"kind": "function", "rva": "0x100", "conv": "pascal", "sig": "void(void)", "bytes": "55 8b ec 83 ec"},
        "NoBytes": {"kind": "function", "rva": "0x100", "conv": "cdecl", "sig": "void(void)"},
        "BadBytes": {"kind": "function", "rva": "0x100", "conv": "cdecl", "sig": "void(void)", "bytes": "55 8B ec 83 ec"},
        "ShortBytes": {"kind": "function", "rva": "0x100", "conv": "cdecl", "sig": "void(void)", "bytes": "55 8b ec"},
        "WildStart": {"kind": "function", "rva": "0x100", "conv": "cdecl", "sig": "void(void)", "bytes": "?? 8b ec 83 ec"},
        "PastEnd": {"kind": "function", "rva": "0xffc", "conv": "cdecl", "sig": "void(void)", "bytes": "55 8b ec 83 ec"},
        "DataBytes": {"kind": "data", "rva": "0x100", "bytes": "55 8b ec 83 ec"},
        "Outside": {"kind": "data", "rva": "0x1000"},
        "Zero": {"kind": "data", "rva": 0},
        "BadHex": {"kind": "data", "rva": "0x10g"},
        "Negative": {"kind": "constant", "value": -1},
        "Fraction": {"kind": "constant", "value": 1.5},
        "WrongField": {"kind": "offset", "rva": "0x10"},
        "Typo": {"kind": "constant", "value": 1, "comment": "x"},
        "Mystery": {"kind": "struct", "value": 1},
        "Bad name": {"kind": "constant", "value": 1},
        "Orphan": {"kind": "call", "rva": "0x100", "target": "Nowhere"},
        "NotCode": {"kind": "call", "rva": "0x100", "target": "Negative2"},
        "Negative2": {"kind": "constant", "value": 2},
        "ImportNoTarget": {"kind": "import", "rva": "0x100", "conv": "stdcall", "sig": "void(void)"},
        "ImportBadTarget": {"kind": "import", "rva": "0x100", "conv": "stdcall", "sig": "void(void)", "target": "Sleep"},
        "ImportBytes": {"kind": "import", "rva": "0x100", "conv": "stdcall", "sig": "void(void)",
                        "target": "kernel32.dll!Sleep", "bytes": "55 8b ec 83 ec"},
        "DataTarget": {"kind": "data", "rva": "0x100", "target": "kernel32.dll!Sleep"}
    )j"), b);
    CHECK(!r.ok);
    CHECK(has_error_containing(r, "\"NoConv\": missing \"conv\""));
    CHECK(has_error_containing(r, "\"BadConv\": unknown calling convention \"pascal\""));
    CHECK(has_error_containing(r, "\"NoBytes\": missing \"bytes\""));
    CHECK(has_error_containing(r, "\"BadBytes\": \"bytes\" must be hex bytes"));
    CHECK(has_error_containing(r, "\"ShortBytes\": \"bytes\" must have 5 to 64 bytes"));
    CHECK(has_error_containing(r, "\"WildStart\": \"bytes\" must not start"));
    CHECK(has_error_containing(r, "\"PastEnd\": \"bytes\" run past the end"));
    CHECK(has_error_containing(r, "\"DataBytes\": only functions have \"bytes\""));
    CHECK(has_error_containing(r, "\"Outside\": rva is outside the image"));
    CHECK(has_error_containing(r, "\"Zero\": rva is outside the image"));
    CHECK(has_error_containing(r, "\"BadHex\": \"rva\" must be"));
    CHECK(has_error_containing(r, "\"Negative\": \"value\" must be"));
    CHECK(has_error_containing(r, "\"Fraction\": \"value\" must be"));
    CHECK(has_error_containing(r, "\"WrongField\": a offset has a \"value\""));
    CHECK(has_error_containing(r, "\"Typo\": unknown field \"comment\""));
    CHECK(has_error_containing(r, "\"Mystery\": unknown kind \"struct\""));
    CHECK(has_error_containing(r, "\"Bad name\": names use"));
    CHECK(has_error_containing(r, "\"Orphan\": target \"Nowhere\" isn't defined"));
    CHECK(has_error_containing(r, "\"NotCode\": target \"Negative2\" isn't a function"));
    CHECK(has_error_containing(r, "\"ImportNoTarget\": missing \"target\""));
    CHECK(has_error_containing(r, "\"ImportBadTarget\": an import's \"target\" is \"module!function\""));
    CHECK(has_error_containing(r, "\"ImportBytes\": only functions have \"bytes\""));
    CHECK(has_error_containing(r, "\"DataTarget\": only calls and imports have a target"));
}

TEST(mappings_reject_bad_header) {
    mappings::Build b;
    auto r = parse_text(R"({"schema": 2, "name": "x", "pe_timestamp": 5, "size_of_image": "big",
                            "sha256": "ABC", "symbols": {}})", b);
    CHECK(!r.ok);
    CHECK(has_error_containing(r, "missing \"file_version\""));
    CHECK(has_error_containing(r, "\"size_of_image\" must be"));
    CHECK(has_error_containing(r, "\"sha256\" must be 64 lowercase hex digits"));

    r = parse_text(R"({"schema": 1, "symbols": {}})", b);
    CHECK(!r.ok);
    CHECK(has_error_containing(r, "unsupported schema 1"));
}

TEST(mappings_parse_bytes) {
    std::vector<int16_t> b;
    CHECK(mappings::parse_bytes("55", b) && b == std::vector<int16_t>({0x55}));
    CHECK(mappings::parse_bytes("00 ?? ff", b) && b == std::vector<int16_t>({0x00, -1, 0xff}));
    CHECK(!mappings::parse_bytes("", b));
    CHECK(!mappings::parse_bytes("5", b));
    CHECK(!mappings::parse_bytes("55 ", b));
    CHECK(!mappings::parse_bytes("55  8b", b));
    CHECK(!mappings::parse_bytes("558b", b));
    CHECK(!mappings::parse_bytes("55 ?8", b));
    CHECK(!mappings::parse_bytes("55 FF", b));
}

TEST(mappings_bind_reports_missing_names) {
    mappings::Build b;
    auto r = parse_text(with_symbols(R"j(
        "BindNative": {"kind": "data", "rva": "0x100"}
    )j"), b);
    CHECK(r.ok);
    BuildMappings bound;
    std::vector<std::string> errors;
    CHECK(!bind_build(b, bound, errors));
    bool wrong_kind = false, missing = false;
    for (const std::string& e : errors) {
        wrong_kind = wrong_kind || e == "\"BindNative\" is a data, expected a function";
        missing = missing || e == "\"CTextFrame::SetText\" isn't mapped";
    }
    CHECK(wrong_kind);
    CHECK(missing);
}

// The real data files, as compiled into tome.mix.
TEST(mappings_embedded_files_are_complete) {
    CHECK(mappings::kEmbeddedFileCount > 0);
    bool found_127b = false;
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        const mappings::EmbeddedFile& file = mappings::kEmbeddedFiles[i];
        mappings::Build b;
        auto r = parse_text(file.text, b);
        for (const std::string& e : r.errors) test::fail(__FILE__, __LINE__, std::string(file.name) + ": " + e);
        BuildMappings bound;
        std::vector<std::string> errors;
        CHECK(bind_build(b, bound, errors));
        for (const std::string& e : errors) test::fail(__FILE__, __LINE__, std::string(file.name) + ": " + e);
        CHECK_EQ(std::string(file.name), b.name + ".json");

        if (b.name == "1.27b") {
            found_127b = true;
            CHECK_EQ(b.sha256, std::string("d51e5680243fc90e19c9d6074f7fac433c466d3cf5f46e2364291725574d8236"));
            CHECK_EQ(bound.jass.bind_native, 0x0090d120u);
            CHECK_EQ(bound.jass.init_natives_calls[2], 0x001e37a2u);
            CHECK_EQ(bound.ui.layout_offset, 0xb4u);
            CHECK_EQ(bound.ui.main_menu_event_offset, 3u * 4);
            CHECK_EQ(bound.ui.main_menu_setup_offset, 55u * 4);
            CHECK_EQ(bound.ui.anim_tag_options, 0x17u);
            CHECK_EQ(bound.play.selection_units_offset, 0x1e8u);
            CHECK_EQ(bound.play.unit_flag_structure, 0x10000u);
            CHECK_EQ(bound.play.handle_first, 0x100000u);
            CHECK_EQ(bound.play.spell_target_x_slot, 243u * 4);
            CHECK_EQ(bound.play.player_state_size, 0x10u);
            CHECK(bound.blp.open != 0);
        }
    }
    CHECK(found_127b);
}

// blp-guard's symbols are optional: a build without them still binds (the
// patch is skipped), but a mistyped one is still an error.
TEST(mappings_bind_blp_symbols_are_optional) {
    for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
        const mappings::EmbeddedFile& file = mappings::kEmbeddedFiles[i];
        if (std::string(file.name) != "1.27b.json") continue;
        mappings::Build b;
        CHECK(parse_text(file.text, b).ok);
        std::vector<mappings::Symbol> all = b.symbols;
        std::erase_if(b.symbols, [](const mappings::Symbol& s) { return s.name.starts_with("CBLPFile"); });
        CHECK(b.symbols.size() + 4 == all.size());
        BuildMappings bound;
        std::vector<std::string> errors;
        CHECK(bind_build(b, bound, errors));
        CHECK(errors.empty());
        CHECK_EQ(bound.blp.open, 0u);
        CHECK_EQ(bound.blp.header_copy, 0u);

        b.symbols = all;
        for (mappings::Symbol& s : b.symbols) {
            if (s.name == "CBLPFile.size") s.kind = mappings::Kind::Constant;
        }
        errors.clear();
        CHECK(!bind_build(b, bound, errors));
        CHECK_EQ(errors.size(), size_t(1));
    }
}
