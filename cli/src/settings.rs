// SPDX-License-Identifier: MIT
//! /settings: every page the app's Settings has.

use crate::app::{App, Screen};
use crate::form::{Did, Form, Kind, Row};
use crate::models::Models;
use crate::overlay::Item;
use crate::state::{at, b, n, s};
use crate::text::{self, bytes};
use crate::theme::{self, p};
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::text::{Line, Span};
use ratatui::Frame;
use serde_json::{json, Map, Value};

pub const PAGES: [(&str, &str); 10] = [
    ("general", "General"),
    ("experts", "Experts"),
    ("providers", "Providers"),
    ("generation", "Generation"),
    ("hardware", "Hardware"),
    ("runtimes", "Runtimes"),
    ("training", "Training"),
    ("tools", "Tools"),
    ("server", "Server"),
    ("about", "About"),
];

pub struct Settings {
    pub page: usize,
    pub form: Form,
}

impl Settings {
    pub fn open(name: &str) -> Settings {
        let wanted = name.to_lowercase();
        let page = PAGES.iter().position(|(id, label)| *id == wanted || label.to_lowercase() == wanted).unwrap_or(0);
        Settings { page, form: Form::default() }
    }
}

/// All of it at once. The pages share most of it.
pub fn entering(app: &mut App) {
    for method in [
        "models", "providers", "runtimes", "devices", "trainer", "trainer.flavors", "about",
        "runtime.progress", "trainer.progress", "processes", "kit.status", "mcp.status", "self.source",
    ] {
        app.fetch(method);
    }
    app.reload_config();
}

fn c_s(app: &App, path: &str) -> String {
    at(&app.config, path).and_then(Value::as_str).unwrap_or("").to_string()
}
fn c_n(app: &App, path: &str) -> f64 {
    at(&app.config, path).and_then(Value::as_f64).unwrap_or(0.0)
}
fn c_b(app: &App, path: &str) -> bool {
    at(&app.config, path).and_then(Value::as_bool).unwrap_or(false)
}

fn toggle(app: &App, path: &str, label: &str, hint: &str) -> Row {
    Row::toggle(path, label, c_b(app, path), hint)
}
fn slider(app: &App, path: &str, label: &str, range: (f64, f64, f64), hint: &str) -> Row {
    Row::num(path, label, c_n(app, path), range, hint)
}
fn whole(app: &App, path: &str, label: &str, hint: &str) -> Row {
    Row::num(path, label, c_n(app, path), (0.0, 0.0, 1.0), hint)
}

/// A dotted path as the nested patch that sets it.
fn patch_of(path: &str, value: Value) -> Value {
    let mut out = value;
    for key in path.rsplit('.') {
        let mut object = Map::new();
        object.insert(key.to_string(), out);
        out = Value::Object(object);
    }
    out
}

fn list<'a>(app: &'a App, key: &str, field: &str) -> Vec<&'a Value> {
    app.data
        .get(key)
        .and_then(|v| if field.is_empty() { Some(v) } else { v.get(field) })
        .and_then(Value::as_array)
        .map(|l| l.iter().collect())
        .unwrap_or_default()
}

fn split_model(value: &str) -> (String, String) {
    match value.split_once('|') {
        Some((provider, model)) => (provider.to_string(), model.to_string()),
        None => (String::new(), value.to_string()),
    }
}

/// Everything a seat can be pointed at: the models folder, then each
/// provider's models unless it has to be a model on this machine.
fn model_options(app: &App, chosen: &str, local_only: bool) -> Vec<(String, String)> {
    let mut out = vec![(String::new(), "(none)".to_string())];
    let mlx_off = app.data.get("models").map(|m| !s(m, "mlx_unavailable").is_empty()).unwrap_or(false);
    for m in list(app, "models", "models") {
        let name = s(m, "name");
        let mlx = s(m, "format") == "mlx";
        if mlx && mlx_off && name != chosen {
            continue;
        }
        let label = format!("{name} ({}{})", if mlx { "MLX, " } else { "" }, bytes(n(m, "bytes")));
        out.push((name, label));
    }
    if !local_only {
        for provider in list(app, "providers", "providers") {
            let id = s(provider, "id");
            for model in provider.get("models").and_then(Value::as_array).into_iter().flatten() {
                let model = model.as_str().unwrap_or("");
                let leaves = if b(provider, "on_your_network") { "" } else { ", leaves this machine" };
                out.push((format!("{id}|{model}"), format!("{model} ({}{leaves})", s(provider, "name"))));
            }
        }
    }
    if !chosen.is_empty() && !out.iter().any(|(v, _)| v == chosen) {
        out.push((chosen.to_string(), format!("{chosen} (not in the models folder)")));
    }
    out
}

fn progress_rows(p_: Option<&Value>, title: &str, cancel: &str, dismiss: &str, rows: &mut Vec<Row>) {
    let Some(v) = p_ else { return };
    let phase = s(v, "phase");
    if phase.is_empty() || phase == "idle" {
        return;
    }
    let label = s(v, "label");
    let said = format!("{title}: {}", if label.is_empty() { phase.clone() } else { label });
    if phase == "failed" {
        rows.push(Row::bad(&said));
        let log = s(v, "log");
        if !log.is_empty() {
            let tail: Vec<&str> = log.lines().rev().take(6).collect();
            rows.push(Row::note(&tail.into_iter().rev().collect::<Vec<_>>().join("\n")));
        }
    } else {
        rows.push(Row::note(&said));
    }
    if b(v, "running") {
        rows.push(Row::button(cancel, "Stop", ""));
    } else {
        rows.push(Row::button(dismiss, "Clear", ""));
    }
}

// --- the pages ----------------------------------------------------------------------

fn general(app: &App) -> Vec<Row> {
    let models = app.data.get("models");
    let count = models.and_then(|m| m.get("models")).and_then(Value::as_array).map_or(0, Vec::len);
    let folder = models.map_or_else(|| c_s(app, "models_dir"), |m| s(m, "directory"));
    let problem = models.map(|m| s(m, "problem")).unwrap_or_default();
    let delegator = c_s(app, "router.model");
    let mut rows = vec![
        Row::head("MODELS"),
        Row::text("models_dir", "Models folder", &folder, "", &format!("{count} models, GGUF or MLX")),
    ];
    if !problem.is_empty() {
        rows.push(Row::bad(&problem));
    }
    rows.push(Row::button("@rescan", "Rescan", ""));
    rows.push(Row::button("@models-reset", "Reset to default", ""));
    rows.push(Row::head("DELEGATOR"));
    rows.push(Row::picks("router.model", "Delegator model", &delegator, model_options(app, &delegator, true), "Picks the expert. GGUF only."));
    if count == 0 {
        rows.push(Row::button("@delegator-get", "Get one", "Qwen3 4B or LFM2.5 1.2B"));
    }
    rows.push(toggle(app, "routing.keep_delegator_loaded", "Always keep loaded", "Uses more memory."));
    rows.push(slider(app, "routing.min_confidence", "Confidence floor", (0.0, 1.0, 0.01), "Below this, the default expert answers."));
    rows.push(Row::head("APPEARANCE"));
    rows.push(Row::choice("@theme", "Theme", &app.theme_choice, &[("auto", "Follow the terminal"), ("dark", "Dark"), ("light", "Light")], ""));
    rows
}

