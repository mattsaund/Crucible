// SPDX-License-Identifier: MIT
//! Everything the terminal is showing, and what each key does to it.

use crate::commands;
use crate::composer::Composer;
use crate::engine::{Engine, Msg};
use crate::models::{self, Models};
use crate::overlay::{self, Overlay};
use crate::screens::{self, Page};
use crate::settings::{self, Settings};
use crate::state::{self, s, Action, State};
use crate::theme;
use ratatui::crossterm::event::{Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use ratatui::text::Line;
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::rc::Rc;
use std::time::{Duration, Instant};

pub type Then = Box<dyn FnOnce(&mut App, Result<Value, String>)>;

pub enum Screen {
    Chat,
    Settings(Settings),
    Models(Models),
    Page(Page),
}

/// Something that needs the terminal to itself for a moment.
pub enum External {
    Suspend,
    Edit(PathBuf),
    Update(String),
}

/// Something said outside the turns: a shell command typed with `!`.
pub struct Local {
    pub after: usize,
    pub action: Action,
}

pub struct Cached {
    pub key: (u64, usize, usize, usize, bool, bool, bool),
    pub rows: Rc<Vec<Line<'static>>>,
}

#[derive(Clone, Copy)]
pub struct Selection {
    pub from: (u16, u16),
    pub to: (u16, u16),
    pub done: bool,
}

pub struct App {
    pub engine: Engine,
    pub state: State,
    pub config: Value,
    pub data: HashMap<String, Value>,
    waiting: HashMap<u64, Then>,
    pub input: Composer,
    pub screen: Screen,
    pub overlay: Option<Overlay>,
    pub scroll: usize,
    pub open: bool,
    /// Whether thinking is drawn in full. It starts closed, and /showthinking
    /// opens it.
    pub show_thinking: bool,
    pub route: String,
    pub menu_at: usize,
    pub note: Option<(String, bool, Instant)>,
    pub quitting: bool,
    pub folder: PathBuf,
    pub began: Option<Instant>,
    pub born: Instant,
    pub queue: VecDeque<String>,
    pub attached: Vec<String>,
    pub local: Vec<Local>,
    pub feed: Vec<Cached>,
    pub feed_total: usize,
    pub feed_area: Rect,
    pub session: String,
    pub ctrl_c: Option<Instant>,
    pub esc: Option<Instant>,
    pub selection: Option<Selection>,
    pub external: Option<External>,
    pub dirty: bool,
    pub truecolor: bool,
    pub gone: Option<String>,
    pub first_push: bool,
    resume: bool,
    trust_asked: String,
    pub files: Vec<String>,
    pub tree_asked: Option<Instant>,
    pub theme_choice: String,
    /// A fine-tune being tried: its recipe and its name.
    pub testing: Option<(String, String)>,
    /// When the status line last changed: most of what it says is news.
    pub status_at: Instant,
    last_status: String,
    last_notice: String,
}

impl App {
    pub fn new(engine: Engine, folder: PathBuf, resume: bool) -> App {
        App {
            engine,
            state: State::default(),
            config: Value::Null,
            data: HashMap::new(),
            waiting: HashMap::new(),
            input: Composer::multiline(),
            screen: Screen::Chat,
            overlay: None,
            scroll: 0,
            open: false,
            show_thinking: false,
            route: String::new(),
            menu_at: 0,
            note: None,
            quitting: false,
            folder,
            began: None,
            born: Instant::now(),
            queue: VecDeque::new(),
            attached: Vec::new(),
            local: Vec::new(),
            feed: Vec::new(),
            feed_total: 0,
            feed_area: Rect::default(),
            session: String::new(),
            ctrl_c: None,
            esc: None,
            selection: None,
            external: None,
            dirty: true,
            truecolor: theme::truecolor(),
            gone: None,
            first_push: true,
            resume,
            trust_asked: String::new(),
            files: Vec::new(),
            tree_asked: None,
            theme_choice: "auto".into(),
            testing: None,
            status_at: Instant::now(),
            last_status: String::new(),
            last_notice: String::new(),
        }
    }

    // --- talking to the engine ------------------------------------------------

    pub fn call(&mut self, method: &str, params: Value, then: impl FnOnce(&mut App, Result<Value, String>) + 'static) {
        let id = self.engine.call(method, params);
        self.waiting.insert(id, Box::new(then));
    }

    /// A call whose only news is if it failed.
    pub fn act(&mut self, method: &str, params: Value) {
        self.call(method, params, |app, result| {
            if let Err(error) = result {
                app.fail(&error);
            }
        });
    }

    /// Ask for a lookup and keep the answer under its name.
    pub fn fetch(&mut self, method: &str) {
        let name = method.to_string();
        self.call(method, json!({}), move |app, result| match result {
            Ok(value) => {
                app.data.insert(name, value);
            }
            Err(error) => {
                app.data.entry(format!("{name}!")).or_insert(Value::String(error));
            }
        });
    }

    pub fn reload_config(&mut self) {
        self.call("config", json!({}), |app, result| {
            if let Ok(value) = result {
                app.config = value;
            }
        });
    }

    /// Change settings. The app and this share the one file.
    pub fn configure(&mut self, patch: Value) {
        self.call("config.set", patch, |app, result| {
            match result {
                Ok(reply) => {
                    if let Some(first) = reply.get("warnings").and_then(Value::as_array).and_then(|w| w.first()).and_then(Value::as_str) {
                        let text = first.to_string();
                        app.fail(&text);
                    }
                }
                Err(error) => app.fail(&error),
            }
            app.reload_config();
        });
    }

    pub fn say(&mut self, text: &str) {
        self.note = Some((text.to_string(), false, Instant::now()));
    }

    pub fn fail(&mut self, text: &str) {
        self.note = Some((text.to_string(), true, Instant::now()));
    }

    // --- what comes in --------------------------------------------------------

    pub fn handle(&mut self, msg: Msg) {
        match msg {
            Msg::Term(event) => self.on_term(event),
            Msg::Push(value) => self.on_push(value),
            Msg::Reply(id, result) => {
                if let Some(then) = self.waiting.remove(&id) {
                    then(self, result);
                }
            }
            Msg::Gone(tail) => {
                self.gone = Some(tail);
                self.quitting = true;
            }
            Msg::Tick => models::tick(self),
        }
        if let Some((_, _, at)) = &self.note {
            if at.elapsed() > Duration::from_secs(8) {
                self.note = None;
            }
        }
        self.dirty = true;
    }

    fn on_push(&mut self, value: Value) {
        self.state.apply(value);
        if self.first_push {
            self.first_push = false;
            self.startup();
        }
        if self.state.status != self.last_status {
            self.last_status = self.state.status.clone();
            self.status_at = Instant::now();
        }
        let notice = self
            .state
            .raw
            .get("notices")
            .and_then(Value::as_array)
            .and_then(|l| l.last())
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        if notice != self.last_notice {
            let first = self.last_notice.is_empty() && self.born.elapsed() < Duration::from_secs(1);
            self.last_notice = notice.clone();
            if !first && !notice.is_empty() {
                let mut chars = notice.chars();
                let said = chars.next().map_or(String::new(), |c| c.to_uppercase().collect::<String>() + chars.as_str());
                self.say(&crate::text::clean(&said));
            }
        }
        // The folder this is working in, as a chat from another project moves it.
        if let Some(project) = self.state.raw.get("project") {
            if state::b(project, "open") && !state::b(project, "scratchpad") {
                let root = s(project, "root");
                if !root.is_empty() {
                    self.folder = PathBuf::from(root);
                }
            }
        }
        let session = s(&self.state.raw, "session");
        if session != self.session {
            self.session = session;
            self.scroll = 0;
            self.local.clear();
            self.feed.clear();
        }
        if self.state.busy {
            self.began.get_or_insert_with(Instant::now);
        } else {
            self.began = None;
            if let Some(next) = self.queue.pop_front() {
                self.submit(next);
            }
        }
        self.ask_trust();
        screens::on_push(self);
    }

    fn startup(&mut self) {
        self.reload_config();
        self.fetch("models");
        self.fetch("providers");
        self.fetch("runtimes");
        self.call("prefs", json!({}), |app, result| {
            let Ok(prefs) = result else { return };
            if let Some(history) = prefs.get("cli_history").and_then(Value::as_array) {
                app.input.history = history.iter().filter_map(|h| h.as_str().map(str::to_string)).collect();
            }
            app.route = s(&prefs, "route");
            let theme_choice = s(&prefs, "cli_theme");
            if !theme_choice.is_empty() {
                app.set_theme(&theme_choice);
            }
        });
        let path = self.folder.to_string_lossy().to_string();
        self.call("project.open", json!({"path": path}), |app, result| {
            if let Err(error) = result {
                app.fail(&error);
            }
            app.after_open();
        });
    }

    fn after_open(&mut self) {
        let project = self.state.raw.get("project");
        if project.is_some_and(|p| !s(p, "pending_trust").is_empty()) {
            return;
        }
        self.refresh_files();
        if std::mem::take(&mut self.resume) {
            self.call("history", json!({}), |app, result| {
                let id = result
                    .ok()
                    .and_then(|h| h.get("sessions").and_then(Value::as_array).and_then(|l| l.first().map(|f| s(f, "id"))))
                    .unwrap_or_default();
                if id.is_empty() {
                    app.say("Nothing to continue");
                } else {
                    app.act("history.open", json!({"id": id}));
                }
            });
        }
    }

    fn ask_trust(&mut self) {
        let Some(project) = self.state.raw.get("project") else { return };
        let asking = s(project, "pending_trust");
        if asking.is_empty() {
            self.trust_asked.clear();
            return;
        }
        if asking == self.trust_asked {
            return;
        }
        self.trust_asked = asking.clone();
        let shown = s(project, "pending_display");
        let shown = if shown.is_empty() { asking } else { shown };
        self.ask(
            "Trust this folder?",
            &[&shown, "", "Experts can read, change and run things here."],
            "Trust",
            "Not now",
            |app, yes| {
                app.call("trust.answer", json!({"trusted": yes}), |app, result| {
                    if let Err(error) = result {
                        app.fail(&error);
                    }
                    app.after_open();
                });
                if !yes {
                    app.say("Working in the Scratchpad");
                }
            },
        );
    }

    pub fn refresh_files(&mut self) {
        self.tree_asked = Some(Instant::now());
        self.call("project.tree", json!({"depth": 6, "limit": 4000}), |app, result| {
            if let Ok(tree) = result {
                app.files = tree
                    .get("entries")
                    .and_then(Value::as_array)
                    .map(|list| {
                        list.iter()
                            .filter(|e| !state::b(e, "dir"))
                            .map(|e| s(e, "path"))
                            .filter(|p| !p.is_empty())
                            .collect()
                    })
                    .unwrap_or_default();
            }
        });
    }

    pub fn set_theme(&mut self, choice: &str) {
        self.theme_choice = choice.to_string();
        let light = match choice {
            "light" => true,
            "dark" => false,
            _ => crate::detected_light(),
        };
        if light != theme::is_light() {
            theme::set_light(light);
            self.feed.clear();
        }
    }

    // --- sending --------------------------------------------------------------

    /// Whatever was typed: a command, a shell line, or a prompt.
    pub fn send(&mut self) {
        let shown = self.input.text.trim().to_string();
        let text = self.input.expanded().trim().to_string();
        if text.is_empty() && self.attached.is_empty() {
            return;
        }
        self.input.remember(&shown);
        self.call("prefs.set", json!({"key": "cli_history", "value": self.input.history}), |_, _| {});
        self.input.clear();
        self.menu_at = 0;
        self.scroll = 0;

        if let Some(rest) = text.strip_prefix('/') {
            if commands::run(self, rest) {
                return;
            }
        }
        if let Some(line) = text.strip_prefix('!') {
            let line = line.trim();
            if !line.is_empty() {
                self.shell(line.to_string());
            }
            return;
        }
        if self.state.busy {
            self.queue.push_back(text);
            self.say("Queued");
            return;
        }
        self.submit(text);
    }

    pub fn submit(&mut self, text: String) {
        let mut prompt = text;
        let mut expert = String::new();
        if let Some(rest) = prompt.strip_prefix('/') {
            if let Some((name, tail)) = rest.split_once(char::is_whitespace) {
                if let Some(id) = commands::expert_named(self, name) {
                    expert = id;
                    prompt = tail.trim().to_string();
                }
            }
        }
        if expert.is_empty() && self.testing.is_some() {
            expert = "lab-test".to_string();
        }
        if expert.is_empty() && self.route_target().is_some() {
            expert = self.route.clone();
        }
        let mut paths: VecDeque<String> = self.attached.drain(..).collect();
        for word in prompt.split_whitespace() {
            let Some(name) = word.strip_prefix('@') else { continue };
            let name = name.trim_end_matches([',', '.', ':', ';', ')']);
            if name.is_empty() {
                continue;
            }
            let root = self.state.raw.get("project").map(|p| s(p, "root")).unwrap_or_default();
            let base = if root.is_empty() { self.folder.clone() } else { PathBuf::from(root) };
            let path = base.join(name);
            if path.is_file() {
                let path = path.to_string_lossy().to_string();
                if !paths.contains(&path) {
                    paths.push_back(path);
                }
            }
        }
        let route = self.route_target().unwrap_or_default();
        let params = json!({"prompt": prompt, "expert": expert, "route": route});
        self.attach_then_send(params, paths, Vec::new());
    }

    /// The engine is handed a picture's bytes with its path; any other file
    /// it reads for itself. One at a time, then the prompt.
    fn attach_then_send(&mut self, mut params: Value, mut paths: VecDeque<String>, mut done: Vec<Value>) {
        let Some(path) = paths.pop_front() else {
            params["attachments"] = json!(done);
            return self.call("submit", params, |app, result| {
                if let Err(error) = result {
                    app.fail(&error);
                }
            });
        };
        let extension = path.rsplit('.').next().unwrap_or("").to_lowercase();
        if !["png", "jpg", "jpeg", "webp", "gif", "bmp"].contains(&extension.as_str()) {
            done.push(json!({"path": path}));
            return self.attach_then_send(params, paths, done);
        }
        let asked = path.clone();
        self.call("attach.image", json!({"path": path}), move |app, result| {
            match result {
                Ok(image) => done.push(json!({"path": asked, "image": {"mime": s(&image, "mime"), "data": s(&image, "data")}})),
                Err(error) => {
                    app.fail(&error);
                    done.push(json!({"path": asked}));
                }
            }
            app.attach_then_send(params, paths, done);
        });
    }

    /// A new chat, in the folder this terminal is in. The engine's new chat
    /// has no folder until its first message, which is right for a window
    /// and wrong here.
    pub fn new_chat(&mut self) {
        let project = self.state.raw.get("project").cloned().unwrap_or(Value::Null);
        let keep = state::b(&project, "open") && !state::b(&project, "scratchpad");
        let root = s(&project, "root");
        self.call("session.new", json!({}), move |app, result| {
            if let Err(error) = result {
                return app.fail(&error);
            }
            if keep {
                app.call("project.open", json!({"path": root}), |app, result| {
                    if let Err(error) = result {
                        app.fail(&error);
                    }
                });
            }
        });
    }

    /// Where prompts go: an expert with a model, or None for the delegator.
    pub fn route_target(&self) -> Option<String> {
        if self.route.is_empty() {
            return None;
        }
        self.state.experts.iter().find(|e| e.id == self.route && e.phase != "unconfigured").map(|e| e.id.clone())
    }

    pub fn shell(&mut self, line: String) {
        let at = self.state.turns.len();
        let index = self.local.len();
        self.local.push(Local { after: at, action: Action { summary: format!("{line}  (running)"), ..Default::default() } });
        let shown = line.clone();
        self.call("project.run", json!({"command": line, "timeout": 600}), move |app, result| {
            let action = match result {
                Ok(done) => Action {
                    summary: crate::text::clean(&s(&done, "summary")),
                    body: crate::text::clean(&s(&done, "output")),
                    ..Default::default()
                },
                Err(error) => Action { summary: format!("could not run {shown}: {error}"), ..Default::default() },
            };
            if let Some(entry) = app.local.get_mut(index) {
                entry.action = action;
            }
            app.feed.clear();
        });
    }

    pub fn stop(&mut self) {
        self.queue.clear();
        self.act("cancel", json!({}));
        self.say("Stopping");
    }

    // --- keys and the mouse ---------------------------------------------------

    fn on_term(&mut self, event: Event) {
        match event {
            Event::Key(key) if key.kind != KeyEventKind::Release => self.on_key(key),
            Event::Paste(text) => self.on_paste(&text),
            Event::Mouse(mouse) => self.on_mouse(mouse),
            Event::Resize(..) => self.feed.clear(),
            _ => {}
        }
    }

    fn on_paste(&mut self, text: &str) {
        match &mut self.overlay {
            Some(Overlay::Typing(m)) => m.field.paste(text),
            Some(_) => {}
            None => match &mut self.screen {
                Screen::Chat => self.input.paste(text),
                Screen::Models(m) => m.query.paste(text),
                _ => {}
            },
        }
    }

    fn on_mouse(&mut self, mouse: MouseEvent) {
        let wheel = match mouse.kind {
            MouseEventKind::ScrollUp => -3,
            MouseEventKind::ScrollDown => 3,
            _ => 0,
        };
        if wheel != 0 {
            if self.overlay.is_some() {
                overlay::wheel(self, wheel);
            } else if matches!(self.screen, Screen::Chat) {
                self.scroll_by(-wheel);
            } else {
                screens::wheel(self, wheel);
            }
            return;
        }
        match mouse.kind {
            MouseEventKind::Down(MouseButton::Left) => {
                self.selection = Some(Selection { from: (mouse.column, mouse.row), to: (mouse.column, mouse.row), done: false });
            }
            MouseEventKind::Drag(MouseButton::Left) => {
                if let Some(selection) = &mut self.selection {
                    selection.to = (mouse.column, mouse.row);
                }
            }
            MouseEventKind::Up(MouseButton::Left) => {
                if let Some(selection) = &mut self.selection {
                    if selection.from == selection.to {
                        self.selection = None;
                    } else {
                        selection.done = true;
                    }
                }
            }
            _ => {}
        }
    }

    pub fn scroll_by(&mut self, rows: isize) {
        let most = self.feed_total.saturating_sub(self.feed_area.height as usize);
        self.scroll = (self.scroll as isize + rows).clamp(0, most as isize) as usize;
    }

    pub fn on_key(&mut self, key: KeyEvent) {
        self.selection = None;
        if self.overlay.is_some() {
            overlay::key(self, key);
            return;
        }
        match self.screen {
            Screen::Chat => self.chat_key(key),
            Screen::Settings(_) => settings::key(self, key),
            Screen::Models(_) => models::key(self, key),
            Screen::Page(_) => screens::key(self, key),
        }
    }

    fn input_cols(&self) -> usize {
        (self.feed_area.width as usize).saturating_sub(5).max(10)
    }

    fn chat_key(&mut self, key: KeyEvent) {
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        let shift = key.modifiers.contains(KeyModifiers::SHIFT);
        let page = (self.feed_area.height as isize - 2).max(1);

        // An edit waiting on an answer takes the keys that answer it, while
        // the box is empty: a y typed into a sentence is not a yes.
        if self.state.pending_edit.is_some() && self.input.text.is_empty() {
            match key.code {
                KeyCode::Char('y') | KeyCode::Char('Y') | KeyCode::Enter if !ctrl => {
                    self.act("edit.approve", json!({"approved": true}));
                    return;
                }
                KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc if !ctrl => {
                    self.act("edit.approve", json!({"approved": false}));
                    return;
                }
                KeyCode::Char('a') | KeyCode::Char('A') if !ctrl => {
                    self.configure(json!({"tools": {"auto_edits": true}}));
                    self.act("edit.approve", json!({"approved": true}));
                    return;
                }
                _ => {}
            }
        }

        match key.code {
            KeyCode::Char('c') if ctrl => {
                if !self.input.text.is_empty() {
                    self.input.clear();
                    self.menu_at = 0;
                } else if self.state.busy {
                    self.stop();
                } else if self.ctrl_c.is_some_and(|t| t.elapsed() < Duration::from_secs(2)) {
                    self.quitting = true;
                } else {
                    self.ctrl_c = Some(Instant::now());
                    self.say("ctrl+c again to quit");
                }
            }
            KeyCode::Char('d') if ctrl => {
                if self.input.text.is_empty() {
                    self.quitting = true;
                }
            }
            KeyCode::Char('o') if ctrl => {
                self.open = !self.open;
                self.feed.clear();
            }
            KeyCode::Char('l') if ctrl => {
                self.feed.clear();
            }
            KeyCode::Char('z') if ctrl => self.external = Some(External::Suspend),
            KeyCode::Esc => {
                if commands::menu_open(self) {
                    self.input.clear();
                } else if self.state.busy {
                    self.stop();
                } else if !self.input.text.is_empty() {
                    if self.esc.is_some_and(|t| t.elapsed() < Duration::from_millis(900)) {
                        self.input.clear();
                        self.esc = None;
                    } else {
                        self.esc = Some(Instant::now());
                        self.say("esc again to clear");
                    }
                } else if self.scroll > 0 {
                    self.scroll = 0;
                }
            }
            KeyCode::BackTab => commands::toggle_auto(self),
            KeyCode::PageUp => self.scroll_by(page),
            KeyCode::PageDown => self.scroll_by(-page),
            KeyCode::Up if shift => self.scroll_by(1),
            KeyCode::Down if shift => self.scroll_by(-1),
            KeyCode::End if ctrl => self.scroll = 0,
            KeyCode::Home if ctrl => self.scroll = usize::MAX / 2,
            KeyCode::Tab => commands::complete(self),
            KeyCode::Up if commands::menu_open(self) => self.menu_at = self.menu_at.saturating_sub(1),
            KeyCode::Down if commands::menu_open(self) => {
                let last = commands::menu(self).len().saturating_sub(1);
                self.menu_at = (self.menu_at + 1).min(last);
            }
            KeyCode::Enter if !shift && !key.modifiers.contains(KeyModifiers::ALT) => {
                if self.input.text[..self.input.at].ends_with('\\') {
                    let at = self.input.at;
                    self.input.text.replace_range(at - 1..at, "\n");
                } else if commands::menu_open(self) {
                    commands::run_chosen(self);
                } else {
                    self.send();
                }
            }
            _ => {
                let cols = self.input_cols();
                let before = self.input.text.len();
                self.input.key(key, cols);
                if self.input.text.len() != before {
                    self.menu_at = 0;
                    commands::after_typing(self);
                }
            }
        }
    }

    pub fn back_to_chat(&mut self) {
        self.screen = Screen::Chat;
        self.feed.clear();
    }

    pub fn animating(&self) -> bool {
        self.state.busy
            || self.began.is_some()
            || self.state.raw.get("setup").is_some_and(|x| state::b(x, "running"))
            || self.state.raw.get("downloads").is_some()
            || models::waiting(self)
    }
}
