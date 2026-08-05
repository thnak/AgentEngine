// D3 proof fixture (decisions/ADR-010-wasm-component-host-manifest-capability-binding.md §7.2),
// extended by D5 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md) -- a minimal,
// real `ae:tool` component used to prove the manifest-vs-import verification and capability-handle
// binding mechanism against a genuinely compiled component, not a hand-crafted stub. "echo" needs no
// capability; "now" imports `ae:tool/clock`; "read-file"/"write-file"/"fetch"/"get-secret" import
// `ae:tool/fs`/`ae:tool/http`/`ae:tool/secrets` respectively, added so D5 can exercise each gated
// callback's own wrong-kind-capability rejection directly (ADR-010 claim 4), not just clock's. Source
// owned and reviewed for this proof, matching D1's provenance standard.

#[allow(warnings)]
mod bindings;

use bindings::ae::tool::clock;
use bindings::ae::tool::fs;
use bindings::ae::tool::http;
use bindings::ae::tool::secrets;
use bindings::ae::tool::types::{ContentItem, DataContent, TextItem, ToolResult};
use bindings::exports::ae::tool::guest::{Guest, InvokeRequest, ToolDescriptor};

struct Component;

fn no_capability_error(tool: &str) -> ToolResult {
    ToolResult {
        content: vec![ContentItem::Text(TextItem {
            text: format!("{tool}: no capability handle supplied"),
            tainted: false,
        })],
        is_error: true,
    }
}

fn text_result(text: String) -> ToolResult {
    ToolResult {
        content: vec![ContentItem::Text(TextItem { text, tainted: false })],
        is_error: false,
    }
}

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
            ToolDescriptor {
                name: "read-file".to_string(),
                description: "Calls ae:tool/fs.fs-read -- D5's per-callback kind-check probe.".to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: false,
            },
            ToolDescriptor {
                name: "write-file".to_string(),
                description: "Calls ae:tool/fs.fs-write -- D5's per-callback kind-check probe.".to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: false,
            },
            ToolDescriptor {
                name: "fetch".to_string(),
                description: "Calls ae:tool/http.http-request -- D5's per-callback kind-check probe."
                    .to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: false,
            },
            ToolDescriptor {
                name: "get-secret".to_string(),
                description: "Calls ae:tool/secrets.resolve-secret -- D5's per-callback kind-check probe."
                    .to_string(),
                args_schema_json: "{\"type\":\"object\"}".to_string(),
                result_schema_json: "{\"type\":\"string\"}".to_string(),
                parallelizable: false,
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
            // The four arms below exist solely so D5 can exercise each gated callback's own
            // wrong-kind-capability rejection directly (ADR-010 claim 4 was proven for
            // now-unix-millis only in D3). Every one of these host functions is an unimplemented
            // stub that traps after its capability-kind check passes (wasm_backend.cpp's
            // cb_fs_read/cb_fs_write/cb_http_request/cb_resolve_secret) -- so on the host's current,
            // documented M2 scope this call never returns to the guest at all: it either traps with
            // "wrong kind" (kind check failed) or "not implemented" (kind check passed). The match
            // arms below only exist to make the call and let the compiler see a well-typed Result;
            // the ToolResult they construct is unreachable under the host's current behavior.
            "read-file" => {
                let Some(cap) = request.capabilities.first() else {
                    return no_capability_error("read-file");
                };
                match fs::fs_read(cap, "/probe") {
                    Ok(_) => text_result("read-file: unexpectedly succeeded".to_string()),
                    Err(_) => text_result("read-file: fs-error returned".to_string()),
                }
            }
            "write-file" => {
                let Some(cap) = request.capabilities.first() else {
                    return no_capability_error("write-file");
                };
                match fs::fs_write(cap, "/probe", &[]) {
                    Ok(_) => text_result("write-file: unexpectedly succeeded".to_string()),
                    Err(_) => text_result("write-file: fs-error returned".to_string()),
                }
            }
            "fetch" => {
                let Some(cap) = request.capabilities.first() else {
                    return no_capability_error("fetch");
                };
                let req = http::HttpRequestData {
                    method: "GET".to_string(),
                    path: "/probe".to_string(),
                    headers: vec![],
                    body: None,
                };
                match http::http_request(cap, &req) {
                    Ok(_) => text_result("fetch: unexpectedly succeeded".to_string()),
                    Err(_) => text_result("fetch: http-error returned".to_string()),
                }
            }
            "get-secret" => {
                let Some(cap) = request.capabilities.first() else {
                    return no_capability_error("get-secret");
                };
                match secrets::resolve_secret(cap) {
                    Ok(_) => text_result("get-secret: unexpectedly succeeded".to_string()),
                    Err(_) => text_result("get-secret: secret-error returned".to_string()),
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
