// SPDX-License-Identifier: MIT
//! The app's other tabs, for a terminal: Files, Git, Preview, Bench, Create.

use crate::app::{App, External, Screen};
use crate::form::{Did, Form, Kind, Row};
use crate::highlight;
use crate::models::{self, Models};
use crate::overlay::Item;
use crate::settings;
use crate::state::{b, n, s};
use crate::text::{self, bytes, clean, clock, fit, line_width, pad, wrap};
use crate::theme::{self, p};
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::text::{Line, Span};
use ratatui::Frame;
use serde_json::{json, Value};
use std::collections::HashSet;
use std::path::PathBuf;
use std::process::{Command, Stdio};

pub enum Page {
    Files(Files),
    Git(Git),
    Preview(Preview),
    Bench(Bench),
    Create(Create),
}

impl Page {
    pub fn files() -> Page {
        Page::Files(Files::default())
    }
    pub fn git() -> Page {
        Page::Git(Git::default())
    }
    pub fn preview() -> Page {
        Page::Preview(Preview::default())
    }
    pub fn bench() -> Page {
        Page::Bench(Bench::new())
    }
    pub fn create() -> Page {
        Page::Create(Create::default())
    }
}

macro_rules! page {
    ($app:expr, $kind:ident) => {
        match &mut $app.screen {
            Screen::Page(Page::$kind(x)) => x,
            _ => return,
        }
    };
}

pub fn open(app: &mut App, page: Page) {
    app.screen = Screen::Page(page);
    match &app.screen {
        Screen::Page(Page::Files(_)) => load_tree(app),
        Screen::Page(Page::Git(_)) => load_git(app),
        Screen::Page(Page::Preview(_)) => load_preview(app),
        Screen::Page(Page::Bench(_)) => load_bench(app, false),
        Screen::Page(Page::Create(_)) => load_create(app),
        _ => {}
    }
}

pub fn key(app: &mut App, key: KeyEvent) {
    match &app.screen {
        Screen::Page(Page::Files(_)) => files_key(app, key),
        Screen::Page(Page::Git(_)) => git_key(app, key),
        Screen::Page(Page::Preview(_)) => preview_key(app, key),
        Screen::Page(Page::Bench(_)) => bench_key(app, key),
        Screen::Page(Page::Create(_)) => create_key(app, key),
        _ => {}
    }
}

pub fn draw(frame: &mut Frame, app: &mut App, area: Rect) {
    match &app.screen {
        Screen::Page(Page::Files(_)) => files_draw(frame, app, area),
        Screen::Page(Page::Git(_)) => git_draw(frame, app, area),
        Screen::Page(Page::Preview(_)) => preview_draw(frame, app, area),
        Screen::Page(Page::Bench(_)) => bench_draw(frame, app, area),
        Screen::Page(Page::Create(_)) => create_draw(frame, app, area),
        _ => {}
    }
}

pub fn wheel(app: &mut App, by: isize) {
    if matches!(app.screen, Screen::Models(_)) {
        return models::wheel(app, by);
    }
    let step = |v: &mut usize| *v = (*v as isize + by).max(0) as usize;
    match &mut app.screen {
        Screen::Page(Page::Files(f)) => step(&mut f.view_top),
        Screen::Page(Page::Git(g)) => step(&mut g.diff_top),
        Screen::Page(Page::Bench(x)) => step(&mut x.form.at),
        Screen::Page(Page::Create(x)) => step(&mut x.at),
        Screen::Settings(x) => step(&mut x.form.at),
        _ => {}
    }
}

pub fn on_push(app: &mut App) {
    settings::on_push(app);
    models::tick(app);
    if matches!(app.screen, Screen::Page(Page::Create(_))) {
        poll_run(app);
    }
    if let Screen::Page(Page::Bench(x)) = &mut app.screen {
        let running = app.state.bench.as_ref().is_some_and(|l| b(l, "running"));
        let ended = x.was_running && !running;
        x.was_running = running;
        if ended {
            load_bench(app, false);
        }
    }
}

pub fn after_editor(app: &mut App) {
    if matches!(app.screen, Screen::Page(Page::Files(_))) {
        load_tree(app);
        let file = page!(app, Files).file.clone();
        if !file.is_empty() {
            read_file(app, file);
        }
    }
}

fn project_root(app: &App) -> PathBuf {
    let root = app.state.raw.get("project").map(|x| s(x, "root")).unwrap_or_default();
    if root.is_empty() {
        app.folder.clone()
    } else {
        PathBuf::from(root)
    }
}

/// In the machine's own program for it: a browser, for a page.
fn open_outside(target: &str) -> Result<(), String> {
    let spawned = if cfg!(target_os = "macos") {
        Command::new("open").arg(target).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn()
    } else if cfg!(windows) {
        Command::new("cmd").args(["/C", "start", "", target]).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn()
    } else {
        Command::new("xdg-open").arg(target).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn()
    };
    match spawned {
        Ok(mut child) => {
            std::thread::spawn(move || {
                let _ = child.wait();
            });
            Ok(())
        }
        Err(error) => Err(format!("could not open it: {error}")),
    }
}

fn chrome(frame: &mut Frame, area: Rect, title: &str, right: &str, keys: &str, note: &Option<(String, bool, std::time::Instant)>) -> Rect {
    let mut head = vec![Span::styled(format!(" {title}"), theme::bold(theme::flame()))];
    let room = (area.width as usize).saturating_sub(line_width(&head) + 4);
    let right = text::fit_left(right, room);
    let free = (area.width as usize).saturating_sub(line_width(&head) + text::width(&right) + 1);
    head.push(Span::raw(" ".repeat(free)));
    head.push(Span::styled(right, theme::dim()));
    frame.buffer_mut().set_line(area.x, area.y, &Line::from(head), area.width);
    frame.buffer_mut().set_string(area.x, area.y + 1, "─".repeat(area.width as usize), theme::fg(p().edge));
    let foot = area.bottom().saturating_sub(1);
    let line = match note {
        Some((said, bad, _)) => Span::styled(format!(" {}", fit(said, area.width as usize - 2)), if *bad { theme::bad() } else { theme::lit() }),
        None => Span::styled(fit(keys, area.width as usize), theme::faint()),
    };
    frame.buffer_mut().set_line(area.x, foot, &Line::from(line), area.width);
    Rect { x: area.x, y: area.y + 2, width: area.width, height: area.height.saturating_sub(3) }
}

fn marked(mut spans: Vec<Span<'static>>, cols: usize, here: bool) -> Line<'static> {
    if here {
        let used = line_width(&spans);
        spans.push(Span::raw(" ".repeat(cols.saturating_sub(used))));
        for span in &mut spans {
            span.style = span.style.bg(p().hover);
        }
    }
    Line::from(spans)
}

fn keep_in_view(at: usize, top: &mut usize, height: usize) {
    if at < *top {
        *top = at;
    }
    if height > 0 && at >= *top + height {
        *top = at + 1 - height;
    }
}

/// Code with its line numbers, wrapped to fit.
fn code_rows(content: &str, lang: &str, cols: usize) -> Vec<Line<'static>> {
    let lines = highlight::code(content, lang);
    let digits = lines.len().to_string().len();
    let room = cols.max(digits + 8);
    let mut out = Vec::with_capacity(lines.len());
    for (i, row) in lines.iter().enumerate() {
        if i + 1 == lines.len() && row.is_empty() {
            break;
        }
        let first = [Span::styled(format!("{:>digits$}  ", i + 1), theme::faint())];
        let rest = [Span::raw(" ".repeat(digits + 2))];
        out.extend(wrap(row, room, &first, &rest));
    }
    out
}

fn diff_rows(diff: &str, cols: usize) -> Vec<Line<'static>> {
    let mut out = Vec::new();
    for line in clean(diff).lines() {
        let (style, bg) = if line.starts_with("+++") || line.starts_with("---") || line.starts_with("diff ") || line.starts_with("index ") {
            (theme::dim(), None)
        } else if line.starts_with("@@") {
            (theme::lit(), None)
        } else if line.starts_with('+') {
            (theme::fg(p().add), Some(p().add_bg))
        } else if line.starts_with('-') {
            (theme::fg(p().del), Some(p().del_bg))
        } else {
            (theme::text(), None)
        };
        for row in wrap(&[Span::styled(line.to_string(), style)], cols, &[], &[]) {
            out.push(match bg {
                Some(color) => text::band(row.spans, cols, color),
                None => row,
            });
        }
    }
    out
}

fn read_rows(content: &str, cols: usize) -> Vec<Line<'static>> {
    let mut out = Vec::new();
    for line in clean(content).lines() {
        out.extend(wrap(&[Span::styled(line.to_string(), theme::text())], cols, &[Span::raw(" ")], &[Span::raw(" ")]));
    }
    if out.is_empty() {
        out.push(Line::from(Span::styled(" Nothing", theme::faint())));
    }
    out
}

fn list_of(v: &Value, key: &str) -> Vec<Value> {
    v.get(key).and_then(Value::as_array).cloned().unwrap_or_default()
}

// --- Files --------------------------------------------------------------------------

struct Entry {
    path: String,
    name: String,
    dir: bool,
    bytes: f64,
}

#[derive(Default)]
pub struct Files {
    entries: Vec<Entry>,
    open: HashSet<String>,
    at: usize,
    top: usize,
    file: String,
    content: String,
    built: usize,
    rows: Vec<Line<'static>>,
    pub view_top: usize,
    in_view: bool,
    hits: Option<Vec<Value>>,
    hit_at: usize,
    hit_top: usize,
    loaded: bool,
}

impl Files {
    fn visible(&self) -> Vec<usize> {
        self.entries
            .iter()
            .enumerate()
            .filter(|(_, e)| {
                let mut up = e.path.as_str();
                while let Some((parent, _)) = up.rsplit_once('/') {
                    if !self.open.contains(parent) {
                        return false;
                    }
                    up = parent;
                }
                true
            })
            .map(|(i, _)| i)
            .collect()
    }
}

