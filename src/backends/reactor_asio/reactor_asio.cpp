// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.2 (§9 D1) -- the production
// `pal::Reactor`: standalone Asio 1.38.2, one io_context run by one reactor thread. The class lives in
// asio_reactor.hpp (internal to this directory) so other I/O families can share its io_context; nothing
// Asio-typed leaves src/backends/reactor_asio/.

#include "asio_reactor.hpp"

namespace agentengine::pal {

std::unique_ptr<Reactor> make_default_reactor() { return std::make_unique<asio_backend::AsioReactor>(); }

}  // namespace agentengine::pal
