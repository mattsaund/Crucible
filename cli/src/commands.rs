// SPDX-License-Identifier: MIT
//! The slash commands, and the menu that opens when one is being typed.

use crate::app::{App, External, Screen};
use crate::models::Models;
use crate::overlay::Item;
use crate::screens::{self, Page};
use crate::settings::{self, Settings};
use crate::state::{b, s};
use crate::text::{self, wrap};
use crate::theme;
use ratatui::text::{Line, Span};
use serde_json::{json, Value};

pub struct Command {
    pub name: &'static str,
    pub also: &'static str,
    pub what: &'static str,
    run: fn(&mut App, &str),
}

fn settings_page(app: &mut App, page: &str) {
    app.screen = Screen::Settings(Settings::open(page));
    settings::entering(app);
}

pub const COMMANDS: &[Command] = &[
    Command { name: "help", also: "?", what: "Everything you can do", run: |a, _| help(a) },
    Command { name: "settings", also: "config", what: "All settings", run: |a, x| settings_page(a, x.trim()) },
    Command { name: "models", also: "model", what: "Search Hugging Face and download", run: |a, x| models_open(a, x) },
    Command { name: "effort", also: "", what: "How hard it thinks", run: effort },
    Command { name: "expert", also: "route", what: "Who answers", run: expert },
    Command { name: "experts", also: "", what: "Models for each expert", run: |a, _| settings_page(a, "experts") },
    Command { name: "new", also: "clear", what: "New chat", run: |a, _| a.new_chat() },
    Command { name: "resume", also: "history", what: "Open an earlier chat", run: |a, _| resume(a) },
    Command { name: "retry", also: "", what: "Ask the last prompt again", run: retry },
    Command { name: "undo", also: "", what: "Delete the last exchange", run: undo },
    Command { name: "showthinking", also: "thinking", what: "Show or hide the thinking", run: show_thinking },
    Command { name: "stop", also: "", what: "Stop what is running", run: |a, _| a.stop() },
    Command { name: "auto", also: "", what: "Allow edits without asking", run: |a, _| toggle_auto(a) },
    Command { name: "attach", also: "", what: "Attach a file", run: attach },
    Command { name: "eject", also: "unload", what: "Free every model from memory", run: |a, _| eject(a) },
    Command { name: "open", also: "cd", what: "Work in another folder", run: open_folder },
    Command { name: "files", also: "code", what: "Browse and edit the project", run: |a, _| screens::open(a, Page::files()) },
    Command { name: "git", also: "", what: "Status, diff, commit, push", run: |a, _| screens::open(a, Page::git()) },
    Command { name: "preview", also: "", what: "Open what it made", run: |a, _| screens::open(a, Page::preview()) },
    Command { name: "create", also: "train", what: "Fine-tune and condense models", run: |a, _| screens::open(a, Page::create()) },
    Command { name: "bench", also: "", what: "Benchmark your models", run: |a, _| screens::open(a, Page::bench()) },
    Command { name: "keep", also: "", what: "Keep the fine-tune being tested", run: keep_test },
    Command { name: "endtest", also: "", what: "Stop testing a fine-tune", run: end_test },
    Command { name: "providers", also: "", what: "Frontier models and keys", run: |a, _| settings_page(a, "providers") },
    Command { name: "generation", also: "", what: "Sampling and loading", run: |a, _| settings_page(a, "generation") },
    Command { name: "hardware", also: "gpu", what: "Graphics cards", run: |a, _| settings_page(a, "hardware") },
    Command { name: "runtimes", also: "", what: "CUDA, Vulkan, Metal", run: |a, _| settings_page(a, "runtimes") },
    Command { name: "training", also: "", what: "The training environment", run: |a, _| settings_page(a, "training") },
    Command { name: "tools", also: "mcp", what: "Commands, web, MCP", run: |a, _| settings_page(a, "tools") },
    Command { name: "server", also: "", what: "Serve a model to other programs", run: |a, _| settings_page(a, "server") },
    Command { name: "usage", also: "cost", what: "Tokens and spending", run: |a, _| usage(a) },
    Command { name: "log", also: "", what: "What the engine printed", run: |a, _| log(a) },
    Command { name: "about", also: "version", what: "Version and files", run: |a, _| settings_page(a, "about") },
    Command { name: "update", also: "", what: "Update Crucible CLI", run: |a, _| update(a) },
    Command { name: "quit", also: "exit", what: "Leave", run: |a, _| a.quitting = true },
];

