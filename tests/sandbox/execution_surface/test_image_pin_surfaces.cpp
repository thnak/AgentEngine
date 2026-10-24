// Proves decisions/ADR-244-required-choices-step-1b.md §2.5 (G-C9) for the Docker and containerd surfaces: an image is
// named by digest. Host-free: `check_image_pinned` is pure and both `create()` calls refuse a tag before any CLI runs.
//
//   P1 (positive control) -- <repository>@sha256:<64 lowercase hex> is accepted.
//   P2 -- a tag, `latest`, no reference, a short, non-hex or uppercase digest, and a digest with no repository are
//         refused with `sandbox.image_not_pinned`.
//   P3 -- `DockerCliBackend::create()` with a tag is refused `sandbox.image_not_pinned`.
//   P4 -- `ContainerdCliBackend::create()` with a tag is refused `sandbox.image_not_pinned` (Linux only).

#include <cstdio>
#include <string>

#include "../../support/pinned_images.hpp"
#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/image_pin.hpp"
#if !defined(_WIN32)
#include "../../support/surface_factories.hpp"
#include "agentengine/sandbox/containerd_execution_surface.hpp"
#endif

namespace {
int g_failed = 0;
void check(bool cond, std::string const& what) {
    std::printf("[%s] %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failed;
}
template <class R>
std::string code_of(R const& r) {
    return r.has_value() ? std::string{} : r.error().code;
}
}  // namespace

int main() {
    using agentengine::check_image_pinned;
    check(check_image_pinned(ae_test::kAlpine).has_value(), "P1: a repository@sha256:<64 hex> image is accepted");
    check(check_image_pinned(ae_test::kPythonAlpine).has_value(), "P1: so is the python image");

    for (char const* bad : {"alpine:latest", "docker.io/library/alpine:latest", "alpine", "", "alpine:3.20",
                            "alpine@sha256:abc",
                            "alpine@sha256:ZZ4b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6",
                            "alpine@sha256:294B683CB724975BEC92580E1E685676BD4B50BDA910DDB8C51D4CABEAEC77E6",
                            "@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6"}) {
        check(code_of(check_image_pinned(bad)) == "sandbox.image_not_pinned", std::string("P2: refused: '") + bad + "'");
    }

    agentengine::DockerCliBackend docker;
    auto const docker_tag = docker.create("alpine:latest", agentengine::ContainerIsolation::conservative());
    check(code_of(docker_tag) == "sandbox.image_not_pinned", "P3: Docker create() with a tag is refused");

#if !defined(_WIN32)
    agentengine::ContainerdCliBackend ctr;
    auto const ctr_tag = ctr.create("ae_pin", "/tmp/ae_pin", "docker.io/library/alpine:latest",
                                    agentengine::ContainerdIsolation::conservative(), ae_test::mount_grants("/tmp"));
    check(code_of(ctr_tag) == "sandbox.image_not_pinned", "P4: containerd create() with a tag is refused");
#endif

    if (g_failed == 0) std::printf("test_image_pin_surfaces: ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
