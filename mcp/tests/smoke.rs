//! End-to-end smoke test: real MCP handshake over stdio against the
//! `irez-mcp` binary, backed by a real irez state created via the CLI.
//!
//! Run with: cargo test --test smoke
//!
//! The irez CLI is located via `IREZ_CLI`, defaulting to `../build/irez`
//! relative to this crate. The state directory is a fresh temp dir.

use std::path::{Path, PathBuf};
use std::process::Stdio;

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};

fn irez_cli() -> PathBuf {
    std::env::var("IREZ_CLI")
        .map(PathBuf::from)
        .unwrap_or_else(|_| Path::new(env!("CARGO_MANIFEST_DIR")).join("../build/irez"))
}

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../fixtures")
        .join(name)
}

/// Create a fresh state dir and ingest the nonfloating fixture.
fn prepare_state(state_dir: &Path) {
    let cli = irez_cli();
    assert!(cli.exists(), "irez CLI not found at {}", cli.display());
    let _ = std::fs::remove_dir_all(state_dir);

    let run = |args: &[&str]| {
        let output = std::process::Command::new(&cli)
            .arg("--state-dir")
            .arg(state_dir)
            .args(args)
            .output()
            .expect("failed to run irez CLI");
        assert!(
            output.status.success(),
            "irez {:?} failed: {}",
            args,
            String::from_utf8_lossy(&output.stderr)
        );
    };

    run(&["init", "--name", "smoke"]);
    run(&[
        "ingest",
        "llvm",
        fixture("nonfloating.ll").to_str().unwrap(),
        "--index",
        "full",
    ]);
    run(&[
        "ingest",
        "llvm",
        fixture("unicode_invalid_name.ll").to_str().unwrap(),
        "--index",
        "full",
    ]);
}

struct McpPipe {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

impl McpPipe {
    async fn spawn(state_dir: &Path) -> Self {
        let mut child = Command::new(env!("CARGO_BIN_EXE_irez-mcp"))
            .env("IREZ_STATE_DIR", state_dir)
            .env("IREZ_CLI", irez_cli())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .expect("failed to spawn irez-mcp");
        let stdin = child.stdin.take().unwrap();
        let stdout = BufReader::new(child.stdout.take().unwrap());
        Self {
            child,
            stdin,
            stdout,
        }
    }

    async fn send(&mut self, msg: Value) {
        let mut line = serde_json::to_string(&msg).unwrap();
        line.push('\n');
        self.stdin.write_all(line.as_bytes()).await.unwrap();
        self.stdin.flush().await.unwrap();
    }

    async fn recv(&mut self) -> Value {
        let mut line = String::new();
        let n = self.stdout.read_line(&mut line).await.unwrap();
        assert!(n > 0, "server closed stdout unexpectedly");
        serde_json::from_str(line.trim()).expect("server emitted non-JSON on stdout")
    }

    /// Send a JSON-RPC request and return its response.
    async fn request(&mut self, id: i64, method: &str, params: Value) -> Value {
        self.send(json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params,
        }))
        .await;
        let response = self.recv().await;
        assert_eq!(response["id"], id, "response id mismatch: {response}");
        response
    }
}

