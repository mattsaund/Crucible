// SPDX-License-Identifier: MIT
//! The engine: crucible-engine, as a child, spoken to in JSON one line at a
//! time. Requests go in, replies and the state come out. See
//! src/gui/headless.cpp for the other end.

use ratatui::crossterm::event::Event as TermEvent;
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub enum Msg {
    Term(TermEvent),
    Push(Value),
    Reply(u64, Result<Value, String>),
    Gone(String),
    Tick,
}

pub struct Engine {
    child: Child,
    stdin: Option<ChildStdin>,
    next: u64,
    pub said: Arc<Mutex<VecDeque<String>>>,
}

const ENGINE: &str = if cfg!(windows) { "crucible-engine.exe" } else { "crucible-engine" };

/// Beside this program, as the download lays it out, or in a build tree.
pub fn find() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("CRUCIBLE_ENGINE") {
        return Some(PathBuf::from(path));
    }
    let me = std::env::current_exe().ok()?;
    let me = std::fs::canonicalize(&me).unwrap_or(me);
    let here = me.parent()?;
    let mut places = vec![here.join(ENGINE), here.join("../lib/crucible-cli").join(ENGINE)];
    for up in here.ancestors().take(5) {
        places.push(up.join("build/bin").join(ENGINE));
    }
    places.into_iter().find(|p| p.is_file())
}

impl Engine {
    pub fn start(program: &Path, tx: Sender<Msg>) -> io::Result<Engine> {
        let mut child = Command::new(program)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().expect("a piped stdout");
        let stderr = child.stderr.take().expect("a piped stderr");

        let said = Arc::new(Mutex::new(VecDeque::new()));
        let keep = Arc::clone(&said);
        std::thread::spawn(move || {
            for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                let mut lines = keep.lock().unwrap();
                if lines.len() >= 200 {
                    lines.pop_front();
                }
                lines.push_back(line);
            }
        });

        let last = Arc::clone(&said);
        std::thread::spawn(move || {
            let mut reader = BufReader::with_capacity(1 << 20, stdout);
            let mut raw = Vec::new();
            loop {
                raw.clear();
                match reader.read_until(b'\n', &mut raw) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
                let Ok(mut doc) = serde_json::from_slice::<Value>(&raw) else { continue };
                let msg = if let Some(state) = doc.get_mut("push") {
                    Msg::Push(state.take())
                } else {
                    let id = doc.get("id").and_then(Value::as_u64).unwrap_or(0);
                    if doc.get("ok").and_then(Value::as_bool) == Some(true) {
                        Msg::Reply(id, Ok(doc.get_mut("result").map(Value::take).unwrap_or(Value::Null)))
                    } else {
                        let error = doc.get("error").and_then(Value::as_str).unwrap_or("failed").to_string();
                        Msg::Reply(id, Err(error))
                    }
                };
                if tx.send(msg).is_err() {
                    return;
                }
            }
            std::thread::sleep(Duration::from_millis(100));
            let tail: Vec<String> = last.lock().unwrap().iter().rev().take(12).rev().cloned().collect();
            let _ = tx.send(Msg::Gone(tail.join("\n")));
        });
        Ok(Engine { child, stdin, next: 1, said })
    }

    /// Ask for something. The answer arrives as `Msg::Reply` with this id.
    pub fn call(&mut self, method: &str, params: Value) -> u64 {
        let id = self.next;
        self.next += 1;
        let mut line = json!({"id": id, "method": method, "params": params}).to_string();
        line.push('\n');
        if let Some(stdin) = &mut self.stdin {
            let _ = stdin.write_all(line.as_bytes()).and_then(|()| stdin.flush());
        }
        id
    }

    /// Closing its input is how it is told. A model being unloaded can take a
    /// moment; one that will not go after that is put down.
    pub fn stop(&mut self) {
        self.stdin = None;
        let deadline = Instant::now() + Duration::from_secs(6);
        while Instant::now() < deadline {
            if matches!(self.child.try_wait(), Ok(Some(_))) {
                return;
            }
            std::thread::sleep(Duration::from_millis(30));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
