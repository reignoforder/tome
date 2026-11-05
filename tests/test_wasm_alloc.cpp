// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/wasm_alloc.h"
#include "test.h"

#include <cstdint>
#include <cstring>

using namespace tome::wasm;

TEST(wasm_alloc_counts_blocks) {
    CappedHeap heap(1000);
    void* a = heap.allocate(400);
    void* b = heap.allocate(500);
    CHECK(a && b);
    CHECK_EQ(heap.used(), 900u);
    CHECK_EQ(reinterpret_cast<uintptr_t>(a) % 8, 0u);
    std::memset(a, 0xAB, 400);
    std::memset(b, 0xCD, 500);
    heap.release(a);
    CHECK_EQ(heap.used(), 500u);
    heap.release(b);
    heap.release(nullptr);
    CHECK_EQ(heap.used(), 0u);
}

TEST(wasm_alloc_refuses_past_the_limit) {
    CappedHeap heap(1000);
    CHECK(heap.allocate(1001) == nullptr);
    CHECK(heap.allocate(SIZE_MAX) == nullptr);  // no wrap in the header
    void* a = heap.allocate(600);
    CHECK(a != nullptr);
    CHECK(heap.allocate(401) == nullptr);
    CHECK_EQ(heap.used(), 600u);
    void* b = heap.allocate(400);
    CHECK(b != nullptr);
    CHECK_EQ(heap.used(), 1000u);
    heap.release(a);
    heap.release(b);
    CHECK_EQ(heap.used(), 0u);
}

TEST(wasm_alloc_reallocates) {
    CappedHeap heap(1000);
    void* a = heap.reallocate(nullptr, 100);  // realloc(NULL) allocates
    CHECK(a != nullptr);
    std::memcpy(a, "tome", 4);
    void* bigger = heap.reallocate(a, 700);
    CHECK(bigger != nullptr);
    CHECK(std::memcmp(bigger, "tome", 4) == 0);
    CHECK_EQ(heap.used(), 700u);
    // Too large: nullptr, and the old block stays (and counted).
    CHECK(heap.reallocate(bigger, 1001) == nullptr);
    void* other = heap.allocate(200);
    CHECK(heap.reallocate(bigger, 900) == nullptr);
    CHECK_EQ(heap.used(), 900u);
    void* smaller = heap.reallocate(bigger, 50);
    CHECK(smaller != nullptr);
    CHECK(std::memcmp(smaller, "tome", 4) == 0);
    CHECK_EQ(heap.used(), 250u);
    heap.release(smaller);
    heap.release(other);
    CHECK_EQ(heap.used(), 0u);
}
