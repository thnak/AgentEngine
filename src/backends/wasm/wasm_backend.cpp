// Implements wasm_backend.hpp. See that header and decisions/ADR-010-wasm-component-host-manifest-
// capability-binding.md for the spec/ADR citations this satisfies. `cb_http_request` additionally
// implements decisions/ADR-011-first-party-egress-proxy.md (M2 Phase F task F1): the first of this
// file's gated I/O callbacks to go from stub to a real backing effect.

#include "backends/wasm/wasm_backend.hpp"

#include "agentengine/sandbox/net_egress_proxy.hpp"

#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/component.h>
#include <wasmtime/engine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <stop_token>
#include <thread>
#include <utility>

namespace agentengine::wasm {

namespace {

// -- shared engine + epoch ticker (ADR-010 §3.4) --------------------------------------------------
// One process-wide engine, one process-wide ticker thread -- bounded at exactly one thread total,
// per CLAUDE.md's machine-safety rule, regardless of how many WasmBackend instances or handles
// exist. wasmtime engines are designed to be cheap to share across many stores; the epoch counter
// they carry is what every store's wall_ms deadline is measured against (wasmtime_engine_increment_
// epoch is engine-global, wasmtime_context_set_epoch_deadline is per-store -- store.h/engine.h).
constexpr std::uint64_t kEpochTickMs = 10;

// store.h: "By default the deadline is the current engine's epoch, immediately interrupting code if
// epoch interruption is enabled." Every store this backend creates MUST set a real deadline before
// running any guest code, even when the caller set no `wall_ms` limit -- an unset deadline is not
// "unlimited," it is "already expired," and every call would trap on its very first instruction (a
// real bug this project's own tests caught, ADR-010 §7.5). A `wall_ms == 0` limit maps to this
// effectively-unbounded tick count, not to skipping the call entirely.
constexpr std::uint64_t kUnboundedTicks = UINT64_MAX / 2;

std::uint64_t ticks_for(std::uint64_t wall_ms) {
    if (wall_ms == 0) return kUnboundedTicks;
    std::uint64_t const ticks = (wall_ms + kEpochTickMs - 1) / kEpochTickMs;
    return ticks == 0 ? 1 : ticks;
}

struct SharedEngine {
    wasm_engine_t* engine;
    std::jthread   ticker;

    SharedEngine() : engine(make_engine()), ticker(make_ticker(engine)) {}
    // A destructor's own body always runs before its members are implicitly destroyed, regardless
    // of declaration order -- so without this explicit stop+join, `ticker` (still running,
    // sleeping up to kEpochTickMs at a time) was only guaranteed to stop *after* `engine` was
    // already deleted below, leaving a real use-after-free race window if the ticker thread woke
    // and called wasmtime_engine_increment_epoch(eng) on the freed engine before jthread's own
    // (implicit, later) destructor could request-stop and join it. Reproduced as an intermittent
    // (~30%) Linux-only segfault at process exit -- see docs/issues/m2-phase-d-wasm-plugin-host.md.
    ~SharedEngine() {
        ticker.request_stop();
        if (ticker.joinable()) ticker.join();
        wasm_engine_delete(engine);
    }
    SharedEngine(SharedEngine const&) = delete;
    SharedEngine& operator=(SharedEngine const&) = delete;

private:
    static wasm_engine_t* make_engine() {
        wasm_config_t* config = wasm_config_new();
        wasmtime_config_epoch_interruption_set(config, true);
        return wasm_engine_new_with_config(config);
    }
    static std::jthread make_ticker(wasm_engine_t* eng) {
        return std::jthread([eng](std::stop_token st) {
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kEpochTickMs));
                wasmtime_engine_increment_epoch(eng);
            }
        });
    }
};

wasm_engine_t* shared_engine() {
    static SharedEngine instance;
    return instance.engine;
}

// -- capability-interface name <-> capability_kind mapping (ADR-010 §3.1) ------------------------
enum class interface_class { always_ok, unimplemented, unknown, fs, http, secrets, clock, random };

interface_class classify_interface(std::string_view name) {
    // "ae:tool/capability@1.0.0" and "ae:tool/types@1.0.0" carry only type/resource declarations
    // that every gated interface's own functions structurally depend on (the `capability-handle`
    // resource type, and the record/variant types content-item/tool-result are built from) -- not
    // an authority of their own, so treated the same as `base`. Confirmed empirically against a
    // real compiled component (§7.2), not assumed from the WIT source alone: which of these two
    // actually surface as separate top-level imports (as opposed to being inlined at each use site)
    // is a Component Model encoding detail this host does not need to predict correctly, only
    // handle correctly if it happens.
    if (name == "ae:tool/base@1.0.0" || name == "ae:tool/capability@1.0.0" ||
        name == "ae:tool/types@1.0.0") {
        return interface_class::always_ok;
    }
    if (name == "ae:tool/blob@1.0.0" || name == "ae:tool/tool-call@1.0.0") {
        return interface_class::unimplemented;
    }
    if (name == "ae:tool/fs@1.0.0") return interface_class::fs;
    if (name == "ae:tool/http@1.0.0") return interface_class::http;
    if (name == "ae:tool/secrets@1.0.0") return interface_class::secrets;
    if (name == "ae:tool/clock@1.0.0") return interface_class::clock;
    if (name == "ae:tool/random@1.0.0") return interface_class::random;
    return interface_class::unknown;
}

