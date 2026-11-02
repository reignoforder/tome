// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hash.h"
#include "test.h"

TEST(fnv1a_known_values) {
    CHECK_EQ(tome::fnv1a64(""), 14695981039346656037ull);
    CHECK_EQ(tome::fnv1a64("a"), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(tome::fnv1a64("foobar"), 0x85944171f73967e8ull);
    CHECK_EQ(tome::fnv1a32(""), 0x811c9dc5u);
    CHECK_EQ(tome::fnv1a32("a"), 0xe40c292cu);
    CHECK_EQ(tome::fnv1a32("foobar"), 0xbf9cf968u);
}

TEST(fnv1a64_mixes_in_pieces) {
    tome::Fnv1a64 hash;
    hash.mix("foo", 3);
    hash.mix("bar", 3);
    CHECK_EQ(hash.value, tome::fnv1a64("foobar"));
}