fn find(name: &str) -> Option<&'static Command> {
    let name = name.to_lowercase();
    COMMANDS.iter().find(|c| c.name == name || (!c.also.is_empty() && c.also == name))
}

pub fn expert_named(app: &App, name: &str) -> Option<String> {
    let wanted = name.to_lowercase();
    app.state
        .experts
        .iter()
        .find(|e| {
            e.id.to_lowercase() == wanted
                || e.name.to_lowercase().replace(' ', "-") == wanted
                || e.tag.to_lowercase() == wanted
        })
        .map(|e| e.id.clone())
}

/// `rest` is what followed the slash. False if it is not a command after all
/// and should be sent as a prompt.
pub fn run(app: &mut App, rest: &str) -> bool {
    let (name, args) = rest.split_once(char::is_whitespace).unwrap_or((rest, ""));
    if let Some(command) = find(name) {
        (command.run)(app, args.trim());
        return true;
    }
    if let Some(id) = expert_named(app, name) {
        if args.trim().is_empty() {
            app.route = id;
            let route = app.route.clone();
            app.call("prefs.set", json!({"key": "route", "value": route}), |_, _| {});
            app.act("route.pick", json!({"expert": app.route, "load": true}));
            return true;
        }
        return false;
    }
    if name.contains('/') || name.contains('.') {
        return false;
    }
    app.fail(&format!("No command /{name}"));
    true
}

// --- the menu -------------------------------------------------------------------

/// The token being typed, when it is a command or a file.
fn token(app: &App) -> Option<(usize, &str)> {
    let head = &app.input.text[..app.input.at];
    let start = head.rfind(char::is_whitespace).map_or(0, |i| i + head[i..].chars().next().map_or(1, char::len_utf8));
    let word = &head[start..];
    if word.starts_with('@') || (word.starts_with('/') && start == 0) {
        Some((start, word))
    } else {
        None
    }
}

/// What the menu offers for that token: name to put in, and what it is.
pub fn menu(app: &App) -> Vec<(String, String)> {
    let Some((_, word)) = token(app) else { return Vec::new() };
    if let Some(wanted) = word.strip_prefix('@') {
        let wanted = wanted.to_lowercase();
        let mut hits: Vec<&String> = app.files.iter().filter(|f| f.to_lowercase().contains(&wanted)).collect();
        hits.sort_by_key(|f| (!f.to_lowercase().rsplit('/').next().unwrap_or("").starts_with(&wanted), f.len()));
        return hits.into_iter().take(8).map(|f| (format!("@{f}"), String::new())).collect();
    }
    let wanted = word.trim_start_matches('/').to_lowercase();
    let mut out: Vec<(String, String)> = Vec::new();
    for c in COMMANDS {
        if c.name.starts_with(&wanted) || (!c.also.is_empty() && c.also.starts_with(&wanted)) {
            out.push((format!("/{}", c.name), c.what.to_string()));
        }
    }
    for e in &app.state.experts {
        let id = e.name.to_lowercase().replace(' ', "-");
        if id.starts_with(&wanted) && !out.iter().any(|(n, _)| *n == format!("/{id}")) {
            out.push((format!("/{id}"), format!("Send to {}", e.name)));
        }
    }
    if !wanted.is_empty() {
        for c in COMMANDS {
            if !c.name.starts_with(&wanted) && c.name.contains(&wanted) {
                out.push((format!("/{}", c.name), c.what.to_string()));
            }
        }
    }
    out
}

pub fn menu_open(app: &App) -> bool {
    !menu(app).is_empty() && !menu_is_exact(app)
}

fn menu_is_exact(app: &App) -> bool {
    let Some((_, word)) = token(app) else { return false };
    word.starts_with('@') && menu(app).first().is_some_and(|(n, _)| n == word) && menu(app).len() == 1
}

/// Put the highlighted entry in the box.
pub fn complete(app: &mut App) {
    let entries = menu(app);
    let Some((name, _)) = entries.get(app.menu_at.min(entries.len().saturating_sub(1))).cloned() else { return };
    let Some((start, _)) = token(app) else { return };
    let end = app.input.at;
    let piece = format!("{name} ");
    app.input.text.replace_range(start..end, &piece);
    app.input.at = start + piece.len();
    app.menu_at = 0;
}

