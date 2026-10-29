#include "core/widescreen.h"

namespace tome::widescreen {

Scale projection_scale(int width, int height) {
    Scale s;
    if (width <= 0 || height <= 0) return s;
    // Compare width * 3 with height * 4 in integers, so 4:3 sizes (800x600,
    // 1024x768) are exactly 1, 1.
    long long wide = static_cast<long long>(width) * 3, tall = static_cast<long long>(height) * 4;
    if (wide > tall) {
        s.x = static_cast<float>(static_cast<double>(tall) / static_cast<double>(wide));
    } else if (wide < tall) {
        s.y = static_cast<float>(static_cast<double>(wide) / static_cast<double>(tall));
    }
    return s;
}

} // namespace tome::widescreen