fn experts(app: &App) -> Vec<Row> {
    let mut rows = vec![Row::button("@new-expert", "New expert", ""), Row::button("@rescan", "Rescan models", "")];
    let config_experts: Vec<&Value> = at(&app.config, "experts").and_then(Value::as_array).map(|l| l.iter().collect()).unwrap_or_default();
    if app.state.experts.is_empty() {
        rows.push(Row::note("No experts yet."));
    }
    for e in &app.state.experts {
        let seat = config_experts.iter().find(|x| s(x, "id") == e.id);
        let model = seat.map(|x| s(x, "model")).unwrap_or_default();
        let provider = seat.map(|x| s(x, "provider")).unwrap_or_default();
        let value = if provider.is_empty() { model } else { format!("{provider}|{model}") };
        let mut title = e.name.to_uppercase();
        if e.default {
            title.push_str("  (DEFAULT)");
        }
        if e.phase == "missing" {
            title.push_str("  (MISSING)");
        }
        rows.push(Row::head(&title));
        let words = |key: &str, between: &str| -> String {
            seat.and_then(|x| x.get(key)).and_then(Value::as_array)
                .map(|l| l.iter().filter_map(Value::as_str).collect::<Vec<_>>().join(between)).unwrap_or_default()
        };
        rows.push(Row::text(&format!("expert-edit:name:{}", e.id), "Name", &e.name, "", ""));
        rows.push(Row::text(&format!("expert-edit:tag:{}", e.id), "Tag", &seat.map(|x| s(x, "tag")).unwrap_or_default(), "", "One to four letters."));
        rows.push(Row::long(&format!("expert-edit:description:{}", e.id), "Description", &e.blurb, "Used for routing."));
        rows.push(Row::text(&format!("expert-edit:keywords:{}", e.id), "Keywords", &words("keywords", ", "), "", "Comma separated. Used without a delegator."));
        rows.push(Row::long(&format!("expert-edit:examples:{}", e.id), "Examples", &words("examples", "\n"), "Questions it should take, one per line."));
        rows.push(Row::picks(&format!("expert:{}", e.id), "Model", &value, model_options(app, &value, false), ""));
        rows.push(Row::button(&format!("expert-remove:{}\n{}", e.id, e.name), "Eject", "The model file stays."));
    }
    rows.push(Row::head("DEFAULT EXPERT"));
    let mut options = vec![(String::new(), "(none)".to_string())];
    options.extend(app.state.experts.iter().map(|e| (e.id.clone(), e.name.clone())));
    rows.push(Row::picks("routing.default_expert", "Default expert", &c_s(app, "routing.default_expert"), options, "Answers when routing is unsure."));
    rows
}

fn providers(app: &App) -> Vec<Row> {
    let mut rows = vec![Row::button("@provider-new", "Add a provider", "")];
    for pv in list(app, "providers", "providers") {
        let id = s(pv, "id");
        let kind = if s(pv, "kind") == "anthropic" { "Anthropic API" } else { "OpenAI-compatible" };
        rows.push(Row::head(&s(pv, "name").to_uppercase()));
        let key = pv.get("key").cloned().unwrap_or(Value::Null);
        let key_line = match s(&key, "source").as_str() {
            "typed" => "key saved".to_string(),
            "variable" | "convention" => format!("${}{}", s(&key, "variable"), if key.get("present") == Some(&json!(false)) { " (not set)" } else { "" }),
            _ => "no key".to_string(),
        };
        let models: Vec<String> = pv.get("models").and_then(Value::as_array).map(|l| l.iter().filter_map(|m| m.as_str().map(str::to_string)).collect()).unwrap_or_default();
        rows.push(Row::note(&format!("{kind}{}\n{}\n{key_line}\n{}", if b(pv, "on_your_network") { ", on your network" } else { "" }, s(pv, "endpoint"), if models.is_empty() { "no model".to_string() } else { models.join(", ") })));
        rows.push(Row::button(&format!("provider-edit:{id}"), "Edit", ""));
        rows.push(Row::button(&format!("provider-remove:{id}\n{}", s(pv, "name")), "Remove", ""));
    }
    rows
}

fn generation(app: &App) -> Vec<Row> {
    vec![
        Row::head("SAMPLING"),
        slider(app, "defaults.temperature", "Temperature", (0.0, 2.0, 0.01), "Higher is more random."),
        slider(app, "defaults.top_p", "Top P", (0.0, 1.0, 0.01), ""),
        slider(app, "defaults.top_k", "Top K", (0.0, 200.0, 1.0), "0 is no limit."),
        slider(app, "defaults.min_p", "Min P", (0.0, 1.0, 0.01), ""),
        slider(app, "defaults.repeat_penalty", "Repeat penalty", (1.0, 2.0, 0.01), "1 is off."),
        whole(app, "defaults.repeat_last_n", "Repeat window", "Tokens."),
        whole(app, "defaults.max_tokens", "Longest reply", "Tokens. -1 is no limit."),
        Row::head("PROMPTING"),
        Row::long("system_prompt", "System prompt", &c_s(app, "system_prompt"), ""),
        Row::head("CONTEXT"),
        Row::choice("tools.overflow", "When it is full", &c_s(app, "tools.overflow"), &[("rolling", "Drop the oldest"), ("middle", "Drop the middle"), ("stop", "Stop")], ""),
        Row::head("LOADING"),
        whole(app, "defaults.n_ctx", "Context", "Tokens."),
        whole(app, "defaults.n_gpu_layers", "GPU layers", "-1 is all, 0 is none."),
        Row::choice("defaults.split_mode", "Split", &c_s(app, "defaults.split_mode"), &[("layer", "By layer"), ("row", "By row"), ("none", "No split")], ""),
        whole(app, "defaults.n_batch", "Batch", ""),
        whole(app, "defaults.n_threads", "Threads", "0 is every core."),
        toggle(app, "defaults.flash_attn", "Flash attention", ""),
    ]
}

fn gpu_order(app: &App) -> Vec<usize> {
    let count = list(app, "devices", "gpus").len();
    let mut order: Vec<usize> = at(&app.config, "gpu.priority")
        .and_then(Value::as_array)
        .map(|l| l.iter().filter_map(|i| i.as_u64().map(|x| x as usize)).filter(|i| *i < count).collect())
        .unwrap_or_default();
    for i in 0..count {
        if !order.contains(&i) {
            order.push(i);
        }
    }
    order
}

