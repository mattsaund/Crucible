// SPDX-License-Identifier: MIT
//! The chat screen: the conversation, the line saying what it is doing, the
//! box, and the line under it.

use crate::app::{App, Cached, Screen};
use crate::commands;
use crate::diff;
use crate::models;
use crate::overlay;
use crate::screens;
use crate::settings;
use crate::state::{b, n, s, Action, Turn};
use crate::text::{self, clock, compact, fit, fit_left, line_width};
use crate::theme::{self, p};
use crate::transcript;
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Clear, Widget};
use ratatui::Frame;
use serde_json::Value;
use std::rc::Rc;

const FLAME: [&str; 12] = [
    "⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀",
    "⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀",
    "⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀",
    "⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀",
    "⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆",
    "⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿",
    "⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿",
    "⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁",
    "⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁",
    "⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀",
    "⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀",
];

const SPIN: [&str; 6] = ["✶", "✸", "✹", "✺", "✹", "✸"];

pub fn draw(frame: &mut Frame, app: &mut App) {
    let area = frame.area();
    if area.width < 20 || area.height < 8 {
        frame.buffer_mut().set_string(0, 0, fit("Too small", area.width as usize), theme::dim());
        return;
    }
    match app.screen {
        Screen::Chat => chat(frame, app, area),
        Screen::Settings(_) => settings::draw(frame, app, area),
        Screen::Models(_) => models::draw(frame, app, area),
        Screen::Page(_) => screens::draw(frame, app, area),
    }
    overlay::draw(frame, app);
    selection(frame, app);
    if !app.truecolor {
        theme::downgrade(frame.buffer_mut());
    }
}

fn selection(frame: &mut Frame, app: &mut App) {
    let Some(sel) = app.selection else { return };
    let (a, b) = if (sel.from.1, sel.from.0) <= (sel.to.1, sel.to.0) { (sel.from, sel.to) } else { (sel.to, sel.from) };
    let area = frame.area();
    let buf = frame.buffer_mut();
    let mut copied = String::new();
    for y in a.1..=b.1.min(area.height.saturating_sub(1)) {
        let left = if y == a.1 { a.0 } else { 0 };
        let right = if y == b.1 { b.0 } else { area.width.saturating_sub(1) };
        let mut row = String::new();
        let mut x = left;
        while x <= right.min(area.width.saturating_sub(1)) {
            if let Some(cell) = buf.cell_mut((x, y)) {
                let symbol = cell.symbol().to_string();
                let wide = text::width(&symbol) > 1;
                row.push_str(&symbol);
                cell.set_style(Style::default().fg(p().ink).bg(p().flame));
                if wide {
                    x += 1;
                    if let Some(next) = buf.cell_mut((x, y)) {
                        next.set_style(Style::default().fg(p().ink).bg(p().flame));
                    }
                }
            }
            x += 1;
        }
        copied.push_str(row.trim_end());
        if y != b.1 {
            copied.push('\n');
        }
    }
    if sel.done {
        crate::copy_to_clipboard(&copied);
        app.selection = None;
        app.say("Copied");
    }
}

/// A step it is taking, in words: the verb and what it is pointed at, on one
/// line, with the project's folder off a path.
fn step_words(status: &str, root: &str) -> Option<String> {
    let (verb, rest) = status.split_once(' ').unwrap_or((status, ""));
    let mut rest = rest.lines().next().unwrap_or("").trim().to_string();
    if !root.is_empty() {
        rest = rest.replace(&format!("{}/", root.trim_end_matches('/')), "");
    }
    let (word, keeps) = match verb {
        "read" => ("Reading", true),
        "write" => ("Writing", true),
        "edit" => ("Editing", true),
        "run" => ("Running", true),
        "list" => ("Listing", true),
        "find" => ("Searching for", true),
        "fetch" => ("Fetching", true),
        "render" => ("Rendering", true),
        "tool" => ("Using", true),
        "git" => ("Running git", true),
        "gh" => ("Running gh", true),
        "start" => ("Starting", true),
        "stop" => ("Stopping", true),
        "key" => ("Pressing", true),
        "python" => ("Running Python", false),
        "note" => ("Taking a note", false),
        "logs" => ("Reading the logs", false),
        "screenshot" => ("Taking a screenshot", false),
        "click" => ("Clicking", false),
        "move" => ("Moving the mouse", false),
        "type" => ("Typing", false),
        "scroll" => ("Scrolling", false),
        _ => return None,
    };
    Some(if keeps && !rest.is_empty() { format!("{word} {rest}") } else { word.to_string() })
}