fn load_tree(app: &mut App) {
    app.call("project.tree", json!({"depth": 8, "limit": 6000}), |app, result| {
        let f = page!(app, Files);
        f.loaded = true;
        let reply = match result {
            Ok(reply) => reply,
            Err(error) => return app.fail(&error),
        };
        let mut entries: Vec<Entry> = list_of(&reply, "entries")
            .iter()
            .map(|e| Entry { path: s(e, "path"), name: s(e, "name"), dir: b(e, "dir"), bytes: n(e, "bytes") })
            .collect();
        let dirs: HashSet<String> = entries.iter().filter(|e| e.dir).map(|e| e.path.clone()).collect();
        // Folders first at every level, then by name without regard to case.
        entries.sort_by_cached_key(|e| {
            let parts: Vec<&str> = e.path.split('/').collect();
            let mut key = Vec::with_capacity(parts.len());
            for (i, part) in parts.iter().enumerate() {
                let so_far = parts[..=i].join("/");
                key.push((!dirs.contains(&so_far), part.to_lowercase()));
            }
            key
        });
        f.entries = entries;
    });
}

fn read_file(app: &mut App, path: String) {
    let asked = path.clone();
    app.call("project.read", json!({"path": path}), move |app, result| {
        let f = page!(app, Files);
        if f.file != asked {
            f.view_top = 0;
        }
        f.file = asked;
        f.built = 0;
        match result {
            Ok(reply) if b(&reply, "image") => f.content = format!("A picture, {}", bytes(n(&reply, "bytes"))),
            Ok(reply) => {
                f.content = clean(&s(&reply, "content"));
                if b(&reply, "cut") {
                    f.content.push_str("\n... (cut, it is long)");
                }
            }
            Err(error) => f.content = error,
        }
    });
}

fn files_key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let root = project_root(app);
    let f = page!(app, Files);

    // Results of a search.
    if let Some(hits) = &f.hits {
        match key.code {
            KeyCode::Esc => f.hits = None,
            KeyCode::Up | KeyCode::Char('k') => f.hit_at = f.hit_at.saturating_sub(1),
            KeyCode::Down | KeyCode::Char('j') => f.hit_at = (f.hit_at + 1).min(hits.len().saturating_sub(1)),
            KeyCode::Enter => {
                if let Some(hit) = hits.get(f.hit_at).cloned() {
                    let path = s(&hit, "path");
                    let line = n(&hit, "line") as usize;
                    f.hits = None;
                    f.in_view = true;
                    f.view_top = line.saturating_sub(4);
                    f.file = path.clone();
                    read_file(app, path);
                }
            }
            _ => {}
        }
        return;
    }

    if f.in_view {
        match key.code {
            KeyCode::Esc | KeyCode::Tab | KeyCode::Left | KeyCode::Char('h') => f.in_view = false,
            KeyCode::Up | KeyCode::Char('k') => f.view_top = f.view_top.saturating_sub(1),
            KeyCode::Down | KeyCode::Char('j') => f.view_top += 1,
            KeyCode::PageUp => f.view_top = f.view_top.saturating_sub(20),
            KeyCode::PageDown | KeyCode::Char(' ') => f.view_top += 20,
            KeyCode::Home | KeyCode::Char('g') => f.view_top = 0,
            KeyCode::End | KeyCode::Char('G') => f.view_top = usize::MAX / 2,
            KeyCode::Char('e') if !f.file.is_empty() => {
                let path = root.join(&f.file);
                app.external = Some(External::Edit(path));
            }
            _ => {}
        }
        return;
    }

    let shown = f.visible();
    let current = shown.get(f.at).map(|i| (f.entries[*i].path.clone(), f.entries[*i].dir));
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => app.back_to_chat(),
        KeyCode::Up | KeyCode::Char('k') => f.at = f.at.saturating_sub(1),
        KeyCode::Down | KeyCode::Char('j') => f.at = (f.at + 1).min(shown.len().saturating_sub(1)),
        KeyCode::PageUp => f.at = f.at.saturating_sub(16),
        KeyCode::PageDown => f.at = (f.at + 16).min(shown.len().saturating_sub(1)),
        KeyCode::Home | KeyCode::Char('g') => f.at = 0,
        KeyCode::End | KeyCode::Char('G') => f.at = shown.len().saturating_sub(1),
        KeyCode::Tab => f.in_view = !f.file.is_empty(),
        KeyCode::Left | KeyCode::Char('h') => {
            if let Some((path, dir)) = current {
                if dir && f.open.remove(&path) {
                    return;
                }
                if let Some((parent, _)) = path.rsplit_once('/') {
                    let parent = parent.to_string();
                    if let Some(place) = shown.iter().position(|i| f.entries[*i].path == parent) {
                        f.at = place;
                    }
                }
            }
        }
        KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => {
            if let Some((path, dir)) = current {
                if dir {
                    if !f.open.remove(&path) {
                        f.open.insert(path);
                    }
                } else if f.file == path {
                    f.in_view = true;
                } else {
                    f.file = path.clone();
                    f.view_top = 0;
                    read_file(app, path);
                }
            }
        }
        KeyCode::Char('e') => {
            if let Some((path, false)) = current {
                f.file = path.clone();
                let full = root.join(&path);
                read_file(app, path);
                app.external = Some(External::Edit(full));
            }
        }
        KeyCode::Char('/') => {
            app.type_in("Search the project", "", "", false, false, |app, query| {
                if query.trim().is_empty() {
                    return;
                }
                app.say("Searching");
                app.call("project.search", json!({"query": query, "case": false}), |app, result| {
                    app.note = None;
                    let reply = match result {
                        Ok(reply) => reply,
                        Err(error) => return app.fail(&error),
                    };
                    let hits = list_of(&reply, "results");
                    if hits.is_empty() {
                        return app.say("Nothing found");
                    }
                    let f = page!(app, Files);
                    f.hit_at = 0;
                    f.hit_top = 0;
                    f.hits = Some(hits);
                });
            });
        }
        KeyCode::Char('n') | KeyCode::Char('N') => {
            let folder = key.code == KeyCode::Char('N');
            let base = match &current {
                Some((path, true)) => format!("{path}/"),
                Some((path, false)) => path.rsplit_once('/').map_or(String::new(), |(d, _)| format!("{d}/")),
                None => String::new(),
            };
            app.type_in(if folder { "New folder" } else { "New file" }, "", &base, false, false, move |app, path| {
                let path = path.trim().to_string();
                if path.is_empty() {
                    return;
                }
                app.call("project.create", json!({"path": path, "dir": folder}), |app, result| {
                    if let Err(error) = result {
                        app.fail(&error);
                    }
                    load_tree(app);
                });
            });
        }
        KeyCode::Char('r') => {
            if let Some((path, _)) = current {
                let from = path.clone();
                app.type_in("Rename", "", &path, false, false, move |app, to| {
                    let to = to.trim().to_string();
                    if to.is_empty() || to == from {
                        return;
                    }
                    app.call("project.rename", json!({"path": from, "to": to}), |app, result| {
                        if let Err(error) = result {
                            app.fail(&error);
                        }
                        load_tree(app);
                    });
                });
            }
        }
        KeyCode::Char('d') => {
            if let Some((path, _)) = current {
                let title = format!("Delete {path}?");
                app.ask(&title, &["It is deleted from the disk."], "Delete", "Keep", move |app, yes| {
                    if !yes {
                        return;
                    }
                    app.call("project.delete", json!({"path": path}), |app, result| {
                        if let Err(error) = result {
                            app.fail(&error);
                        }
                        load_tree(app);
                    });
                });
            }
        }
        KeyCode::Char('R') => load_tree(app),
        _ => {}
    }
}

fn files_draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let note = app.note.clone();
    let root = app.state.project_display();
    let f = page!(app, Files);
    let keys = if f.hits.is_some() {
        " enter open   esc back"
    } else if f.in_view {
        " arrows scroll   e edit   esc tree"
    } else {
        " enter open   e edit   n new   N folder   r rename   d delete   / search   tab file   esc back"
    };
    let title = if f.file.is_empty() { "Files".to_string() } else { format!("Files  {}", f.file) };
    let body = chrome(frame, area, &title, &root, keys, &note);

    let tree_cols = (body.width / 3).clamp(20, 44);
    let height = body.height as usize;
    if let Some(hits) = &f.hits {
        keep_in_view(f.hit_at, &mut f.hit_top, height);
        let cols = body.width as usize;
        for (y, (i, hit)) in hits.iter().enumerate().skip(f.hit_top).take(height).enumerate() {
            let place = format!("{}:{}", s(hit, "path"), n(hit, "line") as u64);
            let spans = vec![
                Span::styled(if i == f.hit_at { " ❯ " } else { "   " }, theme::flame()),
                Span::styled(pad(&place, 36), theme::lit()),
                Span::styled(fit(clean(&s(hit, "text")).trim(), cols.saturating_sub(41)), theme::text()),
            ];
            frame.buffer_mut().set_line(body.x, body.y + y as u16, &marked(spans, cols, i == f.hit_at), body.width);
        }
        return;
    }

    let shown = f.visible();
    f.at = f.at.min(shown.len().saturating_sub(1));
    keep_in_view(f.at, &mut f.top, height);
    if shown.is_empty() {
        let said = if f.loaded { "  Nothing here" } else { "  Reading..." };
        frame.buffer_mut().set_string(body.x, body.y, said, theme::dim());
    }
    for (y, index) in shown.iter().skip(f.top).take(height).enumerate() {
        let e = &f.entries[*index];
        let depth = e.path.matches('/').count();
        let here = f.top + y == f.at;
        let icon = if e.dir { if f.open.contains(&e.path) { "▾ " } else { "▸ " } } else { "  " };
        let style = if e.dir { theme::lit() } else if e.path == f.file { theme::flame() } else { theme::text() };
        let room = (tree_cols as usize).saturating_sub(depth * 2 + 3);
        let spans = vec![
            Span::raw(format!(" {}", "  ".repeat(depth))),
            Span::styled(icon, theme::dim()),
            Span::styled(fit(&e.name, room), style),
        ];
        let line = marked(spans, tree_cols as usize, here && !f.in_view);
        frame.buffer_mut().set_line(body.x, body.y + y as u16, &line, tree_cols);
        let _ = e.bytes;
    }
    let rule_x = body.x + tree_cols;
    for y in body.y..body.bottom() {
        frame.buffer_mut().set_string(rule_x, y, "│", theme::fg(if f.in_view { p().flame } else { p().edge }));
    }
    let view = Rect { x: rule_x + 2, y: body.y, width: body.right().saturating_sub(rule_x + 2), height: body.height };
    if f.file.is_empty() {
        frame.buffer_mut().set_string(view.x, view.y, "Choose a file", theme::faint());
        return;
    }
    if f.built != view.width as usize {
        f.rows = code_rows(&f.content, &highlight::language_of(&f.file), view.width as usize);
        f.built = view.width as usize;
    }
    f.view_top = f.view_top.min(f.rows.len().saturating_sub(height));
    for (y, line) in f.rows.iter().skip(f.view_top).take(height).enumerate() {
        frame.buffer_mut().set_line(view.x, view.y + y as u16, line, view.width);
    }
}