fn hardware(app: &App) -> Vec<Row> {
    let Some(devices) = app.data.get("devices") else { return vec![Row::note("Looking...")] };
    let gpus = list(app, "devices", "gpus");
    if gpus.is_empty() {
        return vec![Row::note("No GPUs found."), Row::button("@page:runtimes", "Runtimes", "")];
    }
    let support = devices.get("support").cloned().unwrap_or(Value::Null);
    let unsplit = s(&support, "split");
    let mode = { let m = c_s(app, "gpu.mode"); if m.is_empty() { "auto".to_string() } else { m } };
    let mut rows = vec![Row::head("SPLIT ACROSS CARDS")];
    if !unsplit.is_empty() {
        rows.push(Row::bad(&unsplit));
    }
    rows.push(Row::choice("gpu.mode", "Mode", &mode, &[("auto", "Automatic"), ("even", "Even, by free memory"), ("priority", "Priority, fill in order"), ("single", "One card")], ""));
    rows.push(Row::head(if mode == "priority" { "ORDER" } else { "CARDS" }));
    let card = |g: &Value| format!("{}  {}, {} free of {}", s(g, "name"), s(g, "backend"), bytes(n(g, "memory_free")), bytes(n(g, "memory_total")));
    if mode == "priority" {
        let order = gpu_order(app);
        for (place, index) in order.iter().enumerate() {
            rows.push(Row::note(&format!("{}. {}", place + 1, card(gpus[*index]))));
            if place > 0 {
                rows.push(Row::button(&format!("gpu-move:{place}:-1"), "Move up", ""));
            }
            if place + 1 < order.len() {
                rows.push(Row::button(&format!("gpu-move:{place}:1"), "Move down", ""));
            }
        }
    } else {
        for g in &gpus {
            rows.push(Row::note(&card(g)));
        }
    }
    if mode == "single" {
        let options = gpus.iter().enumerate().map(|(i, g)| (i.to_string(), s(g, "name"))).collect();
        rows.push(Row::picks("@main-gpu", "The card", &(c_n(app, "gpu.main_gpu") as usize).to_string(), options, ""));
    }
    rows.push(Row::head("MEMORY"));
    let why_only = s(&support, "gpu_only");
    let why_vram = s(&support, "vram_only");
    rows.push(toggle(app, "gpu.gpu_only", "Keep every layer on the GPU", if why_only.is_empty() { "Refuse a partial load." } else { &why_only }).disabled(!why_only.is_empty()));
    rows.push(toggle(app, "gpu.vram_only", "Use dedicated VRAM only", if why_vram.is_empty() { "No spilling into system RAM." } else { &why_vram }).disabled(!why_vram.is_empty()));
    rows
}

fn runtimes(app: &App) -> Vec<Row> {
    let Some(data) = app.data.get("runtimes") else { return vec![Row::note("Looking...")] };
    if !b(data, "loadable") {
        return vec![Row::note("Built into this build.")];
    }
    let mut rows = Vec::new();
    let build = app.data.get("runtime.progress");
    let building = build.is_some_and(|x| b(x, "running"));
    progress_rows(build, &format!("Installing {}", build.map(|x| s(x, "backend")).unwrap_or_default()), "@runtime-cancel", "@runtime-dismiss", &mut rows);
    for r in list(app, "runtimes", "runtimes") {
        let id = s(r, "id");
        let mut title = s(r, "name").to_uppercase();
        if b(r, "active") {
            title.push_str("  (ACTIVE)");
        } else if b(r, "installed") {
            title.push_str("  (IDLE)");
        }
        rows.push(Row::head(&title));
        rows.push(Row::note(&s(r, "blurb")));
        let mut facts = Vec::new();
        if b(r, "installed") {
            facts.push(if b(r, "active") { format!("{} devices", n(r, "devices") as u64) } else { "no devices".to_string() });
            facts.push(bytes(n(r, "bytes")));
            let source = s(r, "source");
            if !source.is_empty() {
                facts.push(source);
            }
        } else {
            facts.push("not installed".to_string());
            let tool = s(r, "needs_tool");
            if !tool.is_empty() {
                facts.push(format!("needs {tool}"));
            }
        }
        rows.push(Row::note(&facts.join(", ")));
        if b(r, "stale") {
            rows.push(Row::bad("Built for another llama.cpp."));
        }
        let missing = s(r, "missing");
        if !missing.is_empty() {
            rows.push(Row::bad(&format!("Missing {missing}. Reinstall.")));
        }
        if !b(r, "installed") && !b(r, "buildable") && !s(r, "blocker").is_empty() {
            rows.push(Row::note(&s(r, "blocker")));
        }
        rows.push(Row::button(&format!("runtime-build:{id}"), if b(r, "installed") { "Reinstall" } else { "Install" }, "").disabled(building));
        if b(r, "installed") {
            rows.push(Row::button(&format!("runtime-remove:{id}\n{}", s(r, "name")), "Remove", "").disabled(building));
        }
    }
    rows.push(Row::note(&format!("\n{}, {}", s(data, "directory"), bytes(n(data, "bytes")))));
    rows
}

fn training(app: &App) -> Vec<Row> {
    let Some(t) = app.data.get("trainer") else { return vec![Row::note("Looking...")] };
    let mut rows = Vec::new();
    let install = app.data.get("trainer.progress");
    let installing = install.is_some_and(|x| b(x, "running"));
    progress_rows(install, "Installing the trainer", "@trainer-cancel", "@trainer-dismiss", &mut rows);
    let present = b(t, "present");
    let ready = b(t, "ready");
    rows.push(Row::head(if ready { "READY" } else if present { "INSTALLED, BUT NOT WORKING" } else { "NOT INSTALLED" }));
    if present {
        let note = s(t, "note");
        if !note.is_empty() {
            rows.push(if ready { Row::note(&note) } else { Row::bad(&note) });
        }
        let names = |key: &str| t.get(key).and_then(Value::as_array).map(|l| l.iter().filter_map(Value::as_str).collect::<Vec<_>>().join(", ")).unwrap_or_default();
        rows.push(Row::note(&format!(
            "Built for {}\nPython {}\nPyTorch {}\nSize {}",
            s(t, "flavor"),
            s(t, "python"),
            { let x = s(t, "torch"); if x.is_empty() { "not importable".to_string() } else { x } },
            bytes(n(t, "bytes"))
        )));
        if !names("usable_gpus").is_empty() {
            rows.push(Row::note(&format!("Cards it can train on: {}", names("usable_gpus"))));
        }
        if !names("unusable_gpus").is_empty() {
            rows.push(Row::bad(&format!("Cards it cannot: {}", names("unusable_gpus"))));
        }
    }
    rows.push(Row::head("INSTALL"));
    for f in list(app, "trainer.flavors", "") {
        let id = s(f, "id");
        let verb = if present && s(t, "flavor") == id { if ready { "Reinstall" } else { "Repair" } } else { "Install" };
        let star = if b(f, "suggested") { ", recommended" } else { "" };
        let hint = format!("{} to download, {} on disk{star}", bytes(n(f, "download")), bytes(n(f, "installed")));
        rows.push(Row::button(&format!("trainer-install:{id}"), &format!("{verb} {id}"), &hint).disabled(installing));
    }
    if present {
        rows.push(Row::button("@trainer-remove", "Remove", "").disabled(installing));
    }
    rows.push(Row::note(&format!("\nAll in {}", s(t, "directory"))));
    rows
}

