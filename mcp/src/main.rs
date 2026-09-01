//! irez-mcp: a thin MCP (stdio) server over the irez JSON CLI.
//!
//! Rust carries no core logic: every tool spawns the `irez` CLI, passes
//! arguments, and returns the CLI's JSON envelope verbatim as structured
//! content. Configuration is read per call from the process environment:
//!
//! - `IREZ_STATE_DIR`: state directory passed as `--state-dir` (default `.irez`)
//! - `IREZ_CLI`: path to the irez binary (default `irez`, resolved via PATH)

use anyhow::Context;
use rmcp::{
    handler::server::{router::tool::ToolRouter, wrapper::Parameters},
    model::*,
    schemars, tool, tool_handler, tool_router,
    transport::stdio,
    ErrorData as McpError, ServerHandler, ServiceExt,
};
use serde::Deserialize;
use serde_json::Value;
use std::process::Stdio;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::Command;

const EXPECTED_TOOL_COUNT: usize = 14;

mod contract;
mod install;

/// Spawn the irez CLI with `--state-dir` plus `args` and wrap its JSON output.
///
/// On exit code 0 the stdout envelope is returned as structured content. On
/// non-zero exit the CLI writes a JSON error object to stderr; it is surfaced
/// as a tool-level error (`isError: true`) with the error JSON preserved.
async fn run_irez(args: Vec<String>) -> Result<CallToolResult, McpError> {
    let cli = std::env::var("IREZ_CLI").unwrap_or_else(|_| "irez".to_string());
    let state_dir = std::env::var("IREZ_STATE_DIR").unwrap_or_else(|_| ".irez".to_string());

    let timeout_secs = std::env::var("IREZ_TOOL_TIMEOUT_SECONDS")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(60);
    let mut command = Command::new(&cli);
    command
        .kill_on_drop(true)
        .arg("--state-dir")
        .arg(&state_dir)
        .args(&args);
    let output = tokio::time::timeout(
        std::time::Duration::from_secs(timeout_secs),
        command.output(),
    )
    .await
    .map_err(|_| {
        McpError::internal_error(
            format!("irez CLI timed out after {timeout_secs} seconds"),
            None,
        )
    })?
    .map_err(|e| {
        McpError::internal_error(format!("failed to spawn irez CLI '{cli}': {e}"), None)
    })?;
    const MAX_OUTPUT: usize = 16 * 1024 * 1024;
    if output.stdout.len().saturating_add(output.stderr.len()) > MAX_OUTPUT {
        return Err(McpError::internal_error(
            "irez CLI output exceeded the 16 MiB MCP limit".to_string(),
            None,
        ));
    }

    if output.status.success() {
        let value: Value = serde_json::from_slice(&output.stdout).map_err(|e| {
            McpError::internal_error(
                format!("irez CLI returned invalid JSON on stdout: {e}"),
                None,
            )
        })?;
        if value.is_object() {
            Ok(CallToolResult::structured(value))
        } else {
            // The envelope is always an object; this is a defensive fallback.
            Ok(CallToolResult::success(vec![ContentBlock::text(
                value.to_string(),
            )]))
        }
    } else {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        match serde_json::from_str::<Value>(&stderr) {
            Ok(value) if value.is_object() => Ok(CallToolResult::structured_error(value)),
            _ => Ok(CallToolResult::error(vec![ContentBlock::text(format!(
                "irez CLI exited with {}: {}",
                output.status, stderr
            ))])),
        }
    }
}

/// Push `--flag value` when the option is set.
fn push_opt(args: &mut Vec<String>, flag: &str, value: &Option<String>) {
    if let Some(v) = value {
        args.push(flag.to_string());
        args.push(v.clone());
    }
}

