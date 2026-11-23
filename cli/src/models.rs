// SPDX-License-Identifier: MIT
//! /models: search Hugging Face, see what will run here, download it, and
//! give it to an expert.

use crate::app::{App, Screen};
use crate::composer::Composer;
use crate::markdown;
use crate::overlay::Item;
use crate::state::{b, n, s};
use crate::text::{self, bytes, clock, compact, fit, line_width};
use crate::theme::{self, p};
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Widget};
use ratatui::Frame;
use serde_json::{json, Value};
use std::time::{Duration, Instant};

const SORTS: [(&str, &str); 4] = [("best", "Best match"), ("downloads", "Most downloads"), ("likes", "Most likes"), ("updated", "Recently updated")];

#[derive(PartialEq, Clone, Copy)]
pub enum Focus {
    Search,
    List,
    Options,
}

pub struct Models {
    pub query: Composer,
    pub here: bool,
    pub format: &'static str,
    pub sort: usize,
    pub results: Vec<Value>,
    pub next: String,
    pub loading: bool,
    pub search_id: u64,
    pub at: usize,
    pub top: usize,
    pub focus: Focus,
    pub detail: Option<Value>,
    pub detail_id: String,
    pub option: usize,
    pub readme: Option<String>,
    pub readme_top: usize,
    pub readme_rows: Vec<Line<'static>>,
    pub readme_key: (String, usize),
    pub error: String,
    pub typed: Option<Instant>,
    pub local_at: usize,
    pub done_seen: Vec<String>,
}

impl Models {
    pub fn new(query: &str) -> Models {
        Models {
            query: Composer::with(query, false),
            here: false,
            format: if cfg!(target_os = "macos") { "mlx" } else { "gguf" },
            sort: 0,
            results: Vec::new(),
            next: String::new(),
            loading: false,
            search_id: 0,
            at: 0,
            top: 0,
            focus: Focus::Search,
            detail: None,
            detail_id: String::new(),
            option: 0,
            readme: None,
            readme_top: 0,
            readme_rows: Vec::new(),
            readme_key: (String::new(), 0),
            error: String::new(),
            typed: None,
            local_at: 0,
            done_seen: Vec::new(),
        }
    }
}

fn me(app: &mut App) -> Option<&mut Models> {
    match &mut app.screen {
        Screen::Models(m) => Some(m),
        _ => None,
    }
}

pub fn entering(app: &mut App) {
    app.fetch("models");
    search(app, false);
}

/// Ask the hub: the first page, or with `more`, the one after.
fn search(app: &mut App, more: bool) {
    let Some(m) = me(app) else { return };
    m.typed = None;
    m.loading = true;
    m.error.clear();
    m.search_id += 1;
    let ticket = m.search_id;
    let params = if more {
        json!({"next": m.next, "sort": SORTS[m.sort].0})
    } else {
        json!({"query": m.query.text.trim(), "format": m.format, "sort": SORTS[m.sort].0})
    };
    app.call("market.search", params, move |app, result| {
        let Some(m) = me(app) else { return };
        if m.search_id != ticket {
            return;
        }
        m.loading = false;
        match result {
            Ok(reply) => {
                let rows = reply.get("listings").and_then(Value::as_array).cloned().unwrap_or_default();
                if more {
                    m.results.extend(rows);
                } else {
                    m.results = rows;
                    m.at = 0;
                    m.top = 0;
                }
                m.next = s(&reply, "next");
            }
            Err(error) => m.error = error,
        }
    });
}

