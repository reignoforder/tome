#include "core/movie_decoder.h"
#include "test.h"

using namespace tome::movie_decoder;

TEST(movie_decoder_frame_bytes_cinematics) {
    // The campaign cinematics: 800x368. The decoder itself asks for 1177600
    // (4 bytes a pixel) for each of these.
    CHECK(frame_bytes(kYuy2, 800, 368) == 588800);
    CHECK(frame_bytes(kUyvy, 800, 368) == 588800);
    CHECK(frame_bytes(kYv12, 800, 368) == 441600);
    CHECK(frame_bytes(kRgb32, 800, 368) == 1177600);
    CHECK(frame_bytes(kRgb24, 800, 368) == 883200);
    CHECK(frame_bytes(kRgb565, 800, 368) == 588800);
    CHECK(frame_bytes(kRgb555, 800, 368) == 588800);
}

TEST(movie_decoder_frame_bytes_padding) {
    // RGB rows are padded to 4 bytes; planar chroma rounds up.
    CHECK(frame_bytes(kRgb24, 3, 2) == 24);
    CHECK(frame_bytes(kRgb565, 3, 2) == 16);
    CHECK(frame_bytes(kYv12, 3, 3) == 9 + 2 * 4);
}

TEST(movie_decoder_row_pixels) {
    // The output type's biWidth when there is one, even past the movie's
    // width (a padded surface); else the movie's width rounded up to 32.
    CHECK(row_pixels(800, 800) == 800);
    CHECK(row_pixels(832, 800) == 832);
    CHECK(row_pixels(0, 800) == 800);
    CHECK(row_pixels(0, 810) == 832);
    CHECK(row_pixels(0, 0) == 0);
    CHECK(row_pixels(0, -5) == 0);
    CHECK(row_pixels(0, 0x7FFFFFFF) == 0);
    CHECK(frame_bytes(kYuy2, row_pixels(832, 800), 368) == 832 * 2 * 368);
}

TEST(movie_decoder_frame_bytes_rejects) {
    CHECK(frame_bytes(0, 800, 368) == 0);
    CHECK(frame_bytes(3, 800, 368) == 0);
    CHECK(frame_bytes(kYuy2, 0, 368) == 0);
    CHECK(frame_bytes(kYuy2, 800, -368) == 0);
    CHECK(frame_bytes(kRgb32, 0x7FFFFFFF, 0x7FFFFFFF) == 0);
    CHECK(frame_bytes(kRgb32, 8192, 8192) == 0);  // 256 MB: not a movie frame
}
