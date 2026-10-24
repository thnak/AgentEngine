// Proves decisions/ADR-244-required-choices-step-1b.md §2.5 (G-C9) for the Kata backend: the image is digest-pinned.
// Host-free: `check_image_pinned` is pure, and `create()` refuses a tag before any `ctr` call, so no VM, root or
// containerd is needed.
//
//   P1 (positive control) -- <repository>@sha256:<64 lowercase hex> is accepted.
//   P2 -- a tag, `latest`, no reference, a short or non-hex or uppercase digest, and a digest with no repository are
//         refused with `sandbox.image_not_pinned`.
//   P3 -- `create()` on a backend built with a tag fails `sandbox.image_not_pinned` and touches nothing.

#include <cstdio>
#include <string>

#include "../../support/kata_images.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/rt/block_on.hpp"
#include "backends/kata/kata_backend.hpp"

namespace {
int g_failed = 0;
void check(bool cond, std::string const& what) {
    std::printf("[%s] %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failed;
}
std::string code_of(agentengine::result<void> const& r) { return r.has_value() ? std::string{} : r.error().code; }
}  // namespace

int main() {
    using agentengine::kata::KataBackend;
    check(KataBackend::check_image_pinned(ae_test_kata::kBusybox).has_value(), "P1: a repository@sha256:<64 hex> image is accepted");
    check(KataBackend::check_image_pinned(ae_test_kata::kPython).has_value(), "P1: so is the python image");

    for (char const* bad : {"docker.io/library/busybox:latest", "busybox", "", "docker.io/library/busybox:1.36",
                            "docker.io/library/busybox@sha256:abc",
                            "docker.io/library/busybox@sha256:ZZ7dc98638c8e305f4dc34e979f1c0fdfdcaeb0fbf8fcff77ae834b6da3d7e6e",
                            "docker.io/library/busybox@sha256:FD7DC98638C8E305F4DC34E979F1C0FDFDCAEB0FBF8FCFF77AE834B6DA3D7E6E",
                            "@sha256:fd7dc98638c8e305f4dc34e979f1c0fdfdcaeb0fbf8fcff77ae834b6da3d7e6e"}) {
        check(code_of(KataBackend::check_image_pinned(bad)) == "sandbox.image_not_pinned",
              std::string("P2: refused: '") + bad + "'");
    }

    KataBackend tagged(ae_test_kata::kRuntime, "docker.io/library/busybox:latest");
    agentengine::SandboxSpec spec{};
    agentengine::EffectContext ctx{.deadline = agentengine::Deadline::none()};
    auto created = agentengine::rt::block_on(tagged.create(spec, ctx));
    check(!created.has_value() && created.error().code == "sandbox.image_not_pinned",
          "P3: create() with a tag is refused (sandbox.image_not_pinned)");

    if (g_failed == 0) std::printf("test_kata_image_pin: ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
