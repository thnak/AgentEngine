// ADR-176 §14/§15 -- the controls for the image-provenance shape gate
// (tests/support/image_provenance_shape.hpp), on their own, with no Docker daemon anywhere near them.
//
// WHY A SEPARATE BINARY. The gate's real call sites live in `test_mandatory_sandbox_provider` and
// `test_task_branch_tools`, which need a live daemon and are therefore EXCLUDED from all three Windows CI
// legs (.github/workflows/ci.yml). A gate whose controls only ever run on one leg is a gate nobody is
// watching. Everything here is pure JSON-schema shape checking, so it runs on every leg, every OS, under
// every sanitizer -- which is where a control belongs.
//
// WHAT IS UNDER TEST. `image_provenance_shape()` answers one question about a provider's contributed
// tools: does each one's reply declare image provenance -- `image`, `image_digest`, `image_digest_kind`,
// all three REQUIRED strings -- or is it named on an explicit exemption list? The descriptors here are
// hand-built, which is the point: these are the shapes nobody has written yet, fed to the gate to prove it
// would catch them. A gate that cannot fail proves nothing (CLAUDE.md).
//
// Each control below corresponds to a finding from ADR-176 §15's red-team round. The first version of this
// gate PASSED four of these six failing shapes.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentengine/core/tool_descriptor.hpp"

#include "support/image_provenance_shape.hpp"
#include "support/memory_cap.hpp"

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

[[nodiscard]] agentengine::ToolDescriptor tool(char const* name, std::string schema) {
    agentengine::ToolDescriptor d;
    d.name = name;
    d.reply_schema_json = std::move(schema);
    return d;
}

// The exact shape AE_JSON_SCHEMA emits for a reply of three plain `std::string`s.
constexpr char const* kConforming =
    R"({"type":"object","properties":{"image":{"type":"string"},"image_digest":{"type":"string"},)"
    R"("image_digest_kind":{"type":"string"}},"required":["image","image_digest","image_digest_kind"]})";

}  // namespace

int main() {
    static_cast<void>(agentengine::test_support::cap_process_memory(std::size_t{256} << 20,
                                                                     std::size_t{1024} << 20));

    // [C1] the shape that must pass, so the rest are not all trivially red.
    check(agentengine::test_support::image_provenance_shape({tool("good", kConforming)}).ok,
          "C1: a reply with all three fields, as required strings, PASSES");

    // [C2] the shape the first version was written for.
    check(!agentengine::test_support::image_provenance_shape(
               {tool("missing_kind",
                     R"({"type":"object","properties":{"image":{"type":"string"},)"
                     R"("image_digest":{"type":"string"}},"required":["image","image_digest"]})")})
               .ok,
          "C2: `image` and `image_digest` with NO kind FAILS");

    // [C3] a companion that is not a string cannot be stamped or read.
    check(!agentengine::test_support::image_provenance_shape(
               {tool("mistyped",
                     R"({"type":"object","properties":{"image":{"type":"string"},)"
                     R"("image_digest":{"type":"string"},"image_digest_kind":{"type":"integer"}},)"
                     R"("required":["image","image_digest","image_digest_kind"]})")})
               .ok,
          "C3: a non-string `image_digest_kind` FAILS");

    // [C4] §15 finding: THE DEFAULT WAS INVERTED. The realistic sixth tool declares no image at all --
    // it runs a command and says nothing about where. The first version returned ok for exactly this,
    // which left the residual ("nothing proves a future caller goes through bound_image()") open in the
    // case that matters most.
    auto const silent = tool("runs_something_says_nothing",
                             R"({"type":"object","properties":{"exit_code":{"type":"integer"}},)"
                             R"("required":["exit_code"]})");
    check(!agentengine::test_support::image_provenance_shape({silent}).ok,
          "C4: a reply declaring NO image provenance FAILS by default -- the case the first version of "
          "this gate passed, and the one the residual was actually about");
    check(agentengine::test_support::image_provenance_shape({silent}, {"runs_something_says_nothing"}).ok,
          "C4: ...and PASSES once its name is on the exemption list -- the exemption is a written "
          "decision, not a silent skip");

    // [C5] §15 finding: the walk was single-level. `std::vector<RunCommandReply>` is a plausible batch
    // reply, and AE_JSON_SCHEMA nests the element schema under `items`. A nested `image` with no
    // companions is the same false claim, one level down, and was invisible to both the gate and the C++
    // stamp.
    check(!agentengine::test_support::image_provenance_shape(
               {tool("nested_object",
                     R"({"type":"object","properties":{"run":{"type":"object","properties":)"
                     R"({"image":{"type":"string"}},"required":["image"]}},"required":["run"]})")})
               .ok,
          "C5: an `image` nested inside another object, with no companions, FAILS");
    check(!agentengine::test_support::image_provenance_shape(
               {tool("array_elements",
                     R"({"type":"object","properties":{"runs":{"type":"array","items":)"
                     R"({"type":"object","properties":{"image":{"type":"string"}},)"
                     R"("required":["image"]}}},"required":["runs"]})")})
               .ok,
          "C5: an `image` inside an ARRAY of objects, with no companions, FAILS -- the batch-reply shape");

    // [C6] §15 FATAL finding, from the wire side. `std::optional<std::string>` publishes
    // `{"type":"string"}` exactly like a plain one and differs ONLY by being absent from `required` --
    // while failing the C++ stamp's `std::same_as<std::string&>` test, so the field would be silently
    // skipped and the reply would ship an image reference with nothing beside it. The C++ side now
    // static_asserts; this is the same hole seen from the schema.
    check(!agentengine::test_support::image_provenance_shape(
               {tool("optional_companions",
                     R"({"type":"object","properties":{"image":{"type":"string"},)"
                     R"("image_digest":{"type":"string"},"image_digest_kind":{"type":"string"}},)"
                     R"("required":["image"]})")})
               .ok,
          "C6: companions that are present but NOT required FAIL -- the `std::optional<std::string>` "
          "spelling, which reads as a plain string on the wire");

    // [C7] a descriptor that publishes no schema makes no provenance claim; four in-tree providers build
    // descriptors by hand this way. Non-empty garbage is still a failure, because it means the tool
    // published something and this gate cannot tell what.
    check(agentengine::test_support::image_provenance_shape({tool("no_schema", "")}).ok,
          "C7: an empty reply schema PASSES -- publishing nothing is not a provenance violation");
    check(!agentengine::test_support::image_provenance_shape({tool("garbage", "{not json")}).ok,
          "C7: a non-empty schema that does not parse FAILS");

    // [C8] the exemption list must not rot into blanket permission.
    std::vector<agentengine::ToolDescriptor> const contributed = {tool("good", kConforming)};
    check(agentengine::test_support::unknown_exemptions(contributed, {"renamed_away"}).size() == 1,
          "C8: an exemption naming no contributed tool is reported as stale -- a renamed tool must not "
          "leave a standing exemption behind");
    check(agentengine::test_support::unknown_exemptions(contributed, {"good"}).empty(),
          "C8: an exemption that matches a real tool is not reported");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