// Whether `granted` (the manifest's requested_capabilities, already confirmed by CapabilitySet::
// contains against the operator grant -- see load_component) covers what `cls` needs to be linked.
// Interface-level = "may this component reach this general capability surface at all"; the *specific*
// operation (fs-read vs fs-write, etc.) is enforced again, precisely, inside each host callback via
// BoundCapability's own kind check (§3.2) -- this function only answers the coarser question.
bool interface_covered(interface_class cls, std::vector<Capability> const& granted) {
    auto has = [&](capability_kind k) {
        return std::any_of(granted.begin(), granted.end(),
                            [k](Capability const& c) { return capability_kind_of(c) == k; });
    };
    switch (cls) {
        case interface_class::always_ok: return true;
        case interface_class::unimplemented: return false;
        case interface_class::unknown: return false;
        case interface_class::fs: return has(capability_kind::fs_read) || has(capability_kind::fs_write);
        case interface_class::http: return has(capability_kind::net_out);
        case interface_class::secrets: return has(capability_kind::secret);
        case interface_class::clock: return has(capability_kind::clock);
        case interface_class::random: return has(capability_kind::entropy);
    }
    return false;
}

// -- wasmtime_component_val_t construction/inspection helpers ------------------------------------
wasm_name_t make_name(std::string_view s) {
    wasm_name_t out;
    wasm_name_new(&out, s.size(), s.data());
    return out;
}

std::string name_to_string(wasm_name_t const& n) { return std::string(n.data, n.size); }

wasmtime_component_val_t make_string_val(std::string_view s) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_STRING;
    v.of.string = make_name(s);
    return v;
}

wasmtime_component_val_t make_record_val(
        std::vector<std::pair<std::string, wasmtime_component_val_t>> fields) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_RECORD;
    std::vector<wasmtime_component_valrecord_entry_t> entries;
    entries.reserve(fields.size());
    for (auto& [name, val] : fields) {
        wasmtime_component_valrecord_entry_t entry{};
        entry.name = make_name(name);
        entry.val = val;
        entries.push_back(entry);
    }
    wasmtime_component_valrecord_new(&v.of.record, entries.size(), entries.data());
    return v;
}

wasmtime_component_val_t make_list_val(std::vector<wasmtime_component_val_t> elements) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_LIST;
    wasmtime_component_vallist_new(&v.of.list, elements.size(), elements.data());
    return v;
}

wasmtime_component_val_t make_resource_val(wasmtime_component_resource_any_t* resource) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_RESOURCE;
    v.of.resource = resource;
    return v;
}

// -- val construction/inspection this file's other gated callbacks never needed (they are all still
// stubs) -- added for cb_http_request (ADR-011): tuple<string,string> (WIT `headers`), enum (WIT
// `http-error`), option (WIT `body`), and result<T,E> (WIT `http-request`'s own return type). ------

wasmtime_component_val_t make_tuple_val(std::vector<wasmtime_component_val_t> elements) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_TUPLE;
    wasmtime_component_valtuple_new(&v.of.tuple, elements.size(), elements.data());
    return v;
}

wasmtime_component_val_t make_enum_val(std::string_view case_name) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_ENUM;
    v.of.enumeration = make_name(case_name);
    return v;
}

// `wasmtime_component_val_new` heap-allocates and takes ownership of `*inner`'s contents (moved, not
// cloned) -- matches this file's existing ownership convention of never explicitly freeing a value
// handed to `results[0]`, since the runtime takes ownership of whatever the callback returns (the
// same trust `cb_random_bytes`'s `make_list_val` already relies on).
wasmtime_component_val_t make_result_val(bool is_ok, wasmtime_component_val_t payload) {
    wasmtime_component_val_t v{};
    v.kind = WASMTIME_COMPONENT_RESULT;
    v.of.result.is_ok = is_ok;
    v.of.result.val = wasmtime_component_val_new(&payload);
    return v;
}

// Non-owning: returns a pointer into `record`'s own storage.
wasmtime_component_val_t const* find_field(wasmtime_component_val_t const& record, std::string_view name) {
    if (record.kind != WASMTIME_COMPONENT_RECORD) return nullptr;
    for (std::size_t i = 0; i < record.of.record.size; ++i) {
        auto const& entry = record.of.record.data[i];
        if (name_to_string(entry.name) == name) return &entry.val;
    }
    return nullptr;
}

std::string string_field(wasmtime_component_val_t const& record, std::string_view name) {
    auto const* f = find_field(record, name);
    if (f == nullptr || f->kind != WASMTIME_COMPONENT_STRING) return {};
    return name_to_string(f->of.string);
}

bool bool_field(wasmtime_component_val_t const& record, std::string_view name) {
    auto const* f = find_field(record, name);
    return f != nullptr && f->kind == WASMTIME_COMPONENT_BOOL && f->of.boolean;
}

}  // namespace

// The "type" tag every capability-handle resource this host creates carries (007's `Capability`
// variant has sixteen alternatives; this host defines exactly one WIT resource kind, so a single
// fixed tag distinguishes it from nothing else -- reserved for a second host-defined resource kind
// if one is ever added).
constexpr std::uint32_t kCapabilityHandleType = 1;

struct WasmBackend::Instance {
    wasmtime_component_t*   component = nullptr;
    PluginManifest          manifest;
    CapabilitySet           operator_grant;  // spec.capabilities, copied at create()
    ResourceLimits          limits;
    bool                    loaded = false;  // load_component() has succeeded
    std::vector<interface_class> import_classes;  // this component's actual verified imports

    ~Instance() {
        if (component != nullptr) wasmtime_component_delete(component);
    }
};

WasmBackend::WasmBackend() = default;
WasmBackend::~WasmBackend() = default;

result<SandboxHandle> WasmBackend::create(SandboxSpec const& spec, EffectContext&) {
    auto instance = std::make_unique<Instance>();
    instance->operator_grant = spec.capabilities;
    instance->limits = spec.limits;

    static std::atomic<std::uint64_t> next_id{0};
    std::string id = "wasm-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
    instances_[id] = std::move(instance);
    return SandboxHandle{id};
}