/// What it is doing, in one short line that stays put until it moves on to
/// something else. Never the words of its thinking: those are in the readout.
fn sentence(app: &App) -> String {
    let state = &app.state;
    let status = state.status.trim();
    match state.mood.as_str() {
        "routing" => return "Choosing an expert".to_string(),
        "talking" => return "Writing the answer".to_string(),
        _ => {}
    }
    if status == "stopping" {
        return String::new();
    }
    let root = state.raw.get("project").map(|x| s(x, "root")).unwrap_or_default();
    if let Some(step) = step_words(status, &root) {
        return step;
    }
    if status.is_empty() || status.ends_with(" is thinking") {
        let worked = state.turns.last().is_some_and(|t| !t.actions.is_empty());
        return (if worked { "Deciding the next step" } else { "Working on your prompt" }).to_string();
    }
    if let Some(name) = status.strip_prefix("swapping in ") {
        return name.to_string();
    }
    let line = status.lines().next().unwrap_or("");
    let mut chars = line.chars();
    match chars.next() {
        Some(c) => c.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}

/// What is loading right now and how far along it is: an expert or the
/// delegator. No figure for one that cannot say, which an MLX model cannot.
fn loading_now(state: &crate::state::State) -> Option<(String, Option<f64>)> {
    if let Some(e) = state.experts.iter().find(|e| e.phase == "loading") {
        return Some((e.name.clone(), (e.progress >= 0.0).then_some(e.progress)));
    }
    let share = state.raw.get("delegator_progress")?.as_f64()?;
    Some(("the delegator".to_string(), (share >= 0.0).then_some(share)))
}

fn working(app: &App, cols: usize) -> Line<'static> {
    let state = &app.state;
    let elapsed = app.born.elapsed().as_millis() as usize;
    let frame = SPIN[(elapsed / 120) % SPIN.len()];
    let glow = if (elapsed / 480) % 2 == 0 { theme::flame() } else { theme::lit() };

    if let Some(edit) = &state.pending_edit {
        return Line::from(vec![
            Span::styled("◆ ", theme::lit()),
            Span::styled("Waiting on you", theme::bold(theme::lit())),
            Span::styled(format!("  {}", fit_left(&edit.path, cols.saturating_sub(24))), theme::dim()),
        ]);
    }
    // Shown whatever else is going on: picking an expert loads it with no
    // prompt running, and that is a load to watch like any other.
    if let Some((name, share)) = loading_now(state) {
        let lead = share.map_or("Loading".to_string(), |v| format!("Loading {:.0}%", v.clamp(0.0, 1.0) * 100.0));
        let hint = if state.busy { "  esc to stop" } else { "" };
        let room = cols.saturating_sub(text::width(&lead) + 4 + hint.len());
        return Line::from(vec![
            Span::styled(format!("{frame} "), glow),
            Span::styled(lead, theme::bold(glow)),
            Span::styled("  ", theme::dim()),
            Span::styled(fit(&name, room), theme::text()),
            Span::styled(hint, theme::faint()),
        ]);
    }
    if state.busy {
        let verb = match state.mood.as_str() {
            "routing" => "Routing",
            "loading" => "Loading",
            "talking" => "Answering",
            "error" => "Stopping",
            _ => "Thinking",
        };
        let verb = if state.status == "stopping" { "Stopping" } else { verb };
        let seconds = app.began.map_or(0, |t| t.elapsed().as_secs());
        let lead = format!("{verb} {}", clock(seconds));
        let hint = "  esc to stop";
        let room = cols.saturating_sub(text::width(&lead) + 4 + hint.len());
        return Line::from(vec![
            Span::styled(format!("{frame} "), glow),
            Span::styled(lead, theme::bold(glow)),
            Span::styled("  ", theme::dim()),
            Span::styled(fit(&sentence(app), room), theme::text()),
            Span::styled(hint, theme::faint()),
        ]);
    }
    if let Some(setup) = state.raw.get("setup") {
        if b(setup, "running") {
            let items = setup.get("items").and_then(Value::as_array);
            if let Some(now) = items.and_then(|l| l.iter().find(|i| s(i, "state") == "working")) {
                let share = now.get("progress").and_then(Value::as_f64).map_or(String::new(), |v| format!(" {:.0}%", v * 100.0));
                return Line::from(vec![
                    Span::styled(format!("{frame} "), glow),
                    Span::styled(format!("Setting up{share}"), theme::bold(glow)),
                    Span::styled(format!("  {}", fit(&s(now, "label"), cols.saturating_sub(24))), theme::text()),
                ]);
            }
        }
    }
    if let Some(list) = state.raw.get("downloads").and_then(Value::as_array) {
        if let Some(d) = list.iter().find(|d| ["running", "starting"].contains(&s(d, "state").as_str())) {
            let total = n(d, "total");
            let share = if total > 0.0 { n(d, "done") / total * 100.0 } else { 0.0 };
            return Line::from(vec![
                Span::styled(format!("{frame} "), glow),
                Span::styled(format!("Downloading {share:.0}%"), theme::bold(glow)),
                Span::styled(format!("  {}", fit(&s(d, "repo"), cols.saturating_sub(24))), theme::text()),
            ]);
        }
    }
    if let Some((text, bad, _)) = &app.note {
        return Line::from(Span::styled(fit(text, cols), if *bad { theme::bad() } else { theme::dim() }));
    }
    // News for a moment, and an error until something else happens.
    let fresh = app.status_at.elapsed().as_secs() < 6;
    if !state.status.is_empty() && (state.mood == "error" || fresh) {
        let style = if state.mood == "error" { theme::bad() } else { theme::dim() };
        return Line::from(Span::styled(fit(&state.status, cols), style));
    }
    let problem = s(&state.raw, "delegator_problem");
    if !problem.is_empty() {
        return Line::from(Span::styled(fit(&format!("Delegator: {problem}"), cols), theme::lit()));
    }
    Line::default()
}