fn tools(app: &App) -> Vec<Row> {
    let root = app.state.raw.get("project").map(|x| s(x, "root")).unwrap_or_default();
    let searching = c_b(app, "tools.web_search");
    let provider = c_s(app, "tools.search_provider");
    let mut rows = vec![
        Row::head("PROJECT"),
        Row::note(if root.is_empty() { "None open." } else { &root }),
        slider(app, "tools.actions", "Actions per prompt", (10.0, 200.0, 10.0), ""),
        slider(app, "tools.workshop_timeout", "Command timeout (seconds)", (5.0, 900.0, 5.0), ""),
    ];
    let processes = app.data.get("processes").and_then(|x| x.get("processes")).and_then(Value::as_array).cloned().unwrap_or_default();
    if !processes.is_empty() {
        rows.push(Row::head("RUNNING NOW"));
        for one in &processes {
            let name = s(one, "name");
            let hint = if b(one, "running") { text::clock(n(one, "seconds") as u64) } else { format!("exited {}", n(one, "status") as i64) };
            rows.push(Row::button(&format!("process-stop:{name}"), &format!("Stop {name}"), &format!("{}  {hint}", s(one, "command"))));
        }
    }
    rows.push(Row::head("THE WEB"));
    rows.push(toggle(app, "tools.web_search", "Web search and pages", "Searches leave this machine."));
    if searching {
        rows.push(Row::choice("tools.search_provider", "Search with", &provider, &[("duckduckgo", "DuckDuckGo"), ("wikipedia", "Wikipedia"), ("searxng", "searxng"), ("brave", "Brave")], if provider == "duckduckgo" { "No key needed." } else { "" }));
        rows.push(Row::text("tools.search_endpoint", "Endpoint", &c_s(app, "tools.search_endpoint"), "http://localhost:8888", if provider == "searxng" { "" } else { "searxng only." }));
        if provider == "brave" {
            rows.push(Row::secret("tools.search_api_key", "API key", &c_s(app, "tools.search_api_key"), "", "Stored in config.json."));
        }
        rows.push(slider(app, "tools.search_results", "Results", (1.0, 20.0, 1.0), ""));
        rows.push(slider(app, "tools.search_timeout", "Search timeout (seconds)", (2.0, 60.0, 1.0), ""));
        rows.push(slider(app, "tools.search_rounds", "Searches per prompt", (1.0, 10.0, 1.0), ""));
    }
    rows.push(Row::head("THIS COMPUTER"));
    rows.push(toggle(app, "tools.computer_control", "Let experts use this computer", "Screenshots, mouse and keyboard, as you."));
    if let Some(kit) = app.data.get("kit.status") {
        rows.push(Row::head("WHAT EXPERTS USE"));
        let pieces = kit.get("pieces").and_then(Value::as_array).cloned().unwrap_or_default();
        let mut missing = Vec::new();
        for piece in &pieces {
            let where_ = if b(piece, "fetched") { "fetched by Crucible" } else if b(piece, "missing") { "missing" } else { "this machine has its own" };
            if b(piece, "missing") {
                missing.push(s(piece, "label"));
            }
            rows.push(Row::note(&format!("{}  {where_}", s(piece, "label"))));
        }
        if !missing.is_empty() {
            rows.push(Row::button("@kit-fetch", "Fetch what is missing", &missing.join(", ")));
        }
    }
    rows.push(Row::head("MCP SERVERS"));
    rows.push(Row::note("Programs that give every expert tools. Each runs as you."));
    let servers = at(&app.config, "tools.mcp").and_then(Value::as_array).cloned().unwrap_or_default();
    let status = app.data.get("mcp.status").and_then(|x| x.get("servers")).and_then(Value::as_array).cloned().unwrap_or_default();
    for (i, server) in servers.iter().enumerate() {
        let name = s(server, "name");
        let live = status.iter().find(|x| s(x, "name") == name).cloned().unwrap_or(Value::Null);
        let said = if !s(&live, "error").is_empty() {
            s(&live, "error")
        } else if b(&live, "running") {
            format!("running, {} tools", n(&live, "tools") as u64)
        } else if b(server, "enabled") {
            "starts when wanted".to_string()
        } else {
            "off".to_string()
        };
        rows.push(Row::toggle(&format!("mcp-toggle:{i}"), &name, b(server, "enabled"), &said));
        rows.push(Row::button(&format!("mcp-remove:{i}"), "Remove", ""));
    }
    rows.push(Row::button("@mcp-add", "Add a server", ""));
    rows
}