result<void> WasmBackend::load_component(SandboxHandle& handle, PluginManifest const& manifest,
                                          std::vector<std::uint8_t> const& component_bytes,
                                          EffectContext&) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end()) {
        return std::unexpected(error{failure_class::contract, "unknown sandbox handle", "wasm.no_handle"});
    }
    Instance& inst = *it->second;
    if (inst.loaded) {
        return std::unexpected(
            error{failure_class::contract, "load_component called twice on the same handle", "wasm.already_loaded"});
    }

    wasmtime_component_t* component = nullptr;
    wasmtime_error_t* err = wasmtime_component_new(
        shared_engine(), component_bytes.data(), component_bytes.size(), &component);
    if (err != nullptr) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(err, &msg);
        std::string message(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        return std::unexpected(
            error{failure_class::contract, "component failed to compile: " + message, "wasm.compile_failed"});
    }

    wasmtime_component_type_t* ty = wasmtime_component_type(component);
    std::size_t const count = wasmtime_component_type_import_count(ty, shared_engine());
    std::vector<interface_class> classes;
    classes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        char const* name_ptr = nullptr;
        std::size_t name_len = 0;
        wasmtime_component_item_t item{};
        if (!wasmtime_component_type_import_nth(ty, shared_engine(), i, &name_ptr, &name_len, &item)) {
            continue;
        }
        std::string const name(name_ptr, name_len);
        wasmtime_component_item_delete(&item);

        interface_class const cls = classify_interface(name);
        if (cls == interface_class::unknown) {
            wasmtime_component_type_delete(ty);
            wasmtime_component_delete(component);
            return std::unexpected(error{failure_class::contract,
                                          "component imports an interface outside the ae:tool contract: " + name,
                                          "wasm.unknown_import"});
        }
        if (cls == interface_class::unimplemented) {
            wasmtime_component_type_delete(ty);
            wasmtime_component_delete(component);
            return std::unexpected(
                error{failure_class::contract,
                      "component imports an unimplemented ae:tool host interface: " + name,
                      "wasm.unimplemented_import"});
        }
        // `always_ok` (base/capability/types -- structural plumbing every gated interface's own
        // functions depend on, ungated by construction, ADR-010 §3.1) needs neither check: there is
        // no capability kind to look for in the manifest or the operator grant, and requiring one
        // would reject every component that imports these purely-structural interfaces, which is
        // every component (a real bug this project's own tests caught, see ADR-010 §7.5).
        if (cls != interface_class::always_ok) {
            if (!interface_covered(cls, manifest.requested_capabilities)) {
                wasmtime_component_type_delete(ty);
                wasmtime_component_delete(component);
                return std::unexpected(error{failure_class::policy,
                                              "component imports " + name + " but the manifest does not request it",
                                              "wasm.manifest_capability_not_requested"});
            }
            // The manifest's request must itself be covered by what the operator actually granted
            // this sandbox (007 §3's real CapabilitySet::contains, not a parallel check) -- kind-only
            // here is deliberate and sufficient at this stage: the *parameterized* check (mount/host/
            // name) is the same one every bound capability already gets inside its own callback
            // (§3.2), the moment it is actually used, not duplicated here against a kind the manifest
            // merely names.
            bool operator_covers = false;
            for (Capability const& requested : manifest.requested_capabilities) {
                if (interface_covered(cls, {requested}) &&
                    inst.operator_grant.contains_kind(capability_kind_of(requested))) {
                    operator_covers = true;
                    break;
                }
            }
            if (!operator_covers) {
                wasmtime_component_type_delete(ty);
                wasmtime_component_delete(component);
                return std::unexpected(error{failure_class::policy,
                                              "manifest requests " + name + " but the operator did not grant it",
                                              "wasm.operator_grant_missing"});
            }
        }
        classes.push_back(cls);
    }
    wasmtime_component_type_delete(ty);

    inst.component = component;
    inst.manifest = manifest;
    inst.import_classes = std::move(classes);
    inst.loaded = true;
    return {};
}

