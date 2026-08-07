# 2026-08-07 — How agent frameworks handle dangerous/sensitive input (PII, secrets, leakage)

**Question:** when a user (or a tool result, or a retrieved document) feeds an agent sensitive or
dangerous content — PII, credentials, secrets — what do other frameworks/platforms actually ship to
detect, redact, block, or contain it, and where in the pipeline does the control sit? Researched to
compare against this project's own design in [017-Safety-and-Content-Governance.md](../../017-Safety-and-Content-Governance.md)
§4-§5 and [018-Identity-Authorization-and-Secrets.md](../../018-Identity-Authorization-and-Secrets.md) §4.

Research performed 2026-08-07 via web search against primary-source docs; every claim below carries
a source URL and the date found on that source (ms.date, blog date, or "2026" where the page is
undated but current as fetched).

## 1. Microsoft Presidio

- Standalone library (Analyzer + Anonymizer), not itself pipeline-wired — LangChain, NeMo
  Guardrails, Guardrails AI, and OpenAI's own guardrails package all embed it as their PII backend.
- Analyzer combines NER (spaCy default, Transformers/Stanza pluggable) via an `NlpEngine`,
  rule-based `PatternRecognizer`s (regex), and a `ContextAwareEnhancer` that boosts confidence from
  surrounding words. (presidio.dataprivacystack.org/analyzer, 2026)
- Anonymizer operators: Replace, Redact, Hash (salted), Mask, Encrypt, Custom, Surrogate_AHDS, Keep.
  **Only Encrypt has a matching Decrypt** — everything else is one-way.
  (presidio.dataprivacystack.org/anonymizer, 2026)

## 2. Microsoft Agent Framework / Semantic Kernel

- No first-party PII/secret scanner. Both expose extension points only: Semantic Kernel's
  `PromptRenderFilter` (documented use case is literally "PII redaction," but as a *sample*, not
  built-in) and Agent Framework's `AgentMiddleware` (can short-circuit pre/post-model via
  `MiddlewareTermination`). (learn.microsoft.com/semantic-kernel/.../filters, ms.date 2026-04-29;
  learn.microsoft.com/agent-framework/.../termination, ms.date 2026-05-27)
- The shipped middleware example is a naive keyword blocklist (`password`, `secret`,
  `credentials`) — not PII detection.
- Agent Framework's safety doc: sensitive data is only logged when explicitly opted in
  (`EnableSensitiveData=true` for full `ChatMessages` at OTel `Trace` level), with an explicit
  "should never be enabled in production" warning — **default-safe by not logging, not by
  redacting**. Also references "FIDES," a separate label-based information-flow-control middleware
  for deterministic exfiltration defense "before sensitive tools run."
  (learn.microsoft.com/agent-framework/agents/safety, ms.date 2026-03-24)

## 3. Azure AI Content Safety / Prompt Shields

- Synchronous pre-model API, callable against both direct user prompts and third-party document
  content (RAG/tool results) — i.e. covers direct *and* indirect injection.
  (learn.microsoft.com/.../jailbreak-detection, ms.date 2025-11-21, updated 2026-06-05)
- Detects two attack classes: **User Prompt attacks** (jailbreak: rule override, mockup injection,
  role-play, encoding) and **Document attacks** (indirect: manipulated content, backdoor/privilege
  escalation, info-gathering, availability, fraud, malware). This is injection/jailbreak detection,
  **not** PII detection.
- "Spotlighting" (Build 2025) improves indirect-injection detection by distinguishing trusted vs.
  untrusted input spans. (azure.microsoft.com/blog/enhance-ai-security-with-azure-prompt-shields, 2025)
- Actions: block/allow only — no redact/mask mode documented for Prompt Shields itself.

## 4. LangChain / LangGraph

- LangChain's **LLM Gateway** (LangSmith) scans outbound requests before they reach the LLM
  provider. (docs.langchain.com/langsmith/llm-gateway-redaction, 2026)
- Presidio-based NER (names, locations, nationality/religion/political affiliation) plus regex for
  structured identifiers (SSN, phone) **and secrets** (API keys/tokens for OpenAI, GitHub, AWS,
  Slack, by pattern).
- **Explicitly documents propagation**: "redacted content also appears in LangSmith traces,
  ensuring sensitive data doesn't persist in observability logs." Documented gaps: LLM provider
  *responses*, traces that bypass the gateway, and system prompts/tool-call arguments are **not**
  covered. Fails closed (scanner failure blocks the request). This is the strongest documented
  redaction-propagation guarantee found in this survey.

## 5. NVIDIA NeMo Guardrails

