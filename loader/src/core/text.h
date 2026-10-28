#pragma once

// Small ASCII text helpers shared by core and Windows code. ASCII only on
// purpose: everything compared this way (file names, flags, key names, DLL
// names) is matched against ASCII literals, and the result mustn't depend on
// the C locale.

#include <string_view>

namespace tome::text {

template <class C>
constexpr C ascii_lower(C c) {
    return c >= C('A') && c <= C('Z') ? static_cast<C>(c - C('A') + C('a')) : c;
}

template <class C>
constexpr bool iequals_impl(std::basic_string_view<C> a, std::basic_string_view<C> b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

// True if `a` and `b` are equal ignoring ASCII case.
constexpr bool iequals(std::string_view a, std::string_view b) { return iequals_impl(a, b); }
constexpr bool iequals(std::wstring_view a, std::wstring_view b) { return iequals_impl(a, b); }

} // namespace tome::text