namespace {

// The per-call capability table every gated host callback closes over, alive only for the duration
// of a single call (ADR-010 §3.5 -- no pooling). `bound[i]` is what `rep == i` (§3.2) resolves to.
struct CallCapabilities {
    std::vector<BoundCapability> const* bound;
};

// Every capability-gated function's callback recovers its bound capability the same way: pull the
// resource argument's "rep" (trusted, guest cannot forge it -- val.h), index into the call's own
// bound-capability table, `.use()` it (fails if revoked), and confirm it is the exact `cap::`
// alternative this function requires. A mismatch (wrong kind OR a revoked handle) is this
// function's own structured error, never a trap that could be mistaken for a host bug.
template <class CapT>
result<CapT> recover_capability(wasmtime_context_t* ctx, wasmtime_component_val_t const* cap_val,
                                 CallCapabilities const& call) {
    if (cap_val == nullptr || cap_val->kind != WASMTIME_COMPONENT_RESOURCE) {
        return std::unexpected(error{failure_class::contract, "missing capability handle", "wasm.no_capability"});
    }
    wasmtime_component_resource_host_t* host_res = nullptr;
    wasmtime_error_t* err =
        wasmtime_component_resource_any_to_host(ctx, cap_val->of.resource, &host_res);
    if (err != nullptr) {
        wasmtime_error_delete(err);
        return std::unexpected(
            error{failure_class::contract, "capability handle is not host-defined", "wasm.foreign_capability"});
    }
    std::uint32_t const rep = wasmtime_component_resource_host_rep(host_res);
    std::uint32_t const ty = wasmtime_component_resource_host_type(host_res);
    wasmtime_component_resource_host_delete(host_res);
    if (ty != kCapabilityHandleType || rep >= call.bound->size()) {
        return std::unexpected(error{failure_class::contract, "invalid capability handle", "wasm.bad_capability"});
    }
    auto used = (*call.bound)[rep].use();
    if (!used) {
        return std::unexpected(used.error());
    }
    if (auto const* typed = std::get_if<CapT>(&*used)) {
        return *typed;
    }
    return std::unexpected(
        error{failure_class::policy, "capability handle is the wrong kind for this function", "wasm.wrong_capability_kind"});
}

wasmtime_error_t* trap_error(char const* msg) {
    return wasmtime_error_new(msg);
}

// The guest may explicitly drop an `own<capability-handle>` it was handed (Canonical ABI resource
// destructor callback); this host's real cleanup already happens through BoundCapability::revoke()
// (§3.2, unconditional at the end of invoke_tool regardless of guest behavior), so there is nothing
// further to do here -- a no-op destructor, not an unimplemented one.
wasmtime_error_t* cb_capability_handle_drop(void*, wasmtime_context_t*, std::uint32_t) { return nullptr; }

// -- host callbacks, one per gated function -------------------------------------------------------
// Every callback here must be exception-safe (ADR-010 §5 F5): none of them may let a C++ exception
// escape across the extern "C" boundary wasmtime's trampoline calls through.

wasmtime_error_t* cb_now_unix_millis(void* env, wasmtime_context_t* ctx,
                                      wasmtime_component_func_type_t const*,
                                      wasmtime_component_val_t* args, std::size_t nargs,
                                      wasmtime_component_val_t* results, std::size_t nresults) {
    if (nargs != 1 || nresults != 1) return trap_error("now-unix-millis: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::Clock>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());
    auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    results[0].kind = WASMTIME_COMPONENT_U64;
    results[0].of.u64 = static_cast<std::uint64_t>(now);
    return nullptr;
}

wasmtime_error_t* cb_random_bytes(void* env, wasmtime_context_t* ctx,
                                   wasmtime_component_func_type_t const*,
                                   wasmtime_component_val_t* args, std::size_t nargs,
                                   wasmtime_component_val_t* results, std::size_t nresults) {
    if (nargs != 2 || nresults != 1) return trap_error("random-bytes: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::Entropy>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());
    if (args[1].kind != WASMTIME_COMPONENT_U32) return trap_error("random-bytes: bad count arg");
    std::uint32_t const count = args[1].of.u32;
    std::vector<wasmtime_component_val_t> bytes(count);
    std::random_device rd;
    for (auto& b : bytes) {
        b.kind = WASMTIME_COMPONENT_U8;
        b.of.u8 = static_cast<std::uint8_t>(rd() & 0xffu);
    }
    results[0] = make_list_val(std::move(bytes));
    return nullptr;
}

wasmtime_error_t* cb_log(void*, wasmtime_context_t*, wasmtime_component_func_type_t const*,
                          wasmtime_component_val_t*, std::size_t, wasmtime_component_val_t*, std::size_t) {
    return nullptr;  // M2 scope: accepted, not routed anywhere yet (no telemetry sink built).
}

wasmtime_error_t* cb_record_metric(void*, wasmtime_context_t*, wasmtime_component_func_type_t const*,
                                    wasmtime_component_val_t*, std::size_t, wasmtime_component_val_t*,
                                    std::size_t) {
    return nullptr;
}

// fs/http/secrets are declared (interface_covered() above already gates them structurally) but have
// no real backing effect in M2 -- MountSpec/NetPolicy resolution is out of this task's scope (D3's
// own task text: "load, verify manifest-vs-imports, instantiate... invoke, destroy" -- not "wire up
// every effect"). Each still enforces the real capability-kind check before doing anything, so the
// security-critical property this ADR is about (F3: per-callback kind enforcement) is real and
// tested even though the underlying I/O is a documented stub, not silently pretended-complete.
wasmtime_error_t* cb_fs_read(void* env, wasmtime_context_t* ctx, wasmtime_component_func_type_t const*,
                              wasmtime_component_val_t* args, std::size_t nargs,
                              wasmtime_component_val_t*, std::size_t nresults) {
    if (nargs != 2 || nresults != 1) return trap_error("fs-read: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::FsRead>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());
    return trap_error("fs-read: not implemented in M2's minimal host");
}

wasmtime_error_t* cb_fs_write(void* env, wasmtime_context_t* ctx, wasmtime_component_func_type_t const*,
                               wasmtime_component_val_t* args, std::size_t nargs,
                               wasmtime_component_val_t*, std::size_t nresults) {
    if (nargs != 3 || nresults != 1) return trap_error("fs-write: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::FsWrite>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());
    return trap_error("fs-write: not implemented in M2's minimal host");
}

// decisions/ADR-011-first-party-egress-proxy.md: the first of this file's gated I/O callbacks to go
// from stub to a real backing effect. Host/port/scheme come entirely from the recovered `cap::NetOut`
// (never from the guest -- see net_egress_proxy.hpp's own file-top comment and the WIT `http`
// interface's banner); the guest's `http-request-data` record supplies only method/path/headers/body.
wasmtime_error_t* cb_http_request(void* env, wasmtime_context_t* ctx,
                                   wasmtime_component_func_type_t const*, wasmtime_component_val_t* args,
                                   std::size_t nargs, wasmtime_component_val_t* results, std::size_t nresults) {
    if (nargs != 2 || nresults != 1) return trap_error("http-request: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::NetOut>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());

    wasmtime_component_val_t const& req_val = args[1];
    sandbox::NetEgressRequest req;
    req.method = string_field(req_val, "method");
    req.path = string_field(req_val, "path");
    if (auto const* headers = find_field(req_val, "headers");
        headers != nullptr && headers->kind == WASMTIME_COMPONENT_LIST) {
        for (std::size_t i = 0; i < headers->of.list.size; ++i) {
            auto const& tup = headers->of.list.data[i];
            if (tup.kind != WASMTIME_COMPONENT_TUPLE || tup.of.tuple.size != 2) continue;
            auto const& k = tup.of.tuple.data[0];
            auto const& v = tup.of.tuple.data[1];
            if (k.kind == WASMTIME_COMPONENT_STRING && v.kind == WASMTIME_COMPONENT_STRING) {
                req.headers.emplace_back(name_to_string(k.of.string), name_to_string(v.of.string));
            }
        }
    }
    if (auto const* body = find_field(req_val, "body");
        body != nullptr && body->kind == WASMTIME_COMPONENT_OPTION && body->of.option != nullptr) {
        wasmtime_component_val_t const& byte_list = *body->of.option;
        if (byte_list.kind == WASMTIME_COMPONENT_LIST) {
            req.body.reserve(byte_list.of.list.size);
            for (std::size_t i = 0; i < byte_list.of.list.size; ++i) {
                auto const& b = byte_list.of.list.data[i];
                if (b.kind == WASMTIME_COMPONENT_U8) req.body.push_back(static_cast<char>(b.of.u8));
            }
        }
    }

    sandbox::HostEgressProxy const proxy;
    auto response = proxy.fetch(req, *cap);
    if (!response) {
        // The WIT `http-error` enum has exactly four cases -- a deliberately small, guest-facing
        // vocabulary (wit/ae-tool.wit's own comment: the guest only needs to know "not allowlisted",
        // "too big", "network trouble", or "timed out"). This collapses net_egress_proxy's finer
        // `error.code` (net.ambiguous_grant, net.malformed_allowlist_entry, net.scheme_unsupported,
        // net.method_not_allowed, net.header_injection_rejected, net.address_blocked,
        // net.host_unresolvable -- all "this request/target was never within what was granted") onto
        // `host-not-allowlisted`; `net.byte_cap_exceeded` maps exactly; everything else (genuine
        // socket-level failure) maps to `network-error`. Nothing currently distinguishes a real
        // connect timeout from any other socket failure (both share `net.connect_failed`), so
        // `timeout` is never emitted here rather than guessed at.
        std::string_view wit_case = "network-error";
        std::string const& code = response.error().code;
        if (code == "net.byte_cap_exceeded") {
            wit_case = "byte-cap-exceeded";
        } else if (code == "net.ambiguous_grant" || code == "net.malformed_allowlist_entry" ||
                   code == "net.scheme_unsupported" || code == "net.method_not_allowed" ||
                   code == "net.header_injection_rejected" || code == "net.address_blocked" ||
                   code == "net.host_unresolvable") {
            wit_case = "host-not-allowlisted";
        }
        results[0] = make_result_val(false, make_enum_val(wit_case));
        return nullptr;
    }

    std::vector<wasmtime_component_val_t> header_tuples;
    header_tuples.reserve(response->headers.size());
    for (auto const& [k, v] : response->headers) {
        header_tuples.push_back(make_tuple_val({make_string_val(k), make_string_val(v)}));
    }
    std::vector<wasmtime_component_val_t> body_bytes;
    body_bytes.reserve(response->body.size());
    for (unsigned char c : response->body) {
        wasmtime_component_val_t b{};
        b.kind = WASMTIME_COMPONENT_U8;
        b.of.u8 = c;
        body_bytes.push_back(b);
    }
    wasmtime_component_val_t status_val{};
    status_val.kind = WASMTIME_COMPONENT_U16;
    status_val.of.u16 = response->status;

    results[0] = make_result_val(true, make_record_val({
                                            {"status", status_val},
                                            {"headers", make_list_val(std::move(header_tuples))},
                                            {"body", make_list_val(std::move(body_bytes))},
                                        }));
    return nullptr;
}

wasmtime_error_t* cb_resolve_secret(void* env, wasmtime_context_t* ctx,
                                     wasmtime_component_func_type_t const*, wasmtime_component_val_t* args,
                                     std::size_t nargs, wasmtime_component_val_t*, std::size_t nresults) {
    if (nargs != 1 || nresults != 1) return trap_error("resolve-secret: bad arity");
    auto* call = static_cast<CallCapabilities*>(env);
    auto cap = recover_capability<cap::Secret>(ctx, &args[0], *call);
    if (!cap) return trap_error(cap.error().message.c_str());
    return trap_error("resolve-secret: not implemented in M2's minimal host");
}

// Builds a fresh linker for exactly this call's granted interfaces (ADR-010 §3.5 -- no pooling,
// no shared linker across calls) and instantiates `inst.component` against it. Every gated
// interface's callbacks close over `&call` (the caller's own stack-local CallCapabilities), so
// `call` must outlive both the returned instance and every func_call made against it.
result<std::pair<wasmtime_component_linker_t*, wasmtime_component_instance_t>> instantiate_locked(
        WasmBackend::Instance& inst, wasmtime_context_t* wctx, CallCapabilities& call) {
    wasmtime_component_linker_t* linker = wasmtime_component_linker_new(shared_engine());

    auto define_instance = [&](char const* name, auto const& adder) -> result<void> {
        wasmtime_component_linker_instance_t* root = wasmtime_component_linker_root(linker);
        wasmtime_component_linker_instance_t* nested = nullptr;
        wasmtime_error_t* err = wasmtime_component_linker_instance_add_instance(
            root, name, std::strlen(name), &nested);
        wasmtime_component_linker_instance_delete(root);
        if (err != nullptr) {
            wasmtime_error_delete(err);
            return std::unexpected(error{failure_class::fatal,
                                          std::string("linker: failed to define ") + name, "wasm.link_failed"});
        }
        adder(nested);
        wasmtime_component_linker_instance_delete(nested);
        return {};
    };

    auto fail = [&](error e) -> result<std::pair<wasmtime_component_linker_t*, wasmtime_component_instance_t>> {
        wasmtime_component_linker_delete(linker);
        return std::unexpected(std::move(e));
    };

    if (auto r = define_instance("ae:tool/base@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
            wasmtime_component_linker_instance_add_func(i, "log", 3, cb_log, &call, nullptr);
            wasmtime_component_linker_instance_add_func(i, "record-metric", 13, cb_record_metric, &call,
                                                         nullptr);
        });
        !r) {
        return fail(r.error());
    }

    // `ae:tool/capability@1.0.0` declares the `capability-handle` resource type every gated
    // interface's functions reference (ADR-010 §6) -- a *type* definition, not a function, so it
    // needs wasmtime_component_linker_instance_add_resource, not add_func. The resource type passed
    // to add_resource must be a HOST-DEFINED type (wasmtime_component_resource_type_new_host), not
    // the component's own introspected type object -- "two host resources with different ty
    // arguments are considered not-equal" (types/resource.h); reusing the component's own
    // introspected type produced "mismatched resource types" traps at call time, a real bug this
    // project's own tests caught (ADR-010 §7.5). `kCapabilityHandleType` is the SAME tag used by
    // every wasmtime_component_resource_host_new() call in invoke_tool() below -- that identity, not
    // the introspected type, is what makes a value type-check as this resource. Always defined
    // (like `base`), regardless of whether this particular component imports it.
    if (auto r = define_instance("ae:tool/capability@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
            wasmtime_component_resource_type_t* rty = wasmtime_component_resource_type_new_host(kCapabilityHandleType);
            wasmtime_component_linker_instance_add_resource(i, "capability-handle",
                                                             std::strlen("capability-handle"), rty,
                                                             cb_capability_handle_drop, &call, nullptr);
            wasmtime_component_resource_type_delete(rty);
        });
        !r) {
        return fail(r.error());
    }

    for (interface_class cls : inst.import_classes) {
        result<void> r;
        switch (cls) {
            case interface_class::fs:
                r = define_instance("ae:tool/fs@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
                    wasmtime_component_linker_instance_add_func(i, "fs-read", 7, cb_fs_read, &call, nullptr);
                    wasmtime_component_linker_instance_add_func(i, "fs-write", 8, cb_fs_write, &call, nullptr);
                });
                break;
            case interface_class::http:
                r = define_instance("ae:tool/http@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
                    wasmtime_component_linker_instance_add_func(i, "http-request", 12, cb_http_request,
                                                                 &call, nullptr);
                });
                break;
            case interface_class::secrets:
                r = define_instance("ae:tool/secrets@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
                    wasmtime_component_linker_instance_add_func(i, "resolve-secret", 14, cb_resolve_secret,
                                                                 &call, nullptr);
                });
                break;
            case interface_class::clock:
                r = define_instance("ae:tool/clock@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
                    wasmtime_component_linker_instance_add_func(i, "now-unix-millis", 15, cb_now_unix_millis,
                                                                 &call, nullptr);
                });
                break;
            case interface_class::random:
                r = define_instance("ae:tool/random@1.0.0", [&](wasmtime_component_linker_instance_t* i) {
                    wasmtime_component_linker_instance_add_func(i, "random-bytes", 12, cb_random_bytes,
                                                                 &call, nullptr);
                });
                break;
            default:
                break;  // always_ok/unimplemented/unknown never appear in import_classes (load_component
                        // already rejected them before this function is ever reached)
        }
        if (!r) return fail(r.error());
    }

