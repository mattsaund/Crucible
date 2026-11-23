// SPDX-License-Identifier: MIT
//! Measuring, cutting and wrapping text for a grid of cells.

use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

pub fn width(text: &str) -> usize {
    UnicodeWidthStr::width(text)
}

pub fn char_width(c: char) -> usize {
    c.width().unwrap_or(0)
}

pub fn line_width(spans: &[Span<'_>]) -> usize {
    spans.iter().map(|s| width(&s.content)).sum()
}

/// `text` in at most `max` cells, with an ellipsis if some was cut.
pub fn fit(text: &str, max: usize) -> String {
    if width(text) <= max {
        return text.to_string();
    }
    if max == 0 {
        return String::new();
    }
    let mut out = String::new();
    let mut used = 0;
    for c in text.chars() {
        let w = char_width(c);
        if used + w + 1 > max {
            break;
        }
        used += w;
        out.push(c);
    }
    out.push('…');
    out
}

/// The same, keeping the end: the part of a path that differs is the end.
pub fn fit_left(text: &str, max: usize) -> String {
    if width(text) <= max {
        return text.to_string();
    }
    if max == 0 {
        return String::new();
    }
    let mut kept: Vec<char> = Vec::new();
    let mut used = 0;
    for c in text.chars().rev() {
        let w = char_width(c);
        if used + w + 1 > max {
            break;
        }
        used += w;
        kept.push(c);
    }
    let mut out = String::from("…");
    out.extend(kept.iter().rev());
    out
}

pub fn pad(text: &str, cells: usize) -> String {
    let cut = fit(text, cells);
    let used = width(&cut);
    cut + &" ".repeat(cells - used)
}

pub fn bytes(n: f64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut value = n.max(0.0);
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{value:.0} B")
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

pub fn compact(n: f64) -> String {
    if n >= 1e6 {
        format!("{:.1}M", n / 1e6)
    } else if n >= 1e4 {
        format!("{:.0}k", n / 1e3)
    } else if n >= 1e3 {
        format!("{:.1}k", n / 1e3)
    } else {
        format!("{n:.0}")
    }
}

pub fn clock(seconds: u64) -> String {
    if seconds < 60 {
        format!("{seconds}s")
    } else if seconds < 3600 {
        format!("{}m {}s", seconds / 60, seconds % 60)
    } else {
        format!("{}h {}m", seconds / 3600, seconds % 3600 / 60)
    }
}

/// What came from a model or a command, safe to put in a terminal: escape
/// sequences out, tabs as spaces, and a carriage return overwriting its line
/// the way it would have.
pub fn clean(text: &str) -> String {
    let plain = !text.bytes().any(|b| (b < 0x20 && b != b'\n') || b == 0x7f) && !text.contains('\u{9b}');
    if plain {
        return text.to_string();
    }
    let mut out = String::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\x1b' | '\u{9b}' => {
                let kind = if c == '\u{9b}' { Some('[') } else { chars.next() };
                match kind {
                    Some('[') => {
                        for n in chars.by_ref() {
                            if ('@'..='~').contains(&n) {
                                break;
                            }
                        }
                    }
                    Some(']' | 'P' | 'X' | '^' | '_') => {
                        while let Some(n) = chars.next() {
                            if n == '\x07' {
                                break;
                            }
                            if n == '\x1b' {
                                chars.next();
                                break;
                            }
                        }
                    }
                    _ => {}
                }
            }
            '\t' => out.push_str("    "),
            '\r' => {
                if chars.peek() != Some(&'\n') {
                    let start = out.rfind('\n').map_or(0, |i| i + 1);
                    out.truncate(start);
                }
            }
            '\n' => out.push('\n'),
            c if c.is_control() => {}
            c => out.push(c),
        }
    }
    out
}

