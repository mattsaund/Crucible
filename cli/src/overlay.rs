// SPDX-License-Identifier: MIT
//! What opens over everything else: a list to choose from, a question, a box
//! to type in, something to read.

use crate::app::App;
use crate::composer::Composer;
use crate::text::{self, fit, wrap};
use crate::theme::{self, p};
use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Clear, Widget};
use ratatui::Frame;

#[derive(Clone, Default)]
pub struct Item {
    pub label: String,
    pub hint: String,
    pub value: String,
    pub on: bool,
    pub off: bool,
}

impl Item {
    pub fn new(label: &str, value: &str) -> Item {
        Item { label: label.into(), value: value.into(), ..Default::default() }
    }
    pub fn hint(mut self, hint: &str) -> Item {
        self.hint = hint.into();
        self
    }
    pub fn on(mut self, on: bool) -> Item {
        self.on = on;
        self
    }
}

pub struct Picker {
    pub title: String,
    pub items: Vec<Item>,
    pub at: usize,
    pub filter: String,
    pub then: Option<Box<dyn FnOnce(&mut App, Item)>>,
}

impl Picker {
    fn shown(&self) -> Vec<usize> {
        let wanted = self.filter.to_lowercase();
        self.items
            .iter()
            .enumerate()
            .filter(|(_, item)| wanted.is_empty() || item.label.to_lowercase().contains(&wanted) || item.hint.to_lowercase().contains(&wanted))
            .map(|(i, _)| i)
            .collect()
    }
}

pub struct Ask {
    pub title: String,
    pub body: Vec<String>,
    pub yes: String,
    pub no: String,
    pub on_yes: bool,
    pub then: Option<Box<dyn FnOnce(&mut App, bool)>>,
}

pub struct Typing {
    pub title: String,
    pub hint: String,
    pub secret: bool,
    pub field: Composer,
    pub then: Option<Box<dyn FnOnce(&mut App, String)>>,
}

pub struct Reader {
    pub title: String,
    pub rows: Vec<Line<'static>>,
    pub top: usize,
    pub height: usize,
}

pub enum Overlay {
    Picker(Picker),
    Ask(Ask),
    Typing(Typing),
    Read(Reader),
}

impl App {
    pub fn pick(&mut self, title: &str, items: Vec<Item>, then: impl FnOnce(&mut App, Item) + 'static) {
        let at = items.iter().position(|i| i.on).unwrap_or(0);
        self.overlay = Some(Overlay::Picker(Picker { title: title.into(), items, at, filter: String::new(), then: Some(Box::new(then)) }));
    }

    pub fn ask(&mut self, title: &str, body: &[&str], yes: &str, no: &str, then: impl FnOnce(&mut App, bool) + 'static) {
        self.overlay = Some(Overlay::Ask(Ask {
            title: title.into(),
            body: body.iter().map(|s| (*s).to_string()).collect(),
            yes: yes.into(),
            no: no.into(),
            on_yes: true,
            then: Some(Box::new(then)),
        }));
    }

    pub fn type_in(&mut self, title: &str, hint: &str, value: &str, secret: bool, multi: bool, then: impl FnOnce(&mut App, String) + 'static) {
        self.overlay = Some(Overlay::Typing(Typing {
            title: title.into(),
            hint: hint.into(),
            secret,
            field: Composer::with(value, multi),
            then: Some(Box::new(then)),
        }));
    }

    pub fn read(&mut self, title: &str, rows: Vec<Line<'static>>) {
        self.overlay = Some(Overlay::Read(Reader { title: title.into(), rows, top: 0, height: 10 }));
    }
}

