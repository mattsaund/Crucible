// SPDX-License-Identifier: MIT
//! Code in color, the same colors the app uses.
//!
//! A block still arriving is only ever longer than it was, so what has been
//! colored is kept and only the new lines are worked out.

use crate::theme;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::Span;
use std::cell::RefCell;
use std::str::FromStr;
use syntect::highlighting as sh;
use syntect::parsing::{ParseState, ScopeStack, SyntaxSet};

fn color(v: u32) -> sh::Color {
    sh::Color { r: (v >> 16) as u8, g: (v >> 8) as u8, b: v as u8, a: 255 }
}

fn item(scopes: &str, v: u32, italic: bool) -> sh::ThemeItem {
    sh::ThemeItem {
        scope: sh::ScopeSelectors::from_str(scopes).expect("a scope selector"),
        style: sh::StyleModifier {
            foreground: Some(color(v)),
            background: None,
            font_style: italic.then_some(sh::FontStyle::ITALIC),
        },
    }
}

/// comment, string, keyword, type, number, name, and everything else.
fn theme_of(c: [u32; 7]) -> sh::Theme {
    sh::Theme {
        name: None,
        author: None,
        settings: sh::ThemeSettings { foreground: Some(color(c[6])), ..Default::default() },
        scopes: vec![
            item("comment, punctuation.definition.comment", c[0], true),
            item("string, punctuation.definition.string, markup.inline", c[1], false),
            item("keyword, storage, keyword.control, keyword.operator.word, markup.heading", c[2], false),
            item("storage.type, entity.name.type, entity.name.class, support.type, support.class, entity.other.inherited-class", c[3], false),
            item("constant.numeric, constant.language, constant.character, constant.other", c[4], false),
            item("entity.name.function, support.function, variable.function, entity.name.tag, entity.other.attribute-name", c[5], false),
        ],
    }
}

struct Live {
    lang: String,
    text: String,
    lines: Vec<Vec<Span<'static>>>,
    parse: ParseState,
    high: sh::HighlightState,
}

struct Colors {
    syntaxes: SyntaxSet,
    dark: sh::Theme,
    light: sh::Theme,
    live: Vec<Live>,
}

thread_local! {
    static COLORS: RefCell<Option<Colors>> = const { RefCell::new(None) };
}

fn spans_of(line: &str, parse: &mut ParseState, high: &mut sh::HighlightState, set: &SyntaxSet, theme: &sh::Theme) -> Vec<Span<'static>> {
    let ops = parse.parse_line(line, set).unwrap_or_default();
    let highlighter = sh::Highlighter::new(theme);
    let mut out = Vec::new();
    for (style, piece) in sh::HighlightIterator::new(high, &ops, line, &highlighter) {
        let piece = piece.trim_end_matches(['\n', '\r']);
        if piece.is_empty() {
            continue;
        }
        let mut s = Style::default().fg(Color::Rgb(style.foreground.r, style.foreground.g, style.foreground.b));
        if style.font_style.contains(sh::FontStyle::ITALIC) {
            s = s.add_modifier(Modifier::ITALIC);
        }
        out.push(Span::styled(piece.to_string(), s));
    }
    out
}

fn plain(text: &str) -> Vec<Vec<Span<'static>>> {
    text.split('\n').map(|l| vec![Span::styled(l.to_string(), theme::text())]).collect()
}

/// The language a file's name says it is in, for coloring.
pub fn language_of(path: &str) -> String {
    let name = path.rsplit(['/', '\\']).next().unwrap_or(path);
    match name.to_lowercase().as_str() {
        "makefile" => return "make".into(),
        "dockerfile" => return "dockerfile".into(),
        "cmakelists.txt" => return "cmake".into(),
        _ => {}
    }
    name.rsplit_once('.').map(|(_, e)| e.to_lowercase()).unwrap_or_default()
}

/// One row of colored spans for each line of `code`.
pub fn code(code: &str, lang: &str) -> Vec<Vec<Span<'static>>> {
    let lang = lang.trim().split([' ', ',', '{']).next().unwrap_or("").to_lowercase();
    if lang.is_empty() || code.len() > 400_000 {
        return plain(code);
    }
    COLORS.with(|cell| {
        let mut slot = cell.borrow_mut();
        let colors = slot.get_or_insert_with(|| Colors {
            syntaxes: SyntaxSet::load_defaults_newlines(),
            dark: theme_of([0x6E6E76, 0xC9A227, 0xFF8700, 0x7FA7C4, 0xB07FC4, 0x7EC699, 0xECECEC]),
            light: theme_of([0x7A7A82, 0x8A6D00, 0xC85A00, 0x2F6F99, 0x7B3FA0, 0x1E7A45, 0x1C1C20]),
            live: Vec::new(),
        });
        let alias = match lang.as_str() {
            "ts" | "tsx" | "typescript" | "jsx" | "mjs" | "cjs" => "js",
            "shell" | "zsh" | "fish" | "console" => "bash",
            "cxx" | "hpp" | "hxx" | "cc" => "cpp",
            "yml" => "yaml",
            "py3" => "py",
            other => other,
        };
        let Some(syntax) = colors.syntaxes.find_syntax_by_token(alias).cloned() else {
            return plain(code);
        };
        let Colors { syntaxes, dark, light, live } = colors;
        let theme = if theme::is_light() { &*light } else { &*dark };

        let found = live
            .iter()
            .enumerate()
            .filter(|(_, l)| l.lang == lang && code.starts_with(l.text.as_str()))
            .max_by_key(|(_, l)| l.text.len())
            .map(|(i, _)| i);
        let at = found.unwrap_or_else(|| {
            if live.len() >= 64 {
                live.remove(0);
            }
            let highlighter = sh::Highlighter::new(theme);
            live.push(Live {
                lang: lang.clone(),
                text: String::new(),
                lines: Vec::new(),
                parse: ParseState::new(&syntax),
                high: sh::HighlightState::new(&highlighter, ScopeStack::new()),
            });
            live.len() - 1
        });
        let entry = &mut live[at];
        let mut from = entry.text.len();
        while let Some(end) = code[from..].find('\n') {
            let line = &code[from..=from + end];
            let spans = spans_of(line, &mut entry.parse, &mut entry.high, syntaxes, theme);
            entry.lines.push(spans);
            from += end + 1;
        }
        entry.text.push_str(&code[entry.text.len()..from]);
        let mut out = entry.lines.clone();
        if from < code.len() {
            let mut parse = entry.parse.clone();
            let mut high = entry.high.clone();
            let mut tail = code[from..].to_string();
            tail.push('\n');
            out.push(spans_of(&tail, &mut parse, &mut high, syntaxes, theme));
        } else {
            out.push(Vec::new());
        }
        out
    })
}
