// Milestone 2 Phase D task D1 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md,
// decision 5): proves the vendored wasmtime C API (root CMakeLists.txt's AGENTENGINE_WITH_WASM
// block) actually builds, links, and runs a real WebAssembly module end to end on this platform --
// nothing more. Deliberately NOT a SandboxBackend proof: no backend implementation exists yet under
// src/backends/wasm/ (see that directory's own README) -- D3 is the security-critical task that
// writes one, through design->red-team->prove->judge per CLAUDE.md before it is real code. This is
// the raw dependency working, the thing D1 itself commits to.
//
// Compiles a trivial WAT module at run time (wasmtime_wat2wasm), instantiates it with zero imports,
// looks up its one export by name, and calls it with real arguments -- exercising the five C API
// object kinds (engine, store, module, instance, func) any real backend built on top of this
// dependency will need, and confirming the result is an actual computation, not just "it linked."

#include <wasm.h>
#include <wasmtime.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// Consumes and deletes `err` (wasmtime's own ownership convention -- an error is a one-shot value),
// returning its message, or "" if `err` is nullptr (the success case).
std::string consume_error(wasmtime_error_t* err) {
    if (err == nullptr) return {};
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string out(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return out;
}

}  // namespace

int main() {
    wasm_engine_t* engine = wasm_engine_new();
    AE_CHECK(engine != nullptr, "D1: wasm_engine_new() succeeds");
    if (engine == nullptr) return 1;

    wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
    AE_CHECK(store != nullptr, "D1: wasmtime_store_new() succeeds");
    if (store == nullptr) {
        wasm_engine_delete(engine);
        return 1;
    }
    wasmtime_context_t* ctx = wasmtime_store_context(store);

    static char const kWat[] =
        "(module (func (export \"add\") (param i32 i32) (result i32) "
        "local.get 0 local.get 1 i32.add))";
    wasm_byte_vec_t wasm_bytes{};
    wasmtime_error_t* err = wasmtime_wat2wasm(kWat, std::strlen(kWat), &wasm_bytes);
    if (err != nullptr) std::cerr << "  wasmtime_wat2wasm error: " << consume_error(err) << "\n";
    AE_CHECK(err == nullptr, "D1: wasmtime_wat2wasm() compiles a trivial WAT module");

    wasmtime_module_t* module = nullptr;
    if (err == nullptr) {
        err = wasmtime_module_new(engine, reinterpret_cast<std::uint8_t const*>(wasm_bytes.data),
                                   wasm_bytes.size, &module);
        wasm_byte_vec_delete(&wasm_bytes);
        if (err != nullptr) std::cerr << "  wasmtime_module_new error: " << consume_error(err) << "\n";
    }
    AE_CHECK(module != nullptr, "D1: wasmtime_module_new() compiles the module");

    wasmtime_instance_t instance{};
    if (module != nullptr) {
        wasm_trap_t* trap = nullptr;
        err = wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap);
        if (err != nullptr) std::cerr << "  wasmtime_instance_new error: " << consume_error(err) << "\n";
        AE_CHECK(err == nullptr && trap == nullptr,
                  "D1: wasmtime_instance_new() instantiates the module with zero imports");
    }

    wasmtime_extern_t add_extern{};
    bool found = module != nullptr &&
                 wasmtime_instance_export_get(ctx, &instance, "add", 3, &add_extern);
    AE_CHECK(found && add_extern.kind == WASMTIME_EXTERN_FUNC,
              "D1: the module's \"add\" export is found and is a function");

    if (found && add_extern.kind == WASMTIME_EXTERN_FUNC) {
        wasmtime_val_t args[2];
        args[0].kind = WASMTIME_I32;
        args[0].of.i32 = 2;
        args[1].kind = WASMTIME_I32;
        args[1].of.i32 = 3;
        wasmtime_val_t result{};
        wasm_trap_t* call_trap = nullptr;
        err = wasmtime_func_call(ctx, &add_extern.of.func, args, 2, &result, 1, &call_trap);
        if (err != nullptr) std::cerr << "  wasmtime_func_call error: " << consume_error(err) << "\n";
        AE_CHECK(err == nullptr && call_trap == nullptr,
                  "D1: calling the exported function succeeds without a trap");
        AE_CHECK(result.kind == WASMTIME_I32 && result.of.i32 == 5,
                  "D1: the real WebAssembly computation (2 + 3) returns 5");
    }

    if (module != nullptr) wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