/// A key for whatever is open. It is put back unless the key finished it.
pub fn key(app: &mut App, key: KeyEvent) {
    let Some(overlay) = app.overlay.take() else { return };
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    let cancel = key.code == KeyCode::Esc || (ctrl && key.code == KeyCode::Char('c'));
    match overlay {
        Overlay::Picker(mut m) => {
            if cancel {
                return;
            }
            let shown = m.shown();
            let place = shown.iter().position(|i| *i == m.at).unwrap_or(0);
            match key.code {
                KeyCode::Up => {
                    if let Some(i) = place.checked_sub(1).and_then(|p| shown.get(p)) {
                        m.at = *i;
                    }
                }
                KeyCode::Char('p') if ctrl => {
                    if let Some(i) = place.checked_sub(1).and_then(|p| shown.get(p)) {
                        m.at = *i;
                    }
                }
                KeyCode::Down => {
                    if let Some(i) = shown.get(place + 1) {
                        m.at = *i;
                    }
                }
                KeyCode::Char('n') if ctrl => {
                    if let Some(i) = shown.get(place + 1) {
                        m.at = *i;
                    }
                }
                KeyCode::PageUp => m.at = shown[place.saturating_sub(8).min(shown.len().saturating_sub(1))..].first().copied().unwrap_or(m.at),
                KeyCode::PageDown => m.at = shown.get((place + 8).min(shown.len().saturating_sub(1))).copied().unwrap_or(m.at),
                KeyCode::Enter => {
                    if let Some(index) = shown.get(place).copied() {
                        let item = m.items[index].clone();
                        if !item.off {
                            if let Some(then) = m.then.take() {
                                then(app, item);
                            }
                            return;
                        }
                    }
                }
                KeyCode::Backspace => {
                    m.filter.pop();
                    if let Some(i) = m.shown().first() {
                        if !m.shown().contains(&m.at) {
                            m.at = *i;
                        }
                    }
                }
                KeyCode::Char(c) if !ctrl => {
                    m.filter.push(c);
                    let now = m.shown();
                    if !now.contains(&m.at) {
                        if let Some(i) = now.first() {
                            m.at = *i;
                        }
                    }
                }
                _ => {}
            }
            app.overlay = Some(Overlay::Picker(m));
        }
        Overlay::Ask(mut m) => {
            let answer = match key.code {
                KeyCode::Enter => Some(m.on_yes),
                KeyCode::Char('y') | KeyCode::Char('Y') => Some(true),
                KeyCode::Char('n') | KeyCode::Char('N') => Some(false),
                KeyCode::Esc => Some(false),
                KeyCode::Char('c') if ctrl => Some(false),
                KeyCode::Left | KeyCode::Right | KeyCode::Tab | KeyCode::BackTab | KeyCode::Up | KeyCode::Down => {
                    m.on_yes = !m.on_yes;
                    None
                }
                _ => None,
            };
            if let Some(yes) = answer {
                if let Some(then) = m.then.take() {
                    then(app, yes);
                }
                return;
            }
            app.overlay = Some(Overlay::Ask(m));
        }
        Overlay::Typing(mut m) => {
            if cancel {
                return;
            }
            let plain_enter = key.code == KeyCode::Enter && !key.modifiers.intersects(KeyModifiers::SHIFT | KeyModifiers::ALT);
            let save = if m.field.multi { ctrl && key.code == KeyCode::Char('s') || plain_enter && m.field.text.is_empty() } else { plain_enter };
            if save {
                if let Some(then) = m.then.take() {
                    let value = m.field.text.clone();
                    then(app, value);
                }
                return;
            }
            if m.field.multi && plain_enter {
                m.field.insert("\n");
            } else {
                m.field.key(key, 60);
            }
            app.overlay = Some(Overlay::Typing(m));
        }
        Overlay::Read(mut m) => {
            let page = m.height.saturating_sub(1).max(1);
            match key.code {
                KeyCode::Esc | KeyCode::Char('q') | KeyCode::Enter => return,
                KeyCode::Char('c') if ctrl => return,
                KeyCode::Up | KeyCode::Char('k') => m.top = m.top.saturating_sub(1),
                KeyCode::Down | KeyCode::Char('j') => m.top += 1,
                KeyCode::PageUp => m.top = m.top.saturating_sub(page),
                KeyCode::PageDown | KeyCode::Char(' ') => m.top += page,
                KeyCode::Home | KeyCode::Char('g') => m.top = 0,
                KeyCode::End | KeyCode::Char('G') => m.top = usize::MAX / 2,
                _ => {}
            }
            app.overlay = Some(Overlay::Read(m));
        }
    }
}