fn default_budget_nodes() -> i64 {
    100
}
fn default_direction() -> String {
    "backward".to_string()
}
fn default_relations() -> String {
    "operand,control".to_string()
}
fn default_budget_depth() -> i64 {
    12
}
fn default_trace_budget_nodes() -> i64 {
    50
}
fn default_trace_budget_depth() -> i64 {
    8
}
fn default_format() -> String {
    "json".to_string()
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ArtifactArgs {
    /// Artifact handle to scope the query.
    pub artifact: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct FunctionsArgs {
    /// Artifact handle to scope the query.
    pub artifact: Option<String>,
    /// Regex filter applied to function names.
    #[serde(rename = "match")]
    pub match_: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct HandleArgs {
    /// Canonical entity handle returned as `handle`, e.g. `irez:abcd:llvm:inst:f0:1`.
    pub handle: Option<String>,
    /// Deprecated compatibility alias for `handle`.
    pub id: Option<String>,
    /// Deprecated compatibility alias for `handle`.
    pub entity_id: Option<String>,
}

impl HandleArgs {
    fn resolve(self) -> Result<String, McpError> {
        let supplied: Vec<String> = [self.handle, self.id, self.entity_id]
            .into_iter()
            .flatten()
            .collect();
        let Some(first) = supplied.first() else {
            return Err(McpError::invalid_params(
                "pass the returned entity `handle` (legacy `id` and `entity_id` are accepted)"
                    .to_string(),
                None,
            ));
        };
        if supplied.iter().any(|value| value != first) {
            return Err(McpError::invalid_params(
                "conflicting handle/id/entity_id values".to_string(),
                None,
            ));
        }
        Ok(first.clone())
    }
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct BudgetArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// Maximum number of nodes to visit.
    #[serde(default = "default_budget_nodes")]
    pub budget_nodes: i64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct SliceArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// Slice direction: "backward" or "forward".
    #[serde(default = "default_direction")]
    pub direction: String,
    /// Comma-separated relation list, e.g. "operand,control".
    #[serde(default = "default_relations")]
    pub relations: String,
    /// Maximum number of nodes to visit.
    #[serde(default = "default_budget_nodes")]
    pub budget_nodes: i64,
    /// Maximum traversal depth.
    #[serde(default = "default_budget_depth")]
    pub budget_depth: i64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct GraphArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// Graph direction: "backward" or "forward".
    #[serde(default = "default_direction")]
    pub direction: String,
    /// Maximum number of nodes to visit.
    #[serde(default = "default_budget_nodes")]
    pub budget_nodes: i64,
    /// Output format: "json" or "exact-ir".
    #[serde(default = "default_format")]
    pub format: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct ShowArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// `summary` is bounded and is the default; `exact` returns complete function IR;
    /// `children` returns a bounded block/return/call list.
    pub view: Option<String>,
    /// Child selector for `view=children`: `block`, `return`, or `call`.
    pub kind: Option<String>,
    /// Maximum children returned by `view=children`.
    #[serde(default = "default_budget_nodes")]
    pub budget_nodes: i64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct TraceReturnArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// `all` or one return-instruction handle returned by `irez_show` children.
    #[serde(rename = "return")]
    pub return_selector: Option<String>,
    /// Maximum graph nodes per return site.
    #[serde(default = "default_trace_budget_nodes")]
    pub budget_nodes: i64,
    /// Maximum operand depth per return site.
    #[serde(default = "default_trace_budget_depth")]
    pub budget_depth: i64,
    /// `summary` (default) returns the return-chain spine, node/relation
    /// counts, scalar/vector value shape, named direct-call targets, per-site
    /// truncation, and source evidence without graph payload. `graph`
    /// returns compact nodes and relations; request it only when the actual
    /// nodes/relations are needed.
    pub detail: Option<String>,
    /// Optional section list added on top of the detail default. Valid
    /// sections: chain, calls, source, nodes, relations, boundaries, flags.
    /// The flags section sparsely lists nodes carrying non-default boolean
    /// instruction attributes (nsw, exact, fast-math family). Per-site
    /// truncation is always present in every projection.
    pub include: Option<Vec<String>>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
pub struct TraceStoresArgs {
    #[serde(flatten)]
    pub entity: HandleArgs,
    /// Maximum graph nodes per store site.
    #[serde(default = "default_trace_budget_nodes")]
    pub budget_nodes: i64,
    /// Maximum operand depth per store site.
    #[serde(default = "default_trace_budget_depth")]
    pub budget_depth: i64,
    /// `summary` (default) or `graph`; same projection contract as
    /// irez_trace_return.
    pub detail: Option<String>,
    /// Optional section list added on top of the detail default. Valid
    /// sections: chain, calls, source, nodes, relations, boundaries, flags.
    pub include: Option<Vec<String>>,
}

#[derive(Clone)]
pub struct IrezMcp {
    tool_router: ToolRouter<Self>,
}

impl Default for IrezMcp {
    fn default() -> Self {
        Self::new()
    }
}

#[tool_router]
impl IrezMcp {
    pub fn new() -> Self {
        Self {
            tool_router: Self::tool_router(),
        }
    }

    #[tool(
        description = "Return investigation status, including the resolved absolute state directory (state_dir) and all schema/adapter/build versions. Check state_dir first when artifact or function counts look wrong — a mismatched state directory is the most common cause."
    )]
    async fn irez_status(&self) -> Result<CallToolResult, McpError> {
        run_irez(vec!["status".to_string()]).await
    }

    #[tool(description = "List ingested artifacts with ids, hashes, and provenance.")]
    async fn irez_artifacts(&self) -> Result<CallToolResult, McpError> {
        run_irez(vec!["artifacts".to_string()]).await
    }

    #[tool(
        description = "Return supported, partial, unknown, and unsupported adapter capabilities."
    )]
    async fn irez_capabilities(
        &self,
        Parameters(args): Parameters<ArtifactArgs>,
    ) -> Result<CallToolResult, McpError> {
        let mut argv = vec!["capabilities".to_string()];
        push_opt(&mut argv, "--artifact", &args.artifact);
        run_irez(argv).await
    }

    #[tool(description = "List cataloged functions, optionally filtered by artifact and regex.")]
    async fn irez_functions(
        &self,
        Parameters(args): Parameters<FunctionsArgs>,
    ) -> Result<CallToolResult, McpError> {
        let mut argv = vec!["functions".to_string()];
        push_opt(&mut argv, "--artifact", &args.artifact);
        push_opt(&mut argv, "--match", &args.match_);
        run_irez(argv).await
    }

    #[tool(
        description = "Show an entity by the returned handle. Functions default to a bounded summary; use view=children to locate returns/calls/blocks, and request view=exact only when complete IR is necessary. Legacy id/entity_id inputs are accepted."
    )]
    async fn irez_show(
        &self,
        Parameters(args): Parameters<ShowArgs>,
    ) -> Result<CallToolResult, McpError> {
        let handle = args.entity.resolve()?;
        let mut argv = vec!["show".to_string(), handle];
        push_opt(&mut argv, "--view", &args.view);
        push_opt(&mut argv, "--kind", &args.kind);
        argv.extend(["--budget-nodes".to_string(), args.budget_nodes.to_string()]);
        run_irez(argv).await
    }

    #[tool(
        description = "Return partial source/inline frames for an entity. Pass the `handle` returned by IREZ; legacy id/entity_id inputs are accepted."
    )]
    async fn irez_source(
        &self,
        Parameters(args): Parameters<HandleArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec!["source".to_string(), args.resolve()?]).await
    }

    #[tool(
        description = "Find direct users of a value with a bounded node budget. Pass the returned entity `handle`; use a forward slice only when transitive influence is required."
    )]
    async fn irez_uses(
        &self,
        Parameters(args): Parameters<BudgetArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec![
            "uses".to_string(),
            args.entity.resolve()?,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
        ])
        .await
    }

    #[tool(
        description = "Traverse a bounded relation slice from the returned entity `handle`. Use backward operand for causes and forward operand for transitive consumers; inspect unknowns and truncation boundaries."
    )]
    async fn irez_slice(
        &self,
        Parameters(args): Parameters<SliceArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec![
            "slice".to_string(),
            args.entity.resolve()?,
            "--direction".to_string(),
            args.direction,
            "--relations".to_string(),
            args.relations,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
            "--budget-depth".to_string(),
            args.budget_depth.to_string(),
        ])
        .await
    }

    #[tool(
        description = "Find control guards for a callsite/instruction `handle`. Returns exact completed control-dependence evidence when available, otherwise a clearly marked conservative CFG fallback."
    )]
    async fn irez_guards(
        &self,
        Parameters(args): Parameters<BudgetArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec![
            "guards".to_string(),
            args.entity.resolve()?,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
        ])
        .await
    }

    #[tool(
        description = "Return a bounded operand/value graph from an entity `handle`. Prefer JSON for reasoning; request exact-ir only when precise LLVM text or flags are material."
    )]
    async fn irez_graph(
        &self,
        Parameters(args): Parameters<GraphArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec![
            "graph".to_string(),
            args.entity.resolve()?,
            "--direction".to_string(),
            args.direction,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
            "--format".to_string(),
            args.format,
        ])
        .await
    }

    #[tool(
        description = "Expand a returned function or direct-call `handle`; external/indirect boundaries remain explicit and must not be repeatedly expanded."
    )]
    async fn irez_expand(
        &self,
        Parameters(args): Parameters<HandleArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec!["expand".to_string(), args.resolve()?]).await
    }

    #[tool(
        description = "Build a bounded handoff packet for a known entity `handle`, combining entity, source, and backward value evidence. Use after the investigation target is known."
    )]
    async fn irez_context(
        &self,
        Parameters(args): Parameters<BudgetArgs>,
    ) -> Result<CallToolResult, McpError> {
        run_irez(vec![
            "context".to_string(),
            args.entity.resolve()?,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
        ])
        .await
    }

    #[tool(
        description = "Trace one or all return values of a function using bounded function-local operand evidence. Pass the `handle` returned by irez_functions; this composed query reports source, direct calls (named targets with external/internal/unknown status and reason), boundaries, capabilities, unknowns, and per-site truncation without claiming semantic interpretation. detail=summary is the default (chain spine, counts, value shape, named call targets, truncation, source); use detail=graph only when compact nodes/relations are actually needed. include=[...] adds sections on top of the detail default (e.g. add \"flags\" for sparse non-default instruction attributes such as fast-math flags instead of per-instruction show calls). For kernel-shaped functions whose results leave via store instructions (ret void / ret ptr null), use irez_trace_stores instead — irez_trace_return flags that shape in unknowns."
    )]
    async fn irez_trace_return(
        &self,
        Parameters(args): Parameters<TraceReturnArgs>,
    ) -> Result<CallToolResult, McpError> {
        let mut argv = vec![
            "trace-return".to_string(),
            args.entity.resolve()?,
            "--return".to_string(),
            args.return_selector.unwrap_or_else(|| "all".to_string()),
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
            "--budget-depth".to_string(),
            args.budget_depth.to_string(),
        ];
        if let Some(detail) = args.detail {
            argv.push("--detail".to_string());
            argv.push(detail);
        }
        if let Some(include) = args.include {
            argv.push("--include".to_string());
            argv.push(include.join(","));
        }
        run_irez(argv).await
    }

    #[tool(
        description = "Trace the stored values of a function using bounded function-local operand evidence — the result channel of kernel-shaped functions (XLA/Numba/Triton) whose returns are `ret void` or `ret ptr null`. Each store site reports the stored value, the destination pointer, value shape, chain/graph projection, source, direct calls, boundaries, and per-site truncation. Pass the `handle` returned by irez_functions; detail/include behave like irez_trace_return."
    )]
    async fn irez_trace_stores(
        &self,
        Parameters(args): Parameters<TraceStoresArgs>,
    ) -> Result<CallToolResult, McpError> {
        let mut argv = vec![
            "trace-stores".to_string(),
            args.entity.resolve()?,
            "--budget-nodes".to_string(),
            args.budget_nodes.to_string(),
            "--budget-depth".to_string(),
            args.budget_depth.to_string(),
        ];
        if let Some(detail) = args.detail {
            argv.push("--detail".to_string());
            argv.push(detail);
        }
        if let Some(include) = args.include {
            argv.push("--include".to_string());
            argv.push(include.join(","));
        }
        run_irez(argv).await
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for IrezMcp {
    fn get_info(&self) -> ServerInfo {
        ServerInfo::new(ServerCapabilities::builder().enable_tools().build())
            .with_server_info(Implementation::new("irez-mcp", env!("CARGO_PKG_VERSION")))
            .with_instructions(
                "IREZ bounded evidence queries. Pass returned `handle` values to entity tools. For return-value questions prefer irez_trace_return (start with detail=summary; request detail=graph only when compact nodes/relations are needed — never repeat an identical call to re-extract fields, use include=[...] sections); for consumers use irez_uses or a forward slice; for conditional calls use irez_guards. Functions shown without a view return summaries, never complete IR. Inspect capabilities, unknowns, boundaries, truncation, and evidence_refs."
                    .to_string(),
            )
    }
}

async fn protocol_self_check() -> anyhow::Result<usize> {
    let mut child = Command::new(std::env::current_exe()?)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true)
        .spawn()?;
    let mut input = child.stdin.take().context("self-check stdin unavailable")?;
    let mut output = BufReader::new(
        child
            .stdout
            .take()
            .context("self-check stdout unavailable")?,
    );
    async fn request(
        input: &mut tokio::process::ChildStdin,
        output: &mut BufReader<tokio::process::ChildStdout>,
        value: Value,
    ) -> anyhow::Result<Value> {
        input
            .write_all((serde_json::to_string(&value)? + "\n").as_bytes())
            .await?;
        input.flush().await?;
        let mut line = String::new();
        tokio::time::timeout(
            std::time::Duration::from_secs(10),
            output.read_line(&mut line),
        )
        .await
        .context("MCP response timed out")??;
        Ok(serde_json::from_str(line.trim())?)
    }
    let initialized = request(
        &mut input,
        &mut output,
        serde_json::json!({
            "jsonrpc":"2.0", "id":1, "method":"initialize", "params":{
                "protocolVersion":"2026-07-28", "capabilities":{},
                "clientInfo":{"name":"irez-installer-check","version":"1"}
            }
        }),
    )
    .await?;
    anyhow::ensure!(
        initialized["result"]["serverInfo"]["name"] == "irez-mcp",
        "MCP initialize returned an unexpected server"
    );
    input
        .write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n")
        .await?;
    input.flush().await?;
    let listed = request(
        &mut input,
        &mut output,
        serde_json::json!({
            "jsonrpc":"2.0", "id":2, "method":"tools/list", "params":{}
        }),
    )
    .await?;
    let tool_count = listed["result"]["tools"]
        .as_array()
        .context("MCP tools/list returned no tool array")?
        .len();
    anyhow::ensure!(
        tool_count == EXPECTED_TOOL_COUNT,
        "MCP tools/list returned {tool_count} tools, expected {EXPECTED_TOOL_COUNT}"
    );
    let called = request(
        &mut input,
        &mut output,
        serde_json::json!({
            "jsonrpc":"2.0", "id":3, "method":"tools/call",
            "params":{"name":"irez_status","arguments":{}}
        }),
    )
    .await?;
    anyhow::ensure!(
        called["result"]["isError"] == false,
        "sample irez_status tool call failed: {called}"
    );
    child.kill().await?;
    Ok(tool_count)
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Subcommand mode: `irez-mcp install|update|doctor|uninstall codex|opencode ...`
    // configures agent hosts instead of starting the server.
    let argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.first().map(String::as_str) == Some("self-check") {
        let tool_count = protocol_self_check().await?;
        println!(
            "{}",
            serde_json::json!({
                "server": "irez-mcp",
                "protocol": "stdio",
                "tool_count": tool_count,
                "status": "ok"
            })
        );
        return Ok(());
    }
    if argv.as_slice() == ["doctor", "--stdio"] {
        let tool_count = protocol_self_check().await?;
        println!("cli_executable: ok");
        println!("state_access: ok");
        println!("mcp_initialize: ok");
        println!("tools_list: ok ({tool_count} tools)");
        println!("sample_tool_call: ok (irez_status)");
        return Ok(());
    }
    if matches!(
        argv.first().map(String::as_str),
        Some("install") | Some("update") | Some("doctor") | Some("check") | Some("uninstall")
    ) {
        return install::run(&argv);
    }

    // Log to stderr only: the stdio transport owns stdout.
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive(tracing::Level::INFO.into()),
        )
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();

    tracing::info!("starting irez-mcp server");

    // Backlog F1: an unset IREZ_STATE_DIR falls back to ./.irez while
    // installed hosts pin an absolute per-user directory — and the CLI
    // defaults to yet another location. Make the fallback loud in the host
    // log so a split-brain state library is diagnosable from day one.
    if std::env::var_os("IREZ_STATE_DIR").is_none() {
        tracing::warn!(
            "IREZ_STATE_DIR is not set; falling back to ./.irez. Installed hosts \
             set IREZ_STATE_DIR explicitly; a CLI ingest without --state-dir uses \
             a different default and will not be visible to this server"
        );
    }

    let service = IrezMcp::new().serve(stdio()).await.inspect_err(|e| {
        tracing::error!("serving error: {:?}", e);
    })?;

    service.waiting().await?;
    Ok(())
}
