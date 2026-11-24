// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/log_guard.h"

namespace tome::log {

std::string printable(std::string_view text, size_t max_bytes) {
    size_t n = text.size();
    bool cut = false;
    if (n > max_bytes) {
        n = max_bytes;
        // Don't split a UTF-8 character: back up over continuation bytes to
        // the start of the one that doesn't fit.
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
        cut = true;
    }
    std::string out;
    out.reserve(n + (cut ? 3 : 0));
    for (size_t i = 0; i < n; ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        out += (c < 0x20 || c == 0x7f) ? '?' : text[i];
    }
    if (cut) out += "...";
    return out;
}

SizeCap::Verdict SizeCap::take(Level level, size_t bytes) {
    Verdict v{true, false};
    const bool important = level == Level::Warn || level == Level::Error;
    if (written_ + bytes > hard_ || (written_ + bytes > soft_ && !important)) v.write = false;
    if (!v.write || written_ + bytes > soft_) {
        if (!noticed_ && written_ < hard_) {
            v.notice = true;
            noticed_ = true;
        }
    }
    if (v.write) written_ += bytes;
    return v;
}

} // namespace tome::log