/// The foot: who has it, how it is set up, what it has cost.
fn status_line(app: &App, cols: usize) -> Line<'static> {
    let state = &app.state;
    let raw = &state.raw;
    let mut left: Vec<Span<'static>> = Vec::new();
    let pinned = app.route_target();
    match state.expert_now() {
        Some(e) => {
            left.push(Span::styled("◆ ", theme::lit()));
            left.push(Span::styled(e.name.clone(), theme::bold(theme::lit())));
            let model = e.model.strip_suffix(".gguf").unwrap_or(&e.model);
            left.push(Span::styled(format!("  {}", fit(model, 30)), theme::dim()));
            if !e.provider.is_empty() {
                left.push(Span::styled(format!("  {}", e.provider), theme::faint()));
            }
        }
        None => {
            if let Some(id) = &pinned {
                left.push(Span::styled("◆ ", theme::lit()));
                left.push(Span::styled(state.expert_name(id), theme::bold(theme::lit())));
            } else {
                left.push(Span::styled("◇ ", theme::dim()));
                left.push(Span::styled("Delegator", theme::dim()));
            }
        }
    }
    if pinned.is_some() && state.expert_now().is_some_and(|e| Some(&e.id) != pinned.as_ref()) {
        left.push(Span::styled("  pinned", theme::faint()));
    }
    let effort = s(raw, "reasoning_effort");
    if !effort.is_empty() {
        left.push(Span::styled(format!("   {effort} effort"), theme::dim()));
    }
    left.push(if b(raw, "auto_edits") {
        Span::styled("   auto edits", theme::flame())
    } else {
        Span::styled("   ask before edits", theme::faint())
    });
    if let Some((_, name)) = &app.testing {
        left.push(Span::styled(format!("   testing {name}"), theme::lit()));
    }
    if !app.queue.is_empty() {
        left.push(Span::styled(format!("   {} queued", app.queue.len()), theme::lit()));
    }
    if !app.attached.is_empty() {
        left.push(Span::styled(format!("   {} attached", app.attached.len()), theme::lit()));
    }

    let usage = raw.get("session_usage").cloned().unwrap_or(Value::Null);
    let size = n(raw, "context_size");
    let used = if size > 0.0 { (n(raw, "context_used") / size * 100.0).round() } else { 0.0 };
    let mut right = format!("{} in  {} out  {used:.0}% context", compact(n(&usage, "input_tokens")), compact(n(&usage, "output_tokens")));
    let tps = n(raw, "tokens_per_second");
    if state.busy && tps > 0.0 {
        right = format!("{tps:.0} tok/s  {right}");
    }
    if raw.get("update").is_some() {
        right = format!("/update  {right}");
    }
    let warm = used >= 75.0;
    let room = line_width(&left);
    let gap = cols.saturating_sub(room + text::width(&right));
    if gap == 0 {
        return Line::from(left);
    }
    left.push(Span::raw(" ".repeat(gap)));
    left.push(Span::styled(right, if warm { theme::lit() } else { theme::faint() }));
    Line::from(left)
}

