// SPDX-License-Identifier: MIT
//! cruciblecli: Crucible in the terminal, in the folder you typed it in.
//!
//! This program is the face. The work is done by crucible-engine, the same
//! engine the app runs, which it starts as a child and talks to over a pipe.

mod app;
mod commands;
mod composer;
mod diff;
mod engine;
mod form;
mod highlight;
mod markdown;
mod models;
mod overlay;
mod screens;
mod settings;
mod state;
mod text;
mod theme;
mod transcript;
mod ui;

use app::{App, External};
use engine::{Engine, Msg};
use ratatui::backend::CrosstermBackend;
use ratatui::crossterm::event::{
    self, DisableBracketedPaste, DisableMouseCapture, EnableBracketedPaste, EnableMouseCapture,
    KeyboardEnhancementFlags, PopKeyboardEnhancementFlags, PushKeyboardEnhancementFlags,
};
use ratatui::crossterm::terminal::{self, EnterAlternateScreen, LeaveAlternateScreen};
use ratatui::crossterm::{cursor, execute};
use ratatui::Terminal;
use serde_json::json;
use std::io::{self, IsTerminal, Write};
use std::path::PathBuf;
use std::process::{Command, ExitCode, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::Duration;

const HELP: &str = "\
Crucible in the terminal

Usage: cruciblecli [folder] [options]

  folder               Where to work. The current folder if none is given.

Options:
  -c, --continue       Pick up the last chat in this folder
  -p, --print PROMPT   Answer once, print it, and exit
      --update         Install the newest version
  -v, --version        Print the version
  -h, --help           Print this
";

const INSTALL_SH: &str = "curl -fsSL https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.sh | sh";
const INSTALL_PS1: &str = "irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install-cli.ps1 | iex";

pub fn update_command() -> String {
    if cfg!(windows) { INSTALL_PS1.to_string() } else { INSTALL_SH.to_string() }
}

static LIGHT_TERMINAL: AtomicBool = AtomicBool::new(false);

pub fn detected_light() -> bool {
    LIGHT_TERMINAL.load(Ordering::Relaxed)
}

/// Whether the terminal's background is a light one. It is asked, and a
/// terminal that does not answer is taken at its COLORFGBG, or as dark.
fn background_is_light() -> bool {
    #[cfg(unix)]
    {
        if let Some(light) = ask_background() {
            return light;
        }
    }
    std::env::var("COLORFGBG")
        .ok()
        .and_then(|v| v.rsplit(';').next().and_then(|b| b.parse::<u8>().ok()))
        .is_some_and(|bg| bg == 7 || bg == 15)
}

#[cfg(unix)]
fn ask_background() -> Option<bool> {
    // The second question is one every terminal answers, so one that does
    // not know the first is found out at once and not by waiting.
    let mut out = io::stdout();
    out.write_all(b"\x1b]11;?\x07\x1b[c").ok()?;
    out.flush().ok()?;
    let deadline = std::time::Instant::now() + Duration::from_millis(150);
    let mut got = Vec::new();
    loop {
        let left = deadline.saturating_duration_since(std::time::Instant::now()).as_millis() as i32;
        if left <= 0 {
            break;
        }
        let mut fds = libc::pollfd { fd: 0, events: libc::POLLIN, revents: 0 };
        // SAFETY: one valid pollfd, for the length given.
        if unsafe { libc::poll(&mut fds, 1, left) } <= 0 {
            break;
        }
        let mut chunk = [0u8; 256];
        // SAFETY: the buffer is valid for its length.
        let read = unsafe { libc::read(0, chunk.as_mut_ptr().cast(), chunk.len()) };
        if read <= 0 {
            break;
        }
        got.extend_from_slice(&chunk[..read as usize]);
        // The end of the answer to the second question.
        if got.windows(2).any(|w| w == b"[?") && got.ends_with(b"c") {
            break;
        }
    }
    let said = String::from_utf8_lossy(&got);
    let at = said.find("rgb:")?;
    let mut parts = said[at + 4..].split(['/', '\x07', '\x1b']);
    let mut channel = || -> Option<f64> {
        let hex = parts.next()?;
        let value = u32::from_str_radix(hex, 16).ok()? as f64;
        Some(value / ((1u64 << (hex.len() * 4)) - 1) as f64)
    };
    let (r, g, b) = (channel()?, channel()?, channel()?);
    Some(0.2126 * r + 0.7152 * g + 0.0722 * b > 0.5)
}

/// Into the clipboard, by whatever this machine has. The escape code is for
/// a terminal over SSH, where the machine's own clipboard is the wrong one.
pub fn copy_to_clipboard(content: &str) {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut encoded = String::with_capacity(content.len() * 4 / 3 + 4);
    for chunk in content.as_bytes().chunks(3) {
        let n = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        encoded.push(TABLE[(n >> 18) as usize & 63] as char);
        encoded.push(TABLE[(n >> 12) as usize & 63] as char);
        encoded.push(if chunk.len() > 1 { TABLE[(n >> 6) as usize & 63] as char } else { '=' });
        encoded.push(if chunk.len() > 2 { TABLE[n as usize & 63] as char } else { '=' });
    }
    let mut out = io::stdout();
    let _ = write!(out, "\x1b]52;c;{encoded}\x07");
    let _ = out.flush();

    let tools: &[(&str, &[&str])] = if cfg!(target_os = "macos") {
        &[("pbcopy", &[])]
    } else if cfg!(windows) {
        &[("clip", &[])]
    } else if std::env::var_os("WAYLAND_DISPLAY").is_some() {
        &[("wl-copy", &[]), ("xclip", &["-selection", "clipboard"])]
    } else {
        &[("xclip", &["-selection", "clipboard"]), ("xsel", &["-ib"])]
    };
    for (tool, args) in tools {
        let spawned = Command::new(tool).args(*args).stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null()).spawn();
        if let Ok(mut child) = spawned {
            if let Some(mut stdin) = child.stdin.take() {
                let _ = stdin.write_all(content.as_bytes());
            }
            std::thread::spawn(move || {
                let _ = child.wait();
            });
            return;
        }
    }
}

