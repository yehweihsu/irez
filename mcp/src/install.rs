//! `irez-mcp install|update|doctor|uninstall codex|opencode`
//!
//! Replaces the retired Python `agent_setup.py`. Registers the MCP server
//! with an agent host, links the investigation skill into the host's personal
//! skill directory, and ensures a per-host state directory exists. Config
//! edits always keep a timestamped backup and never overwrite a conflicting
//! entry without --force.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{bail, Context, Result};
use serde_json::{json, Value};

use crate::contract;

const OWNERSHIP_MARKER: &str = ".irez-install.json";
const SKILL_FILES: &[(&str, &[u8])] = &[
    (
        "SKILL.md",
        include_bytes!("../../skills/irez-investigation/SKILL.md"),
    ),
    (
        "agents/openai.yaml",
        include_bytes!("../../skills/irez-investigation/agents/openai.yaml"),
    ),
    (
        "references/reporting.md",
        include_bytes!("../../skills/irez-investigation/references/reporting.md"),
    ),
    (
        "references/evidence.md",
        include_bytes!("../../skills/irez-investigation/references/evidence.md"),
    ),
    (
        "references/tools.md",
        include_bytes!("../../skills/irez-investigation/references/tools.md"),
    ),
    (
        "references/workflows.md",
        include_bytes!("../../skills/irez-investigation/references/workflows.md"),
    ),
    (
        "references/experiments.md",
        include_bytes!("../../skills/irez-investigation/references/experiments.md"),
    ),
];

fn user_home() -> Result<PathBuf> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .context("neither HOME nor USERPROFILE identifies the user home directory")
}

fn absolute_path(path: PathBuf) -> Result<PathBuf> {
    if path.is_absolute() {
        Ok(path)
    } else {
        Ok(std::env::current_dir()?.join(path))
    }
}

fn resolve_executable(path: PathBuf) -> Result<PathBuf> {
    if path.components().count() > 1 || path.is_absolute() {
        return fs::canonicalize(&path)
            .with_context(|| format!("executable does not exist: {}", path.display()));
    }
    let search = std::env::var_os("PATH").context("PATH is not set")?;
    #[cfg(windows)]
    let suffixes: &[&str] = &["", ".exe", ".cmd", ".bat"];
    #[cfg(not(windows))]
    let suffixes: &[&str] = &[""];
    for directory in std::env::split_paths(&search) {
        for suffix in suffixes {
            let candidate = directory.join(format!("{}{}", path.display(), suffix));
            if candidate.is_file() {
                return fs::canonicalize(candidate).context("cannot canonicalize executable");
            }
        }
    }
    bail!("executable is not on PATH: {}", path.display())
}

#[derive(Clone)]
struct Options {
    platform: String,
    state_dir: PathBuf,
    cli: PathBuf,
    mcp_binary: PathBuf,
    force: bool,
    install_skill: bool,
    opencode_config: PathBuf,
}

fn default_state_dir(platform: &str) -> PathBuf {
    #[cfg(windows)]
    let base = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            user_home()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join("AppData/Local")
        });
    #[cfg(not(windows))]
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            user_home()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join(".local/share")
        });
    base.join("irez").join(platform)
}

fn default_opencode_config() -> Result<PathBuf> {
    default_opencode_config_from(|key| std::env::var_os(key))
}

/// OpenCode reads `~/.config/opencode/opencode.jsonc` on every platform,
/// including Windows (it does not use %APPDATA%). Resolution mirrors
/// `user_home`: HOME first, then USERPROFILE. See docs/MCP_SETUP.md.
fn default_opencode_config_from(
    get: impl Fn(&str) -> Option<std::ffi::OsString>,
) -> Result<PathBuf> {
    #[cfg(windows)]
    let base = {
        let home = get("HOME")
            .or_else(|| get("USERPROFILE"))
            .map(PathBuf::from)
            .context("neither HOME nor USERPROFILE identifies the user home directory")?;
        home.join(".config")
    };
    #[cfg(not(windows))]
    let base = match get("XDG_CONFIG_HOME").map(PathBuf::from) {
        Some(xdg) => xdg,
        None => {
            let home = get("HOME")
                .or_else(|| get("USERPROFILE"))
                .map(PathBuf::from)
                .context("neither HOME nor USERPROFILE identifies the user home directory")?;
            home.join(".config")
        }
    };
    Ok(base.join("opencode").join("opencode.jsonc"))
}

