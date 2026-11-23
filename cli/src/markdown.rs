// SPDX-License-Identifier: MIT
//! A reply, as rows of styled text.

use crate::highlight;
use crate::text::{self, band, line_width, wrap};
use crate::theme::{self, p};
use pulldown_cmark::{Alignment, CodeBlockKind, Event, HeadingLevel, Options, Parser, Tag, TagEnd};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};

/// A block of code on its own band of color, its language in the corner.
pub fn code_block(code: &str, lang: &str, cols: usize, indent: &[Span<'static>]) -> Vec<Line<'static>> {
    let body = code.strip_suffix('\n').unwrap_or(code);
    let room = cols.saturating_sub(line_width(indent)).max(4);
    let bg = p().code_bg;
    let mut out = Vec::new();
    let with_indent = |line: Line<'static>| -> Line<'static> {
        let mut spans = indent.to_vec();
        spans.extend(line.spans);
        Line::from(spans)
    };
    let label = lang.trim().split([' ', ',', '{']).next().unwrap_or("");
    if !label.is_empty() {
        let shift = room.saturating_sub(text::width(label) + 1);
        out.push(with_indent(band(vec![Span::raw(" ".repeat(shift)), Span::styled(label.to_string(), theme::faint())], room, bg)));
    } else {
        out.push(with_indent(band(Vec::new(), room, bg)));
    }
    let lead = [Span::raw(" ")];
    for row in highlight::code(body, lang) {
        for piece in wrap(&row, room.saturating_sub(1).max(1), &lead, &lead) {
            out.push(with_indent(band(piece.spans, room, bg)));
        }
    }
    out.push(with_indent(band(Vec::new(), room, bg)));
    out
}

#[derive(Default)]
struct Table {
    aligns: Vec<Alignment>,
    rows: Vec<Vec<Vec<Span<'static>>>>,
    row: Vec<Vec<Span<'static>>>,
    head: bool,
}

struct Writer {
    out: Vec<Line<'static>>,
    cols: usize,
    spans: Vec<Span<'static>>,
    styles: Vec<Style>,
    lists: Vec<Option<u64>>,
    indents: Vec<usize>,
    bullet: Option<String>,
    quote: usize,
    code: Option<(String, String)>,
    table: Option<Table>,
    links: Vec<String>,
    gap: bool,
}

impl Writer {
    fn style(&self) -> Style {
        *self.styles.last().unwrap_or(&theme::text())
    }

    fn push(&mut self, change: impl Fn(Style) -> Style) {
        self.styles.push(change(self.style()));
    }

    fn margin(&self) -> Vec<Span<'static>> {
        let mut m = Vec::new();
        if self.quote > 0 {
            m.push(Span::styled("▎ ".repeat(self.quote), theme::flame()));
        }
        let lead: usize = self.indents.iter().sum();
        if lead > 0 {
            m.push(Span::raw(" ".repeat(lead)));
        }
        m
    }

    fn before_block(&mut self) {
        if self.gap && !self.out.is_empty() {
            let margin = if self.quote > 0 {
                vec![Span::styled("▎ ".repeat(self.quote), theme::flame())]
            } else {
                Vec::new()
            };
            self.out.push(Line::from(margin));
        }
        self.gap = false;
    }

    fn flush(&mut self) {
        if self.spans.is_empty() && self.bullet.is_none() {
            return;
        }
        let spans = std::mem::take(&mut self.spans);
        let rest = self.margin();
        let mut first = rest.clone();
        if let Some(bullet) = self.bullet.take() {
            let w = text::width(&bullet);
            if let Some(last) = first.last_mut() {
                if last.style == Style::default() {
                    let keep = text::width(&last.content).saturating_sub(w);
                    *last = Span::raw(" ".repeat(keep));
                }
            }
            first.push(Span::styled(bullet, theme::flame()));
        }
        let mut lines: Vec<Vec<Span<'static>>> = vec![Vec::new()];
        for span in spans {
            let mut parts = span.content.split('\n');
            if let Some(head) = parts.next() {
                if !head.is_empty() {
                    lines.last_mut().unwrap().push(Span::styled(head.to_string(), span.style));
                }
            }
            for part in parts {
                lines.push(if part.is_empty() { Vec::new() } else { vec![Span::styled(part.to_string(), span.style)] });
            }
        }
        for (n, line) in lines.iter().enumerate() {
            let wrapped = wrap(line, self.cols, if n == 0 { &first } else { &rest }, &rest);
            self.out.extend(wrapped);
        }
    }

    fn cell_text(&mut self, content: String, style: Style) {
        self.spans.push(Span::styled(content, style));
    }

    fn table(&mut self, t: Table) {
        if t.rows.is_empty() {
            return;
        }
        let margin = self.margin();
        let room = self.cols.saturating_sub(line_width(&margin));
        let columns = t.rows.iter().map(Vec::len).max().unwrap_or(0);
        if columns == 0 {
            return;
        }
        let mut widths = vec![1usize; columns];
        for row in &t.rows {
            for (i, cell) in row.iter().enumerate() {
                widths[i] = widths[i].max(line_width(cell));
            }
        }
        let frame = 3 * columns + 1;
        let usable = room.saturating_sub(frame).max(columns);
        while widths.iter().sum::<usize>() > usable {
            let big = widths.iter().enumerate().max_by_key(|(_, w)| **w).map(|(i, _)| i).unwrap_or(0);
            if widths[big] <= 3 {
                break;
            }
            widths[big] -= 1;
        }
        let edge = theme::faint();
        let rule = |l: &str, m: &str, r: &str| {
            let mid: Vec<String> = widths.iter().map(|w| "─".repeat(w + 2)).collect();
            format!("{l}{}{r}", mid.join(m))
        };
        let with_margin = |spans: Vec<Span<'static>>| {
            let mut all = margin.clone();
            all.extend(spans);
            Line::from(all)
        };
        self.out.push(with_margin(vec![Span::styled(rule("┌", "┬", "┐"), edge)]));
        for (n, row) in t.rows.iter().enumerate() {
            let cells: Vec<Vec<Line<'static>>> = (0..columns)
                .map(|i| {
                    let empty = Vec::new();
                    wrap(row.get(i).unwrap_or(&empty), widths[i], &[], &[])
                })
                .collect();
            let height = cells.iter().map(Vec::len).max().unwrap_or(1);
            for y in 0..height {
                let mut spans = vec![Span::styled("│", edge)];
                for (i, cell) in cells.iter().enumerate() {
                    let piece = cell.get(y).map(|l| l.spans.clone()).unwrap_or_default();
                    let free = widths[i].saturating_sub(line_width(&piece));
                    let (left, right) = match t.aligns.get(i) {
                        Some(Alignment::Right) => (free, 0),
                        Some(Alignment::Center) => (free / 2, free - free / 2),
                        _ => (0, free),
                    };
                    spans.push(Span::raw(" ".repeat(left + 1)));
                    for s in piece {
                        let style = if n == 0 { s.style.add_modifier(Modifier::BOLD) } else { s.style };
                        spans.push(Span::styled(s.content, style));
                    }
                    spans.push(Span::raw(" ".repeat(right + 1)));
                    spans.push(Span::styled("│", edge));
                }
                self.out.push(with_margin(spans));
            }
            if n == 0 && t.rows.len() > 1 {
                self.out.push(with_margin(vec![Span::styled(rule("├", "┼", "┤"), edge)]));
            }
        }
        self.out.push(with_margin(vec![Span::styled(rule("└", "┴", "┘"), edge)]));
    }

    fn event(&mut self, event: Event<'_>) {
        match event {
            Event::Start(tag) => self.open(tag),
            Event::End(tag) => self.close(tag),
            Event::Text(content) => {
                if let Some((_, buffer)) = &mut self.code {
                    buffer.push_str(&content);
                } else {
                    let style = self.style();
                    self.cell_text(text::clean(&content), style);
                }
            }
            Event::Code(content) => {
                let style = theme::lit().bg(p().raised);
                self.cell_text(text::clean(&content), style);
            }
            Event::InlineMath(content) | Event::DisplayMath(content) => {
                let style = theme::lit();
                self.cell_text(text::clean(&content), style);
            }
            Event::Html(content) | Event::InlineHtml(content) => {
                self.cell_text(text::clean(content.trim_end_matches('\n')), theme::dim());
            }
            Event::SoftBreak => {
                let style = self.style();
                self.cell_text(" ".into(), style);
            }
            Event::HardBreak => {
                let style = self.style();
                self.cell_text("\n".into(), style);
            }
            Event::Rule => {
                self.flush();
                self.before_block();
                let margin = self.margin();
                let room = self.cols.saturating_sub(line_width(&margin));
                let mut spans = margin;
                spans.push(Span::styled("─".repeat(room), theme::faint()));
                self.out.push(Line::from(spans));
                self.gap = true;
            }
            Event::TaskListMarker(done) => {
                let style = if done { theme::good() } else { theme::dim() };
                self.cell_text(if done { "☑ ".into() } else { "☐ ".into() }, style);
            }
            Event::FootnoteReference(name) => {
                self.cell_text(format!("[{name}]"), theme::dim());
            }
        }
    }

    fn open(&mut self, tag: Tag<'_>) {
        match tag {
            Tag::Paragraph => {
                self.flush_if_loose();
            }
            Tag::Heading { level, .. } => {
                self.flush();
                self.before_block();
                let color = if matches!(level, HeadingLevel::H1 | HeadingLevel::H2) { p().flame } else { p().lit };
                self.styles.push(Style::default().fg(color).add_modifier(Modifier::BOLD));
            }
            Tag::BlockQuote(_) => {
                self.flush();
                self.before_block();
                self.quote += 1;
                self.push(|s| s.fg(p().dim));
            }
            Tag::CodeBlock(kind) => {
                self.flush();
                self.before_block();
                let lang = match kind {
                    CodeBlockKind::Fenced(info) => info.to_string(),
                    CodeBlockKind::Indented => String::new(),
                };
                self.code = Some((lang, String::new()));
            }
            Tag::List(start) => {
                self.flush();
                if self.lists.is_empty() {
                    self.before_block();
                }
                self.lists.push(start);
            }
            Tag::Item => {
                self.flush();
                let depth = self.lists.len();
                let bullet = match self.lists.last_mut() {
                    Some(Some(n)) => {
                        let b = format!("{n}. ");
                        *n += 1;
                        b
                    }
                    _ => (if depth > 1 { "◦ " } else { "• " }).to_string(),
                };
                self.indents.push(text::width(&bullet));
                self.bullet = Some(bullet);
            }
            Tag::Table(aligns) => {
                self.flush();
                self.before_block();
                self.table = Some(Table { aligns, ..Default::default() });
            }
            Tag::TableHead => {
                if let Some(t) = &mut self.table {
                    t.head = true;
                }
            }
            Tag::TableRow | Tag::TableCell => {}
            Tag::Emphasis => self.push(|s| s.add_modifier(Modifier::ITALIC)),
            Tag::Strong => self.push(|s| s.add_modifier(Modifier::BOLD)),
            Tag::Strikethrough => self.push(|s| s.add_modifier(Modifier::CROSSED_OUT)),
            Tag::Link { dest_url, .. } => {
                self.links.push(dest_url.to_string());
                self.push(|s| s.fg(p().lit).add_modifier(Modifier::UNDERLINED));
            }
            Tag::Image { dest_url, .. } => {
                self.links.push(dest_url.to_string());
                self.push(|s| s.fg(p().dim));
            }
            _ => {}
        }
    }

    fn flush_if_loose(&mut self) {
        if !self.spans.is_empty() {
            self.flush();
        }
        if self.bullet.is_none() {
            self.before_block();
        }
    }

    fn close(&mut self, tag: TagEnd) {
        match tag {
            TagEnd::Paragraph => {
                self.flush();
                self.gap = true;
            }
            TagEnd::Heading(_) => {
                self.flush();
                self.styles.pop();
                self.gap = true;
            }
            TagEnd::BlockQuote(_) => {
                self.flush();
                self.quote = self.quote.saturating_sub(1);
                self.styles.pop();
                self.gap = true;
            }
            TagEnd::CodeBlock => {
                if let Some((lang, code)) = self.code.take() {
                    let margin = self.margin();
                    let block = code_block(&text::clean(&code), &lang, self.cols, &margin);
                    self.out.extend(block);
                }
                self.gap = true;
            }
            TagEnd::List(_) => {
                self.flush();
                self.lists.pop();
                if self.lists.is_empty() {
                    self.gap = true;
                }
            }
            TagEnd::Item => {
                self.flush();
                self.indents.pop();
                self.bullet = None;
                self.gap = false;
            }
            TagEnd::Table => {
                if let Some(t) = self.table.take() {
                    self.table(t);
                }
                self.gap = true;
            }
            TagEnd::TableHead | TagEnd::TableRow => {
                if let Some(t) = &mut self.table {
                    let row = std::mem::take(&mut t.row);
                    t.rows.push(row);
                    t.head = false;
                }
            }
            TagEnd::TableCell => {
                let cell = std::mem::take(&mut self.spans);
                if let Some(t) = &mut self.table {
                    t.row.push(cell);
                }
            }
            TagEnd::Emphasis | TagEnd::Strong | TagEnd::Strikethrough => {
                self.styles.pop();
            }
            TagEnd::Link => {
                self.styles.pop();
                let url = self.links.pop().unwrap_or_default();
                let said: String = self.spans.iter().rev().take(1).map(|s| s.content.to_string()).collect();
                if !url.is_empty() && !said.contains(&url) && url.starts_with("http") {
                    self.cell_text(format!(" ({url})"), theme::faint());
                }
            }
            TagEnd::Image => {
                self.styles.pop();
                self.links.pop();
            }
            _ => {}
        }
    }
}

