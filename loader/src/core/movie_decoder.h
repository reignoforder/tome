// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The size of one decoded movie frame from blizzard.ax, the game's BLZ0
// (DivX) DirectShow decoder (patches/movie_frame_length.cpp,
// docs/reverse-engineering.md "blizzard.ax"). Kept here, apart from the
// DirectShow calls, so it can be tested.
//
// The decoder's buffers hold width * 4 * height bytes whatever output format
// it connected with, and it never sets a sample's length, so each frame
// claims the whole buffer. The patch sets this length after each frame.

#include <cstdint>

namespace tome::movie_decoder {

// The decoder's output format codes (its filter's +0xF4, set by SetMediaType
// and by Transform when a sample changes the type).
enum Format : uint32_t {
    kYuy2 = 1,
    kUyvy = 2,
    kRgb32 = 4,
    kRgb24 = 6,
    kRgb555 = 8,
    kRgb565 = 0xA,
    kYv12 = 0xD,  // also I420
};

// Bytes in one frame of `format` at width x height: RGB rows padded to 4
// bytes as in a DIB, packed YUV 2 bytes a pixel, planar 4:2:0 1.5. 0 for a
// format the decoder doesn't have, a size that isn't positive, or one too
// big to be real (more than 64 MB). `width` is the row width the decoder
// writes (row_pixels), not necessarily the movie's.
uint32_t frame_bytes(uint32_t format, int32_t width, int32_t height);

// The row width in pixels the decoder writes a frame with, as its Transform
// works it out: the output type's biWidth (its filter's +0x1EC, from
// SetMediaType or a sample's new type), or, if that's 0, the movie's width
// rounded up to 32.
int32_t row_pixels(int32_t output_width, int32_t movie_width);

} // namespace tome::movie_decoder
