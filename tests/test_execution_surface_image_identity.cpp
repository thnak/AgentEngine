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
//   [N14] THE DISCRIMINATION, and the check that retired "manifest is unproven". A `docker commit` image
//        is single-platform by construction, so the SAME accessor must answer `index` for the
//        multi-platform reference and `manifest` for the committed one -- one process, one daemon,
//        different digests. Before this, every run this suite had ever produced saw `index` and nothing
//        else, so a backend ignoring the daemon and returning `index` unconditionally would have passed
//        every check above, N11's control included: agreeing with an independent oracle about ONE image
//        cannot tell a lookup from a constant that happens to be right.
//   [N15] the mapper itself, exhaustively over ADR-176 §9's closed list -- both CONFIG spellings
//        included -- with truncated, extended and space-prefixed near-misses as controls, so the
//        exactness that stops a daemon warning from minting a kind is proven per spelling.
//
// NOT PROVEN HERE, and disclosed rather than papered over: a `config` kind arriving from a real backend.
// ADR-176 §13 measures WHY, and it is not this suite's choice of images: what both backends report is the
// media type of an image's TARGET descriptor, which is an index or a manifest, while the one daemon whose
// image ID really is a config digest (the classic graph driver) exposes no `.Descriptor` to say so and is
// therefore answered `unknown` rather than inferred. N15 proves the mapping; nothing here proves a
// producer. Which of `index`/`manifest` versus `unknown` a given run exercises still depends on the
// daemon, which is why N11 and N14 both branch and say which they took.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
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

// [N14] Names the daemon in the log when it cannot answer the kind question. ADR-176 §13 claims a
// SHAPE of daemon behaves this way; without this, the claim rests on which CI runner happened to be
// green that week, and a reader six months from now cannot tell whether the shape changed.
[[nodiscard]] std::string daemon_shape() {
    auto driver = agentengine::docker_cli_detail::run_argv(
        {"docker", "info", "--format", "{{.Driver}}"});
    auto version = agentengine::docker_cli_detail::run_argv(
        {"docker", "version", "--format", "{{.Server.Version}}"});
    return "storage driver '" + (driver.exit_code == 0 ? trailing_line(driver.stdout_text) : "?") +
           "', server " + (version.exit_code == 0 ? trailing_line(version.stdout_text) : "?");
}

// [N14] A locally COMMITTED image -- the one thing a developer machine can produce whose descriptor is a
// single-platform MANIFEST rather than an index. `docker commit` writes one manifest for one platform;
// there is nothing for an index to point at.
//
// This is what closes ADR-176 §6's "the manifest kind is unproven" residual. Before it, every image the
// suite touched was multi-platform, so `index` was the only kind any run had ever observed and the enum's
// central distinction had never once been exercised end to end.
constexpr char const* kCommittedImage = "ae_esii_manifest_probe:latest";

// The source container's name follows `DockerCliBackend::create()`'s OWN convention --
// `ae_des_<pid>_<start_key>_<suffix>` -- and that is a §15 correction, not decoration. A fixed name had
// two defects: `DockerCliBackend::reap_orphans()` matches only `kOrphanNamePrefix`, so a leaked probe
// container could never be cleaned by the machinery that exists for exactly that; and two processes
// running this binary at once (a developer's direct run alongside a `ctest` invocation -- `RESOURCE_LOCK`
// only serializes WITHIN one invocation) would collide, with each `docker rm -f` killing the other's
// container mid-commit. Embedding pid+start_key makes the name unique per process AND reapable.
[[nodiscard]] std::string commit_source_container() {
    return std::string(agentengine::docker_cli_detail::kOrphanNamePrefix) +
           std::to_string(agentengine::docker_cli_detail::current_pid()) + "_" +
           std::to_string(agentengine::docker_cli_detail::current_process_start_key()) + "_esii_commit";
}

// `run_argv` is [[nodiscard]] and these calls genuinely do not care: a leftover from a previous run may
// or may not exist, and either outcome is fine. Named and discarded explicitly rather than cast to void,
// so the disregard is legible.
void run_and_ignore(std::vector<std::string> const& argv) {
    [[maybe_unused]] auto const ignored = agentengine::docker_cli_detail::run_argv(argv);
}

[[nodiscard]] bool build_committed_image(std::string& why_not) {
    std::string const container = commit_source_container();
    run_and_ignore({"docker", "rm", "-f", container});
    run_and_ignore({"docker", "image", "rm", "-f", kCommittedImage});
    auto created = agentengine::docker_cli_detail::run_argv(
        {"docker", "create", "--name", container, kImage, "true"});
    if (created.exit_code != 0) {
        why_not = "`docker create` failed: " + trailing_line(created.stdout_text);
        return false;
    }
    auto committed =
        agentengine::docker_cli_detail::run_argv({"docker", "commit", container, kCommittedImage});
    run_and_ignore({"docker", "rm", "-f", container});
    if (committed.exit_code != 0) {
        why_not = "`docker commit` failed: " + trailing_line(committed.stdout_text);
        return false;
    }
    return true;
}