/// Enter on the menu: a command runs, a file is put in the box.
pub fn run_chosen(app: &mut App) {
    let entries = menu(app);
    let Some((name, _)) = entries.get(app.menu_at.min(entries.len().saturating_sub(1))).cloned() else { return };
    if name.starts_with('@') {
        complete(app);
        return;
    }
    let (typed, _) = app.input.text.split_once(char::is_whitespace).unwrap_or((app.input.text.as_str(), ""));
    if typed != name {
        app.input.set(&name);
    }
    app.send();
}

pub fn after_typing(app: &mut App) {
    let wants_files = token(app).is_some_and(|(_, w)| w.starts_with('@'));
    let stale = app.tree_asked.is_none_or(|t| t.elapsed().as_secs() > 20);
    if wants_files && stale {
        app.refresh_files();
    }
}

// --- each command ---------------------------------------------------------------

fn help(app: &mut App) {
    let mut rows: Vec<Line<'static>> = Vec::new();
    let head = |t: &str| Line::from(Span::styled(format!(" {t}"), theme::bold(theme::flame())));
    rows.push(head("COMMANDS"));
    for c in COMMANDS {
        let also = if c.also.is_empty() { String::new() } else { format!("  /{}", c.also) };
        rows.push(Line::from(vec![
            Span::styled(format!("  {}", text::pad(&format!("/{}", c.name), 14)), theme::lit()),
            Span::styled(c.what.to_string(), theme::text()),
            Span::styled(also, theme::faint()),
        ]));
    }
    rows.push(Line::default());
    rows.push(head("IN THE BOX"));
    for (k, what) in [
        ("enter", "Send"),
        ("shift+enter", "New line (also alt+enter, ctrl+j, or \\ then enter)"),
        ("/name prompt", "Send to one expert"),
        ("!command", "Run a command here"),
        ("@file", "Attach a file"),
        ("up, down", "Earlier prompts"),
    ] {
        rows.push(Line::from(vec![
            Span::styled(format!("  {}", text::pad(k, 14)), theme::lit()),
            Span::styled(what.to_string(), theme::text()),
        ]));
    }
    rows.push(Line::default());
    rows.push(head("KEYS"));
    for (k, what) in [
        ("esc", "Stop, or back"),
        ("ctrl+o", "Show or hide long output"),
        ("shift+tab", "Allow edits without asking"),
        ("page up, down", "Scroll (or the mouse wheel)"),
        ("drag", "Select and copy"),
        ("ctrl+z", "Suspend"),
        ("ctrl+c twice", "Quit"),
    ] {
        rows.push(Line::from(vec![
            Span::styled(format!("  {}", text::pad(k, 14)), theme::lit()),
            Span::styled(what.to_string(), theme::text()),
        ]));
    }
    app.read("Help", rows);
}

fn models_open(app: &mut App, args: &str) {
    app.screen = Screen::Models(Models::new(args));
    crate::models::entering(app);
}

const EFFORTS: [(&str, &str); 4] = [("", "Default"), ("low", "Low"), ("medium", "Medium"), ("high", "High")];

fn effort(app: &mut App, args: &str) {
    let set = |app: &mut App, value: &str| {
        app.state.raw["reasoning_effort"] = json!(value);
        app.configure(json!({"reasoning_effort": value}));
    };
    if !args.is_empty() {
        let wanted = args.to_lowercase();
        match EFFORTS.iter().find(|(v, l)| *v == wanted || l.to_lowercase() == wanted) {
            Some((value, label)) => {
                set(app, value);
                app.say(&format!("{label} effort"));
            }
            None => app.fail("Default, low, medium or high"),
        }
        return;
    }
    let now = s(&app.state.raw, "reasoning_effort");
    let items = EFFORTS.iter().map(|(v, l)| Item::new(l, v).on(*v == now)).collect();
    app.pick("Effort", items, move |app, item| {
        set(app, &item.value);
    });
}