    wasmtime_component_instance_t component_instance{};
    wasmtime_error_t* err =
        wasmtime_component_linker_instantiate(linker, wctx, inst.component, &component_instance);
    if (err != nullptr) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(err, &msg);
        std::string message(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        return fail(error{failure_class::contract, "instantiate failed: " + message, "wasm.instantiate_failed"});
    }
    return std::make_pair(linker, component_instance);
}

}  // namespace

result<std::vector<ToolDescriptor>> WasmBackend::list_tools(SandboxHandle const& handle, EffectContext&) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end() || !it->second->loaded) {
        return std::unexpected(error{failure_class::contract, "component not loaded", "wasm.not_loaded"});
    }
    Instance& inst = *it->second;

    // No capability calls happen while merely listing tools -- an empty, harmless capability table.
    CallCapabilities empty_call{};
    std::vector<BoundCapability> no_caps;
    empty_call.bound = &no_caps;

    wasmtime_store_t* store = wasmtime_store_new(shared_engine(), &empty_call, nullptr);
    wasmtime_context_t* wctx = wasmtime_store_context(store);
    if (inst.limits.memory_bytes > 0) {
        wasmtime_store_limiter(store, static_cast<std::int64_t>(inst.limits.memory_bytes), -1, -1, -1, -1);
    }
    wasmtime_context_set_epoch_deadline(wctx, ticks_for(inst.limits.wall_ms));

    auto instantiate_result = instantiate_locked(inst, wctx, empty_call);
    if (!instantiate_result) {
        wasmtime_store_delete(store);
        return std::unexpected(instantiate_result.error());
    }
    auto [linker, component_instance] = *instantiate_result;

    // Exports nest the same way imports do: the world exports the `guest` interface as one
    // top-level instance (`ae:tool/guest@1.0.0`), and `list-tools`/`invoke` are looked up within it
    // -- not as flat top-level names (a real bug this project's own tests caught, ADR-010 §7.5).
    wasmtime_component_export_index_t* guest_index = wasmtime_component_instance_get_export_index(
        &component_instance, wctx, nullptr, "ae:tool/guest@1.0.0", std::strlen("ae:tool/guest@1.0.0"));
    wasmtime_component_export_index_t* export_index =
        guest_index != nullptr ? wasmtime_component_instance_get_export_index(
                                      &component_instance, wctx, guest_index, "list-tools",
                                      std::strlen("list-tools"))
                                : nullptr;
    wasmtime_component_func_t func{};
    bool const found =
        export_index != nullptr &&
        wasmtime_component_instance_get_func(&component_instance, wctx, export_index, &func);
    if (guest_index != nullptr) wasmtime_component_export_index_delete(guest_index);
    if (export_index != nullptr) wasmtime_component_export_index_delete(export_index);
    if (!found) {
        wasmtime_component_linker_delete(linker);
        wasmtime_store_delete(store);
        return std::unexpected(
            error{failure_class::contract, "component does not export list-tools", "wasm.missing_export"});
    }

    wasmtime_component_val_t result_val{};
    wasmtime_error_t* err = wasmtime_component_func_call(&func, wctx, nullptr, 0, &result_val, 1);
    wasmtime_component_linker_delete(linker);
    if (err != nullptr) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(err, &msg);
        std::string message(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        wasmtime_store_delete(store);
        return std::unexpected(error{failure_class::fatal, "list-tools trapped: " + message, "wasm.trap"});
    }

    std::vector<ToolDescriptor> out;
    if (result_val.kind == WASMTIME_COMPONENT_LIST) {
        for (std::size_t i = 0; i < result_val.of.list.size; ++i) {
            auto const& item = result_val.of.list.data[i];
            ToolDescriptor d;
            d.name = string_field(item, "name");
            d.description = string_field(item, "description");
            d.args_schema_json = string_field(item, "args-schema-json");
            d.result_schema_json = string_field(item, "result-schema-json");
            d.parallelizable = bool_field(item, "parallelizable");
            out.push_back(std::move(d));
        }
    }
    wasmtime_component_val_delete(&result_val);
    wasmtime_store_delete(store);
    return out;
}

