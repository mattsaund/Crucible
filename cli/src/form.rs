// SPDX-License-Identifier: MIT
//! A page of settings: rows with a label, a value, and a way to change it.

use crate::text::{self, fit, pad, wrap};
use crate::theme::{self, p};
use ratatui::buffer::Buffer;
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};

#[derive(Clone)]
pub enum Kind {
    Head,
    Note,
    Bad,
    Toggle(bool),
    Num { value: f64, min: f64, max: f64, step: f64 },
    Text { value: String, secret: bool, multi: bool, placeholder: String },
    Choice { value: String, options: Vec<(String, String)> },
    Button,
}

#[derive(Clone)]
pub struct Row {
    pub id: String,
    pub label: String,
    pub hint: String,
    pub kind: Kind,
    pub off: bool,
}

impl Row {
    fn make(id: &str, label: &str, hint: &str, kind: Kind) -> Row {
        Row { id: id.into(), label: label.into(), hint: hint.into(), kind, off: false }
    }
    pub fn head(label: &str) -> Row {
        Row::make("", label, "", Kind::Head)
    }
    pub fn note(text: &str) -> Row {
        Row::make("", text, "", Kind::Note)
    }
    pub fn bad(text: &str) -> Row {
        Row::make("", text, "", Kind::Bad)
    }
    pub fn toggle(id: &str, label: &str, value: bool, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Toggle(value))
    }
    pub fn num(id: &str, label: &str, value: f64, range: (f64, f64, f64), hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Num { value, min: range.0, max: range.1, step: range.2 })
    }
    pub fn text(id: &str, label: &str, value: &str, placeholder: &str, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Text { value: value.into(), secret: false, multi: false, placeholder: placeholder.into() })
    }
    pub fn secret(id: &str, label: &str, value: &str, placeholder: &str, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Text { value: value.into(), secret: true, multi: false, placeholder: placeholder.into() })
    }
    pub fn long(id: &str, label: &str, value: &str, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Text { value: value.into(), secret: false, multi: true, placeholder: String::new() })
    }
    pub fn choice(id: &str, label: &str, value: &str, options: &[(&str, &str)], hint: &str) -> Row {
        let options = options.iter().map(|(v, l)| ((*v).to_string(), (*l).to_string())).collect();
        Row::make(id, label, hint, Kind::Choice { value: value.into(), options })
    }
    pub fn picks(id: &str, label: &str, value: &str, options: Vec<(String, String)>, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Choice { value: value.into(), options })
    }
    pub fn button(id: &str, label: &str, hint: &str) -> Row {
        Row::make(id, label, hint, Kind::Button)
    }
    pub fn disabled(mut self, off: bool) -> Row {
        self.off = off;
        self
    }
    pub fn can_land(&self) -> bool {
        !matches!(self.kind, Kind::Head | Kind::Note | Kind::Bad)
    }
}

#[derive(Default)]
pub struct Form {
    pub at: usize,
    pub top: usize,
}

pub enum Did {
    Nothing,
    Moved,
    Act,
    Step(f64),
}

impl Form {
    fn land(&mut self, rows: &[Row], by: isize) {
        if rows.is_empty() {
            return;
        }
        let mut i = self.at as isize;
        loop {
            i += by;
            if i < 0 || i >= rows.len() as isize {
                return;
            }
            if rows[i as usize].can_land() {
                self.at = i as usize;
                return;
            }
        }
    }

    /// On a row that can be landed on, after the rows changed under it.
    pub fn settle(&mut self, rows: &[Row]) {
        if rows.is_empty() {
            self.at = 0;
            return;
        }
        self.at = self.at.min(rows.len() - 1);
        if !rows[self.at].can_land() {
            let before = self.at;
            self.land(rows, 1);
            if self.at == before {
                self.land(rows, -1);
            }
        }
    }

