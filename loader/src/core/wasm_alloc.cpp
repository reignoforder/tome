// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/wasm_alloc.h"

#include <cstdlib>
#include <cstring>

namespace tome::wasm {
namespace {

// Keeps the block after it aligned as malloc's (8 or 16).
constexpr size_t kHeader = 16;

size_t& size_of(void* raw) {
    return *static_cast<size_t*>(raw);
}

CappedHeap g_heap(kMaxRuntimeHeap);

} // namespace

bool CappedHeap::reserve(size_t size) {
    size_t used = used_.load(std::memory_order_relaxed);
    do {
        if (size > limit_ - used) return false;
    } while (!used_.compare_exchange_weak(used, used + size, std::memory_order_relaxed));
    return true;
}

void* CappedHeap::allocate(size_t size) {
    if (size > limit_ || !reserve(size)) return nullptr;
    void* raw = std::malloc(kHeader + size);
    if (!raw) {
        used_.fetch_sub(size, std::memory_order_relaxed);
        return nullptr;
    }
    size_of(raw) = size;
    return static_cast<char*>(raw) + kHeader;
}

void* CappedHeap::reallocate(void* block, size_t size) {
    if (!block) return allocate(size);
    void* raw = static_cast<char*>(block) - kHeader;
    const size_t old = size_of(raw);
    if (size > limit_) return nullptr;
    if (size > old && !reserve(size - old)) return nullptr;
    void* moved = std::realloc(raw, kHeader + size);
    if (!moved) {
        if (size > old) used_.fetch_sub(size - old, std::memory_order_relaxed);
        return nullptr;
    }
    if (size < old) used_.fetch_sub(old - size, std::memory_order_relaxed);
    size_of(moved) = size;
    return static_cast<char*>(moved) + kHeader;
}

void CappedHeap::release(void* block) {
    if (!block) return;
    void* raw = static_cast<char*>(block) - kHeader;
    used_.fetch_sub(size_of(raw), std::memory_order_relaxed);
    std::free(raw);
}

void* runtime_malloc(unsigned int size) {
    return g_heap.allocate(size);
}

void* runtime_realloc(void* block, unsigned int size) {
    return g_heap.reallocate(block, size);
}

void runtime_free(void* block) {
    g_heap.release(block);
}

} // namespace tome::wasm