fn welcome(app: &App, cols: usize) -> Vec<Line<'static>> {
    let state = &app.state;
    let mut info: Vec<Line<'static>> = vec![
        Line::default(),
        Line::default(),
        Line::default(),
        Line::from(Span::styled("Crucible", theme::bold(theme::flame()))),
        Line::from(Span::styled(s(&state.raw, "version"), theme::dim())),
        Line::default(),
        Line::from(Span::styled(fit_left(&state.project_display(), cols.saturating_sub(24)), theme::text())),
    ];
    let seated: Vec<&crate::state::Expert> = state.experts.iter().filter(|e| e.phase != "unconfigured").collect();
    info.push(Line::from(Span::styled(
        if state.experts.is_empty() {
            "Starting".to_string()
        } else if seated.is_empty() {
            "No model selected. /models".to_string()
        } else {
            format!("{} experts ready", seated.len())
        },
        if seated.is_empty() { theme::lit() } else { theme::dim() },
    )));
    info.push(Line::default());
    info.push(Line::from(vec![
        Span::styled("/help", theme::lit()),
        Span::styled(" commands   ", theme::dim()),
        Span::styled("/models", theme::lit()),
        Span::styled(" download   ", theme::dim()),
        Span::styled("/settings", theme::lit()),
    ]));
    let mut out = vec![Line::default()];
    for (i, row) in FLAME.iter().enumerate() {
        let mut spans = vec![Span::raw("  "), Span::styled((*row).to_string(), theme::flame()), Span::raw("   ")];
        if let Some(line) = info.get(i) {
            spans.extend(line.spans.iter().cloned());
        }
        out.push(Line::from(spans));
    }
    if let Some(setup) = state.raw.get("setup") {
        if let Some(items) = setup.get("items").and_then(Value::as_array) {
            let failed = items.iter().any(|i| s(i, "state") == "failed");
            if b(setup, "running") || failed {
                out.push(Line::default());
                for item in items {
                    let phase = s(item, "state");
                    let said = match phase.as_str() {
                        "done" => "ready".to_string(),
                        "waiting" => "waiting".to_string(),
                        _ => {
                            let detail = s(item, "detail");
                            if detail.is_empty() { phase.clone() } else { detail }
                        }
                    };
                    let style = match phase.as_str() {
                        "done" => theme::good(),
                        "failed" => theme::bad(),
                        "working" => theme::lit(),
                        _ => theme::dim(),
                    };
                    out.push(Line::from(vec![
                        Span::styled(format!("  {}", text::pad(&s(item, "label"), 34)), theme::text()),
                        Span::styled(fit(&said, cols.saturating_sub(38)), style),
                    ]));
                }
            }
        }
    }
    out.push(Line::default());
    out
}