pub fn run(args: &[String]) -> Result<()> {
    let action = args.first().map(String::as_str).unwrap_or("");
    let mut platform: Option<String> = None;
    let mut state_dir: Option<PathBuf> = None;
    let own_executable = std::env::current_exe().context("cannot resolve own path")?;
    let sibling_cli = own_executable
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join(if cfg!(windows) { "irez.exe" } else { "irez" });
    let mut cli = std::env::var_os("IREZ_CLI")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            if sibling_cli.is_file() {
                sibling_cli
            } else {
                PathBuf::from("irez")
            }
        });
    let mut force = false;
    let mut install_skill = true;
    let mut opencode_config = default_opencode_config()?;

    let mut rest = &args[1..];
    while let Some(arg) = rest.first() {
        match arg.as_str() {
            "--force" => {
                force = true;
                rest = &rest[1..];
            }
            "--no-skill" => {
                install_skill = false;
                rest = &rest[1..];
            }
            "--state-dir" => {
                state_dir = Some(PathBuf::from(
                    rest.get(1).context("--state-dir needs a value")?,
                ));
                rest = &rest[2..];
            }
            "--cli" => {
                cli = PathBuf::from(rest.get(1).context("--cli needs a value")?);
                rest = &rest[2..];
            }
            "--opencode-config" => {
                opencode_config =
                    PathBuf::from(rest.get(1).context("--opencode-config needs a value")?);
                rest = &rest[2..];
            }
            other if platform.is_none() && !other.starts_with("--") => {
                platform = Some(other.to_string());
                rest = &rest[1..];
            }
            other => bail!("unknown argument: {other}"),
        }
    }
    let platform = platform.context("usage: irez-mcp install|update|doctor|uninstall codex|opencode \
                                     [--state-dir DIR] [--cli PATH] [--force] [--no-skill] [--opencode-config PATH]")?;
    if platform != "codex" && platform != "opencode" {
        bail!("unsupported platform: {platform} (expected codex or opencode)");
    }
    let mcp_binary = own_executable;
    let resolved_cli = if action == "uninstall" {
        absolute_path(cli)?
    } else {
        resolve_executable(cli)?
    };
    let options = Options {
        state_dir: absolute_path(state_dir.unwrap_or_else(|| default_state_dir(&platform)))?,
        cli: resolved_cli,
        mcp_binary,
        force,
        install_skill,
        opencode_config: absolute_path(opencode_config)?,
        platform,
    };

    match action {
        "install" => install(&stage_bundle(&options)?),
        "update" => {
            let mut staged = stage_bundle(&options)?;
            staged.force = true;
            backup_state_before_update(&staged)?;
            install(&staged)
        }
        "doctor" | "check" => check(&options),
        "uninstall" => uninstall(&options),
        other => bail!("unknown action: {other} (expected install, update, doctor, or uninstall)"),
    }
}

fn backup_state_before_update(options: &Options) -> Result<()> {
    let database = options.state_dir.join("investigation.sqlite");
    if !database.is_file() {
        return Ok(());
    }
    let backups = options.state_dir.join("backups");
    fs::create_dir_all(&backups)?;
    let destination = backups.join(format!(
        "before-{}-{}.sqlite",
        env!("CARGO_PKG_VERSION"),
        chrono_free_timestamp()
    ));
    run_command(
        Command::new(&options.cli)
            .arg("--state-dir")
            .arg(&options.state_dir)
            .arg("backup")
            .arg(&destination),
        "transactional state backup",
    )?;
    println!("state_backup: {}", destination.display());
    Ok(())
}

fn install_root() -> Result<PathBuf> {
    #[cfg(windows)]
    let base = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or(user_home()?.join("AppData/Local"));
    #[cfg(not(windows))]
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or(user_home()?.join(".local/share"));
    Ok(base
        .join("irez")
        .join("versions")
        .join(env!("CARGO_PKG_VERSION"))
        .join("bin"))
}

