#include "core/movie_decoder.h"

namespace tome::movie_decoder {

namespace {

constexpr uint64_t kMaxFrame = 64u << 20;

uint64_t rgb_row(uint64_t width, uint64_t bits) { return (width * bits / 8 + 3) & ~uint64_t{3}; }

} // namespace

uint32_t frame_bytes(uint32_t format, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return 0;
    const uint64_t w = static_cast<uint64_t>(width), h = static_cast<uint64_t>(height);
    uint64_t bytes = 0;
    switch (format) {
    case kYuy2:
    case kUyvy: bytes = w * 2 * h; break;
    case kRgb32: bytes = rgb_row(w, 32) * h; break;
    case kRgb24: bytes = rgb_row(w, 24) * h; break;
    case kRgb555:
    case kRgb565: bytes = rgb_row(w, 16) * h; break;
    case kYv12: bytes = w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2); break;
    default: return 0;
    }
    return bytes > kMaxFrame ? 0 : static_cast<uint32_t>(bytes);
}

int32_t row_pixels(int32_t output_width, int32_t movie_width) {
    if (output_width != 0) return output_width;
    if (movie_width <= 0 || movie_width > INT32_MAX - 31) return 0;
    return (movie_width + 31) & ~int32_t{31};
}

} // namespace tome::movie_decoder