// --- Git ----------------------------------------------------------------------------

#[derive(Default)]
pub struct Git {
    status: Option<Value>,
    at: usize,
    top: usize,
    diff: String,
    diff_path: String,
    built: usize,
    rows: Vec<Line<'static>>,
    pub diff_top: usize,
    in_diff: bool,
    working: String,
}

fn load_git(app: &mut App) {
    app.call("git.status", json!({}), |app, result| {
        let g = page!(app, Git);
        match result {
            Ok(reply) => {
                g.at = g.at.min(list_of(&reply, "changes").len().saturating_sub(1));
                g.status = Some(reply);
            }
            Err(error) => app.fail(&error),
        }
    });
}

/// A git action: said while it runs, and the status again after.
fn git_do(app: &mut App, what: &str, method: &str, params: Value) {
    page!(app, Git).working = what.to_string();
    app.call(method, params, |app, result| {
        page!(app, Git).working.clear();
        match result {
            Ok(reply) => {
                for key in ["summary", "url"] {
                    let said = s(&reply, key);
                    if !said.is_empty() {
                        app.say(&said);
                        break;
                    }
                }
                let output = s(&reply, "output");
                if !output.is_empty() {
                    let cols = app.feed_area.width.saturating_sub(6).max(40) as usize;
                    app.read("git", read_rows(&output, cols));
                }
            }
            Err(error) => app.fail(&error),
        }
        load_git(app);
    });
}

fn git_key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let g = page!(app, Git);
    let changes = g.status.as_ref().map(|x| list_of(x, "changes")).unwrap_or_default();
    let is_repo = g.status.as_ref().is_some_and(|x| b(x, "repo"));
    if g.in_diff {
        match key.code {
            KeyCode::Esc | KeyCode::Tab | KeyCode::Left | KeyCode::Char('h') => g.in_diff = false,
            KeyCode::Up | KeyCode::Char('k') => g.diff_top = g.diff_top.saturating_sub(1),
            KeyCode::Down | KeyCode::Char('j') => g.diff_top += 1,
            KeyCode::PageUp => g.diff_top = g.diff_top.saturating_sub(20),
            KeyCode::PageDown | KeyCode::Char(' ') => g.diff_top += 20,
            _ => {}
        }
        return;
    }
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => app.back_to_chat(),
        KeyCode::Up | KeyCode::Char('k') => g.at = g.at.saturating_sub(1),
        KeyCode::Down | KeyCode::Char('j') => g.at = (g.at + 1).min(changes.len().saturating_sub(1)),
        KeyCode::Tab => g.in_diff = !g.diff_path.is_empty(),
        KeyCode::Enter | KeyCode::Right | KeyCode::Char('l') => {
            if let Some(change) = changes.get(g.at) {
                let path = s(change, "path");
                if g.diff_path == path {
                    g.in_diff = true;
                    return;
                }
                let asked = path.clone();
                app.call("git.diff", json!({"path": path}), move |app, result| {
                    let g = page!(app, Git);
                    g.diff_path = asked;
                    g.diff_top = 0;
                    g.built = 0;
                    g.diff = match result {
                        Ok(reply) => {
                            let text = s(&reply, "diff");
                            if text.trim().is_empty() { "No changes to show. A new file has no diff yet.".to_string() } else { text }
                        }
                        Err(error) => error,
                    };
                });
            }
        }
        KeyCode::Char('r') => load_git(app),
        KeyCode::Char('i') if !is_repo => git_do(app, "Starting a repository", "git.init", json!({})),
        KeyCode::Char('c') if is_repo => app.type_in("Commit everything", "The message", "", false, false, |app, message| {
            if !message.trim().is_empty() {
                git_do(app, "Committing", "git.commit", json!({"message": message}));
            }
        }),
        KeyCode::Char('p') if is_repo => app.ask("Push?", &["Sends your commits to the remote."], "Push", "Not now", |app, yes| {
            if yes {
                git_do(app, "Pushing", "git.push", json!({}));
            }
        }),
        KeyCode::Char('u') if is_repo => git_do(app, "Pulling", "git.pull", json!({})),
        KeyCode::Char('L') if is_repo => {
            app.call("git.log", json!({"limit": 60}), |app, result| {
                let reply = match result {
                    Ok(reply) => reply,
                    Err(error) => return app.fail(&error),
                };
                let rows = list_of(&reply, "commits")
                    .iter()
                    .map(|c| {
                        Line::from(vec![
                            Span::styled(format!(" {} ", s(c, "hash")), theme::lit()),
                            Span::styled(clean(&s(c, "subject")), theme::text()),
                            Span::styled(format!("  {}, {}", clean(&s(c, "author")), s(c, "when")), theme::faint()),
                        ])
                    })
                    .collect();
                app.read("Log", rows);
            });
        }
        KeyCode::Char('C') if is_repo => {
            app.call("git.ci", json!({}), |app, result| {
                let reply = match result {
                    Ok(reply) => reply,
                    Err(error) => return app.fail(&error),
                };
                if !b(&reply, "available") {
                    return app.say(&s(&reply, "why"));
                }
                let rows = list_of(&reply, "runs")
                    .iter()
                    .map(|run| {
                        let done = s(run, "conclusion");
                        let (word, style) = match done.as_str() {
                            "success" => ("passed".to_string(), theme::good()),
                            "" => (s(run, "status"), theme::lit()),
                            other => (other.to_string(), theme::bad()),
                        };
                        Line::from(vec![
                            Span::styled(format!(" {} ", pad(&word, 12)), style),
                            Span::styled(clean(&s(run, "displayTitle")), theme::text()),
                            Span::styled(format!("  {}", s(run, "workflowName")), theme::faint()),
                        ])
                    })
                    .collect();
                app.read("CI", rows);
            });
        }
        KeyCode::Char('P') if is_repo => {
            let name = app.state.raw.get("project").map(|x| s(x, "name")).unwrap_or_default();
            app.type_in("Publish to GitHub (private)", "The repository's name", &name, false, false, |app, name| {
                if !name.trim().is_empty() {
                    git_do(app, "Publishing", "git.publish", json!({"name": name.trim(), "private": true}));
                }
            });
        }
        KeyCode::Char('t') if is_repo => app.type_in("Release: tag", "v1.0.0", "", false, false, |app, tag| {
            if tag.trim().is_empty() {
                return;
            }
            app.type_in("Release: notes", "", "", false, true, move |app, notes| {
                git_do(app, "Releasing", "git.release", json!({"tag": tag.trim(), "notes": notes}));
            });
        }),
        KeyCode::Char(':') if is_repo => app.type_in("git", "Anything after the word git", "", false, false, |app, args| {
            if !args.trim().is_empty() {
                git_do(app, "Running", "git.run", json!({"args": args}));
            }
        }),
        _ => {}
    }
}

fn git_draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let note = app.note.clone();
    let g = page!(app, Git);
    let status = g.status.clone().unwrap_or(Value::Null);
    let repo = b(&status, "repo");
    let right = if !repo {
        String::new()
    } else {
        let mut bits = vec![s(&status, "branch")];
        let ahead = n(&status, "ahead") as u64;
        let behind = n(&status, "behind") as u64;
        if ahead > 0 {
            bits.push(format!("↑{ahead}"));
        }
        if behind > 0 {
            bits.push(format!("↓{behind}"));
        }
        let remote = s(&status, "remote");
        if !remote.is_empty() {
            bits.push(remote);
        }
        bits.join("  ")
    };
    let keys = if g.in_diff {
        " arrows scroll   esc back"
    } else if repo {
        " enter diff   c commit   p push   u pull   L log   C ci   P publish   t release   : git   esc back"
    } else {
        " i start a repository   esc back"
    };
    let body = chrome(frame, area, "Git", &right, keys, &note);
    if g.status.is_none() {
        frame.buffer_mut().set_string(body.x + 1, body.y, "Reading...", theme::dim());
        return;
    }
    if !b(&status, "git") {
        frame.buffer_mut().set_string(body.x + 1, body.y, "git is not installed", theme::bad());
        return;
    }
    if !repo {
        frame.buffer_mut().set_string(body.x + 1, body.y, "Not a repository", theme::dim());
        return;
    }
    let mut body = body;
    if !g.working.is_empty() {
        frame.buffer_mut().set_string(body.x + 1, body.y, format!("{}...", g.working), theme::lit());
        body.y += 1;
        body.height = body.height.saturating_sub(1);
    }
    let changes = list_of(&status, "changes");
    let list_cols = (body.width * 2 / 5).clamp(24, 56);
    let height = body.height as usize;
    keep_in_view(g.at, &mut g.top, height);
    if changes.is_empty() {
        frame.buffer_mut().set_string(body.x + 1, body.y, "Nothing changed", theme::good());
    }
    for (y, (i, change)) in changes.iter().enumerate().skip(g.top).take(height).enumerate() {
        let here = i == g.at;
        let code = s(change, "status");
        let style = match code.trim().chars().next() {
            Some('A') | Some('?') => theme::fg(p().add),
            Some('D') => theme::fg(p().del),
            _ => theme::lit(),
        };
        let spans = vec![
            Span::styled(if here { " ❯ " } else { "   " }, theme::flame()),
            Span::styled(pad(&code, 3), style),
            Span::styled(text::fit_left(&s(change, "path"), (list_cols as usize).saturating_sub(7)), theme::text()),
        ];
        let line = marked(spans, list_cols as usize, here && !g.in_diff);
        frame.buffer_mut().set_line(body.x, body.y + y as u16, &line, list_cols);
    }
    let rule_x = body.x + list_cols;
    for y in body.y..body.bottom() {
        frame.buffer_mut().set_string(rule_x, y, "│", theme::fg(if g.in_diff { p().flame } else { p().edge }));
    }
    let view = Rect { x: rule_x + 2, y: body.y, width: body.right().saturating_sub(rule_x + 2), height: body.height };
    if g.diff_path.is_empty() {
        frame.buffer_mut().set_string(view.x, view.y, "Enter shows a file's diff", theme::faint());
        return;
    }
    if g.built != view.width as usize {
        g.rows = diff_rows(&g.diff, view.width as usize);
        g.built = view.width as usize;
    }
    g.diff_top = g.diff_top.min(g.rows.len().saturating_sub(height));
    for (y, line) in g.rows.iter().skip(g.diff_top).take(height).enumerate() {
        frame.buffer_mut().set_line(view.x, view.y + y as u16, line, view.width);
    }
}