fn turn_rows(app: &mut App, index: usize, turn: &Rc<Turn>, cols: usize) -> Rc<Vec<Line<'static>>> {
    let key = (turn.rev, turn.reply.len(), turn.reasoning.len(), cols, app.open, app.show_thinking, theme::is_light());
    if let Some(entry) = app.feed.get(index) {
        if entry.key == key {
            return Rc::clone(&entry.rows);
        }
    }
    let rows = Rc::new(transcript::rows(turn, cols, app.open, app.show_thinking, &app.state));
    let entry = Cached { key, rows: Rc::clone(&rows) };
    if index < app.feed.len() {
        app.feed[index] = entry;
    } else {
        while app.feed.len() < index {
            app.feed.push(Cached { key: (u64::MAX, 0, 0, 0, false, false, false), rows: Rc::new(Vec::new()) });
        }
        app.feed.push(entry);
    }
    rows
}

fn local_rows(action: &Action, cols: usize, open: bool) -> Vec<Line<'static>> {
    let turn = Turn { actions: vec![action.clone()], ..Default::default() };
    let turn = Turn { thoughts: vec![String::new(), String::new()], ..turn };
    transcript::rows(&turn, cols, open, false, &crate::state::State::default())
}

/// The question about an edit, in place above the box.
fn edit_rows(app: &App, cols: usize, room: usize) -> Vec<Line<'static>> {
    let Some(edit) = &app.state.pending_edit else { return Vec::new() };
    let rows = diff::of_files(&edit.before, &edit.after);
    let (added, removed) = diff::counts(&rows);
    let new_file = edit.before.is_empty();
    let mut out = vec![Line::from(vec![
        Span::styled("● ", theme::flame()),
        Span::styled(if new_file { "New file " } else { "Edit " }, theme::bold(theme::text())),
        Span::styled(fit_left(&edit.path, cols.saturating_sub(24)), theme::lit()),
        Span::styled(format!("  +{added}"), theme::fg(p().add)),
        Span::styled(if removed > 0 { format!(" -{removed}") } else { String::new() }, theme::fg(p().del)),
    ])];
    let lead = [Span::raw("  ")];
    let limit = room.saturating_sub(3).max(4);
    if rows.is_empty() {
        out.push(Line::from(Span::styled("  no change", theme::dim())));
    } else {
        out.extend(diff::draw(&rows, &edit.path, cols, limit, &lead));
    }
    out.push(Line::from(vec![
        Span::styled("  y", theme::bold(theme::good())),
        Span::styled(" allow   ", theme::dim()),
        Span::styled("n", theme::bold(theme::bad())),
        Span::styled(" deny   ", theme::dim()),
        Span::styled("a", theme::bold(theme::lit())),
        Span::styled(" allow all from now on", theme::dim()),
    ]));
    out
}

