#pragma once
// ADR-244 section 2.5 (G-C9): containerd finds an image only by the exact reference it was pulled with, and the surfaces
// now take a digest reference, so a host that pulled `alpine:latest` does not satisfy them. A live containerd test calls
// this first: a no-op when the reference is already present, otherwise a pull by digest. Linux only (`ctr`).

#include <cstdlib>
#include <string>

namespace ae_test {

inline void ensure_containerd_image(std::string const& reference) {
    std::string const present = "ctr images ls -q 2>/dev/null | grep -qxF '" + reference + "'";
    if (std::system(present.c_str()) == 0) return;
    std::string const pull = "ctr images pull '" + reference + "' >/dev/null 2>&1";
    if (std::system(pull.c_str()) != 0) return;  // the test then fails with ctr's own "image not found"
}

}  // namespace ae_test
