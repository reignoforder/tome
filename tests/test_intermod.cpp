#include "core/intermod.h"
#include "test.h"

using namespace tome::intermod;

TEST(intermod_names) {
    CHECK(valid_name("hp-bars.api"));
    CHECK(valid_name("a"));
    CHECK(!valid_name(""));
    CHECK(!valid_name("Upper"));
    CHECK(!valid_name("has space"));
    CHECK(!valid_name(std::string(65, 'a')));
    CHECK(valid_name(std::string(64, 'a')));
}

TEST(intermod_services) {
    Services s;
    int table = 0;
    CHECK(s.add("a", "a.api", 2, &table) == Result::Ok);
    CHECK(s.add("b", "a.api", 1, &table) == Result::Taken);
    int other = 0;
    CHECK(s.add("a", "a.api", 3, &other) == Result::Updated);
    CHECK(s.find("a.api", 3)->table == &other);
    CHECK(s.add("a", "a.api", 2, &table) == Result::Updated);
    CHECK(s.add("a", "Bad Name", 1, &table) == Result::Invalid);
    CHECK(s.add("a", "a.null", 1, nullptr) == Result::Invalid);

    CHECK(s.find("a.api", 0).has_value());
    CHECK(s.find("a.api", 2).has_value());
    CHECK(!s.find("a.api", 1).has_value());
    CHECK(!s.find("missing", 0).has_value());
    CHECK(s.find("a.api", 0)->table == &table);

    CHECK(!s.remove("b", "a.api"));
    CHECK(s.remove("a", "a.api"));
    CHECK(!s.find("a.api", 0).has_value());
}
