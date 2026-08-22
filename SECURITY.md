# Security Policy

AgentEngine is under active implementation (CLAUDE.md), with a public remote at
[github.com/thnak/AgentEngine](https://github.com/thnak/AgentEngine), but there is still no released
binary, so this document states the process that takes effect **before** any public release (024 §7
Q4 / OpenQuestions.md OQ-11), not a live intake channel today.

## Reporting a vulnerability

- Report privately to the project owner rather than opening a public issue or PR. A dedicated
  contact (a security-reporting email address or GitHub private vulnerability reporting) is added to
  this section before the first public release — a placeholder is not shipped as the real channel.
- Include: affected RFC/component, reproduction steps or a proof of concept, and the invariant or
  gate you believe fails (007 I2–I4, 008, 017, 018, 021 §6 G3 are the highest-severity classes —
  sandbox escape, cross-tenant leakage, and capability widening are release-blocking defect classes
  per 017 §6 and 018 §7).

## Disclosure timeline

- Acknowledgement target: 5 business days.
- A fix or mitigation plan, with a target date, communicated to the reporter before any public
  disclosure.
- **Coordinated disclosure**: the reporter and the project agree on a disclosure date; the default
  absent agreement is 90 days from acknowledgement, standard industry practice, adjustable by mutual
  agreement for complexity.
- A deprecation or fix required to close a vulnerability may move faster than 024 §3's normal
  deprecation window, with the reason stated (024 §3's security exception).

## What gets published, and what doesn't (OQ-10)

Following 022 §8 Q2 / 017 §9 Q4's resolution (OpenQuestions.md OQ-10): advisories publish the
**classification** (which invariant/gate failed, severity, affected versions) and are generated
into the same kind of CI-backed record 021 §2's support table already commits to — never hand
authored as a claim disconnected from what was actually proven. The underlying adversarial
**payloads** (exploit strings, the corpus entries in 022 §3/§5 that reproduce the finding) stay
private until the fix is proven effective against them (i.e., until 017 §8's relevant gate passes
with that entry included) — publishing an attack string ahead of a proven fix hands out an attack
kit for no defensive benefit to anyone who couldn't already reproduce it themselves.

## Scope

In scope: the engine core, the sandbox/isolation backends (008), the plugin ABI (009), the Python
interpreter mediation layer (010), protocol conformance surfaces (011/012/013). Out of scope:
vulnerabilities in a model provider's own service, or in a third-party MCP server or plugin not
shipped by this project. (Historical: this section used to also carve out the vendored
[QuarkCpp](https://github.com/thnak/QuarkCpp) submodule as out of scope, reported upstream instead
— `decisions/ADR-037-remove-quark-as-core-runtime.md` removed that dependency entirely, so there is
no vendored Quark code left in this tree to carve out.)
