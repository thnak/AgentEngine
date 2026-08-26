// PROVE-PHASE NEGATIVE PROBE. Must fail to compile -- abandon()/merge() take BranchHandle BY VALUE
// (§4's "consumes the handle" claim). Since BranchHandle's copy constructor is deleted, passing an
// lvalue directly (without an explicit std::move) cannot compile: the compiler has no way to
// initialize the by-value parameter except by copying, which is deleted. This is a real, compiler-
// enforced guard against ACCIDENTALLY calling abandon()/merge() while still believing you hold a
// usable handle -- a caller is forced to write std::move(...) at the call site, an explicit,
// grep-able admission that ownership is being given up.
#include "ledger.hpp"
#include "../common/block_on.hpp"

int main() {
    using namespace probe;
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("owner");
    Ledger ledger;
    auto root_result = block_on(ledger.create_root_branch(owner));
    BranchHandle root = std::move(*root_result);

    auto abandoned = block_on(ledger.abandon(root));  // <-- THIS LINE MUST FAIL: no std::move, and
                                                          // BranchHandle's copy ctor is deleted
    return abandoned.has_value() ? 0 : 1;
}