fn open_detail(app: &mut App) {
    let Some(m) = me(app) else { return };
    let Some(row) = m.results.get(m.at) else { return };
    let id = s(row, "id");
    if m.detail_id == id && m.detail.is_some() {
        m.focus = Focus::Options;
        return;
    }
    m.detail_id = id.clone();
    m.detail = None;
    m.readme = None;
    m.readme_top = 0;
    m.option = 0;
    m.focus = Focus::Options;
    let format = m.format;
    let wanted = id.clone();
    app.call("market.model", json!({"id": id, "format": format}), move |app, result| {
        let Some(m) = me(app) else { return };
        if m.detail_id != wanted {
            return;
        }
        match result {
            Ok(detail) => {
                let options = detail.get("options").and_then(Value::as_array).cloned().unwrap_or_default();
                m.option = options.iter().position(|o| b(o, "recommended")).unwrap_or(0);
                m.detail = Some(detail);
            }
            Err(error) => {
                m.error = error;
                m.focus = Focus::List;
            }
        }
    });
    let wanted = m_id(app);
    app.call("market.readme", json!({"id": wanted}), move |app, result| {
        let Some(m) = me(app) else { return };
        if m.detail_id != wanted {
            return;
        }
        m.readme = Some(result.ok().map(|r| tidy(&s(&r, "text"))).unwrap_or_default());
        m.readme_key = (String::new(), 0);
    });
}

fn m_id(app: &mut App) -> String {
    me(app).map(|m| m.detail_id.clone()).unwrap_or_default()
}

/// A model card as something to read: no front matter, pictures or HTML.
fn tidy(source: &str) -> String {
    let mut body = source.replace("\r\n", "\n");
    if body.trim_start().starts_with("---") {
        let start = body.find("---").unwrap_or(0);
        if let Some(end) = body[start + 3..].find("\n---") {
            let after = start + 3 + end + 4;
            body = body[after..].to_string();
        }
    }
    let mut out = String::new();
    let mut fenced = false;
    let mut in_comment = false;
    for line in body.lines() {
        let trimmed = line.trim_start();
        if trimmed.starts_with("```") || trimmed.starts_with("~~~") {
            fenced = !fenced;
            out.push_str(line);
            out.push('\n');
            continue;
        }
        if fenced {
            out.push_str(line);
            out.push('\n');
            continue;
        }
        if in_comment {
            if line.contains("-->") {
                in_comment = false;
            }
            continue;
        }
        if trimmed.starts_with("<!--") {
            in_comment = !line.contains("-->");
            continue;
        }
        let mut clean = String::new();
        let mut rest = trimmed;
        while let Some(open) = rest.find('<') {
            clean.push_str(&rest[..open]);
            let after = &rest[open + 1..];
            let tag = after.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '/');
            match (tag, after.find('>')) {
                (true, Some(end)) => rest = &after[end + 1..],
                (true, None) => {
                    rest = "";
                }
                _ => {
                    clean.push('<');
                    rest = after;
                }
            }
        }
        clean.push_str(rest);
        while let Some(start) = clean.find("![") {
            let Some(mid) = clean[start..].find("](") else { break };
            let Some(end) = clean[start + mid..].find(')') else { break };
            clean.replace_range(start..start + mid + end + 1, "");
        }
        let keep = if line.starts_with("  ") && (trimmed.starts_with("- ") || trimmed.starts_with("* ")) {
            format!("{}{}", &line[..line.len() - trimmed.len()], clean)
        } else {
            clean
        };
        out.push_str(&keep);
        out.push('\n');
    }
    let mut squeezed = String::with_capacity(out.len());
    let mut blanks = 0;
    for line in out.lines() {
        blanks = if line.trim().is_empty() { blanks + 1 } else { 0 };
        if blanks < 2 {
            squeezed.push_str(line);
            squeezed.push('\n');
        }
    }
    squeezed.trim().chars().take(40_000).collect()
}

fn local_models(app: &App) -> Vec<Value> {
    app.data.get("models").and_then(|x| x.get("models")).and_then(Value::as_array).cloned().unwrap_or_default()
}