// --- the terminal -----------------------------------------------------------------

fn enter() -> io::Result<()> {
    terminal::enable_raw_mode()?;
    let mut out = io::stdout();
    execute!(out, EnterAlternateScreen, EnableMouseCapture, EnableBracketedPaste)?;
    // Where the terminal can tell Shift+Enter from Enter.
    if matches!(terminal::supports_keyboard_enhancement(), Ok(true)) {
        let _ = execute!(out, PushKeyboardEnhancementFlags(KeyboardEnhancementFlags::DISAMBIGUATE_ESCAPE_CODES));
        ENHANCED.store(true, Ordering::Relaxed);
    }
    Ok(())
}

static ENHANCED: AtomicBool = AtomicBool::new(false);

fn leave() {
    let mut out = io::stdout();
    if ENHANCED.swap(false, Ordering::Relaxed) {
        let _ = execute!(out, PopKeyboardEnhancementFlags);
    }
    let _ = execute!(out, DisableBracketedPaste, DisableMouseCapture, LeaveAlternateScreen, cursor::Show);
    let _ = terminal::disable_raw_mode();
}

fn install_panic_hook() {
    let earlier = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        leave();
        earlier(info);
    }));
}

/// Hand the terminal to something else, and take it back after.
fn with_terminal_away<B: ratatui::backend::Backend>(
    terminal: &mut Terminal<B>,
    paused: &AtomicBool,
    work: impl FnOnce(),
) {
    paused.store(true, Ordering::SeqCst);
    std::thread::sleep(Duration::from_millis(90));
    leave();
    work();
    let _ = enter();
    let _ = terminal.clear();
    paused.store(false, Ordering::SeqCst);
}

