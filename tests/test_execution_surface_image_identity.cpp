// GitHub issue #80 -- an execution surface must report which sandbox image a command actually ran in.
//
// A host could already PIN the image (`DockerExecutionSurface("alpine:3.20")`), but nothing reported back
// what that pin RESOLVED to, so a provenance manifest (node, tool version, sandbox image -- the
// requester's own use case, from AeroCoWorker's RFC 015 §4c.6, a different project's document; this
// repo's own 015 has no §4c) could only ever restate the host's own configuration. Restating a TAG is
// worth nothing: `alpine:latest` names different bytes on two machines, or on one machine a week apart.
// `ImageIdentifiedSurface` (sandbox/execution_surface.hpp) adds `image()` (the configured reference),
// `image_digest()` (what it resolved to, or empty for "not known") and, since ADR-176 §9,
// `image_digest_kind()` -- WHAT that digest digests, so a consumer can tell whether comparing it against
// another record's digest is meaningful at all.
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- every runtime check below
// creates a REAL container. Same posture as every other real-daemon test in this suite: no special CMake
// opt-in, the test just fails cleanly when the daemon is not reachable.
//
// MEMORY: capped at 256 MiB (tests/support/memory_cap.hpp, CLAUDE.md "Machine safety"). This test's own
// peak is a few MiB -- it holds one digest string and shells out -- but every value it handles comes from
// a DAEMON's stdout, read into memory by `run_argv()`, and a `docker inspect`/`docker images ls` against a
// host with thousands of images is not a size this code chooses. `run_argv()` has its own 1 MiB per-stream
// safety cap, so the bound here is the second layer, not the first: it is what keeps a mutant planted in
// the parsing loop (the kind that grew a string quadratically and nearly took this project's development
// machine down -- see memory_cap.hpp's own header) from doing it again on a machine running the suite.
//
// The checks, and what each one exists to rule out:
//   [N1] the CONCEPT discriminates. A surface that runs commands somewhere with no image identity is a
//        legitimate `ExecutionSurface` and must NOT satisfy `ImageIdentifiedSurface` -- otherwise the
//        `if constexpr` dispatch that keeps a fabricated image out of a provenance record is dead code.
//   [N2] `image()` is the reference the host configured, verbatim.
//   [N3] `image_digest()` is EMPTY before the first `reset()`. Nothing has been created, so nothing is
//        known; the accessor must not backfill from `image()`.
//   [N4] after `reset()` it is a well-formed `sha256:<64 hex>`.
//   [N5] THE CONTROL. That digest equals what an INDEPENDENT `docker image inspect` says the configured
//        reference resolves to. A hardcoded, fabricated, or accidentally-constant value cannot pass this.
//   [N6] THE OTHER HALF OF THE CONTROL. `resolve_image_digest()` against a container that does not exist
//        returns EMPTY -- the failure path is real and reachable, not a branch that always yields a
//        digest. Without this, [N4]/[N5] could not tell "resolved" from "cannot fail".
//   [N7] a SECOND `reset()` -- which destroys the first container and creates another -- leaves the same
//        well-formed digest in place. That is the CACHING contract (`reset()`'s own comment, and
//        bench/docker_image_digest_resolution.cpp for the numbers) -- note that comment records a
//        CORRECTION: the ">50% regression on every run_command" figure this check was originally written
//        under was measured WRONG, and the surviving argument is a much smaller saving. This check pins
//        the caching BEHAVIOUR either way, which is why it did not have to change.
//   [N8] a moved-from surface no longer claims a digest, and the moved-TO one carries it -- the digest
//        describes the container that travelled with `instance_`, so it must travel with it. DISCLOSED,
//        not overclaimed: deleting the explicit `other.resolved_digest_.clear()` does NOT fail this check
//        on MSVC's std::string, because a 71-character digest is past the SSO buffer and moving it steals
//        the pointer, leaving the source empty anyway. The standard says a moved-from string is "valid but
//        unspecified", so the explicit clear turns an implementation's habit into this type's guarantee;
//        this check pins the guarantee, and would only catch its removal on a library that behaves
//        differently. A planted mutant confirmed exactly that (and no test in tree catches it).
//   [N9] a `MandatorySandboxProvider` over a surface with NO image identity reports empty strings --
//        absence, never fabrication.
//   [N10] `image_digest_kind()` is `unknown` before the first `reset()`, for the same reason [N3] holds:
//        a kind without a digest describes nothing.
//   [N11] THE CONTROL: the kind equals what an INDEPENDENT `docker image inspect
//        {{.Descriptor.mediaType}}` resolves the same image to. A hardcoded kind cannot pass it. The
//        check then BRANCHES on whether this daemon exposes that field at all: where it does, the kind
//        must be `index` for a multi-platform reference -- and `index`, NOT `manifest`, is the whole
//        point, since ADR-176 §11's round found the enum's first version spelling both the same, which
//        would have licensed comparing an index digest against a per-platform manifest digest and
//        reporting one image as two. Where it does not (CI's Linux Docker), the kind must be `unknown`
//        while the DIGEST is still resolved. Both branches assert; neither is a pass-by-default.
//   [N12] THE OTHER HALF. `unknown` is reachable, so no kind is a constant: an unrecognized media type
//        and a malformed image id both answer `unknown` rather than a guess. The media-type mapping is an
//        EXACT match against a closed list, so a daemon line that merely CONTAINS the word "manifest"
//        cannot mint a kind -- which matters because the CLI helper merges stderr into the text searched.
//   [N13] the kind travels with the digest across a move, in both directions. A kind that outlives its
//        digest would describe an image the surface no longer reports.
//
// NOT PROVEN HERE, and disclosed rather than papered over: the `config` and `manifest` kinds. `config`
// needs a Docker daemon whose image ID is a config digest AND that says so through a media type; no
// reachable daemon does both. `manifest` needs a single-platform reference; every image this suite uses
// is multi-platform. So the executed coverage is `index` (on a daemon exposing `.Descriptor`) and
// `unknown` (everywhere else) -- and which of those two a given run exercises depends on the daemon, which
// is why N11 branches and says which it took. ADR-176 §9 and §12 carry the same gaps.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"
#include "support/memory_cap.hpp"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
    std::fflush(stdout);
}

