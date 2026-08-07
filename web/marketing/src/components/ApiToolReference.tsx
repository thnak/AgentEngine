import {
  gh,
  jsonSchemaTypeMapping,
  toolDescriptorSnippet,
  toolSchemaOutput,
  toolSchemaSnippet,
  toolStaticMembers,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

export function ApiToolReference() {
  return (
    <section className="section" id="tool-reference">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">006 — Tool and Function Plane</span>
          <h2>
            A tool's <span className="grad-text">full shape</span>, field by field
          </h2>
          <p>
            <code>Tool&lt;Derived, Policies...&gt;</code> fixes exactly one declaration surface —
            a schema-typed name, description, argument/reply pair, and an <code>invoke</code>{" "}
            reachable only through the host's ten-step pipeline (006 §3). Every row below is a
            real static member <code>Derived</code> must or may provide.
          </p>
        </div>

        <RevealGroup>
          <RevealItem>
            <ApiTable
              columns={["Member", "Type", "Required", "Notes"]}
              templateColumns="1.3fr 1.6fr 0.7fr 2.4fr"
              rows={toolStaticMembers.map((f) => [
                <code key="name">{f.name}</code>,
                <code key="type">{f.type}</code>,
                f.required ? "required" : "optional",
                f.notes,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-note" style={{ marginTop: 28, borderLeftColor: "var(--accent-pink)" }}>
              <strong>What a field can't carry: a description.</strong> C++23 has no compile-time
              reflection (P2996 lands in C++26), so <code>AE_JSON_SCHEMA(Type, field1, field2, ...)</code>{" "}
              can only see the bare field <em>names</em> you list — there is no macro parameter, no
              doc-comment extraction, and no per-field <code>"description"</code>, <code>"title"</code>,
              or <code>enum</code> value list in the generated schema. A C++ enum flattens to a plain{" "}
              <code>"integer"</code>. If you need a model to understand a parameter beyond its name and
              type, that explanation currently has to live in the tool's own top-level{" "}
              <code>description</code> string.
              <div style={{ marginTop: 8 }}>
                <a href={gh("include/agentengine/core/json_schema.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/json_schema.hpp:5-19
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">C++ type → JSON Schema type</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                What <code>AE_JSON_SCHEMA</code> actually emits
              </h3>
            </div>
            <ApiTable
              columns={["C++ field type", "JSON Schema", "Note"]}
              templateColumns="1.1fr 1.6fr 2.3fr"
              rows={jsonSchemaTypeMapping.map((t) => [
                <code key="cpp">{t.cpp}</code>,
                <code key="json">{t.json}</code>,
                t.note,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="spec-layout" style={{ marginTop: 48 }}>
          <RevealGroup>
            <RevealItem>
              <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 16 }}>
                A real, tested fixture — nested types, an array of objects, and an optional field
                all in one worked example:
              </p>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="test_tool_json_schema.cpp">{highlightCpp(toolSchemaSnippet)}</CodePanel>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 16 }}>
                <code>json_schema_of&lt;SearchReply&gt;()</code> — the richest of the three, showing
                array-of-object nesting and the required/optional split at once:
              </p>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="SearchReply.schema.json">{toolSchemaOutput}</CodePanel>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">Where the schema goes</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>ToolDescriptor</code> — one entry in the per-run tool table
              </h3>
              <p>
                <code>register_agent&lt;A&gt;()</code> compiles every declared tool into this shape
                (006 §6: "resolved at run start into an immutable per-run tool table"). This is a
                real external serialization path, not internal-only validation: Anthropic's client
                parses <code>args_schema_json</code> into <code>{"{name, description, input_schema}"}</code>{" "}
                for <code>/v1/messages</code>; OpenAI's parses it into{" "}
                <code>{'{type:"function", function:{name, description, parameters}}'}</code> for{" "}
                <code>/v1/chat/completions</code> — the exact JSON your tool's field types produced is
                what the model receives.
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tool_pipeline.hpp">{highlightCpp(toolDescriptorSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