// --- Preview ------------------------------------------------------------------------

#[derive(Default)]
pub struct Preview {
    pages: Option<Vec<String>>,
    at: usize,
}

fn load_preview(app: &mut App) {
    app.call("preview.candidates", json!({}), |app, result| {
        let x = page!(app, Preview);
        match result {
            Ok(reply) => {
                x.pages = Some(list_of(&reply, "pages").iter().filter_map(|v| v.as_str().map(str::to_string)).collect());
            }
            Err(error) => {
                x.pages = Some(Vec::new());
                app.fail(&error);
            }
        }
    });
}

fn preview_key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let root = project_root(app);
    let x = page!(app, Preview);
    let pages = x.pages.clone().unwrap_or_default();
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => app.back_to_chat(),
        KeyCode::Up | KeyCode::Char('k') => x.at = x.at.saturating_sub(1),
        KeyCode::Down | KeyCode::Char('j') => x.at = (x.at + 1).min(pages.len().saturating_sub(1)),
        KeyCode::Char('r') => load_preview(app),
        KeyCode::Enter => {
            if let Some(page_name) = pages.get(x.at) {
                let full = root.join(page_name);
                match open_outside(&full.to_string_lossy()) {
                    Ok(()) => app.say("Opened in your browser"),
                    Err(error) => app.fail(&error),
                }
            }
        }
        KeyCode::Char('a') => {
            let Some(page_name) = pages.get(x.at).cloned() else { return };
            app.call("app.name", json!({"page": page_name}), move |app, result| {
                let suggested = result.ok().map(|r| s(&r, "name")).unwrap_or_default();
                app.type_in("Make it an app: name", "", &suggested, false, false, move |app, name| {
                    if name.trim().is_empty() {
                        return;
                    }
                    app.say("Packaging");
                    app.call("app.package", json!({"page": page_name, "name": name.trim()}), |app, result| {
                        let made = match result {
                            Ok(made) => made,
                            Err(error) => return app.fail(&error),
                        };
                        let body = format!("{} is in {}.", s(&made, "name"), s(&made, "where"));
                        let launch = s(&made, "launch");
                        app.ask("Made", &[&body], "Open it", "Done", move |app, yes| {
                            if yes {
                                app.act("app.open", json!({"launch": launch}));
                            }
                        });
                    });
                });
            });
        }
        _ => {}
    }
}

fn preview_draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let note = app.note.clone();
    let root = app.state.project_display();
    let x = page!(app, Preview);
    let body = chrome(frame, area, "Preview", &root, " enter open in your browser   a make it an app   r refresh   esc back", &note);
    let Some(pages) = &x.pages else {
        frame.buffer_mut().set_string(body.x + 1, body.y, "Looking...", theme::dim());
        return;
    };
    if pages.is_empty() {
        frame.buffer_mut().set_string(body.x + 1, body.y, "No pages here yet", theme::dim());
    }
    x.at = x.at.min(pages.len().saturating_sub(1));
    for (y, (i, name)) in pages.iter().enumerate().take(body.height as usize).enumerate() {
        let spans = vec![
            Span::styled(if i == x.at { " ❯ " } else { "   " }, theme::flame()),
            Span::styled(fit(name, body.width as usize - 4), theme::text()),
        ];
        frame.buffer_mut().set_line(body.x, body.y + y as u16, &marked(spans, body.width as usize, i == x.at), body.width);
    }
}

// --- Bench --------------------------------------------------------------------------

const SUITE: [(&str, &str, &str); 9] = [
    ("mmlu_pro", "MMLU-Pro", "knowledge"),
    ("arc", "ARC-Challenge", "reasoning"),
    ("gsm8k", "GSM8K", "math"),
    ("math500", "MATH-500", "math"),
    ("humaneval", "HumanEval", "code"),
    ("mbpp", "MBPP", "code"),
    ("ifeval", "IFEval", "instructions"),
    ("hellaswag", "HellaSwag", "commonsense"),
    ("truthfulqa", "TruthfulQA", "truthfulness"),
];
const SKILLS: [(&str, &str); 7] = [
    ("knowledge", "Knowledge"),
    ("reasoning", "Reasoning"),
    ("math", "Math"),
    ("code", "Code"),
    ("instructions", "Instructions"),
    ("commonsense", "Commonsense"),
    ("truthfulness", "Truthfulness"),
];
const BENCH_TABS: [&str; 3] = ["Board", "My results", "Run"];

pub struct Bench {
    tab: usize,
    pub form: Form,
    picked: HashSet<String>,
    off: HashSet<String>,
    size: String,
    effort: String,
    results: Option<Value>,
    board: Option<Value>,
    at: usize,
    top: usize,
    was_running: bool,
}

impl Bench {
    fn new() -> Bench {
        Bench {
            tab: 0,
            form: Form::default(),
            picked: HashSet::new(),
            off: HashSet::new(),
            size: "quick".into(),
            effort: String::new(),
            results: None,
            board: None,
            at: 0,
            top: 0,
            was_running: false,
        }
    }
}

fn load_bench(app: &mut App, refresh: bool) {
    app.fetch("models");
    app.call("bench.results", json!({}), |app, result| {
        if let Ok(reply) = result {
            page!(app, Bench).results = Some(reply);
        }
    });
    app.call("bench.board", json!({"refresh": refresh}), |app, result| {
        let reply = result.unwrap_or_else(|_| json!({"entries": [], "offline": true}));
        page!(app, Bench).board = Some(reply);
    });
}

fn share(v: Option<&Value>) -> String {
    v.and_then(Value::as_f64).map_or("--".to_string(), |x| format!("{:.0}%", x * 100.0))
}

fn entry_name(e: &Value) -> String {
    let model = e.get("model").cloned().unwrap_or(Value::Null);
    for key in ["name", "file", "repo"] {
        let got = s(&model, key);
        if !got.is_empty() {
            return got.rsplit('/').next().unwrap_or(&got).trim_end_matches(".gguf").to_string();
        }
    }
    "unknown".to_string()
}

fn entry_overall(e: &Value) -> Option<f64> {
    if let Some(v) = e.get("overall").and_then(Value::as_f64) {
        return Some(v);
    }
    let skills = e.get("skills")?.as_object()?;
    let values: Vec<f64> = skills.values().filter_map(Value::as_f64).collect();
    (!values.is_empty()).then(|| values.iter().sum::<f64>() / values.len() as f64)
}

fn bench_rows(app: &App) -> Vec<Row> {
    let Screen::Page(Page::Bench(x)) = &app.screen else { return Vec::new() };
    let live = app.state.bench.as_ref();
    let running = live.is_some_and(|l| b(l, "running"));
    let mut rows = Vec::new();
    if let Some(l) = live {
        let models = list_of(l, "models");
        if !models.is_empty() {
            rows.push(Row::head(if running { "RUNNING" } else if b(l, "stopped") { "STOPPED" } else { "FINISHED" }));
            let error = s(l, "error");
            if !error.is_empty() {
                rows.push(Row::bad(&error));
            }
            if let Some(fetching) = l.get("fetching") {
                rows.push(Row::note(&format!("Fetching {} of {}", n(fetching, "done") as u64, n(fetching, "of") as u64)));
            }
            for m in &models {
                let name = s(m, "name");
                let mut line = format!("{}  {}", name.rsplit('/').next().unwrap_or(&name), s(m, "state"));
                if let Some(v) = m.get("overall").and_then(Value::as_f64) {
                    line.push_str(&format!(", overall {:.0}%", v * 100.0));
                }
                rows.push(Row::note(&line));
                let parts = m.get("benchmarks").cloned().unwrap_or(Value::Null);
                for id in l.get("benchmarks").and_then(Value::as_array).into_iter().flatten() {
                    let id = id.as_str().unwrap_or("");
                    let Some(one) = parts.get(id) else { continue };
                    let label = SUITE.iter().find(|(x, _, _)| *x == id).map_or(id, |(_, l, _)| *l);
                    let done = n(one, "done");
                    let of = n(one, "n");
                    let cells = 20usize;
                    let filled = if of > 0.0 { ((done / of).min(1.0) * cells as f64).round() as usize } else { 0 };
                    let score = if done > 0.0 { format!("{:.0}%", n(one, "correct") / done * 100.0) } else { String::new() };
                    let error = s(one, "error");
                    rows.push(Row::note(&format!(
                        "  {} {}{} {:>4.0}/{:<4.0} {}",
                        pad(label, 14),
                        "█".repeat(filled),
                        "░".repeat(cells - filled),
                        done,
                        of,
                        if error.is_empty() { score } else { error }
                    )));
                }
            }
        }
    }
    rows.push(Row::head("MODELS"));
    let files: Vec<Value> = app.data.get("models").map(|m| list_of(m, "models")).unwrap_or_default();
    let mut any = false;
    for m in files.iter().filter(|m| { let f = s(m, "format"); (f.is_empty() || f == "gguf") && !s(m, "name").to_lowercase().contains("mmproj") }) {
        any = true;
        let path = s(m, "path");
        rows.push(Row::toggle(&format!("m:{path}"), &s(m, "name"), x.picked.contains(&path), &bytes(n(m, "bytes"))));
    }
    if !any {
        rows.push(Row::note("No models yet."));
    }
    rows.push(Row::head("HOW MUCH"));
    rows.push(Row::choice("@size", "Size", &x.size, &[("quick", "Quick, 50 per benchmark"), ("standard", "Standard, 200 per benchmark"), ("full", "Full, every item")], ""));
    rows.push(Row::choice("@effort", "Effort", &x.effort, &[("", "Default"), ("low", "Low"), ("medium", "Medium"), ("high", "High")], ""));
    rows.push(Row::head("BENCHMARKS"));
    for (id, label, skill) in SUITE {
        rows.push(Row::toggle(&format!("b:{id}"), label, !x.off.contains(id), skill));
    }
    rows.push(Row::head(""));
    if running {
        rows.push(Row::button("@stop", "Stop", ""));
    } else {
        rows.push(Row::button("@run", "Run", if x.picked.is_empty() { "Pick a model" } else { "Code tests run on this machine" }).disabled(x.picked.is_empty()));
    }
    rows
}

