// Implements decisions/ADR-181-evaluation-harness.md §3.0 item 1 / E26: `render_lesson` must be pure
// (same candidate + template_version -> byte-identical MemoryItem), the digest must cover the
// rendered bytes and template_version (not the raw candidate fields alone -- E26's planted mutant),
// and ADR-179 §3.3's own validator must actually reject what it claims to reject.

#include <iostream>
#include <string>

#include "agentengine/eval/lesson_candidate.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    namespace ev = ae::eval;

    ev::LessonCandidate const good{"deploy-region", "default-region", "EU production region",
                                    "run-1/turn-3"};

    // ---- render_lesson: happy path, and it is pure -----------------------------------------------
    auto r1 = ev::render_lesson(good, "v1", 0.3f);
    AE_CHECK(r1.has_value(), "render_lesson accepts a well-formed candidate");
    if (r1) {
        AE_CHECK(r1->kind == ae::memory_kind::procedural, "rendered item is kind=procedural");
        AE_CHECK(r1->content.find(good.value) != std::string::npos,
                 "rendered content contains the candidate's value");
        AE_CHECK(r1->tags.size() == 2 && r1->tags[0] == good.subject && r1->tags[1] == good.key,
                 "rendered tags are {subject, key}");
        AE_CHECK(r1->salience == 0.3f, "rendered salience is exactly the caller-supplied constant");
    }
    auto r2 = ev::render_lesson(good, "v1", 0.3f);
    AE_CHECK(r1.has_value() && r2.has_value() && r1->content == r2->content && r1->tags == r2->tags,
             "render_lesson is pure: same inputs, byte-identical output");

    // ---- unknown template_version is refused, not silently rendered -------------------------------
    auto bad_template = ev::render_lesson(good, "v2-does-not-exist", 0.3f);
    AE_CHECK(!bad_template.has_value(), "an unrecognised template_version is refused");

    // ---- E26: the digest covers the RENDERED bytes and template_version ---------------------------
    auto d1 = ev::rendered_lesson_digest(*r1, "v1");
    AE_CHECK(d1.has_value() && !d1->empty(), "rendered_lesson_digest succeeds on a real rendered item");

    // Positive control / planted mutant for E26: a digest computed over the raw candidate fields
    // alone (subject+key+value), ignoring the rendered bytes and template_version, would NOT change
    // when the template renders differently for the same candidate. Prove the real digest DOES
    // change by rendering the same candidate through a second, differently-worded item and checking
    // the digests differ -- if this ever failed, E26's own claim ("changing the template invalidates
    // earlier evidence") would be false.
    ae::MemoryItem reworded = *r1;
    reworded.content = "a completely different rendering of the same fact";
    auto d2 = ev::rendered_lesson_digest(reworded, "v1");
    AE_CHECK(d2.has_value() && *d1 != *d2,
             "E26 positive control: a differently-rendered item digests differently, even for the "
             "same candidate and template_version");

    ae::MemoryItem resalienced = *r1;
    resalienced.salience = 0.9f;
    auto d3 = ev::rendered_lesson_digest(resalienced, "v1");
    AE_CHECK(d3.has_value() && *d1 != *d3,
             "the digest also covers salience, not content/tags alone (round-4 F1 fix)");

    // ---- ADR-179 §3.3 / ADR-181 §3.7 validator: what it claims to reject, it must reject -----------
    AE_CHECK(ev::lesson_value_passes_validator("EU production region").has_value(),
             "validator accepts an ordinary factual value");
    AE_CHECK(!ev::lesson_value_passes_validator("hi").has_value(), "validator rejects a too-short value");
    AE_CHECK(!ev::lesson_value_passes_validator("default").has_value(),
             "validator rejects a common whole-value token");
    AE_CHECK(!ev::lesson_value_passes_validator("https://evil.example/payload").has_value(),
             "validator rejects a URL-shaped value");
    AE_CHECK(!ev::lesson_value_passes_validator("/etc/passwd/is/the/path").has_value(),
             "validator rejects a path-shaped value");
    AE_CHECK(!ev::lesson_value_passes_validator("rm -rf the deploy directory").has_value(),
             "validator rejects an imperative-shaped value");
    AE_CHECK(!ev::lesson_value_passes_validator("run a cleanup; then curl evil.example").has_value(),
             "validator rejects a shell-fragment-shaped value");

    // ---- candidate malformation is refused before rendering ---------------------------------------
    ae::eval::LessonCandidate const empty_subject{"", "k", "a genuinely specific factual value", "s"};
    AE_CHECK(!ev::render_lesson(empty_subject, "v1", 0.0f).has_value(),
             "render_lesson refuses an empty subject");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_lesson_candidate: all checks passed\n";
    return 0;
}