fn expert(app: &mut App, args: &str) {
    let choose = |app: &mut App, id: String| {
        app.route = id.clone();
        app.call("prefs.set", json!({"key": "route", "value": id}), |_, _| {});
        let target = app.route_target().unwrap_or_default();
        app.act("route.pick", json!({"expert": target, "load": true}));
    };
    if !args.is_empty() {
        if ["delegator", "auto"].contains(&args.to_lowercase().as_str()) {
            choose(app, String::new());
        } else if let Some(id) = expert_named(app, &args.replace(' ', "-")) {
            choose(app, id);
        } else {
            app.fail(&format!("No expert {args}"));
        }
        return;
    }
    let now = app.route_target().unwrap_or_default();
    let mut items = vec![Item::new("Delegator", "").hint("Picks the expert").on(now.is_empty())];
    for e in app.state.experts.iter().filter(|e| e.phase != "unconfigured") {
        items.push(Item::new(&e.name, &e.id).hint(&e.model).on(e.id == now));
    }
    app.pick("Who answers", items, move |app, item| choose(app, item.value));
}

fn resume(app: &mut App) {
    app.call("recents", json!({"chats": 60}), |app, result| {
        let reply = match result {
            Ok(r) => r,
            Err(error) => return app.fail(&error),
        };
        let items: Vec<Item> = reply
            .get("chats")
            .and_then(Value::as_array)
            .map(|list| {
                list.iter()
                    .map(|c| {
                        let hint = format!("{}  {}", s(c, "project_name"), s(c, "when"));
                        let value = format!("{}\n{}", s(c, "id"), s(c, "project"));
                        Item::new(&s(c, "title"), &value).hint(&hint)
                    })
                    .collect()
            })
            .unwrap_or_default();
        if items.is_empty() {
            return app.say("No earlier chats");
        }
        app.pick("Resume", items, |app, item| {
            let (id, project) = item.value.split_once('\n').unwrap_or((&item.value, ""));
            app.act("history.open", json!({"id": id, "project": project}));
        });
    });
}

fn last_index(app: &App) -> Option<usize> {
    if app.state.busy {
        return None;
    }
    app.state.turns.len().checked_sub(1)
}

fn retry(app: &mut App, _: &str) {
    match last_index(app) {
        Some(index) => app.act("turn.retry", json!({"index": index})),
        None => app.fail("Nothing to retry"),
    }
}

fn undo(app: &mut App, _: &str) {
    match last_index(app) {
        Some(index) => app.act("turn.delete", json!({"index": index})),
        None => app.fail("Nothing to undo"),
    }
}

fn show_thinking(app: &mut App, args: &str) {
    app.show_thinking = match args.to_lowercase().as_str() {
        "on" | "show" => true,
        "off" | "hide" => false,
        _ => !app.show_thinking,
    };
    app.feed.clear();
    app.say(if app.show_thinking { "Showing the thinking" } else { "Hiding the thinking" });
}

pub fn toggle_auto(app: &mut App) {
    let on = !b(&app.state.raw, "auto_edits");
    app.state.raw["auto_edits"] = json!(on);
    app.configure(json!({"tools": {"auto_edits": on}}));
    app.say(if on { "Edits allowed without asking" } else { "Asking before edits" });
}

fn attach(app: &mut App, args: &str) {
    if args.is_empty() {
        return app.fail("/attach path");
    }
    let path = app.folder.join(shellish(args));
    if !path.exists() {
        return app.fail(&format!("{args} is not there"));
    }
    app.attached.push(path.to_string_lossy().to_string());
    app.say(&format!("Attached {args}"));
}

/// A path as typed: ~ for home, and quotes around one with spaces.
pub fn shellish(raw: &str) -> String {
    let raw = raw.trim().trim_matches(['"', '\'']);
    if let Some(rest) = raw.strip_prefix("~/") {
        if let Some(home) = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")) {
            return std::path::Path::new(&home).join(rest).to_string_lossy().to_string();
        }
    }
    raw.to_string()
}

fn keep_test(app: &mut App, _: &str) {
    let Some((id, name)) = app.testing.clone() else { return app.fail("Nothing is being tested") };
    app.call("lab.keep", json!({"id": id}), move |app, result| {
        match result {
            Ok(_) => {
                app.testing = None;
                app.say(&format!("Kept {name}"));
            }
            Err(error) => app.fail(&error),
        }
        app.reload_config();
    });
}

