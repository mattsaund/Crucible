// SPDX-License-Identifier: MIT
//! A turn, as rows: what was asked, who has it, what it did, what it said.

use crate::diff;
use crate::markdown;
use crate::state::{Action, State, Turn};
use crate::text::{self, band, compact, line_width, wrap};
use crate::theme::{self, p};
use ratatui::style::Modifier;
use ratatui::text::{Line, Span};

const OUTPUT_ROWS: usize = 8;
const DIFF_ROWS: usize = 22;

fn indent(lines: Vec<Line<'static>>, by: usize) -> Vec<Line<'static>> {
    let lead = " ".repeat(by);
    lines
        .into_iter()
        .map(|line| {
            let mut spans = vec![Span::raw(lead.clone())];
            spans.extend(line.spans);
            Line::from(spans)
        })
        .collect()
}

/// A command's line: what it ran and how it ended, read off its summary.
fn command_of(summary: &str) -> Option<(&str, String, bool)> {
    for (tail, ok) in [("  (timed out)", false), ("  (stopped)", false)] {
        if let Some(head) = summary.strip_suffix(tail) {
            let head = head.strip_prefix("$ ").unwrap_or(head);
            return Some((head, tail.trim().trim_matches(['(', ')']).to_string(), ok));
        }
    }
    let at = summary.rfind("  (exit ")?;
    let code = summary[at + 8..].strip_suffix(')')?;
    code.parse::<i32>().ok()?;
    let line = summary[..at].strip_prefix("$ ").unwrap_or(&summary[..at]);
    Some((line, if code == "0" { String::new() } else { format!("exit {code}") }, code == "0"))
}

fn capitalized(text: &str) -> String {
    let mut chars = text.chars();
    match chars.next() {
        Some(c) => c.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}

fn header(mark: Span<'static>, spans: Vec<Span<'static>>, cols: usize) -> Vec<Line<'static>> {
    wrap(&spans, cols, &[mark, Span::raw(" ")], &[Span::raw("  ")])
}

fn output_rows(body: &str, cols: usize, open: bool, out: &mut Vec<Line<'static>>) {
    let body = body.trim_end();
    if body.is_empty() {
        return;
    }
    let lines: Vec<&str> = body.lines().collect();
    let limit = if open { usize::MAX } else { OUTPUT_ROWS };
    let first = [Span::styled("  ⎿ ", theme::faint())];
    let rest = [Span::raw("    ")];
    let mut shown = 0;
    let mut truncated = false;
    'lines: for (i, line) in lines.iter().enumerate() {
        let spans = [Span::styled((*line).to_string(), theme::dim())];
        for row in wrap(&spans, cols, if i == 0 { &first } else { &rest }, &rest) {
            if shown >= limit {
                truncated = true;
                break 'lines;
            }
            out.push(row);
            shown += 1;
        }
    }
    if truncated {
        let total: usize = lines.iter().map(|l| (text::width(l) / cols.saturating_sub(4).max(1)) + 1).sum();
        out.push(Line::from(vec![
            Span::styled(format!("    … +{} lines", total.saturating_sub(shown)), theme::dim()),
            Span::styled("  ctrl+o", theme::faint()),
        ]));
    }
}

/// A step's summary with the project's folder taken off the paths in it. The
/// folder is on screen already, and it can take up whole lines. Commands are
/// left exactly as they were run.
fn from_project(summary: &str, root: &str) -> String {
    if root.is_empty() {
        return summary.to_string();
    }
    let inner = format!("{}/", root.trim_end_matches('/'));
    summary.replace(&inner, "").replace(root, ".")
}

fn action_rows(action: &Action, cols: usize, open: bool, root: &str, out: &mut Vec<Line<'static>>) {
    let summary = action.summary.trim();
    if summary.is_empty() && action.body.is_empty() {
        return;
    }

    if let Some(rest) = summary.find(" takes over: ").map(|i| (&summary[..i], &summary[i + 13..])) {
        let (who, why) = rest;
        out.extend(header(
            Span::styled("◆", theme::lit()),
            vec![
                Span::styled(who.to_string(), theme::bold(theme::lit())),
                Span::styled(" takes over  ", theme::dim()),
                Span::styled(why.to_string(), theme::text()),
            ],
            cols,
        ));
        return;
    }

    if let Some((command, ending, ok)) = command_of(summary) {
        let mark = Span::styled("●", if ok { theme::good() } else { theme::bad() });
        let mut spans = vec![
            Span::styled("$ ", theme::faint()),
            Span::styled(command.to_string(), theme::bold(theme::text())),
        ];
        if !ending.is_empty() {
            spans.push(Span::styled(format!("  {ending}"), theme::bad()));
        }
        out.extend(header(mark, spans, cols));
        output_rows(&action.body, cols, open, out);
        return;
    }

    if !action.language.is_empty() {
        if let Some((rows, cut)) = diff::of_written(&action.body) {
            let (added, removed) = diff::counts(&rows);
            let verb = summary.split(' ').next().unwrap_or("Edited");
            let mut spans = vec![
                Span::styled(capitalized(verb), theme::bold(theme::text())),
                Span::styled(format!(" {}  ", action.language), theme::lit()),
            ];
            if added > 0 {
                spans.push(Span::styled(format!("+{added} "), theme::fg(p().add)));
            }
            if removed > 0 {
                spans.push(Span::styled(format!("-{removed}"), theme::fg(p().del)));
            }
            out.extend(header(Span::styled("●", theme::flame()), spans, cols));
            let limit = if open { usize::MAX } else { DIFF_ROWS };
            let lead = [Span::raw("  ")];
            out.extend(diff::draw(&rows, &action.language, cols, limit, &lead));
            if !cut.is_empty() {
                out.push(Line::from(Span::styled(format!("    {cut}"), theme::faint())));
            }
            return;
        }
    }

    let failed = summary.starts_with("declined") || summary.starts_with("could not") || summary.contains(" could not ");
    let mark = Span::styled("●", if failed { theme::bad() } else { theme::faint() });
    let style = if failed { theme::bad() } else { theme::dim() };
    out.extend(header(mark, vec![Span::styled(capitalized(&from_project(summary, root)), style)], cols));
    if !action.picture.is_empty() {
        out.push(Line::from(vec![
            Span::styled("  ⎿ ", theme::faint()),
            Span::styled(action.picture.clone(), theme::dim()),
        ]));
    }
    if open {
        output_rows(&action.body, cols, true, out);
    }
}