fn server(app: &App) -> Vec<Row> {
    let live = app.state.raw.get("server").cloned().unwrap_or(Value::Null);
    let on = c_b(app, "server.enabled");
    let line = if !on {
        "Off".to_string()
    } else if !s(&live, "error").is_empty() {
        s(&live, "error")
    } else if !b(&live, "listening") {
        "Starting".to_string()
    } else if !b(&live, "answering") {
        "Running".to_string()
    } else {
        "Answering".to_string()
    };
    let provider = c_s(app, "server.provider");
    let model = c_s(app, "server.model");
    let chosen = if provider.is_empty() { model.clone() } else { format!("{provider}|{model}") };
    let delegator = c_s(app, "router.model");
    let delegator_name = delegator.rsplit(['/', '\\']).next().unwrap_or("").to_string();
    let mut options = vec![(String::new(), if delegator_name.is_empty() { "Delegator (none)".to_string() } else { format!("Delegator ({delegator_name})") })];
    options.extend(model_options(app, &chosen, false).into_iter().skip(1));
    let embeddings = c_s(app, "server.embeddings");
    let mut embed = vec![(String::new(), "The model served".to_string())];
    embed.extend(model_options(app, &embeddings, true).into_iter().skip(1));
    let key = c_s(app, "server.api_key");
    let held = !provider.is_empty() && c_b(app, "server.network") && key.is_empty();
    let mut rows = vec![
        toggle(app, "server.enabled", "On", &line),
        Row::picks("@server-model", "Model", &chosen, options, ""),
        Row::picks("server.embeddings", "Embeddings", &embeddings, embed, ""),
        whole(app, "server.port", "Port", ""),
        toggle(app, "server.network", "Serve on the local network", if held { "Needs a key" } else { "" }),
        Row::secret("server.api_key", "API key", &key, "None", "Empty is open."),
        Row::button("@server-key-new", "New key", ""),
    ];
    if !key.is_empty() {
        rows.push(Row::button(&format!("copy:{key}"), "Copy key", ""));
    }
    rows.push(toggle(app, "server.https", "HTTPS", ""));
    if c_b(app, "server.https") {
        rows.push(Row::text("server.certificate", "Certificate", &c_s(app, "server.certificate"), "Self-signed", ""));
        if !c_s(app, "server.certificate").is_empty() {
            rows.push(Row::text("server.certificate_key", "Key", &c_s(app, "server.certificate_key"), "Same file", ""));
        }
        let print = s(&live, "fingerprint");
        if b(&live, "https") && !print.is_empty() {
            rows.push(Row::note(&format!("SHA-256 {print}")));
            rows.push(Row::button(&format!("copy:{print}"), "Copy SHA-256", ""));
            if b(&live, "own_certificate") {
                rows.push(Row::button("@server-cert-new", "New certificate", ""));
            }
        }
    }
    if on {
        let where_ = live.get("addresses").cloned().unwrap_or(Value::Null);
        let mut shown = false;
        for (label, key) in [("This machine", "local"), ("This machine, IPv6", "local6"), ("Network", "network"), ("Network, IPv6", "network6")] {
            let url = s(&where_, key);
            if url.is_empty() {
                continue;
            }
            if !shown {
                rows.push(Row::head("ADDRESSES"));
                shown = true;
            }
            rows.push(Row::button(&format!("copy:{url}"), &url, label));
        }
    }
    if let Some(recent) = live.get("recent").and_then(Value::as_array) {
        if !recent.is_empty() {
            rows.push(Row::head("RECENT"));
            for r in recent {
                let model = { let m = s(r, "model"); if m.is_empty() { "refused".to_string() } else { m } };
                let status = n(r, "status") as i64;
                let what = if status == 200 {
                    format!("{model}  {} in, {} out, {:.1} s", n(r, "prompt") as u64, n(r, "completion") as u64, n(r, "ms") / 1000.0)
                } else {
                    format!("{model}  {status}")
                };
                rows.push(Row::note(&what));
            }
        }
    }
    rows
}

fn about(app: &App) -> Vec<Row> {
    let Some(a) = app.data.get("about") else { return vec![Row::note("Reading...")] };
    let u = a.get("update").cloned().unwrap_or(Value::Null);
    let mut rows = vec![Row::head(&format!("CRUCIBLE {}", s(a, "version")))];
    if b(&u, "available") {
        rows.push(Row::note(&format!("{} is available.", s(&u, "latest"))));
        rows.push(Row::button("@update", "Update now", ""));
    } else if !s(&u, "latest").is_empty() {
        rows.push(Row::note("Up to date."));
    } else {
        rows.push(Row::note("Not checked yet."));
    }
    rows.push(Row::button("@update-check", "Check now", ""));
    rows.push(toggle(app, "ui.check_updates", "Check for new versions", "Daily, from GitHub."));
    rows.push(Row::toggle("@prereleases", "Pre-releases", c_b(app, "ui.prereleases"), ""));
    if let Some(me) = app.data.get("self.source") {
        rows.push(Row::head("CRUCIBLE'S OWN SOURCE"));
        if b(me, "found") {
            rows.push(Row::note(&s(me, "root")));
            rows.push(Row::button("@self-open", "Open it as a project", ""));
            rows.push(Row::button("@self-rebuild", if b(me, "rebuilding") { "Rebuilding..." } else { "Rebuild" }, "").disabled(b(me, "rebuilding")));
            let log = s(me, "log");
            if !log.is_empty() {
                let tail: Vec<&str> = log.lines().rev().take(6).collect();
                rows.push(Row::note(&tail.into_iter().rev().collect::<Vec<_>>().join("\n")));
            }
        } else {
            let why = s(me, "why");
            rows.push(Row::note(if why.is_empty() { "Not found." } else { &why }));
            rows.push(Row::text("ui.source_dir", "Its folder", &c_s(app, "ui.source_dir"), "", ""));
        }
    }
    rows.push(Row::head("WHAT LEAVES THIS MACHINE"));
    rows.push(Row::note("The version check, web searches, model searches and downloads, first-start downloads, base models for training, and prompts to providers."));
    rows.push(Row::head("FILES"));
    let files = a.get("files").cloned().unwrap_or(Value::Null);
    for (label, key) in [("Configuration", "config"), ("Models", "models"), ("Runtimes", "runtimes"), ("Python", "python"), ("Projects and history", "projects"), ("Everything else", "data"), ("Log", "log"), ("Crash reports", "crashes")] {
        rows.push(Row::note(&format!("{} {}", text::pad(label, 22), s(&files, key))));
    }
    rows.push(Row::head("TRUSTED FOLDERS"));
    let trusted = a.get("trusted").and_then(Value::as_array).cloned().unwrap_or_default();
    if trusted.is_empty() {
        rows.push(Row::note("None."));
    }
    let none = trusted.is_empty();
    for folder in trusted {
        rows.push(Row::note(folder.as_str().unwrap_or("")));
    }
    rows.push(Row::button("@trust-clear", "Clear trusted folders", "").disabled(none));
    rows
}

pub fn rows(app: &App) -> Vec<Row> {
    let Screen::Settings(me) = &app.screen else { return Vec::new() };
    if app.config.is_null() {
        return vec![Row::note("Reading...")];
    }
    match PAGES[me.page].0 {
        "general" => general(app),
        "experts" => experts(app),
        "providers" => providers(app),
        "generation" => generation(app),
        "hardware" => hardware(app),
        "runtimes" => runtimes(app),
        "training" => training(app),
        "tools" => tools(app),
        "server" => server(app),
        _ => about(app),
    }
}

// --- changing things --------------------------------------------------------------------

/// Run `then`, and ask again for what it changed.
fn doing(app: &mut App, method: &str, params: Value, refresh: &'static [&'static str]) {
    app.call(method, params, move |app, result| {
        if let Err(error) = result {
            app.fail(&error);
        }
        for name in refresh {
            app.fetch(name);
        }
        app.reload_config();
    });
}