pub fn wheel(app: &mut App, by: isize) {
    match &mut app.overlay {
        Some(Overlay::Read(m)) => {
            m.top = if by < 0 { m.top.saturating_sub(by.unsigned_abs()) } else { m.top + by as usize };
        }
        Some(Overlay::Picker(m)) => {
            let shown = m.shown();
            let place = shown.iter().position(|i| *i == m.at).unwrap_or(0) as isize;
            let next = (place + by).clamp(0, shown.len().saturating_sub(1) as isize) as usize;
            if let Some(i) = shown.get(next) {
                m.at = *i;
            }
        }
        _ => {}
    }
}

fn panel(frame: &mut Frame, area: Rect, title: &str) -> Rect {
    frame.render_widget(Clear, area);
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme::fg(p().flame))
        .title(Span::styled(format!(" {title} "), theme::bold(theme::lit())));
    let inner = block.inner(area);
    block.render(area, frame.buffer_mut());
    inner
}

fn centered(screen: Rect, cols: u16, rows: u16) -> Rect {
    let w = cols.min(screen.width.saturating_sub(2)).max(10);
    let h = rows.min(screen.height.saturating_sub(2)).max(3);
    Rect { x: screen.x + (screen.width - w) / 2, y: screen.y + (screen.height - h) / 2, width: w, height: h }
}

