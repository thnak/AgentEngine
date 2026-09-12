#pragma once
// Directory walks that fail closed through `result<T>` instead of throwing past it.
//
// WHY THIS EXISTS. `std::filesystem`'s iterators have a trap that three separate sites in this tree
// fell into independently, and that ADR-174's round-2 red-team reproduced as a live process kill:
//
//     for (auto const& e : std::filesystem::recursive_directory_iterator(dir, opts, ec)) {
//         if (ec) return std::unexpected(...);   // <-- never runs
//         ...
//     }
//
// Two distinct defects in that shape, both measured on this machine rather than reasoned about:
//
//   1. The `ec` overload covers the CONSTRUCTOR only. A range-for calls the THROWING `operator++`,
//      so the place a walk actually fails -- a concurrent removal, an unreadable subdirectory -- is
//      the place that throws. Measured: "recursive_directory_iterator::operator++: The system cannot
//      find the path specified." escaping a function whose declared contract is `result<T>`.
//   2. When the constructor DOES fail, the iterator compares equal to end, so the loop body never
//      runs at all and the `if (ec)` inside it is dead code. Measured: body ran 0 times, the check
//      fired 0 times, and the caller received an empty result and NO error. Trading a crash for a
//      silent truncation is the worse of the two, which is why the error check below sits
//      immediately after the increment rather than at the top of the body.
//
// `ustar_writer.hpp` already carries a hand-written version of this loop, with the same reasoning in
// its own comment (ADR-174 finding F4). This header is that loop, once, so the correct shape is the
// one that is easy to reach.
//
// The visitor returns `result<void>`; a failure stops the walk and propagates unchanged, so a caller
// keeps its own error codes for its own failures and uses `failure_code`/`what` only for the walk.

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "agentengine/core/error.hpp"

namespace agentengine::fs_walk {

namespace detail {

[[nodiscard]] inline agentengine::error walk_error(std::string_view what, std::string_view code,
                                                   std::error_code const& ec) {
    return agentengine::error{agentengine::failure_class::contract,
                              std::string(what) + ": " + ec.message(), std::string(code),
                              ec.value()};
}

}  // namespace detail

// Every REGULAR FILE under `root`, recursively. A non-regular entry is skipped; a status query that
// itself fails is an error rather than a skip, because "could not tell what this is" and "this is a
// directory" are not the same answer.
template <class Visit>
[[nodiscard]] agentengine::result<void> for_each_regular_file_recursive(
        std::filesystem::path const& root, std::filesystem::directory_options options,
        std::string_view what, std::string_view failure_code, Visit&& visit) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, options, ec);
    if (ec) return std::unexpected(detail::walk_error(what, failure_code, ec));

    std::filesystem::recursive_directory_iterator const walk_end;
    while (it != walk_end) {
        std::filesystem::directory_entry const& entry = *it;
        std::error_code kind_ec;
        bool const regular = entry.is_regular_file(kind_ec);
        if (kind_ec) return std::unexpected(detail::walk_error(what, failure_code, kind_ec));
        if (regular) {
            auto visited = visit(entry);
            if (!visited.has_value()) return std::unexpected(visited.error());
        }
        it.increment(ec);
        // Immediately after the increment, never at the top of the body: a failed increment leaves
        // the iterator EQUAL TO END, so a check at the top would be skipped by the loop condition
        // and the walk would report success having silently dropped everything it had not reached.
        if (ec) return std::unexpected(detail::walk_error(what, failure_code, ec));
    }
    return agentengine::result<void>{};
}

// Every entry directly inside `root`, not recursively, whatever its kind.
template <class Visit>
[[nodiscard]] agentengine::result<void> for_each_directory_entry(
        std::filesystem::path const& root, std::string_view what, std::string_view failure_code,
        Visit&& visit) {
    std::error_code ec;
    std::filesystem::directory_iterator it(root, ec);
    if (ec) return std::unexpected(detail::walk_error(what, failure_code, ec));

    std::filesystem::directory_iterator const walk_end;
    while (it != walk_end) {
        auto visited = visit(*it);
        if (!visited.has_value()) return std::unexpected(visited.error());
        it.increment(ec);
        if (ec) return std::unexpected(detail::walk_error(what, failure_code, ec));
    }
    return agentengine::result<void>{};
}

}  // namespace agentengine::fs_walk