/// A logical line as rows of at most `cols` cells, broken at spaces where it
/// can be. `first` goes in front of the first row and `rest` the others.
pub fn wrap(spans: &[Span<'static>], cols: usize, first: &[Span<'static>], rest: &[Span<'static>]) -> Vec<Line<'static>> {
    let mut cells: Vec<(char, usize, Style)> = Vec::new();
    for span in spans {
        for c in span.content.chars() {
            cells.push((c, char_width(c), span.style));
        }
    }
    let room = |prefix: &[Span<'static>]| cols.saturating_sub(line_width(prefix)).max(1);
    let build = |range: &[(char, usize, Style)], prefix: &[Span<'static>]| {
        let mut row: Vec<Span<'static>> = prefix.to_vec();
        let mut text = String::new();
        let mut style = None;
        for (c, _, s) in range {
            if style != Some(*s) {
                if let Some(old) = style {
                    row.push(Span::styled(std::mem::take(&mut text), old));
                }
                style = Some(*s);
            }
            text.push(*c);
        }
        if let Some(old) = style {
            row.push(Span::styled(text, old));
        }
        Line::from(row)
    };

    let mut out = Vec::new();
    let mut start = 0;
    let mut i = 0;
    let mut used = 0;
    let mut gap: Option<usize> = None;
    let mut avail = room(first);
    while i < cells.len() {
        let (c, w, _) = cells[i];
        if c == ' ' {
            gap = Some(i);
        }
        if used + w > avail && i > start {
            let end = match gap {
                Some(g) if g > start => g,
                _ => i,
            };
            out.push(build(&cells[start..end], if out.is_empty() { first } else { rest }));
            start = if cells[end].0 == ' ' { end + 1 } else { end };
            i = start;
            used = 0;
            gap = None;
            avail = room(rest);
            continue;
        }
        used += w;
        i += 1;
    }
    out.push(build(&cells[start.min(cells.len())..], if out.is_empty() { first } else { rest }));
    out
}

/// A row filled out to `cols` cells in one background.
pub fn band(mut spans: Vec<Span<'static>>, cols: usize, bg: Color) -> Line<'static> {
    let used = line_width(&spans);
    if used < cols {
        spans.push(Span::raw(" ".repeat(cols - used)));
    }
    for span in &mut spans {
        span.style = span.style.bg(bg);
    }
    Line::from(spans)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plain(line: &Line<'_>) -> String {
        line.spans.iter().map(|s| s.content.as_ref()).collect()
    }

    #[test]
    fn wraps_at_spaces_and_never_past_the_width() {
        let spans = [Span::raw("the quick brown fox jumps over the lazy dog")];
        let rows = wrap(&spans, 12, &[Span::raw("> ")], &[Span::raw("  ")]);
        assert!(rows.len() > 1);
        assert!(plain(&rows[0]).starts_with("> the"));
        for row in &rows {
            assert!(width(&plain(row)) <= 12, "{:?}", plain(row));
        }
        let back: Vec<String> = rows.iter().map(|r| plain(r)[2..].to_string()).collect();
        assert_eq!(back.join(" "), "the quick brown fox jumps over the lazy dog");
    }

    #[test]
    fn a_word_longer_than_the_line_is_cut() {
        let spans = [Span::raw("x".repeat(25))];
        let rows = wrap(&spans, 10, &[], &[]);
        assert_eq!(rows.len(), 3);
        assert!(rows.iter().all(|r| width(&plain(r)) <= 10));
    }

    #[test]
    fn wide_characters_count_as_two() {
        let spans = [Span::raw("日本語のテキストです")];
        for row in wrap(&spans, 7, &[], &[]) {
            assert!(width(&plain(&row)) <= 7);
        }
        assert_eq!(fit("日本語", 4), "日…");
    }

    #[test]
    fn an_empty_line_is_one_row() {
        assert_eq!(wrap(&[], 10, &[], &[]).len(), 1);
    }

    #[test]
    fn escape_codes_never_reach_the_terminal() {
        assert_eq!(clean("\x1b[31mred\x1b[0m"), "red");
        assert_eq!(clean("a\x1b]0;a title\x07b"), "ab");
        assert_eq!(clean("a\x1b]52;c;ZXZpbA==\x1b\\b"), "ab");
        assert_eq!(clean("a\u{9b}2Jb"), "ab");
        assert_eq!(clean("bell\x07 and\x08 back"), "bell and back");
    }

    #[test]
    fn a_carriage_return_overwrites_its_line() {
        assert_eq!(clean("10%\r50%\r100%\ndone"), "100%\ndone");
        assert_eq!(clean("windows\r\nline"), "windows\nline");
        assert_eq!(clean("a\tb"), "a    b");
    }

    #[test]
    fn numbers_read_at_a_glance() {
        assert_eq!(compact(950.0), "950");
        assert_eq!(compact(8400.0), "8.4k");
        assert_eq!(compact(12000.0), "12k");
        assert_eq!(bytes(1536.0), "1.5 KB");
        assert_eq!(clock(75), "1m 15s");
    }
}