/// Copy a release bundle into a versioned, stable location. Keeping versions
/// side-by-side makes config rollback possible without relying on PATH.
fn stage_bundle(options: &Options) -> Result<Options> {
    let root = install_root()?;
    fs::create_dir_all(&root)?;
    let cli_name = if cfg!(windows) { "irez.exe" } else { "irez" };
    let mcp_name = if cfg!(windows) {
        "irez-mcp.exe"
    } else {
        "irez-mcp"
    };
    let installed_cli = root.join(cli_name);
    let installed_mcp = root.join(mcp_name);
    copy_unless_same(&options.cli, &installed_cli, "CLI")?;
    copy_unless_same(&options.mcp_binary, &installed_mcp, "MCP server")?;
    let mut staged = options.clone();
    staged.cli = fs::canonicalize(installed_cli)?;
    staged.mcp_binary = fs::canonicalize(installed_mcp)?;
    Ok(staged)
}

fn copy_unless_same(source: &Path, destination: &Path, label: &str) -> Result<()> {
    let same =
        destination.exists() && fs::canonicalize(source).ok() == fs::canonicalize(destination).ok();
    if !same {
        fs::copy(source, destination)
            .with_context(|| format!("cannot install {label} from {}", source.display()))?;
    }
    Ok(())
}

fn run_command(command: &mut Command, what: &str) -> Result<()> {
    let status = command
        .status()
        .with_context(|| format!("failed to run {what}"))?;
    if !status.success() {
        bail!("{what} failed with {status}");
    }
    Ok(())
}

/// Create the state directory and investigation if none exists yet.
fn ensure_state(options: &Options) -> Result<()> {
    fs::create_dir_all(&options.state_dir).context("cannot create state directory")?;
    run_command(
        Command::new(&options.cli)
            .arg("--state-dir")
            .arg(&options.state_dir)
            .args(["init", "--name", "agent"]),
        "irez init",
    )
}

fn write_embedded_skill(destination: &Path) -> Result<()> {
    for (relative, contents) in SKILL_FILES {
        let target = destination.join(relative);
        fs::create_dir_all(target.parent().context("invalid embedded skill path")?)?;
        fs::write(target, contents)?;
    }
    Ok(())
}

fn link_skill(destination: &Path) -> Result<()> {
    if destination.exists() {
        if !destination.join(OWNERSHIP_MARKER).is_file() {
            bail!(
                "refusing to replace unowned skill path: {}",
                destination.display()
            );
        }
        fs::remove_dir_all(destination)?;
    }
    fs::create_dir_all(destination.parent().context("skill has no parent")?)?;
    let temporary = destination.with_extension(format!("tmp-{}", std::process::id()));
    if temporary.exists() {
        fs::remove_dir_all(&temporary)?;
    }
    write_embedded_skill(&temporary)?;
    fs::write(
        temporary.join(OWNERSHIP_MARKER),
        serde_json::to_vec_pretty(&json!({
            "installed_by": "irez",
            "source_version": env!("CARGO_PKG_VERSION")
        }))?,
    )?;
    fs::rename(&temporary, destination)?;
    println!("installed owned skill: {}", destination.display());
    Ok(())
}

fn remove_skill(destination: &Path) -> Result<()> {
    if destination.join(OWNERSHIP_MARKER).is_file() {
        fs::remove_dir_all(destination)?;
        println!("removed owned skill: {}", destination.display());
    } else if destination.exists() {
        println!("kept unowned path: {}", destination.display());
    }
    Ok(())
}