fn change(app: &mut App, id: &str, value: Value) {
    let text = value.as_str().unwrap_or("").to_string();
    if let Some((field, expert)) = id.strip_prefix("expert-edit:").and_then(|rest| rest.split_once(':')) {
        let mut params = json!({"id": expert});
        params[field] = json!(text);
        doing(app, "expert.edit", params, &[]);
        return;
    }
    if let Some(expert) = id.strip_prefix("expert:") {
        let (provider, model) = split_model(&text);
        doing(app, "expert.set", json!({"id": expert, "model": model, "provider": provider}), &[]);
        return;
    }
    if let Some(index) = id.strip_prefix("mcp-toggle:").and_then(|x| x.parse::<usize>().ok()) {
        let mut servers = at(&app.config, "tools.mcp").cloned().unwrap_or(json!([]));
        if let Some(server) = servers.get_mut(index) {
            server["enabled"] = value;
        }
        set_mcp(app, servers);
        return;
    }
    match id {
        "models_dir" => {
            app.call("config.set", json!({"models_dir": text}), |app, result| {
                if let Err(error) = result {
                    app.fail(&error);
                }
                app.fetch("models");
                app.reload_config();
            });
        }
        "@theme" => {
            app.set_theme(&text);
            app.call("prefs.set", json!({"key": "cli_theme", "value": text}), |_, _| {});
        }
        "@main-gpu" => app.configure(json!({"gpu": {"main_gpu": text.parse::<u64>().unwrap_or(0)}})),
        "@server-model" => {
            let (provider, model) = split_model(&text);
            app.configure(json!({"server": {"provider": provider, "model": model}}));
        }
        "@prereleases" => {
            app.call("config.set", json!({"ui": {"prereleases": value}}), |app, result| {
                if let Err(error) = result {
                    app.fail(&error);
                }
                app.reload_config();
                check_updates(app);
            });
        }
        path if path.contains('.') => app.configure(patch_of(path, value)),
        _ => {}
    }
}

fn check_updates(app: &mut App) {
    app.say("Asking");
    app.call("update.check", json!({}), |app, result| {
        match result {
            Ok(reply) if b(&reply, "available") => {
                let latest = s(&reply, "latest");
                app.say(&format!("{latest} is available"));
            }
            Ok(_) => app.say("Up to date"),
            Err(error) => app.fail(&error),
        }
        app.fetch("about");
    });
}

fn set_mcp(app: &mut App, servers: Value) {
    app.call("config.set", json!({"tools": {"mcp": servers}}), |app, result| {
        if let Err(error) = result {
            app.fail(&error);
        }
        app.reload_config();
        app.fetch("mcp.status");
    });
}

fn split_command(line: &str) -> Vec<String> {
    let (mut out, mut word, mut quote) = (Vec::new(), String::new(), None);
    for c in line.trim().chars() {
        match quote {
            Some(q) if c == q => quote = None,
            Some(_) => word.push(c),
            None if c == '"' || c == '\'' => quote = Some(c),
            None if c.is_whitespace() => {
                if !word.is_empty() {
                    out.push(std::mem::take(&mut word));
                }
            }
            None => word.push(c),
        }
    }
    if !word.is_empty() {
        out.push(word);
    }
    out
}

fn new_expert(app: &mut App) {
    app.type_in("New expert", "Rust Async, Tax Law, Kubernetes", "", false, false, |app, name| {
        let name = name.trim().to_string();
        if name.is_empty() {
            return;
        }
        app.type_in("What it knows", "Used for routing.", "", false, false, move |app, description| {
            let mut items = vec![Item::new("(none)", "")];
            items.extend(model_options(app, "", false).into_iter().skip(1).map(|(v, l)| Item::new(&l, &v)));
            app.pick("Model", items, move |app, item| {
                let (provider, model) = split_model(&item.value);
                let params = json!({"name": name, "description": description, "model": model, "provider": provider});
                doing(app, "expert.add", params, &[]);
            });
        });
    });
}

/// The steps of adding or changing a provider: where, the key, then a model
/// from what it lists, or typed when it will not say.
fn provider_flow(app: &mut App, id: Option<String>, name: String, address: String, key_hint: String, model: String) {
    let title = if id.is_some() { format!("Edit {name}") } else { "Add a provider".to_string() };
    app.type_in(&format!("{title}: address"), "https://api.example.com/v1", &address, false, false, move |app, address| {
        let address = address.trim().to_string();
        if address.is_empty() {
            return app.fail("It needs an address");
        }
        app.type_in(&format!("{title}: API key"), &key_hint, "", true, false, move |app, key| {
            let mut request = json!({"base_url": address});
            match &id {
                Some(id) => request["id"] = json!(id),
                None => {
                    let host = address.split("://").nth(1).unwrap_or(&address).split(['/', ':', '?']).next().unwrap_or("Provider").to_string();
                    request["name"] = json!(if name.is_empty() { host } else { name.clone() });
                }
            }
            if !key.is_empty() {
                request["api_key"] = json!(key);
            }
            let listing = request.clone();
            let keep = id.clone();
            app.say("Asking for its models");
            app.call("provider.models", listing, move |app, result| {
                let listed: Vec<String> = result
                    .ok()
                    .and_then(|r| r.get("models").and_then(Value::as_array).map(|l| l.iter().filter_map(|m| m.as_str().map(str::to_string)).collect()))
                    .unwrap_or_default();
                if listed.is_empty() {
                    app.type_in("Model", "It did not list any. Type one.", &model, false, false, move |app, chosen| {
                        save_provider(app, request, keep, chosen);
                    });
                } else {
                    let items = listed.iter().map(|m| Item::new(m, m).on(*m == model)).collect();
                    app.pick("Model", items, move |app, item| save_provider(app, request, keep, item.value));
                }
            });
        });
    });
}

fn save_provider(app: &mut App, mut request: Value, id: Option<String>, model: String) {
    let mut models = vec![model.trim().to_string()];
    if let Some(id) = &id {
        for seat in at(&app.config, "experts").and_then(Value::as_array).into_iter().flatten() {
            if s(seat, "provider") == *id && !models.contains(&s(seat, "model")) {
                models.push(s(seat, "model"));
            }
        }
    }
    models.retain(|m| !m.is_empty());
    request["models"] = json!(models);
    app.call("provider.save", request, |app, result| {
        match result {
            Ok(reply) => {
                let warning = s(&reply, "warning");
                if warning.is_empty() {
                    app.say("Saved");
                } else {
                    app.fail(&warning);
                }
            }
            Err(error) => app.fail(&error),
        }
        app.fetch("providers");
        app.reload_config();
    });
}