result<ToolResult> WasmBackend::invoke_tool(SandboxHandle const& handle, ToolInvokeRequest const& request,
                                             EffectContext& ctx) {
    auto it = instances_.find(handle.opaque_id);
    if (it == instances_.end() || !it->second->loaded) {
        return std::unexpected(error{failure_class::contract, "component not loaded", "wasm.not_loaded"});
    }
    Instance& inst = *it->second;

    // Bind exactly the capabilities this component's manifest requested (never the operator's whole
    // grant) -- ADR-010 §3.2. Revoked unconditionally before returning, mirroring 006 §3 step 10.
    std::vector<BoundCapability> bound;
    for (Capability const& requirement : inst.manifest.requested_capabilities) {
        auto b = inst.operator_grant.bind(requirement);
        if (!b) return std::unexpected(b.error());
        bound.push_back(std::move(*b));
    }
    auto revoke_all = [&bound] {
        for (auto const& b : bound) b.revoke();
    };

    CallCapabilities call{};
    call.bound = &bound;

    wasmtime_store_t* store = wasmtime_store_new(shared_engine(), &call, nullptr);
    wasmtime_context_t* wctx = wasmtime_store_context(store);
    if (inst.limits.memory_bytes > 0) {
        wasmtime_store_limiter(store, static_cast<std::int64_t>(inst.limits.memory_bytes), -1, -1, -1, -1);
    }
    wasmtime_context_set_epoch_deadline(wctx, ticks_for(inst.limits.wall_ms));

    auto instantiate_result = instantiate_locked(inst, wctx, call);
    if (!instantiate_result) {
        revoke_all();
        wasmtime_store_delete(store);
        return std::unexpected(instantiate_result.error());
    }
    auto [linker, component_instance] = *instantiate_result;

    wasmtime_component_export_index_t* guest_index = wasmtime_component_instance_get_export_index(
        &component_instance, wctx, nullptr, "ae:tool/guest@1.0.0", std::strlen("ae:tool/guest@1.0.0"));
    wasmtime_component_export_index_t* export_index =
        guest_index != nullptr
            ? wasmtime_component_instance_get_export_index(&component_instance, wctx, guest_index,
                                                             "invoke", std::strlen("invoke"))
            : nullptr;
    wasmtime_component_func_t func{};
    bool const found =
        export_index != nullptr &&
        wasmtime_component_instance_get_func(&component_instance, wctx, export_index, &func);
    if (guest_index != nullptr) wasmtime_component_export_index_delete(guest_index);
    if (export_index != nullptr) wasmtime_component_export_index_delete(export_index);
    if (!found) {
        wasmtime_component_linker_delete(linker);
        revoke_all();
        wasmtime_store_delete(store);
        return std::unexpected(
            error{failure_class::contract, "component does not export invoke", "wasm.missing_export"});
    }

    // Build this call's capability-handle resources -- one per bound capability, `rep` is its index
    // into `bound` (recover_capability() above trusts this indexing, guest cannot forge it). Each
    // `wasmtime_component_resource_any_t*` is embedded directly into `cap_vals`/`request_val`, whose
    // eventual wasmtime_component_val_delete(&request_val) frees it recursively -- no separate
    // tracking/delete needed here (an earlier version of this function double-freed by doing both,
    // caught as a real crash, ADR-010 §7.5). `host_handles` (the pre-conversion objects) are a
    // distinct allocation `resource_host_to_any` does not consume, and are freed after the call.
    std::vector<wasmtime_component_resource_host_t*> host_handles;
    std::vector<wasmtime_component_val_t> cap_vals;
    for (std::size_t i = 0; i < bound.size(); ++i) {
        auto* h = wasmtime_component_resource_host_new(/*owned=*/true, static_cast<std::uint32_t>(i),
                                                         kCapabilityHandleType);
        host_handles.push_back(h);
        wasmtime_component_resource_any_t* any = nullptr;
        wasmtime_error_t* rerr = wasmtime_component_resource_host_to_any(wctx, h, &any);
        if (rerr != nullptr) {
            wasmtime_error_delete(rerr);
            continue;
        }
        cap_vals.push_back(make_resource_val(any));
    }

    wasmtime_component_val_t request_val = make_record_val({
        {"tool-name", make_string_val(request.tool_name)},
        {"args-json", make_string_val(request.args_json)},
        {"capabilities", make_list_val(std::move(cap_vals))},
        {"run-id", make_string_val(ctx.trace_id)},
        {"span-id", make_string_val(ctx.span_id)},
    });

    wasmtime_component_val_t result_val{};
    wasmtime_error_t* err = wasmtime_component_func_call(&func, wctx, &request_val, 1, &result_val, 1);
    // wasmtime_component_val_delete(&request_val) below already walks the "capabilities" list field
    // and frees each embedded wasmtime_component_resource_any_t* -- val.h's own delete contract IS
    // recursive over RESOURCE-kind sub-values despite the resource type's own separate _delete
    // function existing (that function is for resource_any values NOT embedded in a val tree, e.g.
    // ones returned standalone from a call). A separate explicit delete loop here was a real
    // double-free this project's own tests caught as a crash (ADR-010 §7.5) -- host_handles (the
    // pre-conversion objects, a distinct allocation resource_host_to_any does not consume) are the
    // only thing freed explicitly below.
    wasmtime_component_val_delete(&request_val);
    for (auto* h : host_handles) wasmtime_component_resource_host_delete(h);
    wasmtime_component_linker_delete(linker);
    revoke_all();

    if (err != nullptr) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(err, &msg);
        std::string message(msg.data, msg.size);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(err);
        wasmtime_store_delete(store);
        // A deadline trap surfaces here indistinguishably from any other trap at the wasmtime C API
        // level; the caller (tests, and eventually exec()'s ExecOutcome mapping) classifies "timeout"
        // by wall-clock measurement around this call, not by parsing the trap message.
        return std::unexpected(error{failure_class::fatal, "invoke trapped: " + message, "wasm.trap"});
    }

    ToolResult out;
    out.is_error = bool_field(result_val, "is-error");
    auto const* content_field = find_field(result_val, "content");
    if (content_field != nullptr && content_field->kind == WASMTIME_COMPONENT_LIST) {
        for (std::size_t i = 0; i < content_field->of.list.size; ++i) {
            auto const& item = content_field->of.list.data[i];
            if (item.kind != WASMTIME_COMPONENT_VARIANT) continue;
            std::string const disc = name_to_string(item.of.variant.discriminant);
            ContentItem ci;
            ci.origin = content_origin::tool;
            if (disc == "text" && item.of.variant.val != nullptr) {
                ci.value = Text{string_field(*item.of.variant.val, "text")};
                ci.tainted = bool_field(*item.of.variant.val, "tainted");
            } else if (disc == "data" && item.of.variant.val != nullptr) {
                ci.value = Data{string_field(*item.of.variant.val, "json"), std::nullopt};
                ci.tainted = true;
            } else if (disc == "blob" && item.of.variant.val != nullptr) {
                BlobRef ref;
                ref.digest = string_field(*item.of.variant.val, "digest");
                ref.media_type = string_field(*item.of.variant.val, "media-type");
                ref.store = string_field(*item.of.variant.val, "store");
                ci.value = Media{ref, ref.media_type};
                ci.tainted = true;
            } else {
                continue;
            }
            out.content.push_back(std::move(ci));
        }
    }
    wasmtime_component_val_delete(&result_val);
    wasmtime_store_delete(store);
    return out;
}

