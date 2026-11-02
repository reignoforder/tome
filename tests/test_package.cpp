// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/package.h"
#include "test.h"

using namespace tome;

namespace {

std::vector<zip::Entry> entries(std::initializer_list<const char*> names) {
    std::vector<zip::Entry> out;
    for (const char* name : names) {
        zip::Entry e;
        e.name = name;
        out.push_back(e);
    }
    return out;
}

} // namespace

TEST(package_stem) {
    CHECK_EQ(package::stem("hello.tome"), std::string("hello"));
    CHECK_EQ(package::stem("Hello.TOME"), std::string("Hello"));
    CHECK_EQ(package::stem("fps-unlock-1.2.tome"), std::string("fps-unlock-1.2"));
    CHECK_EQ(package::stem(".tome"), std::string());
    CHECK_EQ(package::stem("hello"), std::string());
    CHECK_EQ(package::stem("hello.tome.zip"), std::string());
}

TEST(package_unpacked_names) {
    std::string name = package::unpacked_name("hello", 0x0123456789abcdefull);
    CHECK_EQ(name, std::string("hello.0123456789abcdef"));
    CHECK(package::is_unpacked_name(name));
    CHECK(package::is_unpacked_name(package::unpacked_name("a.b", 1)));
    std::string partial = package::partial_name(name, 4242);
    CHECK_EQ(partial, std::string("hello.0123456789abcdef.4242.partial"));
    CHECK(package::is_unpacked_name(partial));
    // Anything else in tome/packages is left alone.
    CHECK(!package::is_unpacked_name("hello"));
    CHECK(!package::is_unpacked_name(".0123456789abcdef"));
    CHECK(!package::is_unpacked_name("hello.0123456789ABCDEF"));
    CHECK(!package::is_unpacked_name("hello-0123456789abcdef"));
    CHECK(!package::is_unpacked_name(".partial"));
}

TEST(package_needs_metadata_at_the_top) {
    std::string error;
    CHECK(package::check_entries(entries({"tome.mod.json", "hello.dll"}), error));
    CHECK(package::check_entries(entries({"assets/", "TOME.MOD.JSON"}), error));
    CHECK(!package::check_entries(entries({"hello/", "hello/tome.mod.json", "hello/hello.dll"}), error));
    CHECK(error.find("subfolder") != std::string::npos);
    CHECK(!package::check_entries(entries({"hello.dll"}), error));
    CHECK(error.find("no tome.mod.json") != std::string::npos);
    CHECK(!package::check_entries(entries({"tome.mod.json/"}), error));
}