// [N1] A real, minimal `ExecutionSurface` conformer with no image identity of any kind -- the shape every
// in-tree test double already has, and the shape a future process/chroot conformer would have.
class PlainSurface {
public:
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
    [[nodiscard]] agentengine::result<agentengine::SurfaceRunOutcome> run(std::string const&) {
        return agentengine::SurfaceRunOutcome{0, ""};
    }
    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
};

static_assert(agentengine::ExecutionSurface<PlainSurface>,
              "N1: the no-image double must still be a perfectly valid ExecutionSurface");
static_assert(!agentengine::ImageIdentifiedSurface<PlainSurface>,
              "N1 CONTROL: ImageIdentifiedSurface must REJECT a surface with no image identity -- a "
              "concept satisfied by everything would make the if constexpr dispatch dead code");
static_assert(agentengine::ImageIdentifiedSurface<agentengine::DockerExecutionSurface>,
              "N1: the real Docker surface must report which image it runs");

[[nodiscard]] bool well_formed_digest(std::string_view d) {
    return d.size() == 7 + 64 && d.substr(0, 7) == "sha256:" &&
           d.find_first_not_of("0123456789abcdef", 7) == std::string_view::npos;
}

// Asks Docker itself, through a completely separate invocation, what `image` resolves to. This is what
// makes [N5] a real control rather than a restatement of the value under test.
[[nodiscard]] std::string independent_image_id(std::string const& image) {
    auto r = agentengine::docker_cli_detail::run_argv(
        {"docker", "image", "inspect", "--format", "{{.Id}}", image});
    if (r.exit_code != 0) return {};
    std::string id = r.stdout_text;
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    auto const last_newline = id.find_last_of('\n');
    if (last_newline != std::string::npos) id = id.substr(last_newline + 1);
    return id;
}

constexpr char const* kImage = "alpine:latest";

[[nodiscard]] std::string trailing_line(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    auto const last_newline = text.find_last_of('\n');
    if (last_newline != std::string::npos) text = text.substr(last_newline + 1);
    return text;
}

// [N11] The control behind the KIND. Asks Docker, through a completely separate invocation, for the media
// type of the descriptor the image ID names -- the same question the backend asks, asked independently, so
// a hardcoded or accidentally-constant kind cannot pass.
//
// This replaced a RepoDigests-based oracle that ADR-176 §11's red-team round broke: membership in
// RepoDigests cannot tell an image INDEX from a per-platform MANIFEST, so it would have confirmed a kind
// that erased the distinction the enum exists to carry.
[[nodiscard]] std::string independent_media_type(std::string const& image) {
    auto r = agentengine::docker_cli_detail::run_argv(
        {"docker", "image", "inspect", "--format", "{{.Descriptor.mediaType}}", image});
    if (r.exit_code != 0) return {};
    return trailing_line(r.stdout_text);
}


}  // namespace