- Five rail categories in `config.yml` — input, dialog, output, retrieval, execution. Input rails
  run pre-model (mask or reject); output rails run pre-response; retrieval rails run on RAG chunks.
  (docs.nvidia.com/nemo/guardrails/.../guardrails-process; .../pii-detection)
- Pluggable PII backends: Presidio (regex+NER), NVIDIA's GLiNER-PII NIM (ML/NER, configurable
  confidence threshold), or Private AI. **Sensitive-data detection is a separate rail category from
  dialog/jailbreak rails**, which are typically LLM-judge-based.
- Actions: mask (placeholder, e.g. `[FIRST_NAME]`) or block, uniformly across input/output/retrieval.

## 6. Guardrails AI (guardrails-ai / hub)

- Hub validators composed into a `Guard` wrapped around input or output.
- `detect_pii` uses Presidio (2026 update adds GLiNER combined with Presidio for accuracy);
  `SecretsPresent` detects AWS keys, OpenAI tokens, GitHub tokens, JWTs, private keys by pattern.
  (guardrailsai.com/hub/validator/guardrails/detect_pii; guardrailsai.com/blog/advanced-pii-and-jailbreak)
- Widest documented action vocabulary surveyed: `on_fail` = reask, fix (programmatic
  anonymization), filter, refrain (returns `None`), noop, exception, fix_reask.
- Propagation is best-effort/display-only unless the caller explicitly wires the validated
  (redacted) output into logging — no gateway-level guarantee like LangChain's.

## 7. AWS Bedrock Guardrails

- `sensitiveInformationPolicyConfig`, independently configurable `inputAction`/`outputAction`.
  **Explicitly does not inspect `tool_use` (function-call) parameters.**
  (docs.aws.amazon.com/bedrock/.../guardrails-sensitive-filters.html)
- "Probabilistic machine learning based" detection across ~30 built-in entity types (NAME,
  ADDRESS, EMAIL, CREDIT_DEBIT_CARD_NUMBER, US_SOCIAL_SECURITY_NUMBER, AWS_ACCESS_KEY/SECRET_KEY,
  IBAN, SWIFT_CODE, etc.) plus custom regex. Canadian SIN is explicitly checksum-validated (Luhn).
- Actions: `BLOCK`, `ANONYMIZE` (mask), or `NONE` (detect-only), per entity, per direction.
- **Explicitly NOT propagated**: masking applies only to what's sent to/from the model.
  **CloudWatch invocation logs always contain the original unmodified request regardless of
  guardrail action**; the API's own trace/`match` field returns the **original unmasked value**
  "by design." The clearest documented case in this survey of redaction being display/inference-only.

## 8. OpenAI

- **Moderation API**: free, 13 harm categories (harassment, hate, self-harm, sexual, violence,
  illicit) — explicitly **not** PII/secrets. Docs advise treating scores as signals, not automatic
  blocking. (developers.openai.com/api/docs/guides/moderation, 2026)
- **Agents SDK**: defines input/output/tool guardrail *types* as an extension mechanism only.
  (openai.github.io/openai-agents-python/guardrails)
- **openai-guardrails-python** (separate official preview package): layers PII detection
  (Presidio/spaCy `en_core_web_sm`) and masking on top of the Agents SDK guardrail types, plus
  URL allow/blocklists and moderation-API wrapping, run at client-init/pre-flight. Secret/API-key
  detection exists in examples (`sk-` pattern blocks) but is less fully documented than PII. Docs
  explicitly disclaim liability for third-party service logging/retention of scanned content.
  (github.com/openai/openai-guardrails-python, 2026)

## 9. Anthropic

- **No dedicated PII/secret detection product found.** Documented API safeguards are general Usage
  Policy enforcement: free real-time moderation callable pre-model against end-user prompts, plus
  post-model classifiers ("prompted or specially fine-tuned Claude models") flagging policy
  violations, and direct image-hash CSAM matching for child safety specifically — none framed as
  PII/secrets detection. (support.claude.com/.../api-safeguards-tools;
  anthropic.com/news/building-safeguards-for-claude, 2025)
- Data retention (not redaction): Enterprise/API customers can enable Zero-Data-Retention;
  flagged-conversation inputs/outputs retained 2 years, trust/safety scores 7 years.
  (platform.claude.com/docs/en/manage-claude/api-and-data-retention)
- **Claude Agent SDK** ships a deterministic **hooks** mechanism (`PreToolUse` runs before any tool
  executes and can deny regardless of permission mode/model judgment) plus permission modes — a
  general capability-gating primitive a developer could build PII/secret scanning on top of, but no
  built-in scanner ships. (platform.claude.com/docs/en/agent-sdk/hooks; .../permissions)