fn give_to_expert(app: &mut App, name: String) {
    let mut items = vec![Item::new("Delegator", "@delegator").hint("Picks the expert")];
    for e in &app.state.experts {
        items.push(Item::new(&e.name, &e.id).hint(&e.model));
    }
    app.pick("Give it to", items, move |app, item| {
        if item.value == "@delegator" {
            app.configure(json!({"router": {"model": name}}));
            app.say("Delegator set");
        } else {
            let label = item.label.clone();
            app.call("expert.set", json!({"id": item.value, "model": name, "provider": ""}), move |app, result| {
                match result {
                    Ok(_) => app.say(&format!("{label} set")),
                    Err(error) => app.fail(&error),
                }
                app.reload_config();
            });
        }
    });
}

fn running(app: &App) -> Vec<Value> {
    app.state.raw.get("downloads").and_then(Value::as_array).cloned().unwrap_or_default()
}

fn downloading(app: &App, repo: &str, option: &str) -> Option<Value> {
    running(app).into_iter().find(|d| s(d, "repo") == repo && s(d, "option") == option && ["running", "starting"].contains(&s(d, "state").as_str()))
}

fn press_download(app: &mut App) {
    let Some(m) = me(app) else { return };
    let Some(detail) = m.detail.clone() else { return };
    let options = detail.get("options").and_then(Value::as_array).cloned().unwrap_or_default();
    let Some(option) = options.get(m.option).cloned() else { return };
    let format = s(&detail, "format");
    if b(&detail, "gated") {
        return app.fail("Gated. Accept its license on Hugging Face first.");
    }
    if b(&option, "here") {
        return give_to_expert(app, here_name(app, &option));
    }
    let (repo, name) = (s(&option, "repo"), s(&option, "name"));
    if let Some(going) = downloading(app, &repo, &name) {
        return app.act("market.cancel", json!({"id": s(&going, "id")}));
    }
    app.call("market.download", json!({"repo": repo, "option": name, "format": format}), |app, result| {
        if let Err(error) = result {
            app.fail(&error);
        }
    });
}

/// The file a download landed as, for handing it to an expert.
fn here_name(app: &App, option: &Value) -> String {
    let names: Vec<String> = option.get("names").and_then(Value::as_array).map(|l| l.iter().filter_map(|x| x.as_str().map(str::to_string)).collect()).unwrap_or_default();
    let files = local_models(app);
    for wanted in &names {
        if let Some(found) = files.iter().find(|f| s(f, "name").rsplit('/').next() == Some(wanted.as_str())) {
            return s(found, "name");
        }
    }
    let repo = s(option, "repo");
    let tail = repo.rsplit('/').next().unwrap_or("").to_string();
    files.iter().find(|f| s(f, "name").contains(&tail)).map_or_else(|| names.first().cloned().unwrap_or_default(), |f| s(f, "name"))
}

fn clear_download(app: &mut App) {
    let list = running(app);
    let Some(d) = list.iter().find(|d| ["running", "starting"].contains(&s(d, "state").as_str())).or_else(|| list.first()) else { return };
    let id = s(d, "id");
    if ["running", "starting"].contains(&s(d, "state").as_str()) {
        app.act("market.cancel", json!({"id": id}));
    } else {
        app.act("market.dismiss", json!({"id": id}));
    }
}

