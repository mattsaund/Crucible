// SPDX-License-Identifier: MIT
//! A change to a file, drawn as rows with both line numbers.

use crate::highlight;
use crate::text::{self, band, line_width, wrap};
use crate::theme::{self, p};
use ratatui::text::{Line, Span};
use similar::{ChangeTag, TextDiff};

#[derive(Clone)]
pub struct Row {
    pub tag: char,
    pub old: Option<usize>,
    pub new: Option<usize>,
    pub text: String,
}

fn between(before: &str, after: &str, old_from: usize, new_from: usize, context: usize) -> Vec<Row> {
    let diff = TextDiff::from_lines(before, after);
    let mut rows = Vec::new();
    for (n, group) in diff.grouped_ops(context).iter().enumerate() {
        if n > 0 {
            rows.push(Row { tag: '~', old: None, new: None, text: String::new() });
        }
        for op in group {
            for change in diff.iter_changes(op) {
                let tag = match change.tag() {
                    ChangeTag::Equal => ' ',
                    ChangeTag::Insert => '+',
                    ChangeTag::Delete => '-',
                };
                rows.push(Row {
                    tag,
                    old: change.old_index().map(|i| i + old_from),
                    new: change.new_index().map(|i| i + new_from),
                    text: change.value().trim_end_matches(['\n', '\r']).to_string(),
                });
            }
        }
    }
    rows
}

/// A whole file before and after, as the question about an edit shows it.
pub fn of_files(before: &str, after: &str) -> Vec<Row> {
    if before.is_empty() {
        return after
            .lines()
            .enumerate()
            .map(|(i, l)| Row { tag: '+', old: None, new: Some(i + 1), text: l.to_string() })
            .collect();
    }
    between(before, after, 1, 1, 3)
}

/// What a finished write kept: "@@ line N @@", then the changed stretch.
/// Returns the rows, and the note when some were cut.
pub fn of_written(body: &str) -> Option<(Vec<Row>, String)> {
    let mut lines = body.lines();
    let head = lines.next()?;
    let number: usize = head.strip_prefix("@@ line ")?.strip_suffix(" @@")?.parse().ok()?;
    let mut cut = String::new();
    let (mut old, mut new) = (String::new(), String::new());
    let mut lead = 0;
    let mut seen_change = false;
    let mut kept: Vec<(char, &str)> = Vec::new();
    for line in lines {
        if line.starts_with("... (") {
            cut = line.trim_start_matches("... ").to_string();
            continue;
        }
        let (tag, rest) = match line.chars().next() {
            Some('+') => ('+', &line[1..]),
            Some('-') => ('-', &line[1..]),
            Some(' ') => (' ', &line[1..]),
            _ => (' ', line),
        };
        if tag == ' ' && !seen_change {
            lead += 1;
        }
        if tag != ' ' {
            seen_change = true;
        }
        kept.push((tag, rest));
        if tag != '+' {
            old.push_str(rest);
            old.push('\n');
        }
        if tag != '-' {
            new.push_str(rest);
            new.push('\n');
        }
    }
    let _ = lead;
    // Cut short, the stretch can't be matched up again: as it was kept.
    if !cut.is_empty() {
        let (mut o, mut n) = (number, number);
        let rows = kept
            .iter()
            .map(|(tag, t)| {
                let row = Row {
                    tag: *tag,
                    old: (*tag != '+').then_some(o),
                    new: (*tag != '-').then_some(n),
                    text: t.to_string(),
                };
                if *tag != '+' {
                    o += 1;
                }
                if *tag != '-' {
                    n += 1;
                }
                row
            })
            .collect();
        return Some((rows, cut));
    }
    Some((between(&old, &new, number, number, 3), cut))
}

pub fn counts(rows: &[Row]) -> (usize, usize) {
    (rows.iter().filter(|r| r.tag == '+').count(), rows.iter().filter(|r| r.tag == '-').count())
}