- Third-party compliance integrations (Nightfall AI, Noma Security) plug into a **Claude Compliance
  API** for PII/PHI/PCI/secrets detection in Enterprise usage — a partner-ecosystem feature, not
  first-party. (support.claude.com/.../get-started-with-claude-compliance-api-integrations)

## Cross-cutting patterns

1. **Presidio is the de facto shared PII engine.** LangChain's gateway, NeMo Guardrails, Guardrails
   AI hub, and OpenAI's own `openai-guardrails-python` all wrap it rather than reimplement NER/regex
   detection — unpaid shared infrastructure across at least 4 of the 9 systems surveyed.
2. **Deterministic shape-matching (regex/checksum) for PII/secrets is treated as a first-party,
   "solved" feature; injection/jailbreak detection is treated as inherently heuristic and less
   certain.** Bedrock, LangChain's gateway, and Guardrails AI all ship named entity types and regex
   as plain configuration; Azure Prompt Shields, NeMo's dialog rails, and Anthropic's classifiers all
   frame injection/jailbreak detection as probabilistic, with explicit false-positive/negative
   caveats. **This matches this project's own §9 Q1 resolution in 017 almost exactly** — PII
   shape-matching ships first-party because it is deterministic and testable to 100% precision on
   shape; injection/jailbreak detection stays seam-only because it is an open-ended quality
   commitment. That resolution isn't a novel position — it's the industry's consensus dividing line,
   independently arrived at.
3. **Redaction guarantees to logs/traces are the exception, not the rule, and cut both ways.**
   LangChain's LLM Gateway explicitly guarantees redacted content in LangSmith traces, fail-closed on
   scanner failure. AWS Bedrock explicitly documents the **opposite** for its own logs and trace API
   (original unmasked value preserved "by design"). Agent Framework's default posture is "don't log
   at all" rather than "redact then log." **No framework surveyed guarantees redaction-before-log by
   default without explicit configuration** — which makes this project's G4 gate ("redaction
   propagates to history, telemetry, recordings, and checkpoints," proven by canary scan in
   `tests/test_agent_session_redact.cpp`) a stronger default guarantee than any single framework
   surveyed ships out of the box. Worth being explicit about that as a differentiator, not assuming
   it's table stakes.
4. **Frameworks vs. platforms split cleanly on "built-in or extension point."** Semantic Kernel,
   Agent Framework, LangChain (core), and Claude Agent SDK ship only middleware/filter/hook
   *mechanisms*, leaving PII/secret logic to the developer or a bolt-on. Managed platforms with a
   model-hosting business (Bedrock, Azure AI Content Safety, NeMo as NVIDIA's packaged product) ship
   the actual detector as a first-class, separately billed service.
5. **Input vs. output symmetry is inconsistent across the field.** Bedrock and NeMo explicitly
   support independent input/output configuration for the same entity types; Azure Prompt Shields is
   input-only; Anthropic's classifiers are documented as post-model/output-side only.
6. **Tool-call arguments are a documented blind spot.** Bedrock explicitly excludes `tool_use`
   parameters from sensitive-information scanning; LangChain's gateway explicitly excludes
   system prompts and tool-call arguments too. This project's own filter points (`tool_args`,
   `tool_result` — 017 §4) already cover exactly this gap by design, which the survey suggests is
   an active weak spot elsewhere, not a hypothetical one.

## Relevance to this project

This project's existing design ([017 §4-§5](../../017-Safety-and-Content-Governance.md),
[018 §4](../../018-Identity-Authorization-and-Secrets.md)) already lands on the same shape the
survey converges on, and in two places goes further than anything found:

- **Filter points include `tool_args`/`tool_result`**, closing the blind spot both Bedrock and
  LangChain's gateway document as explicitly out of scope for themselves.
- **The secret seam (018 §4) makes `SecretLease` unprintable by construction** (formatter,
  serializer, debug output all print `***`) rather than relying on a scanner to catch a credential
  after the fact — a stronger structural guarantee than any detect-and-mask approach surveyed, none
  of which prevent a secret from being stringified in the first place.
- **G4's redaction-propagates-to-telemetry/checkpoints guarantee**, canary-tested, is stronger than
  every framework surveyed: most either don't guarantee it (Guardrails AI, Bedrock) or solve it by
  not logging at all (Agent Framework) rather than by redacting-then-logging.

No counter-evidence found suggesting the current design is missing a mechanism class other
frameworks treat as necessary — the survey is confirmatory, not corrective.
