#pragma once
// Implements ADR-102 Phase 3 (identity-native sandbox/worktree design, ADR-099 §7 A3) --
// `ExecutionSurface`, the generic "give me an isolated place, put this tree's content in it, run one
// command, give me back whatever changed" concept `SandboxRuntime` (sandbox_runtime.hpp) drives.
//
// Ported from docs/planning/proofs/execution_surface/execution_surface.hpp (ADR-099's own standalone,
// live-Docker-tested prove-phase original -- kept as-is, this is a new file). Real change made
// during the port: `probe::ExecOutcome` (the run()-outcome shape) is renamed `SurfaceRunOutcome`, not
// reused as bare `ExecOutcome` -- the real, production `agentengine::ExecOutcome`
// (sandbox/sandbox.hpp, `SandboxBackend`'s own outcome vocabulary: klass/stdout_text/stderr_text/
// result_repr/artifacts/ask_prompt, no raw exit code) already exists and answers a genuinely
// different question. `ExecutionSurface::run()` needs a real process exit code
// (`probe_execution_surface.cpp`'s own real check `r3->exec.exit_code == 7`, ported into this
// phase's own test, proves callers genuinely rely on it) -- reusing the `SandboxBackend` name for a
// shape that cannot express what it expresses would be exactly the kind of collision ADR-102 Phase
// 1's own `IdentityHandle`-vs-`Principal` decision already established the discipline for: keep a
// distinct name where the concept genuinely differs, bridge explicitly if a bridge is ever needed
// (deferred to a later phase, per ADR-101's own, separate `TreeBackendExecutionSurface` bridging
// work -- not reused here, per this phase's own explicit scope decision to stay independent of
// ADR-101, itself still Proposed/unjudged).
//
// SCOPE, matching ADR-102 Phase 3's own boundary: this concept and its `SandboxRuntime` consumer are
// deliberately NOT built as a conforming `agentengine::SandboxBackend` (`sandbox.hpp`, 008 §2a) and
// are NOT wired to any of the real backends (`NativeJailBackend`/`WasmBackend`/`KataBackend`) --
// matching ADR-099 §7's own explicit project-owner direction, carried into this port unchanged.
//
// MADE EXPLICIT (ADR-171, GitHub issue #63 step 2): that scope decision is PERMANENT until an ADR
// says otherwise, and it has a consequence a reader must not have to infer. **An `ExecutionSurface`
// conformer inherits none of RFC 008's guarantees** -- not 008 §2's mandatory resource limits, not
// §4's host-mediated egress, and above all not §2 rule 1's empty-by-default authority: `reset()`/
// `run()`/`drain_to()` take no `EffectContext` and no `CapabilitySet`, so there is nothing for a
// conformer to check a caller against, and no conformer does. Whatever authority the calling code
// already holds is the authority the contained command runs with.
//
// A conformer is therefore obliged to contain its OWN blast radius by construction (ADR-171 gave
// `DockerExecutionSurface` deny-all network plus real memory/pids/CPU ceilings by default, for
// exactly this reason), and a caller must never treat "it is an `ExecutionSurface`" as evidence that
// a command run through it was authorized. Promoting any conformer toward a real `SandboxBackend`
// owes 008 §9's full gate first (G1 parity, G2 containment with a positive control, G3
// no-ambient-authority probe, G4 teardown).

#include <concepts>
#include <filesystem>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"