fn run_external<B: ratatui::backend::Backend>(app: &mut App, terminal: &mut Terminal<B>, paused: &AtomicBool, what: External) {
    match what {
        External::Suspend => with_terminal_away(terminal, paused, || {
            #[cfg(unix)]
            // SAFETY: raising a signal in this process.
            unsafe {
                libc::raise(libc::SIGTSTP);
            }
        }),
        External::Edit(path) => {
            let editor = std::env::var("VISUAL")
                .or_else(|_| std::env::var("EDITOR"))
                .unwrap_or_else(|_| if cfg!(windows) { "notepad".into() } else { "nano".into() });
            let mut failed = None;
            with_terminal_away(terminal, paused, || {
                let mut words = editor.split_whitespace();
                let program = words.next().unwrap_or("nano");
                if let Err(error) = Command::new(program).args(words).arg(&path).status() {
                    failed = Some(format!("{program}: {error}"));
                }
            });
            if let Some(error) = failed {
                app.fail(&error);
            }
            screens::after_editor(app);
        }
        External::Update(command) => {
            with_terminal_away(terminal, paused, || {
                let status = if cfg!(windows) {
                    Command::new("powershell").args(["-NoProfile", "-Command", &command]).status()
                } else {
                    Command::new("sh").args(["-c", &command]).status()
                };
                match status {
                    Ok(done) if done.success() => println!("\nUpdated. It takes effect the next time cruciblecli starts."),
                    _ => println!("\nThe update did not finish."),
                }
                println!("Press enter to go back.");
                let _ = io::stdin().read_line(&mut String::new());
            });
        }
    }
    app.feed.clear();
    app.dirty = true;
}