fn chat(frame: &mut Frame, app: &mut App, area: Rect) {
    let cols = area.width as usize;
    let inner_cols = cols.saturating_sub(5).max(8);
    let box_rows = app.input.rows(inner_cols);
    let box_inner = (box_rows.len() as u16).clamp(1, (area.height / 3).max(3));
    let box_h = box_inner + 2;
    let foot = box_h + 2;
    let feed_h = area.height.saturating_sub(foot);
    let feed = Rect { x: area.x, y: area.y, width: area.width, height: feed_h };
    let work = Rect { x: area.x + 1, y: feed.bottom(), width: area.width.saturating_sub(2), height: 1 };
    let box_area = Rect { x: area.x, y: work.bottom(), width: area.width, height: box_h };
    let foot_line = Rect { x: area.x + 1, y: box_area.bottom(), width: area.width.saturating_sub(2), height: 1 };
    app.feed_area = feed;

    // --- the conversation ---------------------------------------------------
    let feed_cols = cols.saturating_sub(2).max(10);
    let turns: Vec<Rc<Turn>> = app.state.turns.clone();
    let mut blocks: Vec<Rc<Vec<Line<'static>>>> = Vec::new();
    if turns.is_empty() {
        blocks.push(Rc::new(welcome(app, feed_cols)));
    }
    for (i, turn) in turns.iter().enumerate() {
        blocks.push(turn_rows(app, i, turn, feed_cols));
        for local in app.local.iter().filter(|l| l.after == i + 1) {
            blocks.push(Rc::new(local_rows(&local.action, feed_cols, app.open)));
        }
    }
    if turns.is_empty() {
        for local in &app.local {
            blocks.push(Rc::new(local_rows(&local.action, feed_cols, app.open)));
        }
    }
    let total: usize = blocks.iter().map(|b| b.len()).sum();
    if app.scroll > 0 && total > app.feed_total {
        app.scroll += total - app.feed_total;
    }
    app.feed_total = total;
    let height = feed.height as usize;
    app.scroll = app.scroll.min(total.saturating_sub(height));
    let last = total - app.scroll;
    let first = last.saturating_sub(height);
    let mut y = feed.y;
    let mut seen = 0;
    for block in &blocks {
        let (from, to) = (seen, seen + block.len());
        seen = to;
        if to <= first || from >= last {
            continue;
        }
        for line in &block[first.max(from) - from..last.min(to) - from] {
            frame.buffer_mut().set_line(feed.x + 1, y, line, feed.width.saturating_sub(1));
            y += 1;
        }
    }
    if app.scroll > 0 {
        let label = format!(" {} rows below, esc to jump ", app.scroll);
        let w = text::width(&label) as u16;
        let x = feed.x + feed.width.saturating_sub(w + 1);
        frame.buffer_mut().set_string(x, feed.bottom().saturating_sub(1), label, Style::default().fg(p().ink).bg(p().lit));
    }

    // --- an edit waiting on an answer ------------------------------------------
    if app.state.pending_edit.is_some() {
        let room = (feed.height as usize * 2 / 3).max(8);
        let rows = edit_rows(app, feed_cols, room);
        let h = (rows.len() as u16).min(feed.height);
        let panel = Rect { x: feed.x, y: feed.bottom() - h, width: feed.width, height: h };
        frame.render_widget(Clear, panel);
        for (i, line) in rows.iter().take(h as usize).enumerate() {
            frame.buffer_mut().set_line(panel.x + 1, panel.y + i as u16, line, panel.width.saturating_sub(1));
        }
    }

    // --- the line above the box ------------------------------------------------
    let line = working(app, work.width as usize);
    frame.buffer_mut().set_line(work.x, work.y, &line, work.width);

    // --- the box ---------------------------------------------------------------
    let edge = if app.state.busy { p().flame } else { p().edge };
    let block = Block::default().borders(Borders::ALL).border_type(BorderType::Rounded).border_style(theme::fg(edge));
    let inner = block.inner(box_area);
    block.render(box_area, frame.buffer_mut());
    let (row, col) = app.input.caret(inner_cols);
    let top = (row + 1).saturating_sub(box_inner as usize);
    frame.buffer_mut().set_string(inner.x + 1, inner.y, if top == 0 { "❯" } else { "⋮" }, theme::flame());
    if app.input.text.is_empty() {
        let hint = if app.state.pending_edit.is_some() {
            "y, n or a"
        } else if app.state.busy {
            "Queue the next prompt"
        } else {
            "Ask anything"
        };
        frame.buffer_mut().set_string(inner.x + 3, inner.y, hint, theme::faint());
    }
    for (i, (start, end)) in box_rows.iter().skip(top).take(box_inner as usize).enumerate() {
        let piece = app.input.text[*start..*end].to_string();
        frame.buffer_mut().set_string(inner.x + 3, inner.y + i as u16, piece, theme::text());
    }
    if app.overlay.is_none() {
        frame.set_cursor_position((inner.x + 3 + col as u16, inner.y + (row - top) as u16));
    }

    // --- the line under it -------------------------------------------------------
    let line = status_line(app, foot_line.width as usize);
    frame.buffer_mut().set_line(foot_line.x, foot_line.y, &line, foot_line.width);

    // --- the slash menu, over the bottom of the conversation -----------------------
    let entries = commands::menu(app);
    if commands::menu_open(app) {
        let shown = entries.len().min(10);
        let h = shown as u16;
        let menu = Rect { x: area.x + 1, y: work.y.saturating_sub(h), width: area.width.saturating_sub(2), height: h };
        frame.render_widget(Clear, menu);
        let at = app.menu_at.min(entries.len() - 1);
        let top = (at + 1).saturating_sub(shown);
        let name_cols = entries.iter().map(|(name, _)| text::width(name)).max().unwrap_or(10).min(menu.width as usize / 2) + 3;
        for (i, (name, what)) in entries.iter().skip(top).take(shown).enumerate() {
            let here = top + i == at;
            let mut spans = vec![
                Span::styled(if here { "❯ " } else { "  " }, theme::flame()),
                Span::styled(text::pad(name, name_cols), if here { theme::bold(theme::lit()) } else { theme::lit() }),
                Span::styled(fit(what, (menu.width as usize).saturating_sub(name_cols + 3)), theme::dim()),
            ];
            let used = line_width(&spans);
            spans.push(Span::raw(" ".repeat((menu.width as usize).saturating_sub(used))));
            for span in &mut spans {
                span.style = span.style.bg(if here { p().hover } else { p().raised });
            }
            frame.buffer_mut().set_line(menu.x, menu.y + i as u16, &Line::from(spans), menu.width);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_step_is_one_short_line() {
        assert_eq!(step_words("read /work/app/src/a.py", "/work/app").as_deref(), Some("Reading src/a.py"));
        assert_eq!(step_words("run python3 a.py\nand more", "").as_deref(), Some("Running python3 a.py"));
        assert_eq!(step_words("python print(1)\nprint(2)", "").as_deref(), Some("Running Python"));
        assert_eq!(step_words("screenshot", "").as_deref(), Some("Taking a screenshot"));
        assert_eq!(step_words("Programming is thinking", ""), None);
    }

    #[test]
    fn a_load_says_how_far_along_it_is() {
        let mut state = crate::state::State::default();
        state.apply(serde_json::json!({"experts": [{"id": "a", "name": "Programming", "phase": "loading", "progress": 0.42}]}));
        assert_eq!(loading_now(&state), Some(("Programming".to_string(), Some(0.42))));
        state.apply(serde_json::json!({"experts": [{"id": "a", "name": "Programming", "phase": "loading", "progress": -1.0}]}));
        assert_eq!(loading_now(&state), Some(("Programming".to_string(), None)));
        state.apply(serde_json::json!({"experts": [], "delegator_progress": 0.5}));
        assert_eq!(loading_now(&state), Some(("the delegator".to_string(), Some(0.5))));
        state.apply(serde_json::json!({"experts": []}));
        assert_eq!(loading_now(&state), None);
    }
}