pub fn key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let Some(m) = me(app) else { return };
    if key.code == KeyCode::Tab || key.code == KeyCode::BackTab {
        m.here = !m.here;
        m.focus = if m.here { Focus::List } else { Focus::Search };
        app.fetch("models");
        return;
    }
    if ctrl && key.code == KeyCode::Char('f') {
        m.format = if m.format == "gguf" { "mlx" } else { "gguf" };
        m.detail = None;
        m.detail_id.clear();
        return search(app, false);
    }
    if ctrl && key.code == KeyCode::Char('s') {
        m.sort = (m.sort + 1) % SORTS.len();
        return search(app, false);
    }
    if ctrl && key.code == KeyCode::Char('x') {
        return clear_download(app);
    }
    if m.here {
        let count = local_models(app).len();
        let Some(m) = me(app) else { return };
        match key.code {
            KeyCode::Esc | KeyCode::Char('q') => app.back_to_chat(),
            KeyCode::Up | KeyCode::Char('k') => m.local_at = m.local_at.saturating_sub(1),
            KeyCode::Down | KeyCode::Char('j') => m.local_at = (m.local_at + 1).min(count.saturating_sub(1)),
            KeyCode::Enter => {
                let at = m.local_at;
                if let Some(file) = local_models(app).get(at) {
                    give_to_expert(app, s(file, "name"));
                }
            }
            _ => {}
        }
        return;
    }
    match m.focus {
        Focus::Search => match key.code {
            KeyCode::Esc => app.back_to_chat(),
            KeyCode::Enter => {
                let pending = m.typed.is_some();
                m.focus = Focus::List;
                if pending {
                    search(app, false);
                }
            }
            KeyCode::Down => m.focus = Focus::List,
            _ => {
                let before = m.query.text.clone();
                m.query.key(key, 60);
                if m.query.text != before {
                    m.typed = Some(Instant::now());
                }
            }
        },
        Focus::List => match key.code {
            KeyCode::Esc => m.focus = Focus::Search,
            KeyCode::Char('/') => m.focus = Focus::Search,
            KeyCode::Up | KeyCode::Char('k') => {
                if m.at == 0 {
                    m.focus = Focus::Search;
                } else {
                    m.at -= 1;
                }
            }
            KeyCode::Down | KeyCode::Char('j') => {
                if m.at + 1 < m.results.len() {
                    m.at += 1;
                }
                if m.at + 5 >= m.results.len() && !m.next.is_empty() && !m.loading {
                    search(app, true);
                }
            }
            KeyCode::PageUp => m.at = m.at.saturating_sub(10),
            KeyCode::PageDown => {
                m.at = (m.at + 10).min(m.results.len().saturating_sub(1));
                if m.at + 5 >= m.results.len() && !m.next.is_empty() && !m.loading {
                    search(app, true);
                }
            }
            KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => open_detail(app),
            KeyCode::Char(c) if !ctrl && c.is_alphanumeric() => {
                m.focus = Focus::Search;
                m.query.insert(&c.to_string());
                m.typed = Some(Instant::now());
            }
            _ => {}
        },
        Focus::Options => {
            let count = m.detail.as_ref().and_then(|d| d.get("options")).and_then(Value::as_array).map_or(0, Vec::len);
            match key.code {
                KeyCode::Esc | KeyCode::Left | KeyCode::Char('h') => m.focus = Focus::List,
                KeyCode::Up | KeyCode::Char('k') => m.option = m.option.saturating_sub(1),
                KeyCode::Down | KeyCode::Char('j') => m.option = (m.option + 1).min(count.saturating_sub(1)),
                KeyCode::PageUp => m.readme_top = m.readme_top.saturating_sub(10),
                KeyCode::PageDown => m.readme_top += 10,
                KeyCode::Enter => press_download(app),
                _ => {}
            }
        }
    }
}

pub fn wheel(app: &mut App, by: isize) {
    let count = local_models(app).len();
    let Some(m) = me(app) else { return };
    if m.here {
        m.local_at = (m.local_at as isize + by.signum()).clamp(0, count.saturating_sub(1) as isize) as usize;
    } else if m.focus == Focus::Options {
        m.readme_top = (m.readme_top as isize + by).max(0) as usize;
    } else {
        m.at = (m.at as isize + by.signum()).clamp(0, m.results.len().saturating_sub(1) as isize) as usize;
    }
}

