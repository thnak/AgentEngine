#pragma once
// Implements the jailed-Python-worker design's IPC transport (008-Sandbox-and-Isolation.md §1b/§3's
// native-jail layer-3 backstop, wired for real per the ADR superseding native_jail_backend.hpp's
// former "Correction (2026-08-23)" comment). `FramedChannel`: generic length-prefixed JSON framing
// over the two anonymous pipe pairs `NativeJailBackend::create_python_worker()` creates — the ONLY
// channel Slice 1 needs (call_tool relay, exec/init/refresh/shutdown control messages). `HandleRelay`
// (real file-handle transfer for the deferred §7/§9 open()/socket() relays) is Slice-2 scope and is
// deliberately NOT declared here — nothing in Slice 1 calls it, and declaring an unimplemented type
// with no caller would be dead surface, not forward-compatibility (the two-mechanism split this
// class's own header comment documents — pipes for framing, a SEPARATE mechanism for handle transfer
// — is what a Slice-2 pass adds alongside this file, not inside it).
//
// Compiled into BOTH `agentengine_native_jail_backend` (the host side, `native_jail_backend.cpp`) and
// `agentengine_python_worker` (the worker side, `python_worker_main.cpp`/`python_worker_mediation.cpp`)
// — two separate binaries, each getting its own copy of this small TU; not a shared library, so there
// is no ODR concern between them.
//
// Framing: a 4-byte little-endian length prefix (payload byte count, capped at kMaxFrameBytes to
// bound host memory against a compromised/malfunctioning peer — 008 §2 item 2's own "unbounded ...
// is a denial-of-service" concern, applied to the control channel itself, not just script stdout),
// followed by that many bytes of UTF-8 JSON text (`agentengine::json`, core/json_value.hpp — this
// project's existing dependency-free JSON type, never a third-party parser in this pair of binaries).

#include <cstdint>

#include <windows.h>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::native_jail {

// Payload byte cap per frame -- 64 MiB, matching the final spec's own stated bound. A single
// `exec_request`/`exec_response` carrying more than this is already well past what this project's
// output-discipline caps (`output_discipline.hpp`) allow through in practice; this is a hard transport
// ceiling, not the same knob as `output_cap_bytes`.
inline constexpr std::uint32_t kMaxFrameBytes = 64u * 1024 * 1024;

// Does NOT own `read_handle`/`write_handle` -- the caller (NativeJailBackend on the host side,
// python_worker_main on the worker side) owns and closes its own pipe handles; this class only ever
// reads/writes through them. Neither handle needs to be provided if the channel is send-only or
// recv-only from a given call site (pass INVALID_HANDLE_VALUE for the unused direction), but both
// Slice-1 call sites are full-duplex.
class FramedChannel {
public:
    FramedChannel(HANDLE read_handle, HANDLE write_handle)
        : read_handle_(read_handle), write_handle_(write_handle) {}

    // Serializes `msg` and writes the length-prefixed frame. Blocking; loops WriteFile to handle a
    // partial write (the Windows pipe-write contract does not guarantee one WriteFile call drains the
    // whole buffer for a large message).
    [[nodiscard]] result<void> send(json::Value const& msg) const;

    // Blocks for the next frame. Returns an error (not a crash) on a broken pipe (peer process died
    // or closed its end) -- this IS `exec_session()`'s own "the worker died mid-call" detection path
    // (native_jail_backend.cpp), not a distinct failure mode to special-case at every call site.
    [[nodiscard]] result<json::Value> recv() const;

private:
    HANDLE read_handle_;
    HANDLE write_handle_;
};

}  // namespace agentengine::native_jail