int main() {
    // Machine safety, before anything allocates. Not asserted with an over-cap probe the way
    // test_json_dump_escape's M0 does: this test has no growth path of its own to bound, so the cap is a
    // ceiling on a mutant, not a behaviour under test -- and an over-cap probe would have to be skipped
    // under every sanitizer leg anyway (memory_cap.hpp's own header explains why).
    bool const capped =
        agentengine::test_support::cap_process_memory(std::size_t{256} << 20, std::size_t{1024} << 20);
    if (!capped) {
        // Said out loud rather than left as a header claim that quietly stops being true. The header says
        // this test is capped at 256 MiB; on a POSIX sanitizer leg, or a kernel where RLIMIT_DATA does not
        // bound mmap, it is not. Not a failure -- the checks below do not need the cap -- but a reader of
        // the log should not have to know memory_cap.hpp's internals to find that out.
        std::printf("[note] memory cap NOT in force -- this run is uncapped (see memory_cap.hpp)\n");
    }

    std::error_code ec;
    std::filesystem::path const work =
        std::filesystem::temp_directory_path(ec) / "ae_image_identity_probe";
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);
    if (ec) {
        std::printf("[FAIL] cannot create the probe work directory: %s\n", ec.message().c_str());
        return 1;
    }

    {
        agentengine::DockerExecutionSurface surface(kImage);

        // [N2] / [N3] -- before anything has been created.
        check(surface.image() == kImage, "N2: image() is the configured reference, verbatim");
        check(surface.image_digest().empty(),
              "N3: image_digest() is empty before reset() -- 'not known', never backfilled from image()");
        check(surface.image_digest_kind() == agentengine::ImageDigestKind::unknown,
              "N10: image_digest_kind() is unknown before reset() -- a kind without a digest describes "
              "nothing");

        auto first = surface.reset(work);
        if (!first.has_value()) {
            std::printf("[FAIL] reset() failed (is a Docker daemon reachable?): %s\n",
                        first.error().message.c_str());
            return 1;
        }

        // [N4] / [N5]
        std::string const resolved(surface.image_digest());
        check(well_formed_digest(resolved),
              "N4: after reset(), image_digest() is a well-formed sha256:<64 hex> -- got '" + resolved + "'");
        std::string const independent = independent_image_id(kImage);
        check(!independent.empty() && resolved == independent,
              "N5 CONTROL: the reported digest equals what an INDEPENDENT `docker image inspect` resolves "
              "the same reference to ('" + independent + "')");
        check(surface.image() == kImage, "N2: image() still reports the configured reference after reset()");

        // [N6] -- the failure path is real.
        agentengine::DockerCliBackend backend;
        std::string const absent =
            backend.resolve_image_digest(agentengine::DockerCliBackend::Instance{
                "ae_no_such_container_for_issue_80"});
        check(absent.empty(),
              "N6 CONTROL: resolve_image_digest() against a container that does not exist returns empty -- "
              "the 'not known' path is reachable, so N4/N5 are not a branch that always yields a digest");

        // [N11] -- the kind, and the independent measurement that makes the answer mean something.
        //
        // BRANCHED ON THE DAEMON'S CAPABILITY, not hardcoded, and that is a correction: the first version
        // asserted `index` unconditionally and went red on CI, whose Linux Docker does not populate
        // `.Descriptor` at all. The backend behaved correctly there -- it reported `unknown`, the
        // documented degradation -- and the CONTROL below passed, agreeing with the independent oracle.
        // Only the hardcoded expectation was wrong. Both branches assert something real, so this cannot
        // become a check that passes whatever happens.
        std::string const media_type = independent_media_type(kImage);
        check(surface.image_digest_kind() == agentengine::image_digest_kind_from_media_type(media_type),
              "N11 CONTROL: the reported kind equals what an INDEPENDENT `docker image inspect "
              "{{.Descriptor.mediaType}}` resolves the same image to -- measured, not hardcoded "
              "(media type: '" + media_type + "')");
        if (media_type.empty()) {
            // A daemon with no `.Descriptor` field. The kind is not knowable, and the contract is that an
            // unknowable kind is `unknown` -- while the DIGEST is still resolved, so the degradation is
            // partial rather than total. Both halves are asserted; neither is assumed.
            check(surface.image_digest_kind() == agentengine::ImageDigestKind::unknown,
                  "N11: this daemon exposes no descriptor media type, so the kind is `unknown` -- never "
                  "guessed from which image store the operator configured");
            check(!surface.image_digest().empty(),
                  "N11: ...and the DIGEST is still resolved on such a daemon -- losing the kind does not "
                  "lose the identity");
            std::printf("[note] this daemon does not expose {{.Descriptor.mediaType}}; the `index` and "
                        "`manifest` kinds are NOT exercised on this run\n");
        } else {
            check(media_type == "application/vnd.oci.image.index.v1+json",
                  "N11 SETUP: " + std::string(kImage) + " is multi-platform on this daemon, so its "
                  "descriptor is an INDEX ('" + media_type + "')");
            check(surface.image_digest_kind() == agentengine::ImageDigestKind::index,
                  "N11: after reset(), image_digest_kind() is `index` -- NOT `manifest`: an index digest "
                  "and a per-platform manifest digest name different objects and must not share a kind");
            check(agentengine::image_digest_kind_name(surface.image_digest_kind()) == "index",
                  "N11: the wire spelling of that kind is \"index\"");
        }

        // [N12] -- `unknown` is reachable, so no kind is a constant.
        check(agentengine::image_digest_kind_from_media_type("application/vnd.oci.image.layer.v1.tar") ==
                  agentengine::ImageDigestKind::unknown,
              "N12 CONTROL: an unrecognized media type maps to `unknown` -- a closed list, not a guess");
        check(agentengine::image_digest_kind_from_media_type(
                  "warning: could not read application/vnd.oci.image.manifest.v1+json") ==
                  agentengine::ImageDigestKind::unknown,
              "N12 CONTROL: a line that merely CONTAINS a known media type is `unknown` -- the match is "
              "exact, so a daemon warning on the merged stderr stream cannot mint a kind");
        check(backend.resolve_image_digest_kind("not-a-digest") == agentengine::ImageDigestKind::unknown,
              "N12 CONTROL: a malformed image id is `unknown` without reaching the daemon");
        check(agentengine::image_digest_kind_name(agentengine::ImageDigestKind::unknown).empty(),
              "N12: `unknown` serializes to the EMPTY string, matching the empty-digest convention");

        // [N7] -- a second reset() destroys the first container and creates another.
        auto second = surface.reset(work);
        check(second.has_value(), "N7: a second reset() succeeds");
        check(well_formed_digest(surface.image_digest()),
              "N7: image_digest() survives a second reset() -- not blanked by destroy-then-create");
        check(surface.image_digest() == resolved,
              "N7: the digest is unchanged across resets of the same image reference");

        // [N8] -- the digest travels with the container it describes.
        std::string const before_move(surface.image_digest());
        auto const kind_before_move = surface.image_digest_kind();
        agentengine::DockerExecutionSurface moved(std::move(surface));
        check(moved.image_digest() == before_move, "N8: a move carries the digest to the destination");
        check(moved.image() == kImage, "N8: a move carries the configured reference too");
        // [N13] -- and the kind with it. Unlike the digest, this one CANNOT pass by accident: an enum is
        // copied intact by a move, so without the explicit reset in the move constructor the moved-from
        // surface would still answer `manifest`. This is the check N8 could not be.
        check(moved.image_digest_kind() == kind_before_move,
              "N13: a move carries the digest KIND to the destination -- whatever kind this daemon "
              "supports, which is the property under test rather than the particular value");
        // NOLINTNEXTLINE(bugprone-use-after-move) -- reading the moved-FROM object is the point of N8.
        check(surface.image_digest().empty(),
              "N8: the moved-FROM surface no longer claims a digest -- its container went with instance_");
        // NOLINTNEXTLINE(bugprone-use-after-move)
        check(surface.image_digest_kind() == agentengine::ImageDigestKind::unknown,
              "N13: the moved-FROM surface no longer claims a KIND either -- a kind describing a digest "
              "the surface no longer reports is the one combination that must be impossible");
    }

    // [N9] -- no image identity means two empty strings, not an invented one.
    {
        agentengine::MandatorySandboxProvider<PlainSurface> provider;
        auto const img = provider.bound_image();
        check(img.reference.empty() && img.digest.empty() && img.digest_kind.empty(),
              "N9: a provider over a surface with no image identity reports absence, not a fabricated "
              "reference -- and no kind either");
    }

    std::filesystem::remove_all(work, ec);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