fn end_test(app: &mut App, _: &str) {
    if app.testing.take().is_none() {
        return app.fail("Nothing is being tested");
    }
    app.act("lab.test.stop", json!({}));
    app.say("Stopped testing");
}

fn eject(app: &mut App) {
    app.act("release", json!({"all": true}));
    app.say("Models unloaded");
}

fn open_folder(app: &mut App, args: &str) {
    if args.is_empty() {
        return app.fail("/open path");
    }
    let path = app.folder.join(shellish(args));
    let Ok(path) = std::fs::canonicalize(&path) else {
        return app.fail(&format!("{args} is not there"));
    };
    let text = path.to_string_lossy().to_string();
    app.call("project.open", json!({"path": text}), move |app, result| match result {
        Ok(_) => {
            app.folder = path;
            app.refresh_files();
        }
        Err(error) => app.fail(&error),
    });
}

fn usage(app: &mut App) {
    let raw = &app.state.raw;
    let tally = |v: Option<&Value>| {
        let v = v.cloned().unwrap_or(Value::Null);
        format!(
            "{} in, {} out, {} prompts",
            text::compact(crate::state::n(&v, "input_tokens")),
            text::compact(crate::state::n(&v, "output_tokens")),
            crate::state::n(&v, "turns") as u64
        )
    };
    let mut rows: Vec<Line<'static>> = vec![
        Line::from(vec![Span::styled(" This chat     ", theme::dim()), Span::styled(tally(raw.get("session_usage")), theme::text())]),
        Line::from(vec![Span::styled(" This project  ", theme::dim()), Span::styled(tally(raw.get("project_usage")), theme::text())]),
    ];
    if let Some(list) = raw.get("local_models").and_then(Value::as_array) {
        if !list.is_empty() {
            rows.push(Line::default());
            rows.push(Line::from(Span::styled(" IN MEMORY", theme::bold(theme::flame()))));
            for m in list {
                rows.push(Line::from(vec![
                    Span::styled(format!("  {}", text::pad(&s(m, "seat"), 16)), theme::dim()),
                    Span::styled(s(m, "file"), theme::text()),
                    Span::styled(format!("  {}", text::bytes(crate::state::n(m, "bytes"))), theme::faint()),
                ]));
            }
        }
    }
    if let Some(list) = raw.get("frontier").and_then(Value::as_array) {
        if !list.is_empty() {
            rows.push(Line::default());
            rows.push(Line::from(Span::styled(" FRONTIER MODELS", theme::bold(theme::flame()))));
            for m in list {
                let money = |part: &str| m.get(part).and_then(|p| p.get("cost")).and_then(Value::as_f64);
                let cost = match (money("session"), money("month")) {
                    (Some(a), Some(b)) => format!("  ${a:.2} this chat, ${b:.2} this month"),
                    _ => String::new(),
                };
                rows.push(Line::from(vec![
                    Span::styled(format!("  {}", s(m, "model")), theme::text()),
                    Span::styled(cost, theme::dim()),
                ]));
                for limit in m.get("limits").and_then(Value::as_array).into_iter().flatten() {
                    rows.push(Line::from(Span::styled(
                        format!("    {}: {} of {} left", s(limit, "what"), crate::state::n(limit, "remaining"), crate::state::n(limit, "limit")),
                        theme::faint(),
                    )));
                }
            }
        }
    }
    app.read("Usage", rows);
}

fn log(app: &mut App) {
    let lines: Vec<String> = app.engine.said.lock().unwrap().iter().cloned().collect();
    let cols = app.feed_area.width.saturating_sub(6).max(20) as usize;
    let mut rows = Vec::new();
    for line in lines {
        rows.extend(wrap(&[Span::styled(text::clean(&line), theme::dim())], cols, &[Span::raw(" ")], &[Span::raw("   ")]));
    }
    if rows.is_empty() {
        rows.push(Line::from(Span::styled(" Nothing", theme::faint())));
    }
    app.read("Engine log", rows);
}

pub fn update(app: &mut App) {
    app.ask("Update Crucible CLI?", &["Leaves for a moment to install the newest version."], "Update", "Not now", |app, yes| {
        if yes {
            app.external = Some(External::Update(crate::update_command()));
        }
    });
}