/// A search typed a moment ago, now that the typing has stopped; and a
/// finished download, which is new in the folder.
pub fn tick(app: &mut App) {
    let due = me(app).is_some_and(|m| m.typed.is_some_and(|t| t.elapsed() > Duration::from_millis(350)));
    if due {
        search(app, false);
    }
    let finished: Vec<String> = running(app).iter().filter(|d| s(d, "state") == "done").map(|d| s(d, "id")).collect();
    let Some(m) = me(app) else { return };
    let new: Vec<String> = finished.iter().filter(|id| !m.done_seen.contains(id)).cloned().collect();
    if !new.is_empty() {
        m.done_seen.extend(new);
        app.fetch("models");
        let wanted = m_id(app);
        if !wanted.is_empty() {
            let format = me(app).map_or("gguf", |m| m.format);
            app.call("market.model", json!({"id": wanted, "format": format}), move |app, result| {
                if let (Some(m), Ok(detail)) = (me(app), result) {
                    if m.detail_id == wanted {
                        m.detail = Some(detail);
                    }
                }
            });
        }
    }
}

pub fn waiting(app: &App) -> bool {
    matches!(&app.screen, Screen::Models(m) if m.typed.is_some() || m.loading)
}

fn params_label(billions: f64) -> String {
    if billions <= 0.0 {
        String::new()
    } else if billions < 0.9995 {
        format!("{:.0}M", billions * 1000.0)
    } else if billions >= 1000.0 {
        format!("{:.1}T", billions / 1000.0)
    } else if billions >= 10.0 {
        format!("{billions:.0}B")
    } else {
        format!("{billions:.1}B").replace(".0B", "B")
    }
}

fn fit_word(fit_id: &str) -> (&'static str, Style) {
    match fit_id {
        "gpu" => ("Fits on GPU", theme::good()),
        "partial" => ("Partial offload", theme::lit()),
        "cpu" => ("CPU only", theme::lit()),
        "mac" => ("Mac only", theme::bad()),
        _ => ("Too large", theme::bad()),
    }
}

fn caps(row: &Value) -> String {
    let mut out = Vec::new();
    if b(row, "vision") {
        out.push("vision");
    }
    if b(row, "tools") {
        out.push("tools");
    }
    if b(row, "reasoning") {
        out.push("reasoning");
    }
    out.join(", ")
}

