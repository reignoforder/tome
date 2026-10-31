#pragma once

// WAMR's own allocations (its loader's and instances' structures, exec
// envs; not modules' linear memories, which it maps itself), with a limit
// on what's outstanding. WAMR sizes some allocations from counts a module
// declares before checking them against the module's bytes: 15 bytes can
// ask for gigabytes (found by fuzz/fuzz_wamr.cpp). Past the limit an
// allocation fails, and so does loading that module, instead of taking the
// game's address space (a 32-bit process).

#include <atomic>
#include <cstddef>

namespace tome::wasm {

// Outstanding bytes WAMR may hold at once, for every Wasm mod together: a
// 16 MiB module (kMaxModuleSize) needs a few times its size.
constexpr size_t kMaxRuntimeHeap = 128u << 20;

class CappedHeap {
public:
    explicit CappedHeap(size_t limit) : limit_(limit) {}

    // malloc/realloc/free with each block's size in a header; nullptr when
    // the block would take the total past the limit (realloc then keeps
    // the old block, as realloc does).
    void* allocate(size_t size);
    void* reallocate(void* block, size_t size);
    void release(void* block);

    size_t used() const { return used_.load(std::memory_order_relaxed); }

private:
    bool reserve(size_t size);

    size_t limit_;
    std::atomic<size_t> used_{0};
};

// The heap WAMR uses (RuntimeInitArgs Alloc_With_Allocator), kMaxRuntimeHeap.
void* runtime_malloc(unsigned int size);
void* runtime_realloc(void* block, unsigned int size);
void runtime_free(void* block);

} // namespace tome::wasm