#[tokio::test]
async fn smoke_handshake_and_tools() {
    let state_dir = std::env::temp_dir().join(format!("irez-mcp-smoke-{}", std::process::id()));
    prepare_state(&state_dir);

    let mut mcp = McpPipe::spawn(&state_dir).await;

    // 1. initialize (protocol 2026-07-28)
    let init = mcp
        .request(
            1,
            "initialize",
            json!({
                "protocolVersion": "2026-07-28",
                "capabilities": {},
                "clientInfo": { "name": "irez-mcp-smoke", "version": "0.1.0" },
            }),
        )
        .await;
    let init_result = &init["result"];
    assert!(
        init_result["protocolVersion"].is_string(),
        "initialize failed: {init}"
    );
    assert_eq!(init_result["serverInfo"]["name"], "irez-mcp");
    assert!(init_result["capabilities"]["tools"].is_object());

    // 2. notifications/initialized
    mcp.send(json!({
        "jsonrpc": "2.0",
        "method": "notifications/initialized",
    }))
    .await;

    // 3. tools/list — all 14 irez tools present
    let list = mcp.request(2, "tools/list", json!({})).await;
    let tools = list["result"]["tools"]
        .as_array()
        .expect("tools/list failed");
    let mut names: Vec<&str> = tools.iter().map(|t| t["name"].as_str().unwrap()).collect();
    names.sort_unstable();
    assert_eq!(
        names,
        vec![
            "irez_artifacts",
            "irez_capabilities",
            "irez_context",
            "irez_expand",
            "irez_functions",
            "irez_graph",
            "irez_guards",
            "irez_show",
            "irez_slice",
            "irez_source",
            "irez_status",
            "irez_trace_return",
            "irez_trace_stores",
            "irez_uses",
        ]
    );

    // 4. tools/call irez_status — envelope comes back as structured content
    let call = mcp
        .request(
            3,
            "tools/call",
            json!({ "name": "irez_status", "arguments": {} }),
        )
        .await;
    let result = &call["result"];
    assert_eq!(
        result["isError"],
        json!(false),
        "irez_status errored: {call}"
    );
    let envelope = &result["structuredContent"];
    assert_eq!(envelope["schema_version"], 1);
    assert_eq!(envelope["command"], "status");
    assert_eq!(envelope["result"]["functions"], 3);
    assert_eq!(envelope["result"]["db_schema_version"], 2);
    assert_eq!(envelope["result"]["api_schema_version"], 1);

    // 5. tools/call with a bad handle — CLI error surfaced as isError
    let err = mcp
        .request(
            4,
            "tools/call",
            json!({
                "name": "irez_show",
                "arguments": { "handle": "bogus:handle" },
            }),
        )
        .await;
    let err_result = &err["result"];
    assert_eq!(
        err_result["isError"],
        json!(true),
        "expected isError: {err}"
    );
    assert_eq!(err_result["structuredContent"]["schema_version"], 1);
    assert!(err_result["structuredContent"]["error"].is_string());

    // 6. Every query tool crosses the real CLI -> UTF-8 JSON -> MCP boundary.
    let artifacts = mcp
        .request(
            5,
            "tools/call",
            json!({ "name": "irez_artifacts", "arguments": {} }),
        )
        .await;
    let artifact_rows = artifacts["result"]["structuredContent"]["result"]
        .as_array()
        .unwrap();
    let normal_artifact = artifact_rows
        .iter()
        .find(|row| {
            row["original_path"]
                .as_str()
                .is_some_and(|path| path.ends_with("nonfloating.ll"))
        })
        .unwrap()["id"]
        .as_str()
        .unwrap()
        .to_string();
    let unicode_artifact = artifact_rows
        .iter()
        .find(|row| {
            row["original_path"]
                .as_str()
                .is_some_and(|path| path.ends_with("unicode_invalid_name.ll"))
        })
        .unwrap()["id"]
        .as_str()
        .unwrap()
        .to_string();

    let capabilities = mcp
        .request(
            6,
            "tools/call",
            json!({ "name": "irez_capabilities", "arguments": { "artifact": normal_artifact } }),
        )
        .await;
    assert_eq!(capabilities["result"]["isError"], json!(false));

    let functions = mcp
        .request(
            7,
            "tools/call",
            json!({ "name": "irez_functions", "arguments": { "artifact": normal_artifact, "match": "choose" } }),
        )
        .await;
    let function = functions["result"]["structuredContent"]["result"][0]["handle"]
        .as_str()
        .unwrap()
        .to_string();

    // Legacy `entity_id` is accepted; canonical output remains `handle`.
    let shown = mcp
        .request(
            8,
            "tools/call",
            json!({ "name": "irez_show", "arguments": {
                "entity_id": function, "future_optional_field": true
            }}),
        )
        .await;
    assert_eq!(shown["result"]["isError"], json!(false));
    assert!(shown["result"]["structuredContent"]["result"]["handle"].is_string());
    assert!(shown["result"]["structuredContent"]["result"]["exact_text"].is_null());

    let conflicting_aliases = mcp
        .request(
            81,
            "tools/call",
            json!({ "name": "irez_source", "arguments": {
                "handle": function, "id": "irez:conflicting"
            }}),
        )
        .await;
    assert!(conflicting_aliases["error"].is_object());

    let children = mcp
        .request(
            9,
            "tools/call",
            json!({ "name": "irez_show", "arguments": {
                "handle": function, "view": "children", "kind": "return", "budget_nodes": 5
            }}),
        )
        .await;
    let return_handle = children["result"]["structuredContent"]["result"]["items"][0]["handle"]
        .as_str()
        .unwrap()
        .to_string();

    let calls = [
        ("irez_source", json!({ "handle": return_handle })),
        (
            "irez_uses",
            json!({ "handle": return_handle, "budget_nodes": 5 }),
        ),
        (
            "irez_slice",
            json!({ "handle": return_handle, "direction": "backward", "relations": "operand", "budget_nodes": 20, "budget_depth": 8 }),
        ),
        (
            "irez_graph",
            json!({ "handle": return_handle, "direction": "backward", "budget_nodes": 20, "format": "json" }),
        ),
        (
            "irez_guards",
            json!({ "handle": return_handle, "budget_nodes": 20 }),
        ),
        ("irez_expand", json!({ "handle": function })),
        (
            "irez_context",
            json!({ "handle": return_handle, "budget_nodes": 20 }),
        ),
        (
            "irez_trace_return",
            json!({ "handle": function, "return": "all", "budget_nodes": 20, "budget_depth": 8 }),
        ),
        (
            "irez_trace_stores",
            json!({ "handle": function, "budget_nodes": 20, "budget_depth": 8 }),
        ),
    ];
    for (offset, (name, arguments)) in calls.into_iter().enumerate() {
        let response = mcp
            .request(
                10 + offset as i64,
                "tools/call",
                json!({ "name": name, "arguments": arguments }),
            )
            .await;
        assert_eq!(
            response["result"]["isError"],
            json!(false),
            "{name}: {response}"
        );
        assert!(response["result"]["structuredContent"].is_object());
    }

    // The LLVM identifier contains invalid UTF-8 bytes. The CLI must replace
    // them, mark the response lossy, and still emit valid MCP JSON.
    let unicode_functions = mcp
        .request(
            30,
            "tools/call",
            json!({ "name": "irez_functions", "arguments": { "artifact": unicode_artifact } }),
        )
        .await;
    assert_eq!(unicode_functions["result"]["isError"], json!(false));
    assert_eq!(
        unicode_functions["result"]["structuredContent"]["encoding_lossy"],
        json!(true)
    );

    mcp.child.kill().await.unwrap();
    let _ = std::fs::remove_dir_all(&state_dir);
}