/// One stretch of thinking, where it happened among the work. A single line
/// until /showthinking opens them all.
fn thought_rows(text: &str, live: bool, hint: bool, show: bool, cols: usize, out: &mut Vec<Line<'static>>) {
    let body = text.trim();
    if body.is_empty() {
        return;
    }
    let word = if live { "Thinking" } else { "Thought" };
    let style = if live { theme::lit() } else { theme::dim() };
    let mut head = vec![Span::styled("✻ ", if live { theme::flame() } else { theme::faint() }), Span::styled(word, style)];
    if show {
        out.push(Line::from(head));
        let italic = theme::dim().add_modifier(Modifier::ITALIC);
        for line in body.lines() {
            out.extend(wrap(&[Span::styled(line.to_string(), italic)], cols, &[Span::raw("  ")], &[Span::raw("  ")]));
        }
    } else {
        if hint {
            head.push(Span::styled("  /showthinking", theme::faint()));
        }
        out.push(Line::from(head));
    }
}

/// Every row of one exchange.
pub fn rows(turn: &Turn, cols: usize, open: bool, show_thinking: bool, state: &State) -> Vec<Line<'static>> {
    let mut out: Vec<Line<'static>> = Vec::new();

    if !turn.attachments.is_empty() {
        out.push(Line::from(vec![
            Span::styled("  + ", theme::flame()),
            Span::styled(turn.attachments.join(", "), theme::dim()),
        ]));
    }
    if !turn.prompt.is_empty() {
        let prompt_lines: Vec<&str> = turn.prompt.lines().collect();
        let shown = if open { prompt_lines.len() } else { prompt_lines.len().min(12) };
        for (i, line) in prompt_lines.iter().take(shown).enumerate() {
            let first = [Span::styled(if i == 0 { "❯ " } else { "  " }, theme::flame())];
            let rest = [Span::raw("  ")];
            let spans = [Span::styled((*line).to_string(), theme::text())];
            for row in wrap(&spans, cols, &first, &rest) {
                out.push(band(row.spans, cols, p().raised));
            }
        }
        if prompt_lines.len() > shown {
            let more = format!("  … +{} lines", prompt_lines.len() - shown);
            out.push(band(vec![Span::styled(more, theme::dim())], cols, p().raised));
        }
    }

    // Who has it.
    if let Some(route) = &turn.route {
        let who = if route.expert.is_empty() { "crucible".to_string() } else { state.expert_name(&route.expert) };
        let mut spans = vec![
            Span::styled("◆ ", theme::lit()),
            Span::styled(who, theme::bold(theme::lit())),
        ];
        if route.confidence > 0.0 {
            spans.push(Span::styled(format!("  {:.0}%", route.confidence * 100.0), theme::faint()));
        }
        if !route.source.is_empty() && route.source != "router model" {
            spans.push(Span::styled(format!("  {}", route.source), theme::faint()));
        }
        if turn.load_ms > 0.0 {
            spans.push(Span::styled(format!("  swapped in {:.1}s", turn.load_ms / 1000.0), theme::faint()));
        }
        out.push(Line::default());
        out.push(Line::from(spans));
    }

    let root = state.raw.get("project").map(|p| crate::state::s(p, "root")).unwrap_or_default();
    // The thinking in among the work, in the order it happened. The last
    // stretch is still going while the turn is, until it has said something.
    let mut hint = true;
    let saying = !turn.reply.trim().is_empty();
    for (i, thought) in turn.thoughts.iter().enumerate() {
        let last = i + 1 == turn.thoughts.len();
        if !thought.trim().is_empty() {
            thought_rows(thought, last && turn.streaming && !saying, hint, show_thinking, cols, &mut out);
            hint = false;
        }
        if let Some(action) = turn.actions.get(i) {
            action_rows(action, cols, open, &root, &mut out);
        }
    }

    if turn.failed {
        let said = if turn.reply.trim().is_empty() { "It could not answer." } else { turn.reply.trim() };
        for line in said.lines() {
            out.extend(wrap(&[Span::styled(line.to_string(), theme::bad())], cols, &[Span::styled("✗ ", theme::bad())], &[Span::raw("  ")]));
        }
    } else if !turn.reply.trim().is_empty() {
        if !out.is_empty() {
            out.push(Line::default());
        }
        out.extend(indent(markdown::render(&turn.reply, cols.saturating_sub(2)), 2));
    }

    if !turn.streaming {
        let mut bits = Vec::new();
        if turn.tps > 0.0 {
            bits.push(format!("{:.1} tok/s", turn.tps));
        }
        if turn.prompt_tokens > 0.0 {
            bits.push(format!("{} in", compact(turn.prompt_tokens)));
        }
        if turn.output_tokens > 0.0 {
            bits.push(format!("{} out", compact(turn.output_tokens)));
        }
        if turn.canceled {
            bits.push("stopped".to_string());
        }
        if !bits.is_empty() {
            out.push(Line::from(Span::styled(format!("  {}", bits.join("   ")), theme::faint())));
        }
    }
    out.push(Line::default());
    let _ = line_width;
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text_of(lines: &[Line<'_>]) -> Vec<String> {
        lines.iter().map(|l| l.spans.iter().map(|s| s.content.as_ref()).collect::<String>().trim_end().to_string()).collect()
    }

    fn turn_rows(show: bool, streaming: bool) -> Vec<String> {
        let turn = Turn {
            thoughts: vec!["about to read".into(), "about to answer".into()],
            actions: vec![Action { summary: "read a.py".into(), ..Default::default() }],
            reasoning: "about to readabout to answer".into(),
            streaming,
            ..Default::default()
        };
        text_of(&rows(&turn, 60, false, show, &State::default()))
    }

    #[test]
    fn thinking_sits_among_the_work_and_starts_closed() {
        let out = turn_rows(false, false);
        let at = |needle: &str| out.iter().position(|l| l.contains(needle)).unwrap();
        assert!(at("Thought  /showthinking") < at("Read a.py"));
        assert!(at("Read a.py") < out.iter().rposition(|l| l.contains("Thought")).unwrap());
        assert_eq!(out.iter().filter(|l| l.contains("/showthinking")).count(), 1);
        assert!(!out.iter().any(|l| l.contains("about to read")));
    }

    #[test]
    fn showthinking_opens_it() {
        let out = turn_rows(true, false);
        let at = |needle: &str| out.iter().position(|l| l.contains(needle)).unwrap();
        assert!(at("about to read") < at("Read a.py"));
        assert!(at("Read a.py") < at("about to answer"));
    }

    #[test]
    fn the_stretch_still_going_says_so() {
        let out = turn_rows(false, true);
        assert!(out.iter().any(|l| l.contains("✻ Thinking")));
        assert_eq!(out.iter().filter(|l| l.contains("✻ Thought")).count(), 1);
    }

    fn action(summary: &str, body: &str, language: &str) -> Vec<String> {
        let mut out = Vec::new();
        let a = Action { summary: summary.into(), body: body.into(), language: language.into(), ..Default::default() };
        action_rows(&a, 40, false, "/work/app", &mut out);
        text_of(&out)
    }

    #[test]
    fn a_command_reads_as_a_command() {
        let out = action("$ npm test  (exit 1)", "failed\n", "");
        assert_eq!(out[0], "● $ npm test  exit 1");
        assert!(out[1].contains("failed"));
        let quiet = action("git status  (exit 0)", "", "");
        assert_eq!(quiet, vec!["● $ git status"]);
    }

    #[test]
    fn long_output_folds() {
        let body: String = (0..30).map(|i| format!("line {i}\n")).collect();
        let out = action("ls  (exit 0)", &body, "");
        assert!(out.len() < 12);
        assert!(out.last().unwrap().contains("+22 lines"));
    }

    #[test]
    fn an_edit_shows_its_diff() {
        let out = action("edited a.py  +1 -1", "@@ line 3 @@\n-old\n+new\n", "a.py");
        assert!(out[0].starts_with("● Edited a.py"));
        assert!(out.iter().any(|l| l.contains("- old")));
        assert!(out.iter().any(|l| l.contains("+ new")));
    }

    #[test]
    fn the_project_folder_comes_off_a_steps_path() {
        assert_eq!(action("read /work/app/src/a.py", "", ""), vec!["● Read src/a.py"]);
        assert_eq!(action("listed /work/app (3 entries)", "", ""), vec!["● Listed . (3 entries)"]);
        assert_eq!(action("$ cat /work/app/a  (exit 0)", "", "")[0], "● $ cat /work/app/a");
    }

    #[test]
    fn a_handoff_names_who_took_it() {
        let out = action("Writing takes over: the docs", "", "");
        assert!(out[0].starts_with("◆ Writing takes over"));
    }
}
