#pragma once
// ADR-244 section 2.5 (G-C9): the Docker and containerd surfaces boot only digest-pinned images. These are the image
// indexes the Linux test host (thnak-remote) has pulled BY DIGEST (`docker pull <ref>`, `sudo ctr images pull <ref>`):
// containerd looks an image up by the exact reference it was pulled with, so a tag-pulled image does not satisfy these.
// kata_images.hpp holds the Kata ones; python is the same digest in both.

namespace ae_test {

inline constexpr char const* kAlpine =
    "docker.io/library/alpine@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6";
inline constexpr char const* kPythonAlpine =
    "docker.io/library/python@sha256:1b668429b3511ab407d8e00648891631b0b1a4d7e15e3ca70f38ab5b91ad4ab4";

}  // namespace ae_test
