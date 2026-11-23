// SPDX-License-Identifier: MIT
//! The text box: a small editor with a caret, word moves and history.

use crate::text::char_width;
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};

#[derive(Default)]
pub struct Composer {
    pub text: String,
    pub at: usize,
    pub multi: bool,
    want: Option<usize>,
    pub history: Vec<String>,
    place: Option<usize>,
    draft: String,
    pub pastes: Vec<String>,
}

fn word_left(text: &str, from: usize) -> usize {
    let head = &text[..from];
    let trimmed = head.trim_end();
    match trimmed.char_indices().rev().find(|(_, c)| c.is_whitespace() || "/.,:;()[]{}\"'".contains(*c)) {
        Some((i, c)) => i + c.len_utf8(),
        None => 0,
    }
}

fn word_right(text: &str, from: usize) -> usize {
    let tail = &text[from..];
    let skip = tail.len() - tail.trim_start().len();
    let rest = &tail[skip..];
    let word = rest.find(|c: char| c.is_whitespace() || "/.,:;()[]{}\"'".contains(c)).map_or(rest.len(), |i| i.max(1));
    let mut to = from + skip + word;
    while !text.is_char_boundary(to) {
        to += 1;
    }
    to
}

impl Composer {
    pub fn multiline() -> Composer {
        Composer { multi: true, ..Default::default() }
    }

    pub fn with(text: &str, multi: bool) -> Composer {
        Composer { text: text.to_string(), at: text.len(), multi, ..Default::default() }
    }

    pub fn set(&mut self, text: &str) {
        self.text = text.to_string();
        self.at = text.len();
        self.want = None;
    }

    pub fn clear(&mut self) {
        self.set("");
        self.pastes.clear();
        self.place = None;
    }

    pub fn insert(&mut self, piece: &str) {
        self.text.insert_str(self.at, piece);
        self.at += piece.len();
        self.want = None;
        self.place = None;
    }

    /// Something pasted. A long one sits in the box as a chip rather than
    /// filling it, and goes out whole.
    pub fn paste(&mut self, piece: &str) {
        let piece = piece.replace("\r\n", "\n").replace('\r', "\n");
        let lines = piece.lines().count();
        if self.multi && (lines > 6 || piece.len() > 1200) {
            self.pastes.push(piece);
            let chip = format!("[pasted {} lines #{}]", lines, self.pastes.len());
            self.insert(&chip);
        } else if self.multi {
            self.insert(&piece);
        } else {
            self.insert(&piece.replace('\n', " "));
        }
    }

    /// What is in the box, with the chips back to what they stand for.
    pub fn expanded(&self) -> String {
        let mut out = self.text.clone();
        for (i, piece) in self.pastes.iter().enumerate() {
            let chip = format!("[pasted {} lines #{}]", piece.lines().count(), i + 1);
            out = out.replace(&chip, piece);
        }
        out
    }

    pub fn remember(&mut self, entry: &str) {
        if entry.trim().is_empty() || self.history.last().is_some_and(|l| l == entry) {
            return;
        }
        self.history.push(entry.to_string());
        if self.history.len() > 300 {
            self.history.remove(0);
        }
    }

    fn older(&mut self) -> bool {
        if self.history.is_empty() {
            return false;
        }
        let next = match self.place {
            None => {
                self.draft = self.text.clone();
                self.history.len() - 1
            }
            Some(0) => return true,
            Some(n) => n - 1,
        };
        self.place = Some(next);
        let entry = self.history[next].clone();
        self.set(&entry);
        true
    }

    fn newer(&mut self) -> bool {
        let Some(now) = self.place else { return false };
        if now + 1 < self.history.len() {
            self.place = Some(now + 1);
            let entry = self.history[now + 1].clone();
            self.set(&entry);
        } else {
            self.place = None;
            let draft = std::mem::take(&mut self.draft);
            self.set(&draft);
        }
        true
    }