// [N16] A tag this test OWNS and can move between two resets -- the staleness case ADR-176 carried as an
// accepted residual until §16 measured that removing it costs ~9% rather than the ">50%" the first draft
// claimed. Nothing else in the suite uses this name, so moving it cannot disturb another test.
constexpr char const* kMovingTag = "ae_esii_moving_tag:latest";

[[nodiscard]] bool point_tag_at(char const* target) {
    auto tagged = agentengine::docker_cli_detail::run_argv({"docker", "tag", target, kMovingTag});
    return tagged.exit_code == 0;
}

void remove_moving_tag() {
    run_and_ignore({"docker", "image", "rm", "-f", kMovingTag});
}

void remove_committed_image() {
    run_and_ignore({"docker", "rm", "-f", commit_source_container()});
    run_and_ignore({"docker", "image", "rm", "-f", kCommittedImage});
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
            std::printf("[note] this daemon does not expose {{.Descriptor.mediaType}} (%s); the "
                        "`index` and `manifest` kinds are NOT exercised on this run\n",
                        daemon_shape().c_str());
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

    // [N14] -- the MANIFEST kind, end to end, against a real daemon.
    //
    // ADR-176 §6 carried "the manifest and config kinds are unproven" as a residual: every image in the
    // suite is multi-platform, so every run had only ever seen `index`.
    //
    // What that leaves missing is DISCRIMINATION BETWEEN TWO NON-`unknown` VALUES -- and not, as an
    // earlier version of this comment claimed, that a constant-`index` backend "would have passed every
    // check written so far". It would not: N12's malformed-id check and N13's moved-from check both
    // require `unknown` and would fail it outright. What no check could do was tell a real media-type
    // lookup from one that is only ever asked questions with a single answer. A locally committed image
    // is single-platform by construction, so this asks the SAME code the SAME question about a different
    // image on the SAME daemon and requires a different answer.
    if (independent_media_type(kImage).empty()) {
        std::printf("[note] N14 skipped: this daemon exposes no descriptor media type (%s), so no image "
                    "on it has a knowable kind\n",
                    daemon_shape().c_str());
    } else if (std::string why_not; !build_committed_image(why_not)) {
        // A FAILURE, not a note -- and that is the difference between a regression gate and a green
        // light. This branch is only reachable when the daemon DOES expose a descriptor media type,
        // so the kind is knowable and `docker commit` -- core Docker, not an optional feature -- has
        // no business failing. Left as a skip, it made a green run unable to distinguish "N14 proved
        // the manifest kind" from "N14 quietly did nothing", which is the same hole that let the
        // first version of this check sit unrun on every CI leg while the residual was marked closed.
        check(false,
              "N14: this daemon exposes a descriptor media type, so a committed image must be "
              "buildable and the `manifest` kind must be exercised -- " +
                  why_not + " (" + daemon_shape() + ")");
    } else {
        // §15 correction: this asserted the OCI spelling as a literal. Which manifest spelling a commit
        // writes is a property of the daemon and its version -- the docker-schema2 spelling demonstrably
        // occurs on this very machine for pulled images -- so a daemon whose commit emits schema2 would
        // have turned a CORRECT system red, with a SETUP failure that reads like a broken backend. What
        // N14 needs is that the descriptor is a manifest of SOME spelling; which one is not the claim.
        std::string const committed_media_type = independent_media_type(kCommittedImage);
        bool const is_a_manifest_spelling =
            committed_media_type == "application/vnd.oci.image.manifest.v1+json" ||
            committed_media_type == "application/vnd.docker.distribution.manifest.v2+json" ||
            committed_media_type == "application/vnd.docker.distribution.manifest.v1+json" ||
            committed_media_type == "application/vnd.docker.distribution.manifest.v1+prettyjws";
        check(is_a_manifest_spelling,
              "N14 SETUP: a committed image's descriptor is a single-platform MANIFEST, measured "
              "independently and matched against every manifest spelling rather than one literal ('" +
                  committed_media_type + "')");

        std::error_code n14_ec;
        std::filesystem::path const n14_work =
            std::filesystem::temp_directory_path(n14_ec) / "ae_image_identity_manifest_probe";
        std::filesystem::path const n14_work_indexed =
            std::filesystem::temp_directory_path(n14_ec) / "ae_image_identity_index_probe";
        for (auto const& dir : {n14_work, n14_work_indexed}) {
            std::filesystem::remove_all(dir, n14_ec);
            std::filesystem::create_directories(dir, n14_ec);
        }
        {
            agentengine::DockerExecutionSurface committed(kCommittedImage);
            auto const started = committed.reset(n14_work);
            check(started.has_value(), "N14: a surface over the committed image starts");
            if (started.has_value()) {
                check(committed.image_digest_kind() == agentengine::ImageDigestKind::manifest,
                      "N14: its kind is `manifest` -- the kind no run of this suite had ever produced "
                      "before, so `index` was never a constant the backend could get away with");
                check(agentengine::image_digest_kind_name(committed.image_digest_kind()) == "manifest",
                      "N14: the wire spelling of that kind is \"manifest\"");
                check(well_formed_digest(committed.image_digest()),
                      "N14: and the digest beside it is well-formed -- a kind without a digest would be "
                      "a claim about nothing");
                // THE DISCRIMINATION. Two images, one daemon, one code path, two different answers.
                agentengine::DockerExecutionSurface indexed(kImage);
                // A SIBLING directory, not `n14_work / "indexed"`: the committed surface's worktree is a
                // live bind mount, and nesting a second container's worktree inside it would have two
                // containers sharing overlapping host state for no reason.
                auto const indexed_started = indexed.reset(n14_work_indexed);
                check(indexed_started.has_value(), "N14: a surface over the multi-platform image starts");
                if (indexed_started.has_value()) {
                    check(indexed.image_digest_kind() == agentengine::ImageDigestKind::index &&
                              committed.image_digest_kind() == agentengine::ImageDigestKind::manifest,
                          "N14: the same accessor reports `index` for the multi-platform image and "
                          "`manifest` for the committed one, in the same process against the same "
                          "daemon -- the distinction the enum exists for, measured rather than asserted");
                    check(indexed.image_digest() != committed.image_digest(),
                          "N14: and they are different digests, so the two kinds are not two names for "
                          "one object");
                }
            }
        }
        std::filesystem::remove_all(n14_work, n14_ec);
        std::filesystem::remove_all(n14_work_indexed, n14_ec);
        remove_committed_image();
    }

    // [N16] -- THE STALENESS CASE, executed. ADR-176 §16.
    //
    // Until §16 this design resolved the digest ONCE per surface and documented the consequence as an
    // accepted residual: if something outside the process moves the tag mid-session, every later container
    // runs a different image while the record still names the first one. That is not "stale but real" for
    // the command that ran in the later container -- under I4 it is WRONG for that effect, and the ">50%
    // regression" figure that bought it was measured false (~9%, ADR-176 §3e/§10).
    //
    // This moves a tag between two `reset()` calls and requires the reported identity to move with it.
    // Before the fix the surface reported the FIRST image forever, so this is also the mutant detector for
    // any future re-introduction of that cache. The DIGEST half runs on any daemon; the KIND half needs a
    // daemon that exposes a descriptor media type and is guarded accordingly.
    {
        std::string n16_why_not;
        if (!build_committed_image(n16_why_not)) {
            std::printf("[note] N16 skipped: %s (%s)\n", n16_why_not.c_str(),
                        daemon_shape().c_str());
        } else if (!point_tag_at(kImage)) {
            std::printf("[note] N16 skipped: `docker tag` failed on this daemon (%s)\n",
                        daemon_shape().c_str());
        } else {
            std::error_code n16_ec;
            std::filesystem::path const n16_work =
                std::filesystem::temp_directory_path(n16_ec) / "ae_image_identity_moving_tag_probe";
            std::filesystem::remove_all(n16_work, n16_ec);
            std::filesystem::create_directories(n16_work, n16_ec);
            {
                agentengine::DockerExecutionSurface surface(kMovingTag);
                auto const first = surface.reset(n16_work);
                check(first.has_value(), "N16: a surface over a tag this test owns starts");
                if (first.has_value()) {
                    std::string const digest_before(surface.image_digest());
                    auto const kind_before = surface.image_digest_kind();
                    check(well_formed_digest(digest_before),
                          "N16 SETUP: the first container resolves a well-formed digest");

                    // The re-pull, simulated exactly: the SAME reference now names a different image.
                    // `docker tag` does to the local tag what an external `docker pull` would, without
                    // needing a registry or a network.
                    check(point_tag_at(kCommittedImage),
                          "N16 SETUP: the tag is moved to a different image between the two resets");

                    auto const second = surface.reset(n16_work);
                    check(second.has_value(), "N16: the surface resets again onto the moved tag");
                    if (second.has_value()) {
                        std::string const digest_after(surface.image_digest());
                        check(well_formed_digest(digest_after),
                              "N16: the second container resolves a well-formed digest too");
                        check(digest_after != digest_before,
                              "N16: the reported digest MOVED with the tag -- the container this command "
                              "would run in is the one the record names, which is the whole of I4's claim "
                              "here. A surface that resolved once per lifetime reports the FIRST image "
                              "forever and fails exactly this check");
                        if (!independent_media_type(kImage).empty()) {
                            check(kind_before == agentengine::ImageDigestKind::index &&
                                      surface.image_digest_kind() ==
                                          agentengine::ImageDigestKind::manifest,
                                  "N16: and the KIND moved with it, index -> manifest -- so the kind "
                                  "cache, keyed by digest, re-resolves rather than describing the image "
                                  "the surface no longer runs");
                        }
                    }
                }
            }
            std::filesystem::remove_all(n16_work, n16_ec);
            remove_moving_tag();
            remove_committed_image();
        }
    }

    // [N15] -- the mapper itself, exhaustively, over the CLOSED list it is documented to accept.
    //
    // This is the other half of ADR-176 §6's residual, and it is deliberately not an end-to-end check.
    // The `config` kind cannot be produced by either backend in tree: a descriptor-exposing Docker daemon
    // reports index or manifest (N11, N14), containerd's TYPE column reports the same two
    // (test_containerd_execution_surface), and a daemon whose image ID really IS a config digest -- the
    // classic graph driver -- exposes no `.Descriptor` at all to say so, which is exactly why N11 has an
    // empty-media-type branch. ADR-176 §13 records that measurement and why the enum keeps the value
    // anyway. What is provable here is the mapping itself, and every spelling of it.
    {
        // §15 correction: this block used to carry its OWN hand-copy of the eight media types. The copies
        // were identical -- a red-team pass diffed them entry by entry -- but duplication could only ever
        // catch a REMOVAL or a RE-MAPPING. Add a ninth entry to the mapper and this test would have
        // stayed green while "proves the mapping exhaustively" quietly stopped being true, and an
        // addition is exactly how a wrong kind would enter. The table now lives in execution_surface.hpp
        // and is iterated here, so "exhaustive" is true by construction rather than by diligence.
        bool all_mapped = true;
        for (auto const& known : agentengine::kImageDigestKindMediaTypes) {
            if (agentengine::image_digest_kind_from_media_type(known.media_type) != known.kind) {
                all_mapped = false;
                std::printf("[detail] N15: '%.*s' did not map to its documented kind\n",
                            static_cast<int>(known.media_type.size()), known.media_type.data());
            }
        }
        check(all_mapped,
              "N15: every media type on ADR-176 §9's closed list maps to the kind it documents -- the "
              "list ITERATED from the mapper's own table, including both CONFIG spellings, which no "
              "in-tree backend has been observed to emit");
        check(std::size(agentengine::kImageDigestKindMediaTypes) == 8,
              "N15: the closed list is the eight entries ADR-176 §9 documents -- a ninth is a deliberate "
              "edit that should update the ADR, not something that slips past a green suite");
        check(agentengine::image_digest_kind_from_media_type(
                  "application/vnd.oci.image.config.v1+json") == agentengine::ImageDigestKind::config &&
                  agentengine::image_digest_kind_name(agentengine::ImageDigestKind::config) == "config",
              "N15: the config kind round-trips to the wire spelling \"config\", so the value a future "
              "backend produces is already the value a consumer would read");

        // The controls. Without these, a mapper that returned a kind for ANY string would pass above.
        //
        // §15 correction: these near-misses used to be three hand-written variations of ONE of the eight
        // spellings -- the config one, which no backend can even produce -- so "the exactness is proven
        // per spelling" was false; it was proven once, for the least important entry. They are now
        // DERIVED from each entry in turn, so the index/manifest spellings whose confusion the enum
        // exists to prevent get the same treatment as every other.
        bool all_rejected = true;
        auto reject = [&](std::string const& candidate, char const* how) {
            if (agentengine::image_digest_kind_from_media_type(candidate) !=
                agentengine::ImageDigestKind::unknown) {
                all_rejected = false;
                std::printf("[detail] N15 CONTROL: %s -- '%s' was accepted and should not have been\n",
                            how, candidate.c_str());
            }
        };
        for (auto const& known : agentengine::kImageDigestKindMediaTypes) {
            std::string const exact(known.media_type);
            reject(exact.substr(0, exact.size() - 1), "truncated by one character");
            reject(exact + "x", "extended by one character");
            reject(" " + exact, "prefixed with a space");
            reject("warning: unexpected " + exact, "embedded in a daemon warning line");
        }
        reject("", "the empty string");
        reject("application/vnd.oci.image.layer.v1.tar+gzip", "a layer media type");
        reject("application/vnd.oci.descriptor.v1+json", "a descriptor media type");
        check(all_rejected,
              "N15 CONTROL: for EVERY entry on the list, a truncated, extended, space-prefixed or "
              "warning-embedded spelling of it is `unknown` -- the match is exact per spelling, so no "
              "entry can be reached by accident");
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