/// Strip JSONC comments and trailing commas with a real scanner (state
/// machine over string/comment states), not regexes: V00_00 bug B11 was a
/// regex stripper eating `//` inside string values.
pub fn jsonc_to_json(input: &str) -> String {
    let chars: Vec<char> = input.chars().collect();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    let mut in_string = false;
    while i < chars.len() {
        let c = chars[i];
        if in_string {
            out.push(c);
            if c == '\\' && i + 1 < chars.len() {
                out.push(chars[i + 1]);
                i += 2;
                continue;
            }
            if c == '"' {
                in_string = false;
            }
            i += 1;
            continue;
        }
        match c {
            '"' => {
                in_string = true;
                out.push(c);
                i += 1;
            }
            '/' if i + 1 < chars.len() && chars[i + 1] == '/' => {
                while i < chars.len() && chars[i] != '\n' {
                    i += 1;
                }
            }
            '/' if i + 1 < chars.len() && chars[i + 1] == '*' => {
                i += 2;
                while i + 1 < chars.len() && !(chars[i] == '*' && chars[i + 1] == '/') {
                    if chars[i] == '\n' {
                        out.push('\n'); // keep line numbers stable
                    }
                    i += 1;
                }
                i = (i + 2).min(chars.len());
            }
            ',' => {
                // Drop the comma when only whitespace follows before } or ].
                let mut j = i + 1;
                while j < chars.len() && chars[j].is_whitespace() {
                    j += 1;
                }
                if j < chars.len() && (chars[j] == '}' || chars[j] == ']') {
                    i += 1; // skip comma
                } else {
                    out.push(c);
                    i += 1;
                }
            }
            _ => {
                out.push(c);
                i += 1;
            }
        }
    }
    out
}

fn read_jsonc(path: &Path) -> Result<Value> {
    if !path.exists() {
        return Ok(json!({}));
    }
    let text =
        fs::read_to_string(path).with_context(|| format!("cannot read {}", path.display()))?;
    let stripped = jsonc_to_json(&text);
    serde_json::from_str(&stripped)
        .with_context(|| format!("cannot parse {} as JSONC", path.display()))
}

fn write_json_config(path: &Path, data: &Value) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    if path.exists() {
        let stamp = chrono_free_timestamp();
        let backup = path.with_file_name(format!(
            "{}.bak-{stamp}",
            path.file_name().unwrap().to_string_lossy()
        ));
        fs::copy(path, &backup).with_context(|| format!("cannot back up {}", path.display()))?;
        println!("backed up config: {}", backup.display());
    }
    let temporary = path.with_extension(format!("tmp-{}", std::process::id()));
    fs::write(&temporary, serde_json::to_string_pretty(data)? + "\n")?;
    #[cfg(windows)]
    if path.exists() {
        fs::remove_file(path)?;
    }
    fs::rename(&temporary, path)?;
    Ok(())
}

/// Timestamp for backup names without pulling in a date library.
fn chrono_free_timestamp() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    // Days since epoch -> civil date (Howard Hinnant's algorithm).
    let days = (secs / 86400) as i64;
    let secs_of_day = secs % 86400;
    let z = days + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let year = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if month <= 2 { year + 1 } else { year };
    format!(
        "{year:04}{month:02}{day:02}-{:02}{:02}{:02}",
        secs_of_day / 3600,
        (secs_of_day % 3600) / 60,
        secs_of_day % 60
    )
}

fn server_config(options: &Options) -> Value {
    json!({
        "type": "local",
        "command": [options.mcp_binary.to_string_lossy()],
        "enabled": true,
        "environment": {
            "IREZ_STATE_DIR": options.state_dir.to_string_lossy(),
            "IREZ_CLI": options.cli.to_string_lossy(),
        }
    })
}

fn ownership_manifest(options: &Options) -> PathBuf {
    options
        .state_dir
        .join(format!("install-{}.json", options.platform))
}

fn skill_destination(platform: &str) -> Result<PathBuf> {
    match platform {
        "codex" => Ok(user_home()?.join(".codex/skills/irez-investigation")),
        "opencode" => Ok(default_opencode_config()?
            .parent()
            .context("OpenCode config has no parent")?
            .join("skills/irez-investigation")),
        _ => bail!("unsupported platform: {platform}"),
    }
}

fn ownership_data(options: &Options) -> Option<Value> {
    fs::read_to_string(ownership_manifest(options))
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
}

/// FNV-1a 64-bit fingerprint over raw bytes. This detects "same version
/// string, different binary" drift between installs; it is a change detector,
/// not a security boundary (release bundles carry real SHA-256 manifests).
fn fnv1a64(bytes: &[u8]) -> String {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("{hash:016x}")
}