    /// The byte range of each row the text takes at this width.
    pub fn rows(&self, cols: usize) -> Vec<(usize, usize)> {
        let cols = cols.max(1);
        let mut out = Vec::new();
        let mut start = 0;
        let mut used = 0;
        for (i, c) in self.text.char_indices() {
            if c == '\n' {
                out.push((start, i));
                start = i + 1;
                used = 0;
                continue;
            }
            let w = char_width(c);
            if used + w > cols {
                out.push((start, i));
                start = i;
                used = 0;
            }
            used += w;
        }
        out.push((start, self.text.len()));
        out
    }

    /// Which row the caret is on, and how far along it.
    pub fn caret(&self, cols: usize) -> (usize, usize) {
        let rows = self.rows(cols);
        let mut row = rows.len() - 1;
        for (i, (start, end)) in rows.iter().enumerate() {
            if self.at >= *start && self.at <= *end {
                row = i;
                if self.at < *end || i + 1 == rows.len() || rows[i + 1].0 > *end {
                    break;
                }
            }
        }
        let col = self.text[rows[row].0..self.at.max(rows[row].0)].chars().map(char_width).sum();
        (row, col)
    }

    fn move_row(&mut self, cols: usize, by: isize) -> bool {
        let rows = self.rows(cols);
        let (row, col) = self.caret(cols);
        let target = row as isize + by;
        if target < 0 || target >= rows.len() as isize {
            return false;
        }
        let want = *self.want.get_or_insert(col);
        let (start, end) = rows[target as usize];
        let mut at = start;
        let mut used = 0;
        for (i, c) in self.text[start..end].char_indices() {
            if used >= want {
                break;
            }
            used += char_width(c);
            at = start + i + c.len_utf8();
        }
        self.at = at;
        true
    }

    fn line_start(&self) -> usize {
        self.text[..self.at].rfind('\n').map_or(0, |i| i + 1)
    }

    fn line_end(&self) -> usize {
        self.text[self.at..].find('\n').map_or(self.text.len(), |i| self.at + i)
    }

    fn back(&self) -> usize {
        self.text[..self.at].char_indices().next_back().map_or(0, |(i, _)| i)
    }

    fn forward(&self) -> usize {
        self.text[self.at..].chars().next().map_or(self.at, |c| self.at + c.len_utf8())
    }

    fn cut(&mut self, from: usize, to: usize) {
        self.text.replace_range(from..to, "");
        self.at = from;
        self.want = None;
        self.place = None;
    }