fn bench_change(app: &mut App, row: &Row) {
    let id = row.id.clone();
    if id == "@run" {
        let (models, size, effort, benchmarks) = {
            let x = page!(app, Bench);
            let files: Vec<Value> = app.data.get("models").map(|m| list_of(m, "models")).unwrap_or_default();
            let models: Vec<Value> = files
                .iter()
                .filter(|m| x.picked.contains(&s(m, "path")))
                .map(|m| {
                    let name = s(m, "name");
                    json!({"path": s(m, "path"), "name": name.rsplit('/').next().unwrap_or(&name)})
                })
                .collect();
            let benchmarks: Vec<&str> = SUITE.iter().map(|(i, _, _)| *i).filter(|i| !x.off.contains(*i)).collect();
            (models, x.size.clone(), x.effort.clone(), json!(benchmarks))
        };
        app.call("bench.start", json!({"models": models, "size": size, "effort": effort, "benchmarks": benchmarks}), |app, result| {
            if let Err(error) = result {
                app.fail(&error);
            }
        });
        page!(app, Bench).was_running = true;
        return;
    }
    if id == "@stop" {
        return app.act("bench.stop", json!({}));
    }
    if let Some(path) = id.strip_prefix("m:") {
        let x = page!(app, Bench);
        if !x.picked.remove(path) {
            x.picked.insert(path.to_string());
        }
        return;
    }
    if let Some(bench) = id.strip_prefix("b:") {
        let x = page!(app, Bench);
        if !x.off.remove(bench) && x.off.len() + 1 < SUITE.len() {
            x.off.insert(bench.to_string());
        }
        return;
    }
    if let Kind::Choice { value, options } = &row.kind {
        let items = options.iter().map(|(v, l)| Item::new(l, v).on(v == value)).collect();
        app.pick(&row.label, items, move |app, item| {
            let x = page!(app, Bench);
            if id == "@size" {
                x.size = item.value;
            } else {
                x.effort = item.value;
            }
        });
    }
}

fn bench_open(app: &mut App, entry: Value, mine: bool) {
    let mut rows = vec![
        Line::from(Span::styled(format!(" {}", entry_name(&entry)), theme::bold(theme::lit()))),
    ];
    let model = entry.get("model").cloned().unwrap_or(Value::Null);
    let repo = s(&model, "repo");
    if !repo.is_empty() {
        rows.push(Line::from(Span::styled(format!(" {repo}"), theme::dim())));
    }
    rows.push(Line::default());
    rows.push(Line::from(vec![
        Span::styled(" Overall       ", theme::dim()),
        Span::styled(share(entry_overall(&entry).as_ref().map(|v| json!(v)).as_ref()), theme::bold(theme::flame())),
    ]));
    let skills = entry.get("skills").cloned().unwrap_or(Value::Null);
    for (id, label) in SKILLS {
        let value = skills.get(id).and_then(Value::as_f64);
        let filled = value.map_or(0, |v| (v.clamp(0.0, 1.0) * 24.0).round() as usize);
        rows.push(Line::from(vec![
            Span::styled(format!(" {}", pad(label, 14)), theme::dim()),
            Span::styled("█".repeat(filled), theme::flame()),
            Span::styled("░".repeat(24 - filled), theme::faint()),
            Span::styled(format!(" {}", share(skills.get(id))), theme::text()),
        ]));
    }
    if let Some(tps) = entry.get("speed").and_then(|x| x.get("gen_tps")).and_then(Value::as_f64) {
        rows.push(Line::default());
        rows.push(Line::from(vec![Span::styled(" Speed         ", theme::dim()), Span::styled(format!("{tps:.1} tok/s"), theme::text())]));
    }
    let _ = mine;
    app.read("Result", rows);
}

fn bench_post(app: &mut App, id: String) {
    app.call("bench.posting", json!({"id": id}), move |app, result| {
        let got = match result {
            Ok(got) => got,
            Err(error) => return app.fail(&error),
        };
        if !b(&got, "public") {
            let why = if b(&got, "made") { "Made here, so it is on nobody's Hugging Face." } else if b(&got, "offline") { "Hugging Face unreachable." } else { "Not on Hugging Face." };
            return app.fail(why);
        }
        let where_ = if b(&got, "gh") {
            format!("A public issue on {}, from your GitHub account.", s(&got, "repo"))
        } else {
            format!("Opens {} on GitHub.", s(&got, "repo"))
        };
        app.ask("Post to the public board?", &[&where_], "Post", "Cancel", move |app, yes| {
            if !yes {
                return;
            }
            app.say("Posting");
            app.call("bench.post", json!({"id": id}), |app, result| match result {
                Ok(done) => {
                    let url = s(&done, "url");
                    if b(&done, "posted") {
                        app.say("Posted");
                        load_bench(app, true);
                    } else if let Err(error) = open_outside(&url) {
                        app.fail(&error);
                    } else {
                        app.say("Opened in your browser");
                    }
                }
                Err(error) => app.fail(&error),
            });
        });
    });
}

fn bench_key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let rows = bench_rows(app);
    let x = page!(app, Bench);
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => return app.back_to_chat(),
        KeyCode::Tab => {
            x.tab = (x.tab + 1) % BENCH_TABS.len();
            x.at = 0;
            x.top = 0;
            return;
        }
        KeyCode::BackTab => {
            x.tab = (x.tab + BENCH_TABS.len() - 1) % BENCH_TABS.len();
            x.at = 0;
            x.top = 0;
            return;
        }
        _ => {}
    }
    if x.tab == 2 {
        match x.form.key(&rows, key) {
            Did::Act => {
                let row = rows[x.form.at].clone();
                bench_change(app, &row);
            }
            Did::Step(by) => {
                let row = rows[x.form.at].clone();
                match &row.kind {
                    Kind::Toggle(_) => bench_change(app, &row),
                    Kind::Choice { value, options } => {
                        let place = options.iter().position(|(v, _)| v == value).unwrap_or(0) as isize;
                        let next = (place + by as isize).rem_euclid(options.len() as isize) as usize;
                        let chosen = options[next].0.clone();
                        if row.id == "@size" { x.size = chosen } else { x.effort = chosen }
                    }
                    _ => {}
                }
            }
            _ => {}
        }
        return;
    }
    let list = if x.tab == 0 {
        x.board.as_ref().map(|v| board_rows(v)).unwrap_or_default()
    } else {
        x.results.as_ref().map(|v| list_of(v, "results")).unwrap_or_default()
    };
    match key.code {
        KeyCode::Up | KeyCode::Char('k') => x.at = x.at.saturating_sub(1),
        KeyCode::Down | KeyCode::Char('j') => x.at = (x.at + 1).min(list.len().saturating_sub(1)),
        KeyCode::PageUp => x.at = x.at.saturating_sub(10),
        KeyCode::PageDown => x.at = (x.at + 10).min(list.len().saturating_sub(1)),
        KeyCode::Char('r') if x.tab == 0 => load_bench(app, true),
        KeyCode::Enter => {
            if let Some(entry) = list.get(x.at).cloned() {
                bench_open(app, entry, x_is_mine(app));
            }
        }
        KeyCode::Char('m') if x.tab == 0 => {
            if let Some(entry) = list.get(x.at) {
                let repo = entry.get("model").map(|m| s(m, "repo")).unwrap_or_default();
                if !repo.is_empty() {
                    app.screen = Screen::Models(Models::new(&repo));
                    models::entering(app);
                }
            }
        }
        KeyCode::Char('d') if x.tab == 1 => {
            if let Some(entry) = list.get(x.at) {
                let id = s(entry, "id");
                let title = format!("Delete the result for {}?", entry_name(entry));
                app.ask(&title, &["It is gone from this machine."], "Delete", "Keep", move |app, yes| {
                    if yes {
                        app.call("bench.delete", json!({"id": id}), |app, result| {
                            if let Err(error) = result {
                                app.fail(&error);
                            }
                            load_bench(app, false);
                        });
                    }
                });
            }
        }
        KeyCode::Char('p') if x.tab == 1 => {
            if let Some(entry) = list.get(x.at) {
                if !s(entry, "made").is_empty() {
                    return app.fail("Made here, so the board cannot take it");
                }
                let id = s(entry, "id");
                bench_post(app, id);
            }
        }
        _ => {}
    }
}

fn x_is_mine(app: &App) -> bool {
    matches!(&app.screen, Screen::Page(Page::Bench(x)) if x.tab == 1)
}

/// The board's results, best first, one per model.
fn board_rows(board: &Value) -> Vec<Value> {
    let mut rows: Vec<Value> = list_of(board, "entries").into_iter().filter(Value::is_object).collect();
    rows.sort_by(|a, c| entry_overall(c).partial_cmp(&entry_overall(a)).unwrap_or(std::cmp::Ordering::Equal));
    let mut seen = HashSet::new();
    rows.retain(|e| {
        let model = e.get("model").cloned().unwrap_or(Value::Null);
        let key = { let sha = s(&model, "sha256"); if sha.is_empty() { format!("{}/{}", s(&model, "repo"), s(&model, "file")) } else { sha } };
        seen.insert(key)
    });
    rows
}