fn draw_detail(frame: &mut Frame, app: &mut App, area: Rect) {
    let live = running(app);
    let Screen::Models(m) = &mut app.screen else { return };
    if m.detail_id.is_empty() {
        frame.buffer_mut().set_string(area.x + 1, area.y, "Enter opens a model", theme::faint());
        return;
    }
    let Some(d) = m.detail.clone() else {
        frame.buffer_mut().set_string(area.x + 1, area.y, "Reading...", theme::dim());
        return;
    };
    let cols = area.width as usize;
    let mut y = area.y;
    let put = |frame: &mut Frame, line: Line<'static>, y: &mut u16| {
        if *y < area.bottom() {
            frame.buffer_mut().set_line(area.x, *y, &line, area.width);
            *y += 1;
        }
    };
    put(frame, Line::from(Span::styled(format!(" {}", fit(&s(&d, "name"), cols.saturating_sub(2))), theme::bold(theme::lit()))), &mut y);
    put(frame, Line::from(Span::styled(format!(" {}", s(&d, "author")), theme::dim())), &mut y);
    let context = n(&d, "context");
    let facts: Vec<String> = [
        params_label(n(&d, "params_b")),
        s(&d, "architecture"),
        if context > 0.0 { format!("{:.0}K context", context / 1000.0) } else { String::new() },
        s(&d, "license"),
        caps(&d),
    ]
    .into_iter()
    .filter(|x| !x.is_empty())
    .collect();
    put(frame, Line::from(Span::styled(format!(" {}", fit(&facts.join("   "), cols.saturating_sub(2))), theme::dim())), &mut y);
    if b(&d, "gated") {
        put(frame, Line::from(Span::styled(" Gated. Accept its license on Hugging Face first.", theme::bad())), &mut y);
    }
    y += 1;

    let options = d.get("options").and_then(Value::as_array).cloned().unwrap_or_default();
    if options.is_empty() {
        put(frame, Line::from(Span::styled(" Nothing to download in this format", theme::dim())), &mut y);
    }
    let area_rows = area.bottom().saturating_sub(y) as usize;
    let room = options.len().min((area_rows / 2).max(4));
    let start = (m.option + 1).saturating_sub(room);
    for (i, o) in options.iter().enumerate().skip(start).take(room) {
        let here = i == m.option;
        let (word, style) = fit_word(&s(o, "fit"));
        let state = if b(o, "here") {
            Span::styled("here  ", theme::good())
        } else if live.iter().any(|x| s(x, "repo") == s(o, "repo") && s(x, "option") == s(o, "name") && ["running", "starting"].contains(&s(x, "state").as_str())) {
            Span::styled("going ", theme::lit())
        } else {
            Span::raw("      ")
        };
        let star = if b(o, "recommended") { "★ " } else { "  " };
        let size = bytes(n(o, "bytes"));
        let name_cols = cols.saturating_sub(34).max(6);
        let mut spans = vec![
            Span::styled(if here { "❯" } else { " " }, theme::flame()),
            Span::styled(star, theme::lit()),
            Span::styled(text::pad(&s(o, "name"), name_cols), if here { theme::bold(theme::text()) } else { theme::text() }),
            state,
            Span::styled(text::pad(word, 16), style),
            Span::styled(format!("{size:>9}"), theme::dim()),
        ];
        let used = line_width(&spans);
        if here {
            spans.push(Span::raw(" ".repeat(cols.saturating_sub(used))));
            for span in &mut spans {
                span.style = span.style.bg(p().hover);
            }
        }
        put(frame, Line::from(spans), &mut y);
    }
    if m.focus == Focus::Options {
        let hint = match options.get(m.option) {
            Some(o) if b(o, "here") => " enter give it to an expert",
            Some(o) if live.iter().any(|x| s(x, "repo") == s(o, "repo") && s(x, "option") == s(o, "name") && ["running", "starting"].contains(&s(x, "state").as_str())) => " enter cancel",
            Some(_) => " enter download",
            None => "",
        };
        put(frame, Line::from(Span::styled(hint, theme::lit())), &mut y);
    }
    y += 1;

    // The card.
    if y >= area.bottom() {
        return;
    }
    let Some(readme) = &m.readme else {
        frame.buffer_mut().set_string(area.x + 1, y, "Reading...", theme::dim());
        return;
    };
    let wrap_cols = cols.saturating_sub(2);
    let key = (m.detail_id.clone(), wrap_cols);
    if m.readme_key != key {
        m.readme_rows = markdown::render(readme, wrap_cols);
        m.readme_key = key;
    }
    let height = area.bottom().saturating_sub(y) as usize;
    m.readme_top = m.readme_top.min(m.readme_rows.len().saturating_sub(height));
    for line in m.readme_rows.iter().skip(m.readme_top).take(height) {
        frame.buffer_mut().set_line(area.x + 1, y, line, area.width.saturating_sub(1));
        y += 1;
    }
}