fn file_fingerprint(path: &Path) -> Result<String> {
    let bytes =
        fs::read(path).with_context(|| format!("cannot read binary: {}", path.display()))?;
    Ok(fnv1a64(&bytes))
}

/// Deterministic fingerprint of the embedded skill payload actually shipped
/// inside this installer binary.
fn skill_fingerprint() -> String {
    let mut bytes = Vec::new();
    for (name, content) in SKILL_FILES {
        bytes.extend_from_slice(name.as_bytes());
        bytes.push(0);
        bytes.extend_from_slice(content);
        bytes.push(0);
    }
    fnv1a64(&bytes)
}

fn registration_owned(options: &Options) -> bool {
    ownership_data(options)
        .as_ref()
        .and_then(|value| value.get("installed_by"))
        .and_then(Value::as_str)
        == Some("irez")
}

fn registration_matches(options: &Options) -> bool {
    ownership_data(options).and_then(|value| value.get("server").cloned())
        == Some(server_config(options))
}

fn install(options: &Options) -> Result<()> {
    ensure_state(options)?;
    match options.platform.as_str() {
        "codex" => {
            let probe = Command::new("codex").args(["mcp", "get", "irez"]).output();
            match probe {
                Ok(output) if output.status.success() && !options.force => {
                    if !registration_owned(options) {
                        bail!("codex MCP 'irez' exists but is not owned by this installer; use --force to replace it");
                    }
                    if !registration_matches(options) {
                        bail!("owned codex MCP 'irez' differs; use --force to replace it");
                    }
                    println!("codex MCP 'irez' is already registered by this installer.");
                }
                Ok(output) if output.status.success() => {
                    run_command(
                        Command::new("codex").args(["mcp", "remove", "irez"]),
                        "codex mcp remove",
                    )?;
                    codex_add(options)?;
                }
                Ok(_) => codex_add(options)?,
                Err(e) => bail!("codex is not on PATH: {e}"),
            }
            if options.install_skill {
                link_skill(&skill_destination("codex")?)?;
            }
        }
        "opencode" => {
            let mut data = read_jsonc(&options.opencode_config)?;
            let server = server_config(options);
            let servers = data
                .as_object_mut()
                .context("opencode config is not a JSON object")?
                .entry("mcp")
                .or_insert_with(|| json!({}));
            let existing = servers.get("irez");
            if let Some(existing) = existing {
                if *existing != server && !options.force {
                    bail!("opencode MCP 'irez' differs; use --force to replace it.");
                }
                if *existing == server && !options.force && !registration_owned(options) {
                    bail!("opencode MCP 'irez' exists but is not owned by this installer; use --force to adopt it");
                }
            }
            servers["irez"] = server;
            write_json_config(&options.opencode_config, &data)?;
            if options.install_skill {
                link_skill(&skill_destination("opencode")?)?;
            }
        }
        _ => unreachable!(),
    }
    fs::write(
        ownership_manifest(options),
        serde_json::to_vec_pretty(&json!({
            "installed_by": "irez",
            "installer_version": env!("CARGO_PKG_VERSION"),
            "registration_schema_version": 1,
            "skill_version": if options.install_skill { Value::String(env!("CARGO_PKG_VERSION").into()) } else { Value::Null },
            // Contract versions come from the generated contract module
            // (contract.json), never from literals edited by hand.
            "irez_version": contract::IREZ_VERSION,
            "db_schema_version": contract::DB_SCHEMA_VERSION,
            "api_schema_version": contract::API_SCHEMA_VERSION,
            "analysis_schema_version": contract::ANALYSIS_SCHEMA_VERSION,
            "adapter_id": contract::ADAPTER_ID,
            "adapter_version": contract::ADAPTER_VERSION,
            "cli_contract_version": contract::CLI_CONTRACT_VERSION,
            "mcp_contract_version": contract::MCP_CONTRACT_VERSION,
            // Compatibility alias for pre-release ownership manifests.
            "state_schema_version": contract::DB_SCHEMA_VERSION,
            // Binary fingerprints let doctor prove that the binaries on disk
            // are still the ones this installer registered.
            "fingerprint_algorithm": "fnv1a64",
            "cli_fingerprint": file_fingerprint(&options.cli)?,
            "mcp_fingerprint": file_fingerprint(&options.mcp_binary)?,
            "skill_fingerprint": if options.install_skill { Value::String(skill_fingerprint()) } else { Value::Null },
            "server": server_config(options)
        }))?,
    )?;
    println!(
        "{} setup complete; restart the agent host.",
        options.platform
    );
    Ok(())
}

