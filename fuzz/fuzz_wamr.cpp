// A WebAssembly module, as a Wasm mod ships it (docs/wasm-mods.md): WAMR's
// loader, Tome's import check, instantiation with Tome's limits, then every
// exported function the way Tome calls one (its own exec env, an
// instruction budget), WAMR allocating from Tome's capped heap
// (core/wasm_alloc.h). WAMR is built with Tome's configuration (fast
// interpreter, instruction metering, reference types, software bound
// checks), for the host: x86-64 Linux, where Tome's is x86-32 Windows
// (fuzz/CMakeLists.txt).
// The imports are Tome's list (core/wasm_imports.h) with stubs returning 0:
// what's fuzzed is WAMR, the imports' checks have fuzz_wasm_abi.
#include "core/wasm_alloc.h"
#include "core/wasm_imports.h"

#include <wasm_export.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

// BH_VPRINTF (fuzz/CMakeLists.txt): WAMR's log lines, dropped.
extern "C" int tome_wamr_vprintf(const char*, va_list) {
    return 0;
}

namespace {

// Far below Tome's kInstructionBudget (a fuzz input should take milliseconds), enough for
// loops to run and the budget to run out.
constexpr uint64_t kBudget = 100000;
using tome::wasm::kStackSize;

// Called by WAMR's invokeNative (assembly, System V): extra arguments are
// ignored. 'F' results come from xmm0, the rest from rax.
int64_t stub_int(wasm_exec_env_t) {
    return 0;
}
double stub_double(wasm_exec_env_t) {
    return 0;
}

bool returns_double(const char* sig) {
    const char* close = std::strchr(sig, ')');
    return close && close[1] == 'F';
}

#define STUB(name, sig) \
    {#name, returns_double(sig) ? reinterpret_cast<void*>(stub_double) : reinterpret_cast<void*>(stub_int), sig, nullptr},
NativeSymbol g_imports[] = {TOME_WASM_IMPORTS(STUB)};
#undef STUB

bool init() {
    RuntimeInitArgs args;
    std::memset(&args, 0, sizeof(args));
    // Tome's allocator for WAMR, with its limit (core/wasm_alloc.h).
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(tome::wasm::runtime_malloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(tome::wasm::runtime_realloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(tome::wasm::runtime_free);
    if (!wasm_runtime_full_init(&args)) return false;
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
    return wasm_runtime_register_natives("tome", g_imports, sizeof(g_imports) / sizeof(g_imports[0]));
}

// What wasm_mods.cpp refuses after loading: anything but a linked function
// of module "tome".
bool imports_ok(wasm_module_t module) {
    const int32_t count = wasm_runtime_get_import_count(module);
    for (int32_t i = 0; i < count; ++i) {
        wasm_import_t import;
        wasm_runtime_get_import_type(module, i, &import);
        if (import.kind != WASM_IMPORT_EXPORT_KIND_FUNC || std::strcmp(import.module_name, "tome") != 0 ||
            !import.linked)
            return false;
    }
    return true;
}

void call_exports(wasm_module_t module, wasm_module_inst_t inst) {
    const int32_t count = wasm_runtime_get_export_count(module);
    for (int32_t i = 0; i < count; ++i) {
        wasm_export_t exp;
        wasm_runtime_get_export_type(module, i, &exp);
        if (exp.kind != WASM_IMPORT_EXPORT_KIND_FUNC) continue;
        wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, exp.name);
        if (!fn) continue;
        // Zeros for every parameter: 64-bit values take two cells, references one.
        const uint32_t params = wasm_func_type_get_param_count(exp.u.func_type);
        const uint32_t results = wasm_func_type_get_result_count(exp.u.func_type);
        uint32_t cells = 0;
        for (uint32_t p = 0; p < params; ++p) {
            const wasm_valkind_t kind = wasm_func_type_get_param_valkind(exp.u.func_type, p);
            cells += (kind == WASM_I64 || kind == WASM_F64) ? 2 : (kind == WASM_I32 || kind == WASM_F32) ? 1 : 0;
            if (kind != WASM_I32 && kind != WASM_I64 && kind != WASM_F32 && kind != WASM_F64) return;
        }
        std::vector<uint32_t> argv(cells + 2 * results + 2, 0);
        // A fresh env per call, as Tome makes one per thread: a trap leaves
        // nothing behind for the next.
        wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, kStackSize);
        if (!env) return;
        wasm_runtime_set_instruction_count_limit(env, kBudget);
        wasm_runtime_clear_exception(inst);
        wasm_runtime_call_wasm(env, fn, cells, argv.data());
        wasm_runtime_destroy_exec_env(env);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const bool ready = init();
    if (!ready) __builtin_trap();
    // WAMR may write to the bytes it loads; Tome keeps its own copy alive
    // for the module's life, as here.
    std::vector<uint8_t> bytes(data, data + size);
    char why[128];
    wasm_module_t module = wasm_runtime_load(bytes.data(), static_cast<uint32_t>(bytes.size()), why, sizeof(why));
    if (!module) return 0;
    if (imports_ok(module)) {
        InstantiationArgs args;
        std::memset(&args, 0, sizeof(args));
        args.default_stack_size = kStackSize;
        args.max_memory_pages = tome::wasm::kMaxMemoryPages;
        if (wasm_module_inst_t inst = wasm_runtime_instantiate_ex(module, &args, why, sizeof(why))) {
            call_exports(module, inst);
            wasm_runtime_deinstantiate(inst);
        }
    }
    wasm_runtime_unload(module);
    return 0;
}