fn bench_draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let note = app.note.clone();
    let rows = bench_rows(app);
    let x = page!(app, Bench);
    let keys = match x.tab {
        0 => " enter open   m find it in /models   r refresh   tab next   esc back",
        1 => " enter open   p post to the board   d delete   tab next   esc back",
        _ => " arrows move   enter change   tab next   esc back",
    };
    let tabs: String = BENCH_TABS.iter().enumerate().map(|(i, t)| if i == x.tab { format!("[{t}]") } else { format!(" {t} ") }).collect::<Vec<_>>().join(" ");
    let body = chrome(frame, area, "Bench", &tabs, keys, &note);
    if x.tab == 2 {
        x.form.draw(frame.buffer_mut(), body, &rows);
        return;
    }
    let list = if x.tab == 0 {
        match &x.board {
            None => return frame.buffer_mut().set_string(body.x + 1, body.y, "Reading...", theme::dim()),
            Some(v) => board_rows(v),
        }
    } else {
        match &x.results {
            None => return frame.buffer_mut().set_string(body.x + 1, body.y, "Reading...", theme::dim()),
            Some(v) => list_of(v, "results"),
        }
    };
    if list.is_empty() {
        let offline = x.tab == 0 && x.board.as_ref().is_some_and(|v| b(v, "offline"));
        let said = if offline { "The board could not be reached" } else if x.tab == 0 { "Nothing on the board yet" } else { "No results yet. Tab to Run." };
        return frame.buffer_mut().set_string(body.x + 1, body.y, said, theme::dim());
    }
    let cols = body.width as usize;
    let skill_cols = if cols >= 110 { 7 } else { 0 };
    let name_cols = cols.saturating_sub(12 + skill_cols * 6 + 2).max(12);
    let mut head = vec![Span::styled(format!("   {} {:>7} ", pad("Model", name_cols), "Overall"), theme::dim())];
    if skill_cols > 0 {
        for (_, label) in SKILLS {
            head.push(Span::styled(format!("{:>6}", &label[..4]), theme::dim()));
        }
    }
    frame.buffer_mut().set_line(body.x, body.y, &Line::from(head), body.width);
    let height = body.height.saturating_sub(1) as usize;
    x.at = x.at.min(list.len() - 1);
    keep_in_view(x.at, &mut x.top, height);
    for (y, (i, entry)) in list.iter().enumerate().skip(x.top).take(height).enumerate() {
        let overall = entry_overall(entry).map(|v| json!(v));
        let mut spans = vec![
            Span::styled(if i == x.at { " ❯ " } else { "   " }, theme::flame()),
            Span::styled(pad(&entry_name(entry), name_cols), theme::text()),
            Span::styled(format!(" {:>7} ", share(overall.as_ref())), theme::lit()),
        ];
        if skill_cols > 0 {
            let skills = entry.get("skills").cloned().unwrap_or(Value::Null);
            for (id, _) in SKILLS {
                spans.push(Span::styled(format!("{:>6}", share(skills.get(id))), theme::dim()));
            }
        }
        frame.buffer_mut().set_line(body.x, body.y + 1 + y as u16, &marked(spans, cols, i == x.at), body.width);
    }
}

// --- Create -------------------------------------------------------------------------

const QUANTIZATIONS: [&str; 5] = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0", "F16"];

#[derive(Default)]
pub struct Create {
    recipes: Option<Value>,
    run: Option<Value>,
    pub at: usize,
    top: usize,
    open: Option<String>,
    form: Form,
    draft: Option<Value>,
    polling: bool,
}

fn load_create(app: &mut App) {
    app.fetch("models");
    app.fetch("trainer");
    reload_recipes(app);
    app.call("lab.run", json!({}), |app, result| {
        if let Ok(run) = result {
            page!(app, Create).run = Some(run);
        }
    });
}

fn reload_recipes(app: &mut App) {
    app.call("lab.recipes", json!({}), |app, result| match result {
        Ok(reply) => page!(app, Create).recipes = Some(reply),
        Err(error) => app.fail(&error),
    });
}

fn poll_run(app: &mut App) {
    let x = page!(app, Create);
    let running = x.run.as_ref().is_some_and(|r| s(r, "phase") == "running");
    if !running || x.polling {
        return;
    }
    x.polling = true;
    app.call("lab.run", json!({}), |app, result| {
        let x = page!(app, Create);
        x.polling = false;
        if let Ok(run) = result {
            let over = s(&run, "phase") != "running";
            x.run = Some(run);
            if over {
                // Its rows are about to change. The cursor goes back to the
                // top rather than landing on whatever is where Stop was.
                x.form = Form::default();
                reload_recipes(app);
            }
        }
    });
}

fn blank_recipe() -> Value {
    json!({"id": "", "name": "", "purpose": "", "base": {"source": "hub", "id": ""}, "data": [], "tools": [],
           "method": "qlora", "format": "gguf", "quantization": "Q4_K_M", "parameters_b": 0,
           "epochs": 2, "context": 512, "learning_rate": 0.0001, "stage": "draft"})
}

fn recipes_of(app: &App) -> Vec<Value> {
    match &app.screen {
        Screen::Page(Page::Create(x)) => {
            let mut list = x.recipes.as_ref().map(|r| list_of(r, "recipes")).unwrap_or_default();
            let rank = |stage: &str| ["training", "testing", "finished", "draft"].iter().position(|x| *x == stage).unwrap_or(9);
            list.sort_by_key(|r| rank(&s(r, "stage")));
            list
        }
        _ => Vec::new(),
    }
}

fn spec_line(r: &Value) -> String {
    let base = r.get("base").cloned().unwrap_or(Value::Null);
    let mut parts = Vec::new();
    let label = { let l = s(&base, "label"); if l.is_empty() { s(&base, "id") } else { l } };
    if !label.is_empty() {
        parts.push(label.rsplit('/').next().unwrap_or(&label).to_string());
    }
    if s(r, "method") == "condense" {
        let from = s(r, "source_quantization");
        parts.push(format!("{} to {}", if from.is_empty() { "original".to_string() } else { from }, s(r, "quantization")));
        let size = if n(r, "trained_bytes") > 0.0 { n(r, "trained_bytes") } else { n(r, "export_bytes") };
        if size > 0.0 {
            parts.push(bytes(size));
        }
    } else {
        if n(r, "parameters_b") > 0.0 {
            parts.push(format!("{}B", n(r, "parameters_b")));
        }
        parts.push(if s(r, "method") == "lora" { "LoRA".to_string() } else { "QLoRA".to_string() });
        parts.push(format!("{} {}", s(r, "format").to_uppercase(), s(r, "quantization")));
    }
    parts.join("   ")
}

fn run_rows(run: &Value, rows: &mut Vec<Row>) {
    let phase = s(run, "phase");
    if phase == "running" {
        let cells = 28usize;
        let percent = n(run, "percent").clamp(0.0, 1.0);
        let filled = (percent * cells as f64).round() as usize;
        rows.push(Row::head(&s(run, "label").to_uppercase()));
        rows.push(Row::note(&format!("{}{} {:.0}%", "█".repeat(filled), "░".repeat(cells - filled), percent * 100.0)));
        let left = n(run, "seconds_left") as u64;
        rows.push(Row::note(&if left > 0 { format!("about {} left", clock(left)) } else { format!("running for {}", clock(n(run, "seconds") as u64)) }));
        let device = s(run, "device");
        if !device.is_empty() {
            rows.push(Row::note(&format!("On {device}")));
        }
        if n(run, "records") > 0.0 {
            rows.push(Row::note(&format!("{} records", n(run, "records") as u64)));
        }
        if n(run, "loss") > 0.0 {
            rows.push(Row::note(&format!("Loss {:.4}", n(run, "loss"))));
        }
        for note in list_of(run, "notes") {
            rows.push(Row::note(note.as_str().unwrap_or("")));
        }
        rows.push(Row::button("@run-stop", "Stop the run", "It can't be resumed."));
    } else if phase == "failed" {
        rows.push(Row::bad(&s(run, "error")));
        let hint = s(run, "hint");
        if !hint.is_empty() {
            rows.push(Row::note(&hint));
        }
        let log: Vec<String> = list_of(run, "log").iter().filter_map(|l| l.as_str().map(str::to_string)).collect();
        if !log.is_empty() {
            let tail: Vec<String> = log.iter().rev().take(8).rev().cloned().collect();
            rows.push(Row::note(&tail.join("\n")));
        }
    } else if phase == "canceled" {
        rows.push(Row::note("Stopped. Nothing was kept."));
    }
}

fn detail_rows(app: &App, r: &Value) -> Vec<Row> {
    let Screen::Page(Page::Create(x)) = &app.screen else { return Vec::new() };
    let id = s(r, "id");
    let stage = s(r, "stage");
    let mut rows = vec![Row::head(&s(r, "name").to_uppercase()), Row::note(&format!("{}  {}", s(r, "stage_text"), spec_line(r)))];
    let purpose = s(r, "purpose");
    if !purpose.is_empty() {
        rows.push(Row::note(&purpose));
    }
    let run = x.run.as_ref().filter(|run| s(run, "recipe") == id);
    if let Some(run) = run {
        run_rows(run, &mut rows);
    }
    let running = run.is_some_and(|run| s(run, "phase") == "running");
    if !running {
        match stage.as_str() {
            "draft" => {
                let missing: Vec<String> = list_of(r, "missing").iter().filter_map(|m| m.as_str().map(str::to_string)).collect();
                if !missing.is_empty() {
                    rows.push(Row::note(&format!("Needs {}", missing.join(", "))));
                }
                rows.push(Row::button("@edit", "Continue setup", ""));
                rows.push(Row::button("@train", if s(r, "method") == "condense" { "Condense" } else { "Train" }, "").disabled(!missing.is_empty()));
            }
            "training" => {
                rows.push(Row::note("Not running."));
                rows.push(Row::button("@train", "Start again", ""));
                rows.push(Row::button("@draft", "Put it back to a draft", ""));
                if s(r, "method") != "condense" {
                    rows.push(Row::button("@attach", "Trained elsewhere: choose the file", ""));
                }
            }
            _ => {
                if !s(r, "trained_display").is_empty() {
                    rows.push(Row::note(&s(r, "trained_display")));
                }
                rows.push(Row::button("@test", "Test it", "A chat with it, then keep it or not.").disabled(!b(r, "trained_there")));
                let base = r.get("base").map(|x| s(x, "path")).unwrap_or_default();
                if b(r, "trained_there") && base.to_lowercase().ends_with(".gguf") {
                    rows.push(Row::button("@versus", "Bench it against its base", ""));
                }
                rows.push(Row::button("@edit", "Edit", ""));
            }
        }
    }
    rows.push(Row::head(""));
    rows.push(Row::button("@delete", "Delete", "").disabled(running));
    rows
}