fn codex_add(options: &Options) -> Result<()> {
    run_command(
        Command::new("codex")
            .args(["mcp", "add", "irez"])
            .arg("--env")
            .arg(format!("IREZ_STATE_DIR={}", options.state_dir.display()))
            .arg("--env")
            .arg(format!("IREZ_CLI={}", options.cli.display()))
            .arg("--")
            .arg(&options.mcp_binary),
        "codex mcp add",
    )
}

fn check(options: &Options) -> Result<()> {
    let status_output = Command::new(&options.cli)
        .arg("--state-dir")
        .arg(&options.state_dir)
        .arg("status")
        .output()
        .context("cannot run irez status")?;
    if !status_output.status.success() {
        bail!(
            "irez status failed: {}",
            String::from_utf8_lossy(&status_output.stderr)
        );
    }
    let status_envelope: Value = serde_json::from_slice(&status_output.stdout)
        .context("irez status did not return a JSON envelope")?;
    println!("cli_executable: ok");
    println!("state_access: ok");
    if !registration_owned(options) {
        bail!("installer ownership manifest is missing");
    }
    let manifest = ownership_data(options).context("cannot read ownership manifest")?;
    let status_result = status_envelope
        .get("result")
        .cloned()
        .unwrap_or(Value::Null);

    // An identical "0.1.0" string on two builds proves nothing, so doctor
    // checks every contract version the installer recorded AND the binary
    // fingerprints of the files it registered.
    for key in [
        "irez_version",
        "db_schema_version",
        "api_schema_version",
        "analysis_schema_version",
        "adapter_version",
    ] {
        let expected = manifest.get(key).cloned().unwrap_or(Value::Null);
        let actual = status_result.get(key).cloned().unwrap_or(Value::Null);
        if expected != actual {
            bail!(
                "version mismatch for {key}: installer recorded {expected}, \
                 running CLI reports {actual}; reinstall or update"
            );
        }
    }
    println!("version_alignment: ok");
    match status_result.get("build_revision").and_then(Value::as_str) {
        Some("unknown") | None => println!(
            "cli_build_revision: unknown (rebuild from a VCS checkout for full provenance)"
        ),
        Some(revision) => println!("cli_build_revision: {revision}"),
    }
    for (key, path) in [
        ("cli_fingerprint", &options.cli),
        ("mcp_fingerprint", &options.mcp_binary),
    ] {
        match manifest.get(key).and_then(Value::as_str) {
            Some(expected) => {
                let actual = file_fingerprint(path)?;
                if actual != expected {
                    bail!(
                        "{key} mismatch: the binary on disk ({actual}) is not the \
                         binary recorded at install time ({expected}); reinstall"
                    );
                }
                println!("{key}: ok");
            }
            None => println!("{key}: not recorded (pre-fingerprint install)"),
        }
    }

    // Cross-check the standalone adapter next to the CLI when one exists:
    // its adapter_version must match what the CLI reports through status.
    let adapter_name = if cfg!(windows) {
        "irez-llvm-index.exe"
    } else {
        "irez-llvm-index"
    };
    match options.cli.parent().map(|dir| dir.join(adapter_name)) {
        Some(adapter_path) if adapter_path.is_file() => {
            let output = Command::new(&adapter_path)
                .arg("version")
                .output()
                .context("cannot run irez-llvm-index version")?;
            if !output.status.success() {
                bail!("irez-llvm-index version failed");
            }
            let adapter_info: Value = serde_json::from_slice(&output.stdout)
                .context("irez-llvm-index version did not return JSON")?;
            let cli_adapter = status_result.get("adapter_version").cloned();
            let bin_adapter = adapter_info.get("adapter_version").cloned();
            if cli_adapter != bin_adapter {
                bail!(
                    "adapter version mismatch: CLI reports {}, irez-llvm-index reports {}",
                    cli_adapter.unwrap_or(Value::Null),
                    bin_adapter.unwrap_or(Value::Null)
                );
            }
            println!("adapter_version_alignment: ok");
        }
        _ => println!("adapter_version_alignment: skipped (no irez-llvm-index next to CLI)"),
    }

    let installed_server = manifest
        .get("server")
        .cloned()
        .context("ownership manifest has no server configuration")?;

    match options.platform.as_str() {
        "codex" => run_command(
            Command::new("codex").args(["mcp", "get", "irez"]),
            "codex MCP registration",
        )?,
        "opencode" => {
            let data = read_jsonc(&options.opencode_config)?;
            let actual = data.get("mcp").and_then(|v| v.get("irez"));
            if actual != Some(&installed_server) {
                bail!("OpenCode MCP registration is missing or differs from the owned config");
            }
        }
        _ => unreachable!(),
    }
    println!("host_registration: ok");
    run_command(
        Command::new(&options.mcp_binary)
            .arg("self-check")
            .env("IREZ_CLI", &options.cli)
            .env("IREZ_STATE_DIR", &options.state_dir),
        "irez-mcp self-check",
    )?;
    println!("mcp_initialize: ok");
    println!("tools_list: ok");
    println!("sample_tool_call: ok (irez_status)");
    let skill_expected = manifest
        .get("skill_version")
        .is_some_and(|value| !value.is_null());
    let skill = skill_destination(&options.platform)?;
    if skill_expected
        && (!skill.join(OWNERSHIP_MARKER).is_file() || !skill.join("SKILL.md").is_file())
    {
        bail!(
            "owned skill installation is missing or incomplete: {}",
            skill.display()
        );
    }
    if skill_expected {
        if let Some(expected) = manifest.get("skill_fingerprint").and_then(Value::as_str) {
            if expected != skill_fingerprint() {
                bail!(
                    "skill_fingerprint mismatch: this installer's embedded skill differs \
                     from the one recorded at install time"
                );
            }
            println!("skill_fingerprint: ok");
        }
    }
    println!(
        "skill_installation: {}",
        if skill_expected {
            "ok"
        } else {
            "not requested"
        }
    );
    Ok(())
}

