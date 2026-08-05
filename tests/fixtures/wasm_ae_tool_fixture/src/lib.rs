// D3 proof fixture (decisions/ADR-010-wasm-component-host-manifest-capability-binding.md §7.2) --
// a minimal, real `ae:tool` component: two tools, "echo" (zero capabilities) and "now" (imports
// `ae:tool/clock`), used to prove the manifest-vs-import verification and capability-handle binding
// mechanism against a genuinely compiled component, not a hand-crafted stub. Source owned and
// reviewed for this proof, matching D1's provenance standard.

#[allow(warnings)]
mod bindings;

use bindings::ae::tool::clock;
use bindings::ae::tool::types::{ContentItem, DataContent, TextItem, ToolResult};
use bindings::exports::ae::tool::guest::{Guest, InvokeRequest, ToolDescriptor};

struct Component;

impl Guest for Component {
    fn list_tools() -> Vec<ToolDescriptor> {
        vec![
            ToolDescriptor {
                name: "echo".to_string(),
                description: "Returns its input text unchanged.".to_string(),
                args_schema_json: "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}"
                    .to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: true,
            },
            ToolDescriptor {
                name: "now".to_string(),
                description: "Returns the current time via ae:tool/clock.".to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"integer\"}".to_string(),
                parallelizable: false,
            },
            ToolDescriptor {
                name: "spin".to_string(),
                description: "Loops until interrupted -- proves wall_ms epoch enforcement.".to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: true,
            },
        ]
    }

    fn invoke(request: InvokeRequest) -> ToolResult {
        match request.tool_name.as_str() {
            "echo" => ToolResult {
                content: vec![ContentItem::Text(TextItem {
                    text: request.args_json,
                    tainted: false,
                })],
                is_error: false,
            },
            "now" => {
                let Some(cap) = request.capabilities.first() else {
                    return ToolResult {
                        content: vec![ContentItem::Text(TextItem {
                            text: "now: no capability handle supplied".to_string(),
                            tainted: false,
                        })],
                        is_error: true,
                    };
                };
                let millis = clock::now_unix_millis(cap);
                ToolResult {
                    content: vec![ContentItem::Data(DataContent {
                        json: millis.to_string(),
                        schema_id: None,
                    })],
                    is_error: false,
                }
            }
            "spin" => {
                // No capability needed -- a pure compute loop the host's epoch deadline must still
                // be able to interrupt (epoch interruption checks happen at function-entry/loop-
                // backedge points the compiler inserts, independent of what the guest code does).
                let mut x: u64 = 0;
                loop {
                    x = x.wrapping_add(1).wrapping_mul(2654435761);
                    core::hint::black_box(x);
                }
            }
            _ => ToolResult {
                content: vec![ContentItem::Text(TextItem {
                    text: format!("unknown tool: {}", request.tool_name),
                    tainted: false,
                })],
                is_error: true,
            },
        }
    }
}

bindings::export!(Component with_types_in bindings);