fn wizard_rows(draft: &Value) -> Vec<Row> {
    let names = |key: &str| -> Vec<Value> { list_of(draft, key) };
    let base = draft.get("base").cloned().unwrap_or(Value::Null);
    let base_label = { let l = s(&base, "label"); if l.is_empty() { s(&base, "id") } else { l } };
    let mut rows = vec![
        Row::head("NAME"),
        Row::text("name", "Name", &s(draft, "name"), "", ""),
        Row::text("purpose", "What it is for", &s(draft, "purpose"), "", ""),
        Row::head("BASE MODEL"),
        Row::note(if base_label.is_empty() { "None yet" } else { &base_label }),
        Row::button("@base-hub", "Search Hugging Face", ""),
        Row::button("@base-file", "A file on this machine", ""),
        Row::head("DATA"),
        Row::note("Questions and answers."),
    ];
    for (i, one) in names("data").iter().enumerate() {
        rows.push(Row::button(&format!("@rm-data:{i}"), &format!("Remove {}", s(one, "label")), ""));
    }
    rows.push(Row::button("@data-hub", "Add from Hugging Face", ""));
    rows.push(Row::button("@data-file", "Add a file", ""));
    rows.push(Row::head("TOOLS"));
    rows.push(Row::note("Optional."));
    for (i, one) in names("tools").iter().enumerate() {
        rows.push(Row::button(&format!("@rm-tools:{i}"), &format!("Remove {}", s(one, "label")), ""));
    }
    rows.push(Row::button("@tools-file", "Add a file", ""));
    rows.push(Row::head("TARGET"));
    rows.push(Row::choice("method", "Method", &s(draft, "method"), &[("qlora", "QLoRA, least memory"), ("lora", "LoRA, full base")], ""));
    rows.push(Row::choice("format", "Format", &s(draft, "format"), &[("gguf", "GGUF"), ("mlx", "MLX")], ""));
    let quants: Vec<(&str, &str)> = QUANTIZATIONS.iter().map(|q| (*q, *q)).collect();
    rows.push(Row::choice("quantization", "Quantization", &s(draft, "quantization"), &quants, ""));
    rows.push(Row::num("parameters_b", "Parameters (billions)", n(draft, "parameters_b"), (0.0, 0.0, 0.1), ""));
    rows.push(Row::num("epochs", "Epochs", n(draft, "epochs"), (1.0, 50.0, 1.0), ""));
    rows.push(Row::num("context", "Context", n(draft, "context"), (128.0, 8192.0, 64.0), ""));
    rows.push(Row::text("learning_rate", "Learning rate", &n(draft, "learning_rate").to_string(), "", ""));
    rows.push(Row::head(""));
    rows.push(Row::button("@save", "Save", ""));
    let mut missing = Vec::new();
    if s(draft, "name").trim().is_empty() {
        missing.push("a name");
    }
    if s(&base, "id").is_empty() {
        missing.push("a base model");
    }
    if names("data").is_empty() {
        missing.push("data");
    }
    let hint = if missing.is_empty() { String::new() } else { format!("Needs {}", missing.join(", ")) };
    rows.push(Row::button("@start", "Train", &hint).disabled(!missing.is_empty()));
    rows
}

fn local_asset(path: String) -> Value {
    let label = path.rsplit(['/', '\\']).next().unwrap_or(&path).to_string();
    json!({"source": "local", "id": path, "path": path, "label": label})
}

fn hub_pick(app: &mut App, kind: &'static str, slot: &'static str) {
    app.type_in(if kind == "dataset" { "Search datasets" } else { "Search models" }, "", "", false, false, move |app, query| {
        if query.trim().is_empty() {
            return;
        }
        app.say("Searching");
        app.call("hub.search", json!({"query": query.trim(), "kind": kind}), move |app, result| {
            app.note = None;
            let reply = match result {
                Ok(reply) => reply,
                Err(error) => return app.fail(&error),
            };
            let items: Vec<Item> = list_of(&reply, "items")
                .iter()
                .map(|i| {
                    let hint = format!("{}{}", s(i, "summary"), if b(i, "gated") { "  (gated)" } else { "" });
                    Item::new(&s(i, "id"), &format!("{}\n{}", s(i, "id"), n(i, "parameters_b"))).hint(&hint)
                })
                .collect();
            if items.is_empty() {
                return app.say("Nothing found");
            }
            app.pick("Choose", items, move |app, item| {
                let (id, params) = item.value.split_once('\n').unwrap_or((&item.value, "0"));
                let asset = json!({"source": "hub", "id": id, "label": id});
                let params: f64 = params.parse().unwrap_or(0.0);
                let x = page!(app, Create);
                let Some(draft) = &mut x.draft else { return };
                if slot == "base" {
                    draft["base"] = asset;
                    draft["parameters_b"] = json!(params);
                } else if let Some(list) = draft[slot].as_array_mut() {
                    if !list.iter().any(|a| s(a, "id") == id) {
                        list.push(asset);
                    }
                }
            });
        });
    });
}

fn file_pick(app: &mut App, slot: &'static str) {
    app.type_in("A file on this machine", "Its path", "", false, false, move |app, typed| {
        let raw = crate::commands::shellish(&typed);
        if raw.is_empty() {
            return;
        }
        let path = app.folder.join(raw);
        if !path.exists() {
            return app.fail("It is not there");
        }
        let asset = local_asset(path.to_string_lossy().to_string());
        let x = page!(app, Create);
        let Some(draft) = &mut x.draft else { return };
        if slot == "base" {
            draft["base"] = asset;
        } else if let Some(list) = draft[slot].as_array_mut() {
            list.push(asset);
        }
    });
}

fn save_draft(app: &mut App, then_train: bool) {
    let x = page!(app, Create);
    let Some(mut draft) = x.draft.clone() else { return };
    if s(&draft, "name").trim().is_empty() {
        return app.fail("It needs a name");
    }
    if s(&draft, "id").is_empty() {
        let slug: String = s(&draft, "name").to_lowercase().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '-' }).collect();
        draft["id"] = json!(slug.trim_matches('-'));
    }
    app.call("lab.save", draft, move |app, result| {
        let saved = match result {
            Ok(saved) => saved,
            Err(error) => return app.fail(&error),
        };
        let id = s(&saved, "id");
        let x = page!(app, Create);
        x.draft = None;
        x.open = Some(id.clone());
        x.form = Form::default();
        if then_train {
            train(app, id);
        } else {
            reload_recipes(app);
        }
    });
}

fn train(app: &mut App, id: String) {
    app.call("lab.train", json!({"id": id}), |app, result| {
        if let Err(error) = result {
            app.fail(&error);
        }
        app.call("lab.run", json!({}), |app, result| {
            if let Ok(run) = result {
                page!(app, Create).run = Some(run);
            }
            reload_recipes(app);
        });
    });
}

fn condense(app: &mut App) {
    let files: Vec<Value> = app.data.get("models").map(|m| list_of(m, "models")).unwrap_or_default();
    let items: Vec<Item> = files
        .iter()
        .filter(|m| s(m, "format") != "mlx" && !s(m, "name").to_lowercase().contains("mmproj"))
        .map(|m| Item::new(&s(m, "name"), &s(m, "path")).hint(&bytes(n(m, "bytes"))))
        .collect();
    if items.is_empty() {
        return app.fail("No model to condense");
    }
    app.pick("Condense which model?", items, |app, item| {
        let path = item.value.clone();
        let label = item.label.clone();
        let size_of = item.hint.clone();
        app.say("Measuring");
        let asked = path.clone();
        app.call("lab.condense.sizes", json!({"path": path}), move |app, result| {
            app.note = None;
            let sizes = match result {
                Ok(sizes) => sizes,
                Err(error) => return app.fail(&error),
            };
            let rows = sizes.get("sizes").cloned().unwrap_or(Value::Null);
            let suggested = s(&sizes, "suggested");
            let mut items = Vec::new();
            for q in QUANTIZATIONS {
                let (value, fits) = match rows.get(q) {
                    Some(v) if v.is_object() => (n(v, "bytes"), s(v, "fit")),
                    Some(v) => (v.as_f64().unwrap_or(0.0), String::new()),
                    None => continue,
                };
                if value <= 0.0 {
                    continue;
                }
                let tail = if fits.is_empty() { String::new() } else { format!("  {fits}") };
                items.push(Item::new(q, q).hint(&format!("{}{tail}", bytes(value))).on(q == suggested));
            }
            if items.is_empty() {
                items = QUANTIZATIONS.iter().map(|q| Item::new(q, q)).collect();
            }
            let elements = n(&sizes, "elements");
            let title = format!("{label} ({size_of}) to");
            app.pick(&title, items, move |app, item| {
                let quant = item.value.clone();
                let stem = label.rsplit('/').next().unwrap_or(&label).trim_end_matches(".gguf").to_string();
                let suggested_name = format!("{stem} {quant}");
                app.type_in("Name it", "", &suggested_name, false, false, move |app, name| {
                    if name.trim().is_empty() {
                        return;
                    }
                    let slug: String = name.to_lowercase().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '-' }).collect();
                    let mut recipe = blank_recipe();
                    recipe["id"] = json!(slug.trim_matches('-'));
                    recipe["name"] = json!(name.trim());
                    recipe["method"] = json!("condense");
                    recipe["quantization"] = json!(quant);
                    recipe["parameters_b"] = json!((elements / 1e7).round() / 100.0);
                    recipe["base"] = json!({"source": "local", "id": asked, "path": asked, "label": label});
                    app.call("lab.save", recipe, |app, result| match result {
                        Ok(saved) => {
                            let id = s(&saved, "id");
                            page!(app, Create).open = Some(id.clone());
                            train(app, id);
                        }
                        Err(error) => app.fail(&error),
                    });
                });
            });
        });
    });
}