namespace agentengine {

// The real outcome of ONE command run inside an ExecutionSurface -- a raw process exit code (a
// non-zero value is a normal, meaningful RESULT, never itself a `result<>`-level error) plus
// whatever the command wrote to stdout/stderr (merged, matching the real conformer's own `_popen`-
// based capture convention).
struct SurfaceRunOutcome {
    int exit_code = -1;
    std::string stdout_text;
};

// T::reset(host_dir)   -- wipe any prior execution state, then materialize `host_dir`'s CURRENT real
//                          content into the surface's own isolated view. Idempotent: calling it
//                          again re-synchronizes from `host_dir`'s latest content.
// T::run(command)      -- execute `command` INSIDE the surface's isolation boundary, never in this
//                          process. A non-zero `SurfaceRunOutcome::exit_code` is a normal, meaningful
//                          result (the contained command failed), not a `result<>` error -- only a
//                          failure to even ATTEMPT execution (the surface itself is broken) is.
// T::drain_to(host_dir) -- pull everything the surface's own view currently holds back onto real
//                          disk at `host_dir`, overwriting whatever was there. The caller is
//                          responsible for scanning `host_dir` afterward (`RealIoFileSystem::
//                          scan_and_drain_into_tree()`) to turn real bytes into a real, committed
//                          `Tree` -- this concept's own job ends at "the bytes are back on real
//                          disk."
template <class T>
concept ExecutionSurface = requires(T& t, std::filesystem::path const& host_dir,
                                       std::string const& command) {
    { t.reset(host_dir) } -> std::same_as<agentengine::result<void>>;
    { t.run(command) } -> std::same_as<agentengine::result<SurfaceRunOutcome>>;
    { t.drain_to(host_dir) } -> std::same_as<agentengine::result<void>>;
};

// GitHub issue #80 -- WHICH IMAGE DID THIS ACTUALLY RUN IN. A host could already PIN a surface's image
// (`DockerExecutionSurface("alpine:3.20")`), but nothing reported back what the pin resolved to, so a
// provenance record (the requester's own per-piece manifest -- node, tool version, sandbox image; the
// cited "015 §4c.6" is AeroCoWorker's RFC, a DIFFERENT project's document, not this repo's own 015,
// which has no §4c) could only ever restate the reference the host itself configured. That is worth
// nothing when the reference was a TAG: `alpine:latest` names a different set of bytes on two machines,
// or on one machine a week apart.
//
// A SEPARATE, OPT-IN refinement of `ExecutionSurface`, deliberately not folded into it: a surface that
// runs commands somewhere with no image identity at all (every in-tree test double, and any future
// process/chroot conformer) is a perfectly valid `ExecutionSurface`, and forcing it to invent an answer
// would make the field a lie rather than an absence. Callers ask with `if constexpr` and record nothing
// when the answer does not exist.
//
//   T::image()        -- the reference the surface was CONFIGURED with, verbatim. Never empty for a
//                          conformer; it is the constructor argument.
//   T::image_digest() -- the content-addressed identity of the image this surface runs, as the backend
//                          reported it at the moment the surface's FIRST execution environment was
//                          created. EMPTY, never fabricated, when nothing has been created yet or the
//                          backend could not answer -- an empty digest means "not known", and a caller
//                          must record its absence rather than substitute `image()`.
//
//                          "FIRST", not "current", and the distinction is a real one a conformer may not
//                          quietly narrow. A surface that re-creates its environment per command (both
//                          in-tree conformers do; `SandboxRuntime::run()` calls `reset()` once per tool
//                          call) resolves this ONCE and reuses it, because resolving per command costs a
//                          CLI round trip on every call to re-derive a value that only something outside
//                          this process can change. So from the second command onward the value names
//                          the image the first environment ran, which is the same image unless the tag
//                          was re-pulled out of process -- stale then, never fabricated. An earlier
//                          version of this contract said "CURRENT ... at the moment that environment was
//                          created", which the caching made false; ADR-176 §6 carries the residual and
//                          §9 the correction.
//   T::image_digest_kind() -- WHAT that digest digests, so a consumer can tell whether two digests are
//                          even candidates for comparison. See `ImageDigestKind`.
//
// COMPARE DIGESTS ONLY WHEN THE KINDS MATCH, and never when either is `unknown`. Every conformer answers
// `sha256:<64 hex>`, but they do not all digest the same thing, and a bare digest carries no tag saying
// which -- so `image_digest_kind()` exists to make the question answerable instead of leaving a consumer
// to guess from the surface type. ADR-176 §6 originally left this as follow-on work and named the
// guessing as the residual; ADR-176 §9 closes it, because a provenance field whose comparability a
// reader has to infer is a field that will eventually be compared wrongly.
//
// Matching kinds make a comparison MEANINGFUL; they do not make two records the same image. Two hosts can
// legitimately hold different objects for one reference -- one the index, one a single platform's
// manifest -- and those are different kinds, so the rule declines the comparison rather than answering it
// wrongly. What the rule guarantees is the direction that matters for provenance: when the kinds match
// and the digests differ, the images really are different.
//
// I3: a conformer's answers are serialized into tool replies the model reads. A host assembling a
// provenance record must take them from the surface or from the reply struct it received -- never from a
// digest restated in model output, which is an ordinary untrusted string like any other.

// GitHub issue #80 / ADR-176 §9 -- what an `image_digest()` actually digests.
//
// The two values are NOT interchangeable and do not agree for one image: a `manifest` digest names the
// object a registry hands back for a reference, a `config` digest names the image's config blob. A
// consumer comparing across kinds gets "different image" for one image.
//
//   index    -- the digest of an image INDEX (a manifest list): the object a MULTI-PLATFORM reference
//               resolves to, which names one manifest per platform.
//   manifest -- the digest of a single image MANIFEST: one platform's layers plus its config.
//   config   -- the digest of the image's CONFIG BLOB. Docker documents its image ID as this under the
//               classic graph driver.
//   unknown  -- no digest, or a digest whose kind the backend could not ESTABLISH. It does NOT mean "not
//               a manifest digest": it means nothing was measured. A consumer must not compare an
//               `unknown` against anything, including another `unknown`.
//
// INDEX AND MANIFEST ARE SEPARATE KINDS, and collapsing them was a real defect in this enum's first
// version, caught by ADR-176 §11's red-team round. They are digests of two DIFFERENT objects for one
// image -- an index names the manifests, so its digest can never equal any of theirs. Spelling both
// "manifest" made the rule above ("compare only when the kinds match") license exactly the false
// "different image" verdict this type exists to prevent, for one image recorded from a host holding the
// index and a host holding one platform's manifest. The distinction costs nothing to keep: every backend
// reads it from a media type that already states it.
enum class ImageDigestKind { unknown, index, manifest, config };

// The wire spelling, for a reply struct or an audit record. `unknown` is the EMPTY string, matching the
// empty-digest convention: absence is absence, never the word "unknown" masquerading as a value.
[[nodiscard]] constexpr std::string_view image_digest_kind_name(ImageDigestKind kind) noexcept {
    switch (kind) {
        case ImageDigestKind::index:    return "index";
        case ImageDigestKind::manifest: return "manifest";
        case ImageDigestKind::config:   return "config";
        case ImageDigestKind::unknown:  break;
    }
    return "";
}

// The one place a media type becomes a kind, shared by every conformer so two backends cannot disagree
// about what `application/vnd.oci.image.index.v1+json` means.
//
// EXACT match against a closed list, never a substring test, and that is a correctness requirement rather
// than tidiness. A backend reads this string out of a CLI's output, and the CLI helpers in this tree MERGE
// stderr into the text they return -- so the candidate string can be any line a daemon chose to emit. A
// substring rule ("contains manifest") would let a warning mentioning a media type mint a confident kind
// for a digest nobody measured. An exact match against known types cannot: an unrecognized string is
// `unknown`, which is the honest answer and the safe one. This closed list is also why adding a new
// registry media type is a deliberate edit here rather than an accident somewhere else.
//
// The list is a NAMED TABLE rather than a chain of `||`s, and that is ADR-176 §13's doing: the test that
// proves this mapping used to carry its own hand-copy of the same eight strings. The two were identical
// when written and a red-team pass diffed them entry by entry to confirm it -- but the duplication could
// only ever catch a REMOVAL or a RE-MAPPING. Add a ninth entry here and the test would have stayed green
// while "proven exhaustively" quietly stopped being true, and an addition is precisely how a wrong kind
// would enter. One table, iterated by the mapper and read by the test, cannot drift from itself.
struct ImageDigestKindMapping {
    std::string_view media_type;
    ImageDigestKind kind;
};

inline constexpr ImageDigestKindMapping kImageDigestKindMediaTypes[] = {
    {"application/vnd.oci.image.index.v1+json", ImageDigestKind::index},
    {"application/vnd.docker.distribution.manifest.list.v2+json", ImageDigestKind::index},
    {"application/vnd.oci.image.manifest.v1+json", ImageDigestKind::manifest},
    {"application/vnd.docker.distribution.manifest.v2+json", ImageDigestKind::manifest},
    {"application/vnd.docker.distribution.manifest.v1+json", ImageDigestKind::manifest},
    {"application/vnd.docker.distribution.manifest.v1+prettyjws", ImageDigestKind::manifest},
    // ADR-176 §13: no backend in this tree has been observed to emit either config spelling -- both
    // read the media type of an image's TARGET descriptor, which is an index or a manifest. They stay
    // because this is a shared OCI mapper: answering `unknown` for an unambiguous config media type
    // would lie to the next backend rather than merely being incomplete.
    {"application/vnd.oci.image.config.v1+json", ImageDigestKind::config},
    {"application/vnd.docker.container.image.v1+json", ImageDigestKind::config},
};

[[nodiscard]] constexpr ImageDigestKind image_digest_kind_from_media_type(
    std::string_view media_type) noexcept {
    for (auto const& mapping : kImageDigestKindMediaTypes) {
        if (media_type == mapping.media_type) return mapping.kind;
    }
    return ImageDigestKind::unknown;
}

// ADR-176 §14/§15. What a tool reply must look like to CARRY image provenance: three plain, assignable
// `std::string` members. Named here, rather than written inline at the one place that checks it, for two
// reasons -- `MandatorySandboxProvider::stamp_image_provenance()` `static_assert`s on it, and
// `tests/compile_fail/image_provenance_unstampable_reply.cpp` proves a non-conforming reply is REJECTED
// while its positive control proves a conforming one still builds. A requirement that only exists inside
// an `if constexpr` cannot be shown to reject anything.
//
// Plain `std::string` deliberately, and not merely "assignable from a string". `std::optional<std::string>`
// and `Described<std::string, "...">` both publish `{"type":"string"}` in the reply schema, so neither is
// distinguishable on the wire -- but the first is absent from the schema's `required` list when unset and
// serializes away entirely, and both defeat an `empty()`-based "has this already been answered" test. A
// provenance field is not a place for a spelling that can vanish.
template <class R>
concept ImageProvenanceReply = requires(R& r) {
    { r.image } -> std::same_as<std::string&>;
    { r.image_digest } -> std::same_as<std::string&>;
    { r.image_digest_kind } -> std::same_as<std::string&>;
};

template <class T>
concept ImageIdentifiedSurface = ExecutionSurface<T> && requires(T const& t) {
    { t.image() } -> std::convertible_to<std::string_view>;
    { t.image_digest() } -> std::convertible_to<std::string_view>;
    { t.image_digest_kind() } -> std::same_as<ImageDigestKind>;
};

}  // namespace agentengine