pub fn draw(frame: &mut Frame, app: &mut App) {
    let screen = frame.area();
    let Some(overlay) = app.overlay.as_mut() else { return };
    match overlay {
        Overlay::Picker(m) => {
            let shown = m.shown();
            let widest = m.items.iter().map(|i| text::width(&i.label) + text::width(&i.hint) + 8).max().unwrap_or(30) as u16;
            let area = centered(screen, widest.clamp(40, 96), (shown.len() as u16 + 5).clamp(8, 24));
            let inner = panel(frame, area, &m.title);
            let cols = inner.width as usize;
            let filter = if m.filter.is_empty() {
                Line::from(Span::styled(" type to filter", theme::faint()))
            } else {
                Line::from(vec![Span::styled(" ❯ ", theme::flame()), Span::styled(m.filter.clone(), theme::text())])
            };
            frame.buffer_mut().set_line(inner.x, inner.y, &filter, inner.width);
            let height = inner.height.saturating_sub(2) as usize;
            let place = shown.iter().position(|i| *i == m.at).unwrap_or(0);
            let top = place.saturating_sub(height.saturating_sub(1));
            for (row, index) in shown.iter().skip(top).take(height).enumerate() {
                let item = &m.items[*index];
                let here = *index == m.at;
                let mark = if item.on { "✓ " } else { "  " };
                let base = if item.off { theme::faint() } else { theme::text() };
                let hint_cols = cols.saturating_sub(text::width(&item.label) + 6);
                let mut spans = vec![
                    Span::styled(if here { "❯" } else { " " }, theme::flame()),
                    Span::styled(mark, theme::good()),
                    Span::styled(fit(&item.label, cols.saturating_sub(4)), base),
                ];
                if !item.hint.is_empty() && hint_cols > 4 {
                    spans.push(Span::styled(format!("  {}", fit(&item.hint, hint_cols)), theme::faint()));
                }
                let mut line = Line::from(spans);
                if here {
                    let used = text::line_width(&line.spans);
                    line.spans.push(Span::raw(" ".repeat(cols.saturating_sub(used))));
                    for s in &mut line.spans {
                        s.style = s.style.bg(p().hover);
                    }
                }
                frame.buffer_mut().set_line(inner.x, inner.y + 1 + row as u16, &line, inner.width);
            }
            if shown.is_empty() {
                let none = Line::from(Span::styled("  Nothing", theme::faint()));
                frame.buffer_mut().set_line(inner.x, inner.y + 1, &none, inner.width);
            }
            let foot = Line::from(Span::styled(" enter choose   esc back", theme::faint()));
            frame.buffer_mut().set_line(inner.x, inner.y + inner.height - 1, &foot, inner.width);
        }
        Overlay::Ask(m) => {
            let cols = 60usize.min(screen.width.saturating_sub(6) as usize);
            let mut lines: Vec<Line<'static>> = Vec::new();
            for said in &m.body {
                lines.extend(wrap(&[Span::styled(said.clone(), theme::text())], cols, &[Span::raw(" ")], &[Span::raw(" ")]));
            }
            let area = centered(screen, cols as u16 + 4, lines.len() as u16 + 5);
            let inner = panel(frame, area, &m.title);
            for (y, line) in lines.iter().enumerate().take(inner.height.saturating_sub(2) as usize) {
                frame.buffer_mut().set_line(inner.x, inner.y + y as u16, line, inner.width);
            }
            let button = |label: &str, here: bool| {
                Span::styled(format!(" {label} "), if here { Style::default().fg(p().ink).bg(p().flame) } else { theme::dim() })
            };
            let foot = Line::from(vec![Span::raw(" "), button(&m.yes, m.on_yes), Span::raw("  "), button(&m.no, !m.on_yes)]);
            frame.buffer_mut().set_line(inner.x, inner.y + inner.height - 1, &foot, inner.width);
        }
        Overlay::Typing(m) => {
            let cols = 72u16.min(screen.width.saturating_sub(6));
            let field_cols = cols.saturating_sub(4) as usize;
            let rows = m.field.rows(field_cols);
            let tall = if m.field.multi { (rows.len() as u16 + 1).clamp(6, screen.height.saturating_sub(10).max(6)) } else { rows.len().clamp(1, 4) as u16 };
            let extra = if m.hint.is_empty() { 3 } else { 4 };
            let area = centered(screen, cols + 2, tall + extra);
            let inner = panel(frame, area, &m.title);
            let (row, col) = m.field.caret(field_cols);
            let top = (row + 1).saturating_sub(tall as usize);
            for (y, (start, end)) in rows.iter().skip(top).take(tall as usize).enumerate() {
                let piece = &m.field.text[*start..*end];
                let shown = if m.secret { "•".repeat(piece.chars().count()) } else { piece.to_string() };
                let line = Line::from(vec![Span::raw(" "), Span::styled(shown, theme::text())]);
                frame.buffer_mut().set_line(inner.x, inner.y + y as u16, &line, inner.width);
            }
            frame.set_cursor_position((inner.x + 1 + col as u16, inner.y + (row - top) as u16));
            let mut y = inner.y + tall;
            if !m.hint.is_empty() {
                let hint = Line::from(Span::styled(format!(" {}", fit(&m.hint, field_cols)), theme::faint()));
                frame.buffer_mut().set_line(inner.x, y, &hint, inner.width);
                y += 1;
            }
            let keys = if m.field.multi { " ctrl+s save   esc back" } else { " enter save   esc back" };
            if y < inner.y + inner.height {
                frame.buffer_mut().set_line(inner.x, y, &Line::from(Span::styled(keys, theme::faint())), inner.width);
            }
        }
        Overlay::Read(m) => {
            frame.render_widget(Clear, screen);
            let inner = panel(frame, screen, &m.title);
            m.height = inner.height.saturating_sub(1) as usize;
            m.top = m.top.min(m.rows.len().saturating_sub(m.height));
            for (y, line) in m.rows.iter().skip(m.top).take(m.height).enumerate() {
                frame.buffer_mut().set_line(inner.x, inner.y + y as u16, line, inner.width);
            }
            let foot = if m.rows.len() > m.height {
                format!(" {}/{}   arrows scroll   esc back", (m.top + m.height).min(m.rows.len()), m.rows.len())
            } else {
                " esc back".to_string()
            };
            frame.buffer_mut().set_line(inner.x, inner.y + inner.height - 1, &Line::from(Span::styled(foot, theme::faint())), inner.width);
        }
    }
}