/// The rows drawn, at most `limit` of them, in the language of `path`.
pub fn draw(rows: &[Row], path: &str, cols: usize, limit: usize, indent: &[Span<'static>]) -> Vec<Line<'static>> {
    let lang = highlight::language_of(path);
    let side = |want: char| -> Vec<Vec<Span<'static>>> {
        let joined: Vec<&str> =
            rows.iter().filter(|r| r.tag == ' ' || r.tag == want).map(|r| r.text.as_str()).collect();
        highlight::code(&joined.join("\n"), &lang)
    };
    let (new_side, old_side) = (side('+'), side('-'));
    let (mut in_new, mut in_old) = (0, 0);
    let digits = rows
        .iter()
        .filter_map(|r| r.new.or(r.old))
        .max()
        .map_or(1, |m| m.to_string().len());
    let room = cols.saturating_sub(line_width(indent)).max(8);
    let gutter = digits + 3;
    let mut out = Vec::new();
    let with = |spans: Vec<Span<'static>>| {
        let mut all = indent.to_vec();
        all.extend(spans);
        Line::from(all)
    };
    let mut shown = 0;
    for (i, row) in rows.iter().enumerate() {
        let spans = match row.tag {
            '+' => {
                in_new += 1;
                new_side.get(in_new - 1).cloned()
            }
            '-' => {
                in_old += 1;
                old_side.get(in_old - 1).cloned()
            }
            ' ' => {
                in_new += 1;
                in_old += 1;
                new_side.get(in_new - 1).cloned()
            }
            _ => None,
        };
        if shown >= limit {
            continue;
        }
        if row.tag == '~' {
            out.push(with(vec![Span::styled(format!("{:>w$}  ⋮", "", w = digits), theme::faint())]));
            shown += 1;
            continue;
        }
        let spans = spans.unwrap_or_else(|| vec![Span::styled(row.text.clone(), theme::text())]);
        let (bg, sign_style) = match row.tag {
            '+' => (Some(p().add_bg), theme::fg(p().add)),
            '-' => (Some(p().del_bg), theme::fg(p().del)),
            _ => (None, theme::faint()),
        };
        let number = row.new.or(row.old).map_or(String::new(), |n| n.to_string());
        let first = vec![
            Span::styled(format!("{number:>digits$} "), theme::faint()),
            Span::styled(format!("{} ", row.tag), sign_style),
        ];
        let rest = vec![Span::raw(" ".repeat(gutter))];
        for piece in wrap(&spans, room, &first, &rest) {
            shown += 1;
            let line = match bg {
                Some(color) => band(piece.spans, room, color),
                None => piece,
            };
            out.push(with(line.spans));
            if shown >= limit && i + 1 < rows.len() {
                break;
            }
        }
    }
    let left = rows.len().saturating_sub(shown);
    if left > 0 && shown >= limit {
        out.push(with(vec![
            Span::styled(format!("{:>w$}  … +{left} lines", "", w = digits), theme::dim()),
            Span::styled("  ctrl+o", theme::faint()),
        ]));
    }
    let _ = text::width;
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_new_file_is_all_additions() {
        let rows = of_files("", "one\ntwo\n");
        assert_eq!(counts(&rows), (2, 0));
        assert_eq!(rows[1].new, Some(2));
    }

    #[test]
    fn an_edit_keeps_its_context_and_line_numbers() {
        let before: String = (1..=20).map(|i| format!("line {i}\n")).collect();
        let after = before.replace("line 10\n", "line ten\n");
        let rows = of_files(&before, &after);
        assert_eq!(counts(&rows), (1, 1));
        let added = rows.iter().find(|r| r.tag == '+').unwrap();
        assert_eq!((added.new, added.text.as_str()), (Some(10), "line ten"));
        assert!(rows.len() <= 8, "only the context around it");
    }

    #[test]
    fn what_a_write_kept_is_read_back() {
        let body = "@@ line 12 @@\n keep\n-old\n+new\n after\n";
        let (rows, cut) = of_written(body).unwrap();
        assert!(cut.is_empty());
        assert_eq!(counts(&rows), (1, 1));
        assert_eq!(rows[0].old, Some(12));
        let added = rows.iter().find(|r| r.tag == '+').unwrap();
        assert_eq!(added.new, Some(13));
    }

    #[test]
    fn a_stretch_cut_short_says_so() {
        let body = "@@ line 1 @@\n+one\n... (40 more lines)\n";
        let (rows, cut) = of_written(body).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(cut, "(40 more lines)");
    }

    #[test]
    fn anything_else_is_not_a_write() {
        assert!(of_written("just some output").is_none());
    }
}
