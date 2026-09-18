// GitHub issue #80 -- an execution surface must report which sandbox image a command actually ran in.
//
// A host could already PIN the image (`DockerExecutionSurface("alpine:3.20")`), but nothing reported back
// what that pin RESOLVED to, so a provenance manifest (node, tool version, sandbox image -- the
// requester's own use case, from AeroCoWorker's RFC 015 §4c.6, a different project's document; this
// repo's own 015 has no §4c) could only ever restate the host's own configuration. Restating a TAG is worth nothing: `alpine:latest`
// names different bytes on two machines, or on one machine a week apart. `ImageIdentifiedSurface`
// (sandbox/execution_surface.hpp) adds `image()` (the configured reference) and `image_digest()` (what it
// resolved to, or empty for "not known").
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
//        well-formed digest in place. That is the CACHING contract (`reset()`'s own comment: resolving per
//        reset measured a >50% regression on every run_command), and the check that the destroy-then-
//        create sequence does not blank it.
//   [N8] a moved-from surface no longer claims a digest, and the moved-TO one carries it -- the digest
//        describes the container that travelled with `instance_`, so it must travel with it. DISCLOSED,
//        not overclaimed: deleting the explicit `other.resolved_digest_.clear()` does NOT fail this check
//        on MSVC's std::string, because a 71-character digest is past the SSO buffer and moving it steals
//        the pointer, leaving the source empty anyway. The standard says a moved-from string is "valid but
//        unspecified", so the explicit clear turns an implementation's habit into this type's guarantee;
//        this check pins the guarantee, and would only catch its removal on a library that behaves
//        differently. A planted mutant confirmed exactly that (and no test in tree catches it).
//   [N9] a `MandatorySandboxProvider` over a surface with NO image identity reports two empty strings --
//        absence, never fabrication.

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

}  // namespace

int main() {
    // Machine safety, before anything allocates. Not asserted with an over-cap probe the way
    // test_json_dump_escape's M0 does: this test has no growth path of its own to bound, so the cap is a
    // ceiling on a mutant, not a behaviour under test -- and an over-cap probe would have to be skipped
    // under every sanitizer leg anyway (memory_cap.hpp's own header explains why).
    (void)agentengine::test_support::cap_process_memory(std::size_t{256} << 20, std::size_t{1024} << 20);

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

        // [N7] -- a second reset() destroys the first container and creates another.
        auto second = surface.reset(work);
        check(second.has_value(), "N7: a second reset() succeeds");
        check(well_formed_digest(surface.image_digest()),
              "N7: image_digest() survives a second reset() -- not blanked by destroy-then-create");
        check(surface.image_digest() == resolved,
              "N7: the digest is unchanged across resets of the same image reference");

        // [N8] -- the digest travels with the container it describes.
        std::string const before_move(surface.image_digest());
        agentengine::DockerExecutionSurface moved(std::move(surface));
        check(moved.image_digest() == before_move, "N8: a move carries the digest to the destination");
        check(moved.image() == kImage, "N8: a move carries the configured reference too");
        // NOLINTNEXTLINE(bugprone-use-after-move) -- reading the moved-FROM object is the point of N8.
        check(surface.image_digest().empty(),
              "N8: the moved-FROM surface no longer claims a digest -- its container went with instance_");
    }

    // [N9] -- no image identity means two empty strings, not an invented one.
    {
        agentengine::MandatorySandboxProvider<PlainSurface> provider;
        auto const img = provider.bound_image();
        check(img.reference.empty() && img.digest.empty(),
              "N9: a provider over a surface with no image identity reports absence, not a fabricated "
              "reference");
    }

    std::filesystem::remove_all(work, ec);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
