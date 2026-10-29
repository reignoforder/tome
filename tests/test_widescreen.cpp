#include "core/tome_settings.h"
#include "core/widescreen.h"
#include "test.h"

#include <cmath>

using namespace tome::widescreen;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
} // namespace

TEST(widescreen_four_by_three_unchanged) {
    for (auto [w, h] : {std::pair{800, 600}, {1024, 768}, {1600, 1200}}) {
        Scale s = projection_scale(w, h);
        CHECK(s.x == 1.0f);
        CHECK(s.y == 1.0f);
    }
}

TEST(widescreen_wider_shows_more_at_the_sides) {
    Scale s = projection_scale(1920, 1080);  // 16:9
    CHECK(near(s.x, 0.75f));                 // (4/3) / (16/9)
    CHECK(s.y == 1.0f);
    s = projection_scale(1680, 1050);  // 16:10
    CHECK(near(s.x, (4.0f / 3.0f) / 1.6f));
    s = projection_scale(3440, 1440);  // 21:9
    CHECK(near(s.x, (4.0f / 3.0f) / (3440.0f / 1440.0f)));
}

TEST(widescreen_narrower_shows_more_at_the_top) {
    Scale s = projection_scale(1280, 1024);  // 5:4
    CHECK(s.x == 1.0f);
    CHECK(near(s.y, (5.0f / 4.0f) / (4.0f / 3.0f)));
}

TEST(widescreen_empty_area) {
    for (auto [w, h] : {std::pair{0, 0}, {1280, 0}, {0, 720}, {-1, 600}}) {
        Scale s = projection_scale(w, h);
        CHECK(s.x == 1.0f);
        CHECK(s.y == 1.0f);
    }
}

TEST(tome_settings_widescreen_defaults_on) {
    using namespace tome;
    const settings::Setting* s = tome_settings::schema().find(tome_settings::kWidescreen);
    CHECK(s != nullptr);
    CHECK(s->type == settings::Type::Bool);
    CHECK(tome_settings::get_bool(json::Value::object(), tome_settings::kWidescreen));
    json::Value values = json::Value::object();
    values.set(tome_settings::kWidescreen, json::Value::boolean(false));
    CHECK(!tome_settings::get_bool(values, tome_settings::kWidescreen));
    values.set(tome_settings::kWidescreen, json::Value::string("off"));  // wrong type: the default
    CHECK(tome_settings::get_bool(values, tome_settings::kWidescreen));
}
