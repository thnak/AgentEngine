#pragma once
// ADR-244 §2.5 (G-C9): the Kata backend takes a stated runtime and a digest-pinned image. These are the image indexes the
// Kata test host (thnak-remote) has pulled; a different host pulls them by digest, so the bytes are the same.
// Host prerequisite: containerd names an image by the reference it was pulled with, so each must be pulled BY DIGEST
// first (`sudo ctr images pull <ref>`); the backend does not pull, and a tag-pulled copy is not found under the digest ref.

namespace ae_test_kata {

inline constexpr char const* kRuntime = "io.containerd.kata-clh.v2";
inline constexpr char const* kBusybox =
    "docker.io/library/busybox@sha256:fd7dc98638c8e305f4dc34e979f1c0fdfdcaeb0fbf8fcff77ae834b6da3d7e6e";
inline constexpr char const* kPython =
    "docker.io/library/python@sha256:1b668429b3511ab407d8e00648891631b0b1a4d7e15e3ca70f38ab5b91ad4ab4";

}  // namespace ae_test_kata
