// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A WebAssembly binary built by hand, for tests: fixture modules without a
// Wasm toolchain. Function bodies are raw instruction bytes (see op::).
//
//   wasm::Builder b;
//   uint32_t log = b.import("tome", "log", {I32, I32, I32}, {});
//   b.memory(1);
//   b.data(16, "hello");
//   b.export_func("tome_mod_load", b.func({}, {I32}, {op::i32_const(1), op::i32_const(16), op::i32_const(5),
//                                                     op::call(log), op::i32_const(0)}));
//   std::string bytes = b.bytes();

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace wasm_test {

using Bytes = std::vector<uint8_t>;
constexpr uint8_t I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c;

inline void uleb(Bytes& b, uint32_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        b.push_back(byte);
    } while (v);
}

inline void sleb(Bytes& b, int32_t v) {
    bool more = true;
    while (more) {
        uint8_t byte = v & 0x7f;
        v >>= 7;  // arithmetic
        more = !((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)));
        if (more) byte |= 0x80;
        b.push_back(byte);
    }
}

inline void name(Bytes& b, const std::string& s) {
    uleb(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

// Instructions, each as its bytes.
namespace op {
inline Bytes i32_const(int32_t v) {
    Bytes b{0x41};
    sleb(b, v);
    return b;
}
inline Bytes f32_const(float v) {
    Bytes b{0x43};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
    return b;
}
inline Bytes local_get(uint32_t i) {
    Bytes b{0x20};
    uleb(b, i);
    return b;
}
inline Bytes call(uint32_t f) {
    Bytes b{0x10};
    uleb(b, f);
    return b;
}
inline Bytes drop() { return {0x1a}; }
inline Bytes i32_eq() { return {0x46}; }
inline Bytes if_() { return {0x04, 0x40}; }  // no result; close with end()
inline Bytes end() { return {0x0b}; }
inline Bytes unreachable() { return {0x00}; }
inline Bytes loop_forever() { return {0x03, 0x40, 0x0c, 0x00, 0x0b}; }  // loop br 0 end
inline Bytes i32_load(uint32_t offset = 0) {
    Bytes b{0x28, 0x02};
    uleb(b, offset);
    return b;
}
} // namespace op

class Builder {
public:
    uint32_t type(std::initializer_list<uint8_t> params, std::initializer_list<uint8_t> results) {
        Bytes t{0x60};
        uleb(t, static_cast<uint32_t>(params.size()));
        t.insert(t.end(), params);
        uleb(t, static_cast<uint32_t>(results.size()));
        t.insert(t.end(), results);
        for (size_t i = 0; i < types_.size(); ++i) {
            if (types_[i] == t) return static_cast<uint32_t>(i);
        }
        types_.push_back(t);
        return static_cast<uint32_t>(types_.size() - 1);
    }

    // Imports come before every function: call them first.
    uint32_t import(const std::string& module, const std::string& field, std::initializer_list<uint8_t> params,
                    std::initializer_list<uint8_t> results) {
        Bytes i;
        name(i, module);
        name(i, field);
        i.push_back(0x00);
        uleb(i, type(params, results));
        imports_.push_back(i);
        return imported_++;
    }

    // A global import (Tome refuses anything but functions).
    void import_global(const std::string& module, const std::string& field) {
        Bytes i;
        name(i, module);
        name(i, field);
        i.insert(i.end(), {0x03, I32, 0x00});
        imports_.push_back(i);
    }

    uint32_t func(std::initializer_list<uint8_t> params, std::initializer_list<uint8_t> results,
                  std::initializer_list<Bytes> body) {
        funcs_.push_back(type(params, results));
        Bytes code{0x00};  // no locals beyond the parameters
        for (const Bytes& b : body) code.insert(code.end(), b.begin(), b.end());
        code.push_back(0x0b);
        bodies_.push_back(code);
        return imported_ + static_cast<uint32_t>(funcs_.size() - 1);
    }

    void export_func(const std::string& field, uint32_t index) {
        Bytes e;
        name(e, field);
        e.push_back(0x00);
        uleb(e, index);
        exports_.push_back(e);
    }

    // The start function: WAMR runs it while instantiating (Tome refuses it).
    void start(uint32_t index) { start_ = index; }

    void memory(uint32_t pages) {
        memory_pages_ = pages;
        Bytes e;
        name(e, "memory");
        e.push_back(0x02);
        uleb(e, 0);
        exports_.push_back(e);
    }

    void data(uint32_t offset, const std::string& bytes) {
        Bytes d{0x00};
        Bytes c = op::i32_const(static_cast<int32_t>(offset));
        d.insert(d.end(), c.begin(), c.end());
        d.push_back(0x0b);
        name(d, bytes);
        data_.push_back(d);
    }

    std::string bytes() const {
        Bytes m{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
        section(m, 1, types_);
        section(m, 2, imports_);
        Bytes f;
        uleb(f, static_cast<uint32_t>(funcs_.size()));
        for (uint32_t t : funcs_) uleb(f, t);
        if (!funcs_.empty()) raw_section(m, 3, f);
        if (memory_pages_) {
            Bytes mem{0x01, 0x00};
            uleb(mem, memory_pages_);
            raw_section(m, 5, mem);
        }
        section(m, 7, exports_);
        if (start_ != UINT32_MAX) {
            Bytes st;
            uleb(st, start_);
            raw_section(m, 8, st);
        }
        Bytes code;
        uleb(code, static_cast<uint32_t>(bodies_.size()));
        for (const Bytes& b : bodies_) {
            uleb(code, static_cast<uint32_t>(b.size()));
            code.insert(code.end(), b.begin(), b.end());
        }
        if (!bodies_.empty()) raw_section(m, 10, code);
        section(m, 11, data_);
        return std::string(m.begin(), m.end());
    }

private:
    static void raw_section(Bytes& m, uint8_t id, const Bytes& body) {
        m.push_back(id);
        uleb(m, static_cast<uint32_t>(body.size()));
        m.insert(m.end(), body.begin(), body.end());
    }
    static void section(Bytes& m, uint8_t id, const std::vector<Bytes>& items) {
        if (items.empty()) return;
        Bytes body;
        uleb(body, static_cast<uint32_t>(items.size()));
        for (const Bytes& i : items) body.insert(body.end(), i.begin(), i.end());
        raw_section(m, id, body);
    }

    std::vector<Bytes> types_, imports_, exports_, bodies_, data_;
    std::vector<uint32_t> funcs_;
    uint32_t imported_ = 0;
    uint32_t memory_pages_ = 0;
    uint32_t start_ = UINT32_MAX;
};

} // namespace wasm_test