fn uninstall(options: &Options) -> Result<()> {
    if !registration_owned(options) {
        bail!("refusing to uninstall an MCP registration not owned by this installer");
    }
    match options.platform.as_str() {
        "codex" => {
            let _ = Command::new("codex")
                .args(["mcp", "remove", "irez"])
                .status();
            remove_skill(&skill_destination("codex")?)?;
        }
        "opencode" => {
            if options.opencode_config.exists() {
                let mut data = read_jsonc(&options.opencode_config)?;
                if let Some(servers) = data.get_mut("mcp").and_then(Value::as_object_mut) {
                    let installed =
                        ownership_data(options).and_then(|value| value.get("server").cloned());
                    if servers.get("irez") != installed.as_ref() {
                        bail!("refusing to remove an OpenCode registration changed since installation");
                    }
                    if servers.remove("irez").is_some() {
                        write_json_config(&options.opencode_config, &data)?;
                    }
                }
            }
            remove_skill(&skill_destination("opencode")?)?;
        }
        _ => unreachable!(),
    }
    if ownership_manifest(options).exists() {
        fs::remove_file(ownership_manifest(options))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        default_opencode_config_from, jsonc_to_json, link_skill, remove_skill, OWNERSHIP_MARKER,
    };
    use std::ffi::OsString;
    use std::path::PathBuf;

    fn env_from<'a>(pairs: &'a [(&'a str, &'a str)]) -> impl Fn(&str) -> Option<OsString> + 'a {
        move |key| {
            pairs
                .iter()
                .find(|(name, _)| *name == key)
                .map(|(_, value)| OsString::from(value))
        }
    }

    #[cfg(windows)]
    #[test]
    fn opencode_config_windows_uses_dot_config_under_home() {
        // OpenCode reads %USERPROFILE%\.config\opencode\opencode.jsonc on
        // Windows; %APPDATA% must not influence the result.
        let path = default_opencode_config_from(env_from(&[
            ("USERPROFILE", r"C:\Users\tester"),
            ("APPDATA", r"C:\Users\tester\AppData\Roaming"),
        ]))
        .unwrap();
        assert_eq!(
            path,
            PathBuf::from(r"C:\Users\tester\.config\opencode\opencode.jsonc")
        );
    }

    #[cfg(windows)]
    #[test]
    fn opencode_config_windows_prefers_home_over_userprofile() {
        let path = default_opencode_config_from(env_from(&[
            ("HOME", r"C:\Users\gitbash"),
            ("USERPROFILE", r"C:\Users\tester"),
        ]))
        .unwrap();
        assert_eq!(
            path,
            PathBuf::from(r"C:\Users\gitbash\.config\opencode\opencode.jsonc")
        );
    }

    #[cfg(windows)]
    #[test]
    fn opencode_config_windows_errors_without_home_vars() {
        assert!(default_opencode_config_from(env_from(&[])).is_err());
    }

    #[cfg(not(windows))]
    #[test]
    fn opencode_config_unix_prefers_xdg_config_home() {
        let path = default_opencode_config_from(env_from(&[
            ("XDG_CONFIG_HOME", "/tmp/xdg"),
            ("HOME", "/home/tester"),
        ]))
        .unwrap();
        assert_eq!(path, PathBuf::from("/tmp/xdg/opencode/opencode.jsonc"));
    }

    #[cfg(not(windows))]
    #[test]
    fn opencode_config_unix_falls_back_to_home_dot_config() {
        let path = default_opencode_config_from(env_from(&[("HOME", "/home/tester")])).unwrap();
        assert_eq!(
            path,
            PathBuf::from("/home/tester/.config/opencode/opencode.jsonc")
        );
    }

    #[test]
    fn strips_line_and_block_comments() {
        let input = "{\n// line comment\n\"a\": 1, /* block */ \"b\": 2\n}";
        let value: serde_json::Value = serde_json::from_str(&jsonc_to_json(input)).unwrap();
        assert_eq!(value["a"], 1);
        assert_eq!(value["b"], 2);
    }

    #[test]
    fn keeps_comment_like_text_inside_strings() {
        // The V00_00 regex stripper mangled exactly this case (B11).
        let input = r#"{"url": "https://example.com // not a comment", "x": "/* no */"}"#;
        let value: serde_json::Value = serde_json::from_str(&jsonc_to_json(input)).unwrap();
        assert_eq!(value["url"], "https://example.com // not a comment");
        assert_eq!(value["x"], "/* no */");
    }

    #[test]
    fn strips_trailing_commas() {
        let input = "{\n\"a\": [1, 2,],\n\"b\": {\"c\": 3,},\n}";
        let value: serde_json::Value = serde_json::from_str(&jsonc_to_json(input)).unwrap();
        assert_eq!(value["a"], serde_json::json!([1, 2]));
        assert_eq!(value["b"]["c"], 3);
    }

    #[test]
    fn keeps_escaped_quotes_in_strings() {
        let input = r#"{"a": "quote \" and //", }"#;
        let value: serde_json::Value = serde_json::from_str(&jsonc_to_json(input)).unwrap();
        assert_eq!(value["a"], "quote \" and //");
    }

    #[test]
    fn embedded_skill_is_owned_updateable_and_removable() {
        let root = std::env::temp_dir().join(format!(
            "irez-installer-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let destination = root.join("irez-investigation");
        link_skill(&destination).unwrap();
        assert!(destination.join("SKILL.md").is_file());
        assert!(destination.join("references/workflows.md").is_file());
        assert!(destination.join("references/experiments.md").is_file());
        assert!(destination.join(OWNERSHIP_MARKER).is_file());
        std::fs::write(destination.join("stale-file"), b"old").unwrap();
        link_skill(&destination).unwrap();
        assert!(!destination.join("stale-file").exists());
        remove_skill(&destination).unwrap();
        assert!(!destination.exists());
        let _ = std::fs::remove_dir_all(root);
    }
}
