// Positive control for effect_context_deadline_unstated: the same program with the deadline stated, both spellings.

#include <chrono>

#include "agentengine/core/effect_context.hpp"

int main() {
    using namespace agentengine;
    EffectContext none{.deadline = Deadline::none()};
    EffectContext timed{.deadline = Deadline::at(std::chrono::steady_clock::now() + std::chrono::seconds{5})};
    return none.deadline.is_none() && !timed.deadline.is_none() ? 0 : 1;
}