pub fn draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let downloads = running(app);
    let local = local_models(app);
    let note = app.note.clone();
    let dir = app.data.get("models").map(|x| s(x, "display")).unwrap_or_default();
    let Screen::Models(m) = &mut app.screen else { return };

    // The top line.
    let tab = |label: &str, on: bool| {
        if on {
            Span::styled(format!(" {label} "), Style::default().fg(p().ink).bg(p().flame))
        } else {
            Span::styled(format!(" {label} "), theme::dim())
        }
    };
    let mut head = vec![
        Span::styled(" Models  ", theme::bold(theme::flame())),
        tab("Hugging Face", !m.here),
        Span::raw(" "),
        tab("On this machine", m.here),
    ];
    let right = if m.here { dir.clone() } else { format!("{}   {}", m.format.to_uppercase(), SORTS[m.sort].1) };
    let free = (area.width as usize).saturating_sub(line_width(&head) + text::width(&right) + 1);
    head.push(Span::raw(" ".repeat(free)));
    head.push(Span::styled(right, theme::dim()));
    frame.buffer_mut().set_line(area.x, area.y, &Line::from(head), area.width);

    let strip = (downloads.len() as u16).min(4);
    let foot = 2 + strip;
    let mut top = area.y + 1;

    if m.here {
        let body = Rect { x: area.x, y: top + 1, width: area.width, height: area.height.saturating_sub(foot + 2) };
        if local.is_empty() {
            frame.buffer_mut().set_string(body.x + 2, body.y, "Nothing here yet. Tab to search.", theme::dim());
        }
        let height = body.height as usize;
        let start = (m.local_at + 1).saturating_sub(height);
        for (i, file) in local.iter().enumerate().skip(start).take(height) {
            let here = i == m.local_at;
            let cols = body.width as usize;
            let mut spans = vec![
                Span::styled(if here { " ❯ " } else { "   " }, theme::flame()),
                Span::styled(text::pad(&s(file, "name"), cols.saturating_sub(24)), if here { theme::bold(theme::text()) } else { theme::text() }),
                Span::styled(format!("{:>6}", s(file, "format").to_uppercase()), theme::faint()),
                Span::styled(format!("{:>11}", bytes(n(file, "bytes"))), theme::dim()),
            ];
            if here {
                let used = line_width(&spans);
                spans.push(Span::raw(" ".repeat(cols.saturating_sub(used))));
                for span in &mut spans {
                    span.style = span.style.bg(p().hover);
                }
            }
            frame.buffer_mut().set_line(body.x, body.y + (i - start) as u16, &Line::from(spans), body.width);
        }
    } else {
        // The search box.
        let edge = if m.focus == Focus::Search { p().flame } else { p().edge };
        let box_area = Rect { x: area.x, y: top, width: area.width, height: 3 };
        let block = Block::default().borders(Borders::ALL).border_type(BorderType::Rounded).border_style(theme::fg(edge));
        let inner = block.inner(box_area);
        block.render(box_area, frame.buffer_mut());
        frame.buffer_mut().set_string(inner.x + 1, inner.y, "⌕", theme::flame());
        if m.query.text.is_empty() {
            frame.buffer_mut().set_string(inner.x + 3, inner.y, "Search Hugging Face", theme::faint());
        } else {
            frame.buffer_mut().set_string(inner.x + 3, inner.y, fit(&m.query.text, inner.width as usize - 4), theme::text());
        }
        if m.focus == Focus::Search && app.overlay.is_none() {
            let (_, col) = m.query.caret(10_000);
            frame.set_cursor_position((inner.x + 3 + (col as u16).min(inner.width.saturating_sub(4)), inner.y));
        }
        top += 3;

        // The results, and the one opened.
        let height = area.bottom().saturating_sub(top + foot);
        let list_cols = if area.width >= 90 { area.width * 45 / 100 } else { area.width };
        let list_area = Rect { x: area.x, y: top, width: list_cols, height };
        let cols = list_area.width as usize;
        let per = 2usize;
        let rows_fit = (height as usize / per).max(1);
        if m.at < m.top {
            m.top = m.at;
        }
        if m.at >= m.top + rows_fit {
            m.top = m.at + 1 - rows_fit;
        }
        if !m.error.is_empty() {
            frame.buffer_mut().set_string(list_area.x + 2, list_area.y, fit(&m.error, cols.saturating_sub(3)), theme::bad());
        } else if m.results.is_empty() {
            let said = if m.loading { "Searching..." } else { "Nothing found" };
            frame.buffer_mut().set_string(list_area.x + 2, list_area.y, said, theme::dim());
        }
        for (i, row) in m.results.iter().enumerate().skip(m.top).take(rows_fit) {
            let y = list_area.y + ((i - m.top) * per) as u16;
            let here = i == m.at;
            let active = here && m.focus != Focus::Search;
            let name = fit(&s(row, "name"), cols.saturating_sub(4));
            let mut first = vec![
                Span::styled(if active { " ❯ " } else { "   " }, theme::flame()),
                Span::styled(name, if active { theme::bold(theme::text()) } else { theme::text() }),
            ];
            let capabilities = caps(row);
            if !capabilities.is_empty() && line_width(&first) + capabilities.len() + 2 < cols {
                first.push(Span::styled(format!("  {capabilities}"), theme::faint()));
            }
            let meta = [
                s(row, "author"),
                params_label(n(row, "params_b")),
                format!("↓{}", compact(n(row, "downloads"))),
                format!("♥{}", compact(n(row, "likes"))),
            ]
            .into_iter()
            .filter(|x| !x.is_empty())
            .collect::<Vec<_>>()
            .join("   ");
            let mut second = vec![Span::raw("   "), Span::styled(fit(&meta, cols.saturating_sub(4)), theme::dim())];
            if active {
                for line in [&mut first, &mut second] {
                    let used = line_width(line);
                    line.push(Span::raw(" ".repeat(cols.saturating_sub(used))));
                    for span in line.iter_mut() {
                        span.style = span.style.bg(p().raised);
                    }
                }
            }
            frame.buffer_mut().set_line(list_area.x, y, &Line::from(first), list_area.width);
            frame.buffer_mut().set_line(list_area.x, y + 1, &Line::from(second), list_area.width);
        }
        if area.width >= 90 {
            let rule_x = list_area.right();
            for y in top..top + height {
                frame.buffer_mut().set_string(rule_x, y, "│", theme::fg(p().edge));
            }
            let detail = Rect { x: rule_x + 1, y: top, width: area.right().saturating_sub(rule_x + 1), height };
            draw_detail(frame, app, detail);
        } else if m.focus == Focus::Options {
            frame.render_widget(ratatui::widgets::Clear, list_area);
            draw_detail(frame, app, list_area);
        }
    }

    // The downloads, then the keys.
    let mut y = area.bottom().saturating_sub(1 + strip);
    for d in downloads.iter().take(strip as usize) {
        let state = s(d, "state");
        let total = n(d, "total");
        let done = n(d, "done");
        let rate = n(d, "rate");
        let share = if total > 0.0 { (done / total).min(1.0) } else { 0.0 };
        let name = format!("{} {}", s(d, "repo").rsplit('/').next().unwrap_or(""), s(d, "option"));
        let words = match state.as_str() {
            "starting" => "Starting".to_string(),
            "running" => {
                let left = if rate > 0.0 && total > done { format!("  {}", clock(((total - done) / rate) as u64)) } else { String::new() };
                format!("{} of {}  {}/s{left}", bytes(done), bytes(total), bytes(rate))
            }
            "done" => "Downloaded".to_string(),
            "canceled" => "Canceled".to_string(),
            _ => s(d, "error"),
        };
        let cells = 16usize;
        let filled = (share * cells as f64).round() as usize;
        let mut spans = vec![Span::styled(format!(" {} ", text::pad(&name, 34)), theme::text())];
        if state == "running" || state == "starting" {
            spans.push(Span::styled("█".repeat(filled), theme::flame()));
            spans.push(Span::styled("░".repeat(cells - filled), theme::faint()));
            spans.push(Span::styled(format!(" {:>3.0}%  ", share * 100.0), theme::lit()));
        }
        let style = if state == "failed" { theme::bad() } else if state == "done" { theme::good() } else { theme::dim() };
        spans.push(Span::styled(words, style));
        frame.buffer_mut().set_line(area.x, y, &Line::from(spans), area.width);
        y += 1;
    }
    let foot_y = area.bottom().saturating_sub(1);
    let keys = if let Some((said, bad, _)) = &note {
        Span::styled(format!(" {said}"), if *bad { theme::bad() } else { theme::lit() })
    } else {
        Span::styled(" enter open, download   tab switch   ctrl+f format   ctrl+s sort   ctrl+x cancel   esc back", theme::faint())
    };
    frame.buffer_mut().set_line(area.x, foot_y, &Line::from(keys), area.width);
}