    pub fn key(&mut self, rows: &[Row], key: KeyEvent) -> Did {
        self.settle(rows);
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        match key.code {
            KeyCode::Up | KeyCode::Char('k') => self.land(rows, -1),
            KeyCode::Down | KeyCode::Char('j') => self.land(rows, 1),
            KeyCode::Char('p') if ctrl => self.land(rows, -1),
            KeyCode::Char('n') if ctrl => self.land(rows, 1),
            KeyCode::PageUp => {
                for _ in 0..8 {
                    self.land(rows, -1);
                }
            }
            KeyCode::PageDown => {
                for _ in 0..8 {
                    self.land(rows, 1);
                }
            }
            KeyCode::Home | KeyCode::Char('g') => {
                self.at = 0;
                self.settle(rows);
            }
            KeyCode::End | KeyCode::Char('G') => {
                self.at = rows.len().saturating_sub(1);
                if !rows.is_empty() && !rows[self.at].can_land() {
                    self.land(rows, -1);
                }
            }
            KeyCode::Enter | KeyCode::Char(' ') => {
                return if rows.get(self.at).is_some_and(|r| r.can_land() && !r.off) { Did::Act } else { Did::Nothing };
            }
            KeyCode::Left | KeyCode::Char('h') | KeyCode::Right | KeyCode::Char('l') => {
                let by = if matches!(key.code, KeyCode::Left | KeyCode::Char('h')) { -1.0 } else { 1.0 };
                return match rows.get(self.at).map(|r| (&r.kind, r.off)) {
                    Some((Kind::Num { .. } | Kind::Toggle(_) | Kind::Choice { .. }, false)) => Did::Step(by),
                    _ => Did::Nothing,
                };
            }
            _ => return Did::Nothing,
        }
        Did::Moved
    }

    pub fn draw(&mut self, buf: &mut Buffer, area: Rect, rows: &[Row]) {
        self.settle(rows);
        let cols = area.width as usize;
        let label_cols = (cols / 3).clamp(18, 34);
        let mut lines: Vec<Line<'static>> = Vec::new();
        let mut spans_of: Vec<(usize, usize)> = Vec::with_capacity(rows.len());
        for (i, row) in rows.iter().enumerate() {
            let start = lines.len();
            let here = i == self.at && row.can_land();
            row_lines(row, here, cols, label_cols, &mut lines);
            spans_of.push((start, lines.len()));
        }
        let height = area.height as usize;
        if let Some((start, end)) = spans_of.get(self.at) {
            if *start < self.top {
                self.top = *start;
                // A heading right above stays with what it heads.
                if self.at > 0 && matches!(rows[self.at - 1].kind, Kind::Head) {
                    self.top = spans_of[self.at - 1].0;
                }
            } else if *end > self.top + height {
                self.top = end - height;
            }
        }
        self.top = self.top.min(lines.len().saturating_sub(height));
        for (y, line) in lines.iter().skip(self.top).take(height).enumerate() {
            buf.set_line(area.x, area.y + y as u16, line, area.width);
        }
    }
}

fn slider(value: f64, min: f64, max: f64, step: f64) -> Vec<Span<'static>> {
    const CELLS: usize = 14;
    let share = if max > min { ((value - min) / (max - min)).clamp(0.0, 1.0) } else { 0.0 };
    let at = (share * (CELLS - 1) as f64).round() as usize;
    let number = if step.fract() == 0.0 { format!("{value:.0}") } else { format!("{value:.2}") };
    vec![
        Span::styled("━".repeat(at), theme::flame()),
        Span::styled("●", theme::lit()),
        Span::styled("━".repeat(CELLS - 1 - at), theme::faint()),
        Span::styled(format!("  {number}"), theme::text()),
    ]
}

