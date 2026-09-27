#pragma once
// Base64 for HandleRelay's socket-byte relay
// (docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md §2 items 2/3):
// `connect_send`/`connect_recv` payloads carry arbitrary guest bytes across the worker_query wire,
// which is JSON text (core/json_value.hpp strings are not a byte-safe transport on their own).
// Shared by both sides of the relay (native_jail_handle_relay.cpp, the host; python_worker_mediation.cpp,
// the worker).
//
// Since ADR-203 (#120 S8) these are wrappers over core/base64.hpp, the one base64 codec. This file used
// to argue for its own copy because the only other codec it could reach was the Message JSON codec's,
// which would have coupled the native-jail worker-mediation TUs to the content model. core/base64.hpp
// is a std-only, layer-V header with no such coupling, so that reason is gone.
//
// The decode rule is this wire's own and is STRICT (decisions/ADR-205-strict-relay-base64.md): only
// canonical padded base64 -- exactly what `encode` produces -- is accepted; a bad length, a character
// outside the alphabet (line breaks included), '=' anywhere but the last one or two positions, anything
// after the padding, and non-zero leftover bits are all rejected whole. Until ADR-205 the wire stopped at
// the first '=' and ignored the rest (`QQ==!!garbage` decoded to `A`); both sides of the relay only ever
// produce `encode` output, so no legitimate frame changed. The Message JSON and A2A codecs still decode
// leniently (`base64::decode_lenient`); they are protocol-facing and out of ADR-205's scope.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/base64.hpp"

namespace agentengine::native_jail::relay_base64 {

[[nodiscard]] inline std::string encode(std::byte const* data, std::size_t n) {
    return agentengine::base64::encode(std::span<std::byte const>(data, n));
}

[[nodiscard]] inline std::string encode(std::vector<std::byte> const& bytes) {
    return agentengine::base64::encode(bytes);
}

// Returns nullopt on anything but canonical padded base64 (`base64::decode_strict`) -- callers treat
// that as a protocol violation (RT1's own "never trust an unparseable frame" posture): the host denies
// the one connect_send/file_write call, the worker raises RuntimeError to the guest. Never a partial
// value.
[[nodiscard]] inline std::optional<std::vector<std::byte>> decode(std::string_view text) {
    return agentengine::base64::decode_strict(text);
}

}  // namespace agentengine::native_jail::relay_base64