    /// A key that edits or moves. False when it means nothing here, for the
    /// caller to do something with.
    pub fn key(&mut self, key: KeyEvent, cols: usize) -> bool {
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        let alt = key.modifiers.contains(KeyModifiers::ALT);
        let keep_want = matches!(key.code, KeyCode::Up | KeyCode::Down);
        let handled = match key.code {
            KeyCode::Char('a') if ctrl => { self.at = self.line_start(); true }
            KeyCode::Char('e') if ctrl => { self.at = self.line_end(); true }
            KeyCode::Char('b') if ctrl => { self.at = self.back(); true }
            KeyCode::Char('f') if ctrl => { self.at = self.forward(); true }
            KeyCode::Char('b') if alt => { self.at = word_left(&self.text, self.at); true }
            KeyCode::Char('f') if alt => { self.at = word_right(&self.text, self.at); true }
            KeyCode::Char('u') if ctrl => { let s = self.line_start(); self.cut(s, self.at); true }
            KeyCode::Char('k') if ctrl => { let e = self.line_end(); self.cut(self.at, e); true }
            KeyCode::Char('w') if ctrl => { let s = word_left(&self.text, self.at); self.cut(s, self.at); true }
            KeyCode::Char('d') if alt => { let e = word_right(&self.text, self.at); self.cut(self.at, e); true }
            KeyCode::Char('j') if ctrl && self.multi => { self.insert("\n"); true }
            KeyCode::Char(c) if !ctrl && !alt => {
                let mut buffer = [0u8; 4];
                self.insert(c.encode_utf8(&mut buffer));
                true
            }
            KeyCode::Enter if self.multi && (alt || key.modifiers.contains(KeyModifiers::SHIFT)) => {
                self.insert("\n");
                true
            }
            KeyCode::Backspace if ctrl || alt => { let s = word_left(&self.text, self.at); self.cut(s, self.at); true }
            KeyCode::Backspace => {
                if self.at > 0 {
                    let s = self.back();
                    self.cut(s, self.at);
                }
                true
            }
            KeyCode::Delete => {
                if self.at < self.text.len() {
                    let e = self.forward();
                    self.cut(self.at, e);
                }
                true
            }
            KeyCode::Left if ctrl || alt => { self.at = word_left(&self.text, self.at); true }
            KeyCode::Right if ctrl || alt => { self.at = word_right(&self.text, self.at); true }
            KeyCode::Left => { self.at = self.back(); true }
            KeyCode::Right => { self.at = self.forward(); true }
            KeyCode::Home => { self.at = self.line_start(); true }
            KeyCode::End => { self.at = self.line_end(); true }
            KeyCode::Up => self.move_row(cols, -1) || (self.multi && self.older()),
            KeyCode::Down => self.move_row(cols, 1) || (self.multi && self.newer()),
            _ => false,
        };
        if handled && !keep_want {
            self.want = None;
        }
        handled
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn press(c: &mut Composer, code: KeyCode, mods: KeyModifiers) {
        c.key(KeyEvent::new(code, mods), 20);
    }

    #[test]
    fn typing_moving_and_deleting() {
        let mut c = Composer::multiline();
        for ch in "héllo wörld".chars() {
            press(&mut c, KeyCode::Char(ch), KeyModifiers::NONE);
        }
        press(&mut c, KeyCode::Left, KeyModifiers::CONTROL);
        assert_eq!(&c.text[c.at..], "wörld");
        press(&mut c, KeyCode::Backspace, KeyModifiers::NONE);
        assert_eq!(c.text, "héllowörld");
        press(&mut c, KeyCode::Char('u'), KeyModifiers::CONTROL);
        assert_eq!(c.text, "wörld");
        press(&mut c, KeyCode::End, KeyModifiers::NONE);
        press(&mut c, KeyCode::Char('w'), KeyModifiers::CONTROL);
        assert_eq!(c.text, "");
    }

    #[test]
    fn up_and_down_walk_rows_then_history() {
        let mut c = Composer::multiline();
        c.remember("first");
        c.remember("second");
        c.set("draft");
        press(&mut c, KeyCode::Up, KeyModifiers::NONE);
        assert_eq!(c.text, "second");
        press(&mut c, KeyCode::Up, KeyModifiers::NONE);
        assert_eq!(c.text, "first");
        press(&mut c, KeyCode::Down, KeyModifiers::NONE);
        press(&mut c, KeyCode::Down, KeyModifiers::NONE);
        assert_eq!(c.text, "draft");

        c.set("one\ntwo");
        press(&mut c, KeyCode::Up, KeyModifiers::NONE);
        assert_eq!(c.caret(20).0, 0);
        assert_eq!(c.text, "one\ntwo");
    }

    #[test]
    fn the_caret_is_where_the_rows_say() {
        let mut c = Composer::multiline();
        c.set("abcdefghij");
        assert_eq!(c.rows(4), vec![(0, 4), (4, 8), (8, 10)]);
        assert_eq!(c.caret(4), (2, 2));
        c.at = 4;
        assert_eq!(c.caret(4).0, 1);
    }

    #[test]
    fn a_long_paste_is_a_chip_that_goes_out_whole() {
        let mut c = Composer::multiline();
        c.insert("look at ");
        let long: String = (0..20).map(|i| format!("line {i}\n")).collect();
        c.paste(&long);
        assert!(c.text.starts_with("look at [pasted 20 lines #1]"));
        assert!(c.expanded().contains("line 19"));
        assert!(!c.expanded().contains("[pasted"));
    }

    #[test]
    fn a_one_line_field_takes_a_paste_as_one_line() {
        let mut c = Composer::with("", false);
        c.paste("a\nb\r\nc");
        assert_eq!(c.text, "a b c");
    }
}