/// Markdown as rows for a column `cols` wide.
pub fn render(source: &str, cols: usize) -> Vec<Line<'static>> {
    let options = Options::ENABLE_TABLES
        | Options::ENABLE_STRIKETHROUGH
        | Options::ENABLE_TASKLISTS
        | Options::ENABLE_GFM;
    let mut writer = Writer {
        out: Vec::new(),
        cols: cols.max(8),
        spans: Vec::new(),
        styles: vec![theme::text()],
        lists: Vec::new(),
        indents: Vec::new(),
        bullet: None,
        quote: 0,
        code: None,
        table: None,
        links: Vec::new(),
        gap: false,
    };
    // Whole, before it is parsed: the parser splits text at a bracket, and an
    // escape code cut in two no longer looks like one.
    let source = text::clean(source);
    for event in Parser::new_ext(&source, options) {
        writer.event(event);
    }
    writer.flush();
    writer.out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows(source: &str, cols: usize) -> Vec<String> {
        render(source, cols).iter().map(|l| l.spans.iter().map(|s| s.content.as_ref()).collect::<String>()).collect()
    }

    #[test]
    fn numbered_lists_keep_their_numbers() {
        let out = rows("3. three\n4. four\n5. five\n", 40);
        assert_eq!(out, vec!["3. three", "4. four", "5. five"]);
    }

    #[test]
    fn nested_lists_indent_and_wrap_under_their_text() {
        let out = rows("- one\n  - inner item that wraps past the edge of it\n- two\n", 20);
        assert_eq!(out[0], "• one");
        assert!(out[1].starts_with("  ◦ inner"));
        assert!(out[2].starts_with("    "), "{:?}", out[2]);
        assert_eq!(out.last().unwrap(), "• two");
    }

    #[test]
    fn bold_around_code_has_no_stars_left() {
        let out = rows("**use `foo` here**", 40);
        assert_eq!(out, vec!["use foo here"]);
    }

    #[test]
    fn a_code_block_still_arriving_is_drawn() {
        let out = rows("before\n\n```python\nprint(1)\n", 30);
        assert!(out.iter().any(|l| l.contains("print(1)")));
        assert!(out.iter().any(|l| l.contains("python")));
    }

    #[test]
    fn every_row_fits() {
        let source = "# A heading long enough to wrap around\n\nA paragraph with `code` that goes on for a while and wraps.\n\n| a | b |\n|---|---|\n| one long cell of text | two |\n\n> quoted text that also wraps around the edge\n";
        for line in render(source, 24) {
            let width: usize = line.spans.iter().map(|s| text::width(&s.content)).sum();
            assert!(width <= 24, "{width}: {line:?}");
        }
    }

    #[test]
    fn a_table_lines_up() {
        let out = rows("| name | n |\n|---|--:|\n| a | 1 |\n| bb | 22 |\n", 40);
        let widths: Vec<usize> = out.iter().map(|l| text::width(l)).collect();
        assert!(widths.windows(2).all(|w| w[0] == w[1]), "{out:?}");
        assert!(out[1].contains("name"));
    }

    #[test]
    fn escape_codes_in_a_reply_are_gone() {
        let out = rows("hi \x1b[31mred\x1b[0m", 40);
        assert_eq!(out, vec!["hi red"]);
    }
}