fn run_tui(engine_path: PathBuf, folder: PathBuf, resume: bool) -> Result<(), String> {
    let (tx, rx) = mpsc::channel::<Msg>();
    let engine = Engine::start(&engine_path, tx.clone()).map_err(|e| format!("could not start the engine: {e}"))?;

    install_panic_hook();
    terminal::enable_raw_mode().map_err(|e| e.to_string())?;
    let light = background_is_light();
    LIGHT_TERMINAL.store(light, Ordering::Relaxed);
    theme::set_light(light);
    let _ = terminal::disable_raw_mode();
    enter().map_err(|e| e.to_string())?;

    let paused = Arc::new(AtomicBool::new(false));
    {
        let paused = Arc::clone(&paused);
        let tx = tx.clone();
        std::thread::spawn(move || loop {
            if paused.load(Ordering::SeqCst) {
                std::thread::sleep(Duration::from_millis(25));
                continue;
            }
            if event::poll(Duration::from_millis(50)).unwrap_or(false) {
                if paused.load(Ordering::SeqCst) {
                    continue;
                }
                if let Ok(next) = event::read() {
                    if tx.send(Msg::Term(next)).is_err() {
                        return;
                    }
                }
            }
        });
    }

    let backend = CrosstermBackend::new(io::BufWriter::with_capacity(1 << 16, io::stdout()));
    let mut terminal = Terminal::new(backend).map_err(|e| e.to_string())?;
    terminal.clear().map_err(|e| e.to_string())?;
    let mut app = App::new(engine, folder, resume);
    let mut titled = PathBuf::new();

    let outcome = loop {
        if app.dirty {
            app.dirty = false;
            if app.folder != titled {
                titled = app.folder.clone();
                let name = titled.file_name().map_or_else(|| titled.to_string_lossy().to_string(), |n| n.to_string_lossy().to_string());
                let _ = execute!(io::stdout(), terminal::SetTitle(format!("Crucible: {name}")));
            }
            if let Err(error) = terminal.draw(|frame| ui::draw(frame, &mut app)) {
                break Err(error.to_string());
            }
        }
        if let Some(what) = app.external.take() {
            run_external(&mut app, &mut terminal, &paused, what);
            continue;
        }
        if app.quitting {
            break Ok(());
        }
        let wait = if app.animating() { Duration::from_millis(100) } else { Duration::from_millis(1000) };
        match rx.recv_timeout(wait) {
            Ok(msg) => {
                app.handle(msg);
                while let Ok(msg) = rx.try_recv() {
                    app.handle(msg);
                    if app.quitting {
                        break;
                    }
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if app.animating() || app.note.is_some() {
                    app.handle(Msg::Tick);
                }
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break Ok(()),
        }
    };

    let _ = execute!(io::stdout(), terminal::SetTitle(""));
    leave();
    let gone = app.gone.take();
    if gone.is_none() {
        app.engine.stop();
    }
    match (outcome, gone) {
        (Err(error), _) => Err(error),
        (Ok(()), Some(tail)) => {
            let mut said = "the engine stopped".to_string();
            if !tail.trim().is_empty() {
                said.push_str(":\n");
                said.push_str(&tail);
            }
            Err(said)
        }
        (Ok(()), None) => Ok(()),
    }
}

// --- once, with no screen -----------------------------------------------------------

fn run_print(engine_path: PathBuf, folder: PathBuf, prompt: String) -> Result<(), String> {
    let (tx, rx) = mpsc::channel::<Msg>();
    let mut engine = Engine::start(&engine_path, tx).map_err(|e| format!("could not start the engine: {e}"))?;
    let path = folder.to_string_lossy().to_string();
    let open_id = engine.call("project.open", json!({"path": path}));
    let mut submit_id = 0;
    let mut state = state::State::default();
    let mut started = false;
    let deadline_idle = Duration::from_secs(600);
    let outcome = loop {
        let msg = match rx.recv_timeout(deadline_idle) {
            Ok(msg) => msg,
            Err(_) => break Err("nothing happened for ten minutes".to_string()),
        };
        match msg {
            Msg::Reply(id, Err(error)) if id == open_id || id == submit_id => break Err(error),
            Msg::Reply(id, Ok(_)) if id == open_id => {
                submit_id = engine.call("submit", json!({"prompt": prompt}));
            }
            Msg::Push(value) => {
                state.apply(value);
                let pending = state.raw.get("project").map(|p| state::s(p, "pending_trust")).unwrap_or_default();
                if !pending.is_empty() {
                    break Err(format!("{pending} is not trusted. Open it with cruciblecli once and trust it."));
                }
                if state.pending_edit.is_some() {
                    engine.call("edit.approve", json!({"approved": false}));
                }
                if state.busy {
                    started = true;
                } else if started || state.turns.last().is_some_and(|t| !t.streaming && submit_id != 0 && !t.reply.is_empty()) {
                    let turn = state.turns.last().cloned();
                    match turn {
                        Some(t) if t.failed => break Err(t.reply.clone()),
                        Some(t) => {
                            println!("{}", t.reply.trim());
                            break Ok(());
                        }
                        None => break Err("no answer".to_string()),
                    }
                }
            }
            Msg::Gone(tail) => break Err(format!("the engine stopped: {tail}")),
            _ => {}
        }
    };
    engine.stop();
    outcome
}

fn main() -> ExitCode {
    let mut folder: Option<PathBuf> = None;
    let mut resume = false;
    let mut print = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                print!("{HELP}");
                return ExitCode::SUCCESS;
            }
            "-v" | "--version" => {
                println!("cruciblecli {}", env!("CARGO_PKG_VERSION"));
                return ExitCode::SUCCESS;
            }
            "--update" => {
                let command = update_command();
                let status = if cfg!(windows) {
                    Command::new("powershell").args(["-NoProfile", "-Command", &command]).status()
                } else {
                    Command::new("sh").args(["-c", &command]).status()
                };
                return if status.is_ok_and(|s| s.success()) { ExitCode::SUCCESS } else { ExitCode::FAILURE };
            }
            "-c" | "--continue" => resume = true,
            "-p" | "--print" => match args.next() {
                Some(prompt) => print = Some(prompt),
                None => {
                    eprintln!("cruciblecli: --print needs a prompt");
                    return ExitCode::FAILURE;
                }
            },
            other if other.starts_with('-') => {
                eprintln!("cruciblecli: no option {other}. See --help.");
                return ExitCode::FAILURE;
            }
            other => folder = Some(PathBuf::from(other)),
        }
    }

    let folder = match folder.map_or_else(std::env::current_dir, Ok).and_then(std::fs::canonicalize) {
        Ok(path) if path.is_dir() => path,
        Ok(path) => {
            eprintln!("cruciblecli: {} is not a folder", path.display());
            return ExitCode::FAILURE;
        }
        Err(error) => {
            eprintln!("cruciblecli: {error}");
            return ExitCode::FAILURE;
        }
    };
    let Some(engine_path) = engine::find() else {
        eprintln!("cruciblecli: crucible-engine is not installed beside it. Reinstall with:\n  {}", update_command());
        return ExitCode::FAILURE;
    };

    let outcome = if let Some(prompt) = print {
        run_print(engine_path, folder, prompt)
    } else {
        if !io::stdin().is_terminal() || !io::stdout().is_terminal() {
            eprintln!("cruciblecli: this needs a terminal. For a script, use --print.");
            return ExitCode::FAILURE;
        }
        run_tui(engine_path, folder, resume)
    };
    match outcome {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("cruciblecli: {error}");
            ExitCode::FAILURE
        }
    }
}