result<ExecOutcome> WasmBackend::exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx) {
    if (request.language != "ae:tool") {
        return std::unexpected(
            error{failure_class::contract, "WasmBackend::exec only accepts language \"ae:tool\"", "wasm.bad_language"});
    }
    // Minimal hand-rolled decode: {"tool_name":"...","args_json":"..."} -- this project's own
    // json::Value parser (core/json_value.hpp) is the real one; a full dependency here is deferred
    // until this adapter has a real caller (it does not yet -- see this file's header comment).
    auto extract = [&](std::string_view key) -> std::string {
        auto pos = request.source.find("\"" + std::string(key) + "\"");
        if (pos == std::string::npos) return {};
        pos = request.source.find(':', pos);
        pos = request.source.find('"', pos);
        auto end = request.source.find('"', pos + 1);
        return request.source.substr(pos + 1, end - pos - 1);
    };
    ToolInvokeRequest req{extract("tool_name"), extract("args_json")};

    auto const started = std::chrono::steady_clock::now();
    auto result = invoke_tool(handle, req, ctx);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    ExecOutcome outcome;
    if (!result) {
        auto const it = instances_.find(handle.opaque_id);
        bool const likely_timeout = it != instances_.end() &&
            elapsed >= std::chrono::milliseconds(it->second->limits.wall_ms) && it->second->limits.wall_ms > 0;
        outcome.klass = likely_timeout ? exec_outcome_class::timeout : exec_outcome_class::crash;
        outcome.stderr_text = result.error().message;
        return outcome;
    }
    outcome.klass = result->is_error ? exec_outcome_class::policy_violation : exec_outcome_class::ok;
    for (auto const& item : result->content) {
        if (auto const* t = std::get_if<Text>(&item.value)) outcome.stdout_text += t->text;
        else if (auto const* d = std::get_if<Data>(&item.value)) outcome.stdout_text += d->json;
    }
    return outcome;
}

void WasmBackend::destroy(SandboxHandle& handle) {
    instances_.erase(handle.opaque_id);
}

}  // namespace agentengine::wasm