fn button(app: &mut App, id: &str) {
    let (head, tail) = id.split_once(':').unwrap_or((id, ""));
    let (arg, name) = tail.split_once('\n').unwrap_or((tail, ""));
    let (arg, name) = (arg.to_string(), name.to_string());
    match head {
        "@rescan" => {
            app.fetch("models");
            app.say("Rescanned");
        }
        "@models-reset" => change(app, "models_dir", json!("")),
        "@delegator-get" => {
            app.screen = Screen::Models(Models::new("Qwen3-4B-Instruct-2507"));
            crate::models::entering(app);
        }
        "@new-expert" => new_expert(app),
        "expert-remove" => app.ask(&format!("Eject {name}?"), &["The model file stays."], "Eject", "Keep", move |app, yes| {
            if yes {
                doing(app, "expert.remove", json!({"id": arg}), &[]);
            }
        }),
        "@provider-new" => {
            let known = app.data.get("providers").and_then(|x| x.get("known")).and_then(Value::as_array).cloned().unwrap_or_default();
            let mut items = vec![Item::new("Custom", "").hint("Any address")];
            items.extend(known.iter().map(|k| Item::new(&s(k, "name"), &s(k, "name")).hint(&s(k, "note"))));
            app.pick("Template", items, move |app, item| {
                let preset = known.iter().find(|k| s(k, "name") == item.value);
                let address = preset.map(|k| {
                    let url = s(k, "base_url");
                    if url.is_empty() && s(k, "kind") == "anthropic" { "https://api.anthropic.com".to_string() } else { url }
                }).unwrap_or_default();
                let hint = preset.map(|k| s(k, "key_variable")).filter(|v| !v.is_empty()).map_or("key or env:NAME".to_string(), |v| format!("key, env:NAME, or empty for ${v}"));
                provider_flow(app, None, item.value, address, hint, String::new());
            });
        }
        "provider-edit" => {
            let found = list(app, "providers", "providers").into_iter().find(|x| s(x, "id") == arg).cloned();
            if let Some(pv) = found {
                let model = pv.get("models").and_then(|m| m.get(0)).and_then(Value::as_str).unwrap_or("").to_string();
                provider_flow(app, Some(arg), s(&pv, "name"), s(&pv, "endpoint"), "Empty keeps the one saved".to_string(), model);
            }
        }
        "provider-remove" => app.ask(&format!("Remove {name}?"), &["Its key is forgotten."], "Remove", "Keep", move |app, yes| {
            if yes {
                doing(app, "provider.remove", json!({"id": arg}), &["providers"]);
            }
        }),
        "gpu-move" => {
            let (from, by) = arg.split_once(':').unwrap_or(("0", "0"));
            let from: usize = from.parse().unwrap_or(0);
            let by: isize = by.parse().unwrap_or(0);
            let mut order = gpu_order(app);
            let to = from as isize + by;
            if to >= 0 && (to as usize) < order.len() {
                order.swap(from, to as usize);
                app.configure(json!({"gpu": {"priority": order}}));
            }
        }
        "@page" => {
            if let Screen::Settings(me) = &mut app.screen {
                me.page = PAGES.iter().position(|(p, _)| *p == arg).unwrap_or(0);
                me.form = Form::default();
            }
        }
        "runtime-build" => doing(app, "runtime.build", json!({"backend": arg}), &["runtime.progress"]),
        "@runtime-cancel" => doing(app, "runtime.cancel", json!({}), &["runtime.progress"]),
        "@runtime-dismiss" => doing(app, "runtime.dismiss", json!({}), &["runtime.progress"]),
        "runtime-remove" => app.ask(&format!("Remove {name}?"), &["Its files are deleted."], "Remove", "Keep", move |app, yes| {
            if yes {
                doing(app, "runtime.remove", json!({"backend": arg}), &["runtimes", "devices"]);
            }
        }),
        "trainer-install" => doing(app, "trainer.install", json!({"flavor": arg}), &["trainer.progress"]),
        "@trainer-cancel" => doing(app, "trainer.cancel", json!({}), &["trainer.progress"]),
        "@trainer-dismiss" => doing(app, "trainer.dismiss", json!({}), &["trainer.progress"]),
        "@trainer-remove" => app.ask("Remove the training environment?", &["Downloaded base models go too. Your experts stay."], "Remove", "Keep", |app, yes| {
            if yes {
                doing(app, "trainer.remove", json!({}), &["trainer"]);
            }
        }),
        "process-stop" => doing(app, "process.stop", json!({"name": arg}), &["processes"]),
        "@kit-fetch" => doing(app, "kit.fetch", json!({}), &["kit.status"]),
        "mcp-remove" => {
            let mut servers = at(&app.config, "tools.mcp").cloned().unwrap_or(json!([]));
            if let (Some(list), Ok(index)) = (servers.as_array_mut(), arg.parse::<usize>()) {
                if index < list.len() {
                    list.remove(index);
                }
            }
            set_mcp(app, servers);
        }
        "@mcp-add" => app.type_in("MCP server: name", "github", "", false, false, |app, name| {
            let name = name.trim().to_string();
            if name.is_empty() {
                return;
            }
            app.type_in("Command", "npx -y @modelcontextprotocol/server-github", "", false, false, move |app, command| {
                let words = split_command(&command);
                if words.is_empty() {
                    return app.fail("An MCP server needs a command");
                }
                app.type_in("Environment (optional)", "GITHUB_TOKEN=...", "", true, false, move |app, env| {
                    let mut vars = Map::new();
                    for pair in env.split_whitespace() {
                        if let Some((k, v)) = pair.split_once('=') {
                            vars.insert(k.to_string(), json!(v));
                        }
                    }
                    let mut servers: Vec<Value> = at(&app.config, "tools.mcp").and_then(Value::as_array).cloned().unwrap_or_default();
                    servers.retain(|x| s(x, "name") != name);
                    servers.push(json!({"name": name, "command": words[0], "args": words[1..], "env": vars, "enabled": true}));
                    set_mcp(app, json!(servers));
                });
            });
        }),
        "@server-key-new" => {
            let make = |app: &mut App| {
                let key = random_key();
                app.configure(json!({"server": {"api_key": key}}));
                app.say("New key made");
            };
            if c_s(app, "server.api_key").is_empty() {
                make(app);
            } else {
                app.ask("Make a new key?", &["The old one stops working."], "New key", "Keep", move |app, yes| {
                    if yes {
                        make(app);
                    }
                });
            }
        }
        "@server-cert-new" => app.ask("Make a new certificate?", &["Clients given the old one stop trusting it."], "New certificate", "Keep", |app, yes| {
            if yes {
                app.act("server.certificate.new", json!({}));
            }
        }),
        "copy" => {
            crate::copy_to_clipboard(&arg);
            app.say("Copied");
        }
        "@update" => crate::commands::update(app),
        "@update-check" => check_updates(app),
        "@self-open" => {
            let root = app.data.get("self.source").map(|x| s(x, "root")).unwrap_or_default();
            app.act("project.open", json!({"path": root}));
            app.back_to_chat();
        }
        "@self-rebuild" => doing(app, "self.rebuild", json!({}), &["self.source"]),
        "@trust-clear" => app.ask("Clear trusted folders?", &["Each folder is asked about again the next time it is opened."], "Clear", "Keep", |app, yes| {
            if yes {
                doing(app, "trust.clear", json!({}), &["about"]);
            }
        }),
        _ => {}
    }
}