fn row_lines(row: &Row, here: bool, cols: usize, label_cols: usize, out: &mut Vec<Line<'static>>) {
    match &row.kind {
        Kind::Head => {
            if !out.is_empty() {
                out.push(Line::default());
            }
            out.push(Line::from(Span::styled(format!(" {}", row.label), theme::bold(theme::flame()))));
            return;
        }
        Kind::Note | Kind::Bad => {
            let style = if matches!(row.kind, Kind::Bad) { theme::bad() } else { theme::dim() };
            let lead = [Span::raw(" ")];
            for text_line in row.label.lines() {
                out.extend(wrap(&[Span::styled(text_line.to_string(), style)], cols.saturating_sub(1), &lead, &lead));
            }
            return;
        }
        _ => {}
    }

    let base = if row.off { theme::faint() } else { theme::text() };
    let value: Vec<Span<'static>> = match &row.kind {
        Kind::Toggle(on) => vec![
            Span::styled(if *on { "■ " } else { "□ " }, if *on { theme::flame() } else { theme::dim() }),
            Span::styled(if *on { "On" } else { "Off" }, if *on { theme::text() } else { theme::dim() }),
        ],
        Kind::Num { value, min, max, step } if *max > *min => slider(*value, *min, *max, *step),
        Kind::Num { value, step, .. } => {
            let shown = if step.fract() == 0.0 { format!("{value:.0}") } else { format!("{value}") };
            vec![Span::styled(shown, base)]
        }
        Kind::Text { value, secret, multi, placeholder } => {
            let room = cols.saturating_sub(label_cols + 4);
            if value.is_empty() {
                vec![Span::styled(fit(placeholder, room), theme::faint())]
            } else if *secret {
                vec![Span::styled("•".repeat(value.chars().count().min(16)), base)]
            } else if *multi {
                let first = value.lines().next().unwrap_or("");
                let more = value.lines().count();
                let tail = if more > 1 { format!("  +{} lines", more - 1) } else { String::new() };
                vec![Span::styled(fit(first, room.saturating_sub(tail.len())), base), Span::styled(tail, theme::faint())]
            } else {
                vec![Span::styled(fit(value, room), base)]
            }
        }
        Kind::Choice { value, options } => {
            let shown = options.iter().find(|(v, _)| v == value).map_or(value.as_str(), |(_, l)| l.as_str());
            let room = cols.saturating_sub(label_cols + 6);
            let shown = if shown.is_empty() { "(none)" } else { shown };
            vec![Span::styled(fit(shown, room), base), Span::styled(" ▾", theme::faint())]
        }
        _ => Vec::new(),
    };

    let mut spans: Vec<Span<'static>> = Vec::new();
    let marker = if here { "❯ " } else { "  " };
    spans.push(Span::styled(marker, theme::flame()));
    if matches!(row.kind, Kind::Button) {
        let style = if row.off {
            theme::faint()
        } else if here {
            Style::default().fg(p().ink).bg(p().flame)
        } else {
            theme::lit()
        };
        spans.push(Span::styled(format!(" {} ", row.label), style));
        if !row.hint.is_empty() {
            spans.push(Span::styled(format!("  {}", fit(&row.hint, cols.saturating_sub(text::width(&row.label) + 8))), theme::dim()));
        }
    } else {
        spans.push(Span::styled(pad(&row.label, label_cols), if row.off { theme::faint() } else if here { theme::text() } else { theme::dim() }));
        spans.extend(value);
    }
    let mut line = Line::from(spans);
    if here {
        let used = text::line_width(&line.spans);
        if used < cols {
            line.spans.push(Span::raw(" ".repeat(cols - used)));
        }
        for span in &mut line.spans {
            if span.style.bg.is_none() {
                span.style = span.style.bg(p().raised);
            }
        }
    }
    out.push(line);
    if here && !row.hint.is_empty() && !matches!(row.kind, Kind::Button) {
        let lead = [Span::raw(format!("  {}", " ".repeat(label_cols)))];
        out.extend(wrap(&[Span::styled(row.hint.clone(), theme::faint())], cols, &lead, &lead));
    }
}