fn detail_action(app: &mut App, id: &str, r: &Value) {
    let recipe = s(r, "id");
    match id {
        "@edit" => {
            let x = page!(app, Create);
            x.draft = Some(r.clone());
            x.form = Form::default();
        }
        "@train" => train(app, recipe),
        "@run-stop" => {
            app.call("lab.run", json!({"cancel": true}), |app, result| {
                if let Ok(run) = result {
                    page!(app, Create).run = Some(run);
                }
                reload_recipes(app);
            });
        }
        "@draft" => {
            let mut back = r.clone();
            back["stage"] = json!("draft");
            app.call("lab.save", back, |app, result| {
                if let Err(error) = result {
                    app.fail(&error);
                }
                reload_recipes(app);
            });
        }
        "@attach" => app.type_in("The trained file", "Its path", "", false, false, move |app, typed| {
            let path = app.folder.join(crate::commands::shellish(&typed));
            app.call("lab.attach", json!({"id": recipe, "path": path.to_string_lossy()}), |app, result| {
                if let Err(error) = result {
                    app.fail(&error);
                }
                reload_recipes(app);
            });
        }),
        "@test" => {
            let name = s(r, "name");
            app.call("lab.test", json!({"id": recipe}), move |app, result| match result {
                Ok(_) => {
                    app.testing = Some((recipe, name));
                    app.back_to_chat();
                    app.say("Testing. /keep keeps it, /endtest stops");
                }
                Err(error) => app.fail(&error),
            });
        }
        "@versus" => {
            let made = s(r, "trained_path");
            let base = r.get("base").map(|x| s(x, "path")).unwrap_or_default();
            let leaf = |p: &str| p.rsplit(['/', '\\']).next().unwrap_or(p).to_string();
            let models = json!([{"path": made, "name": leaf(&made)}, {"path": base, "name": leaf(&base)}]);
            let benchmarks: Vec<&str> = SUITE.iter().map(|(i, _, _)| *i).collect();
            app.call("bench.start", json!({"models": models, "size": "quick", "effort": "", "benchmarks": benchmarks}), |app, result| {
                if let Err(error) = result {
                    return app.fail(&error);
                }
                let mut page_now = Bench::new();
                page_now.tab = 2;
                page_now.was_running = true;
                open(app, Page::Bench(page_now));
            });
        }
        "@delete" => {
            let title = format!("Delete {}?", s(r, "name"));
            app.ask(&title, &["The recipe goes. A file it made stays."], "Delete", "Keep", move |app, yes| {
                if !yes {
                    return;
                }
                app.call("lab.delete", json!({"id": recipe}), |app, result| {
                    if let Err(error) = result {
                        app.fail(&error);
                    }
                    page!(app, Create).open = None;
                    reload_recipes(app);
                });
            });
        }
        _ => {}
    }
}

fn wizard_change(app: &mut App, row: &Row) {
    let id = row.id.clone();
    match id.as_str() {
        "@base-hub" => return hub_pick(app, "model", "base"),
        "@base-file" => return file_pick(app, "base"),
        "@data-hub" => return hub_pick(app, "dataset", "data"),
        "@data-file" => return file_pick(app, "data"),
        "@tools-file" => return file_pick(app, "tools"),
        "@save" => return save_draft(app, false),
        "@start" => return save_draft(app, true),
        _ => {}
    }
    if let Some(rest) = id.strip_prefix("@rm-") {
        let (slot, index) = rest.split_once(':').unwrap_or(("", "0"));
        let index: usize = index.parse().unwrap_or(0);
        let x = page!(app, Create);
        if let Some(list) = x.draft.as_mut().and_then(|d| d[slot].as_array_mut()) {
            if index < list.len() {
                list.remove(index);
            }
        }
        return;
    }
    let set = move |app: &mut App, value: Value| {
        let x = page!(app, Create);
        if let Some(draft) = &mut x.draft {
            draft[id.as_str()] = value;
        }
    };
    match &row.kind {
        Kind::Text { value, .. } => {
            let is_rate = row.id == "learning_rate";
            app.type_in(&row.label, &row.hint, value, false, false, move |app, typed| {
                if is_rate {
                    match typed.trim().parse::<f64>() {
                        Ok(v) => set(app, json!(v)),
                        Err(_) => app.fail("A number"),
                    }
                } else {
                    set(app, json!(typed));
                }
            });
        }
        Kind::Num { value, step, .. } => {
            let whole_number = step.fract() == 0.0;
            let shown = if whole_number { format!("{value:.0}") } else { format!("{value}") };
            app.type_in(&row.label, "", &shown, false, false, move |app, typed| match typed.trim().parse::<f64>() {
                Ok(v) if whole_number => set(app, json!(v as i64)),
                Ok(v) => set(app, json!(v)),
                Err(_) => app.fail("A number"),
            });
        }
        Kind::Choice { value, options } => {
            let items = options.iter().map(|(v, l)| Item::new(l, v).on(v == value)).collect();
            app.pick(&row.label, items, move |app, item| set(app, json!(item.value)));
        }
        _ => {}
    }
}

fn create_key(app: &mut App, key: KeyEvent) {
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    if ctrl && key.code == KeyCode::Char('c') {
        return app.back_to_chat();
    }
    let list = recipes_of(app);
    let x = page!(app, Create);

    if let Some(draft) = x.draft.clone() {
        let rows = wizard_rows(&draft);
        if key.code == KeyCode::Esc {
            x.draft = None;
            x.form = Form::default();
            return;
        }
        match x.form.key(&rows, key) {
            Did::Act => {
                let row = rows[x.form.at].clone();
                wizard_change(app, &row);
            }
            Did::Step(by) => {
                let row = rows[x.form.at].clone();
                let id = row.id.clone();
                let next = match &row.kind {
                    Kind::Num { value, min, max, step } => {
                        let v = if max > min { (value + step * by).clamp(*min, *max) } else { (value + step * by).max(0.0) };
                        Some(if step.fract() == 0.0 { json!(v as i64) } else { json!((v * 10.0).round() / 10.0) })
                    }
                    Kind::Choice { value, options } => {
                        let place = options.iter().position(|(v, _)| v == value).unwrap_or(0) as isize;
                        Some(json!(options[(place + by as isize).rem_euclid(options.len() as isize) as usize].0))
                    }
                    _ => None,
                };
                if let (Some(value), Some(d)) = (next, x.draft.as_mut()) {
                    d[id.as_str()] = value;
                }
            }
            _ => {}
        }
        return;
    }

    if let Some(open_id) = x.open.clone() {
        let Some(r) = list.iter().find(|r| s(r, "id") == open_id).cloned() else {
            x.open = None;
            return;
        };
        if matches!(key.code, KeyCode::Esc | KeyCode::Char('q')) {
            x.open = None;
            x.form = Form::default();
            return;
        }
        let rows = detail_rows(app, &r);
        let x = page!(app, Create);
        if let Did::Act = x.form.key(&rows, key) {
            let id = rows[x.form.at].id.clone();
            detail_action(app, &id, &r);
        }
        return;
    }

    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => app.back_to_chat(),
        KeyCode::Up | KeyCode::Char('k') => x.at = x.at.saturating_sub(1),
        KeyCode::Down | KeyCode::Char('j') => x.at = (x.at + 1).min(list.len().saturating_sub(1)),
        KeyCode::Enter => {
            if let Some(r) = list.get(x.at) {
                x.open = Some(s(r, "id"));
                x.form = Form::default();
            }
        }
        KeyCode::Char('n') => {
            x.draft = Some(blank_recipe());
            x.form = Form::default();
        }
        KeyCode::Char('c') => condense(app),
        KeyCode::Char('r') => reload_recipes(app),
        _ => {}
    }
}

fn create_draw(frame: &mut Frame, app: &mut App, area: Rect) {
    let note = app.note.clone();
    let list = recipes_of(app);
    let trainer_ready = app.data.get("trainer").is_some_and(|t| b(t, "ready"));
    let open_id = page!(app, Create).open.clone();
    let opened = open_id.as_ref().and_then(|id| list.iter().find(|r| s(r, "id") == *id).cloned());
    let detail = opened.as_ref().map(|r| detail_rows(app, r));
    let x = page!(app, Create);

    if let Some(draft) = x.draft.clone() {
        let title = if s(&draft, "id").is_empty() { "New expert".to_string() } else { format!("Edit {}", s(&draft, "name")) };
        let body = chrome(frame, area, &title, "", " arrows move   enter change   esc back", &note);
        let rows = wizard_rows(&draft);
        return x.form.draw(frame.buffer_mut(), body, &rows);
    }
    if let (Some(r), Some(rows)) = (&opened, &detail) {
        let body = chrome(frame, area, "Create", &s(r, "name"), " arrows move   enter choose   esc back", &note);
        return x.form.draw(frame.buffer_mut(), body, rows);
    }
    let right = if trainer_ready { "" } else { "Training needs /training" };
    let body = chrome(frame, area, "Create", right, " enter open   n new fine-tune   c condense a model   r refresh   esc back", &note);
    if x.recipes.is_none() {
        return frame.buffer_mut().set_string(body.x + 1, body.y, "Reading...", theme::dim());
    }
    if list.is_empty() {
        frame.buffer_mut().set_string(body.x + 1, body.y, "Nothing made yet", theme::dim());
        return;
    }
    let cols = body.width as usize;
    let height = body.height as usize / 2;
    x.at = x.at.min(list.len() - 1);
    keep_in_view(x.at, &mut x.top, height.max(1));
    for (row, (i, r)) in list.iter().enumerate().skip(x.top).take(height.max(1)).enumerate() {
        let y = body.y + (row * 2) as u16;
        let here = i == x.at;
        let stage_style = match s(r, "stage").as_str() {
            "finished" => theme::good(),
            "training" => theme::lit(),
            "testing" => theme::flame(),
            _ => theme::dim(),
        };
        let first = vec![
            Span::styled(if here { " ❯ " } else { "   " }, theme::flame()),
            Span::styled(pad(&s(r, "name"), cols.saturating_sub(20)), if here { theme::bold(theme::text()) } else { theme::text() }),
            Span::styled(s(r, "stage_text"), stage_style),
        ];
        let second = vec![Span::raw("   "), Span::styled(fit(&spec_line(r), cols.saturating_sub(4)), theme::dim())];
        frame.buffer_mut().set_line(body.x, y, &marked(first, cols, here), body.width);
        frame.buffer_mut().set_line(body.x, y + 1, &marked(second, cols, here), body.width);
    }
}