fn random_key() -> String {
    let mut raw = [0u8; 24];
    let filled = std::fs::File::open("/dev/urandom").and_then(|mut f| std::io::Read::read_exact(&mut f, &mut raw)).is_ok();
    if !filled {
        // Windows has no such file. Hashing the clock and an address each
        // round is what the standard library offers without a crate.
        use std::hash::{BuildHasher, Hasher};
        for chunk in raw.chunks_mut(8) {
            let mut h = std::collections::hash_map::RandomState::new().build_hasher();
            h.write_u128(std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos()));
            let bytes_ = h.finish().to_le_bytes();
            chunk.copy_from_slice(&bytes_[..chunk.len()]);
        }
    }
    let hex: String = raw.iter().map(|b| format!("{b:02x}")).collect();
    format!("sk-crucible-{hex}")
}

fn activate(app: &mut App, row: &Row) {
    let id = row.id.clone();
    match &row.kind {
        Kind::Toggle(on) => change(app, &id, json!(!on)),
        Kind::Num { value, step, .. } => {
            let whole_number = step.fract() == 0.0;
            let shown = if whole_number { format!("{value:.0}") } else { format!("{value}") };
            app.type_in(&row.label, &row.hint, &shown, false, false, move |app, typed| match typed.trim().parse::<f64>() {
                Ok(v) if whole_number => change(app, &id, json!(v as i64)),
                Ok(v) => change(app, &id, json!(v)),
                Err(_) => app.fail("A number"),
            });
        }
        Kind::Text { value, secret, multi, .. } => {
            let current = if *secret { "" } else { value.as_str() };
            app.type_in(&row.label, &row.hint, current, *secret, *multi, move |app, typed| change(app, &id, json!(typed)));
        }
        Kind::Choice { value, options } => {
            let items = options.iter().map(|(v, l)| Item::new(l, v).on(v == value)).collect();
            app.pick(&row.label, items, move |app, item| change(app, &id, json!(item.value)));
        }
        Kind::Button => button(app, &id),
        _ => {}
    }
}

fn step(app: &mut App, row: &Row, by: f64) {
    match &row.kind {
        Kind::Toggle(on) => change(app, &row.id, json!(!on)),
        Kind::Num { value, min, max, step } => {
            let next = if max > min { (value + step * by).clamp(*min, *max) } else { value + step * by };
            let next = (next / step).round() * step;
            if step.fract() == 0.0 {
                change(app, &row.id, json!(next as i64));
            } else {
                change(app, &row.id, json!((next * 1000.0).round() / 1000.0));
            }
        }
        Kind::Choice { value, options } if !options.is_empty() => {
            let at = options.iter().position(|(v, _)| v == value).unwrap_or(0) as isize;
            let next = (at + by as isize).rem_euclid(options.len() as isize) as usize;
            change(app, &row.id, json!(options[next].0));
        }
        _ => {}
    }
}

pub fn key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    let shown = rows(app);
    let Screen::Settings(me) = &mut app.screen else { return };
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => return app.back_to_chat(),
        KeyCode::Char('c') if ctrl => return app.back_to_chat(),
        KeyCode::Tab | KeyCode::Char(']') => {
            me.page = (me.page + 1) % PAGES.len();
            me.form = Form::default();
            return;
        }
        KeyCode::BackTab | KeyCode::Char('[') => {
            me.page = (me.page + PAGES.len() - 1) % PAGES.len();
            me.form = Form::default();
            return;
        }
        _ => {}
    }
    match me.form.key(&shown, key) {
        Did::Act => {
            let row = shown[me.form.at].clone();
            activate(app, &row);
        }
        Did::Step(by) => {
            let row = shown[me.form.at].clone();
            step(app, &row, by);
        }
        _ => {}
    }
}

pub fn draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let shown = rows(app);
    let page = if let Screen::Settings(me) = &app.screen { me.page } else { 0 };
    let nav_cols: u16 = if area.width >= 72 { 16 } else { 0 };

    let title = Line::from(vec![
        Span::styled(" Settings", theme::bold(theme::flame())),
        Span::styled(format!("  {}", PAGES[page].1), theme::lit()),
    ]);
    frame.buffer_mut().set_line(area.x, area.y, &title, area.width);
    let rule = "─".repeat(area.width as usize);
    frame.buffer_mut().set_string(area.x, area.y + 1, &rule, theme::fg(p().edge));

    let body = Rect { x: area.x, y: area.y + 2, width: area.width, height: area.height.saturating_sub(4) };
    if nav_cols > 0 {
        for (i, (_, label)) in PAGES.iter().enumerate() {
            let here = i == page;
            let line = Line::from(vec![
                Span::styled(if here { " ▎" } else { "  " }, theme::flame()),
                Span::styled(text::pad(label, nav_cols as usize - 2), if here { theme::bold(theme::text()) } else { theme::dim() }),
            ]);
            if (i as u16) < body.height {
                frame.buffer_mut().set_line(body.x, body.y + i as u16, &line, nav_cols);
            }
        }
    }
    let page_area = Rect { x: body.x + nav_cols, y: body.y, width: body.width.saturating_sub(nav_cols), height: body.height };
    if let Screen::Settings(me) = &mut app.screen {
        me.form.draw(frame.buffer_mut(), page_area, &shown);
    }
    let foot_y = area.bottom().saturating_sub(1);
    if let Some((said, bad, _)) = &app.note {
        let style = if *bad { theme::bad() } else { theme::lit() };
        frame.buffer_mut().set_string(area.x + 1, foot_y - 1, text::fit(said, area.width as usize - 2), style);
    }
    let keys = " arrows move   enter change   left, right adjust   tab next page   esc back";
    frame.buffer_mut().set_string(area.x, foot_y, text::fit(keys, area.width as usize), theme::faint());
}

/// While Settings is open: an install's progress is asked for as it goes.
pub fn on_push(app: &mut App) {
    if !matches!(app.screen, Screen::Settings(_)) {
        return;
    }
    if app.data.get("runtime.progress").is_some_and(|x| b(x, "running")) {
        let was_running = true;
        app.call("runtime.progress", json!({}), move |app, result| {
            if let Ok(value) = result {
                let done = was_running && !b(&value, "running");
                app.data.insert("runtime.progress".into(), value);
                if done {
                    app.fetch("runtimes");
                    app.fetch("devices");
                }
            }
        });
    }
    if app.data.get("trainer.progress").is_some_and(|x| b(x, "running")) {
        app.call("trainer.progress", json!({}), move |app, result| {
            if let Ok(value) = result {
                let done = !b(&value, "running");
                app.data.insert("trainer.progress".into(), value);
                if done {
                    app.fetch("trainer");
                }
            }
        });
    }
}
