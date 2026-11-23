// SPDX-License-Identifier: MIT
//! The app's palette, for a terminal. Orange is where to look.

use ratatui::buffer::Buffer;
use ratatui::style::{Color, Modifier, Style};
use std::sync::atomic::{AtomicBool, Ordering};

#[derive(Clone, Copy)]
pub struct Palette {
    pub raised: Color,
    pub hover: Color,
    pub edge: Color,
    pub flame: Color,
    pub lit: Color,
    pub error: Color,
    pub good: Color,
    pub text: Color,
    pub dim: Color,
    pub faint: Color,
    pub ink: Color,
    pub add: Color,
    pub del: Color,
    pub add_bg: Color,
    pub del_bg: Color,
    pub code_bg: Color,
}

const fn rgb(v: u32) -> Color {
    Color::Rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)
}

const DARK: Palette = Palette {
    raised: rgb(0x16161A),
    hover: rgb(0x26262C),
    edge: rgb(0x3A3A40),
    flame: rgb(0xFF8700),
    lit: rgb(0xFFAF00),
    error: rgb(0xFF5F5F),
    good: rgb(0x7EC699),
    text: rgb(0xECECEC),
    dim: rgb(0x8C8C92),
    faint: rgb(0x5A5A60),
    ink: rgb(0x08080A),
    add: rgb(0x7EC699),
    del: rgb(0xE5847B),
    add_bg: rgb(0x12301F),
    del_bg: rgb(0x3A1A18),
    code_bg: rgb(0x141417),
};

const LIGHT: Palette = Palette {
    raised: rgb(0xF2F2F4),
    hover: rgb(0xE4E4E8),
    edge: rgb(0xC4C4CA),
    flame: rgb(0xC85A00),
    lit: rgb(0xA85000),
    error: rgb(0xC62828),
    good: rgb(0x2E7D4F),
    text: rgb(0x1C1C20),
    dim: rgb(0x5C5C64),
    faint: rgb(0x8E8E96),
    ink: rgb(0xFFFFFF),
    add: rgb(0x1E7A45),
    del: rgb(0xB3362B),
    add_bg: rgb(0xDDF3E4),
    del_bg: rgb(0xFBE3E0),
    code_bg: rgb(0xF6F6F8),
};

static LIGHT_ON: AtomicBool = AtomicBool::new(false);

pub fn set_light(on: bool) {
    LIGHT_ON.store(on, Ordering::Relaxed);
}

pub fn is_light() -> bool {
    LIGHT_ON.load(Ordering::Relaxed)
}

pub fn p() -> &'static Palette {
    if is_light() {
        &LIGHT
    } else {
        &DARK
    }
}

pub fn fg(color: Color) -> Style {
    Style::default().fg(color)
}
pub fn text() -> Style {
    fg(p().text)
}
pub fn dim() -> Style {
    fg(p().dim)
}
pub fn faint() -> Style {
    fg(p().faint)
}
pub fn flame() -> Style {
    fg(p().flame)
}
pub fn lit() -> Style {
    fg(p().lit)
}
pub fn bad() -> Style {
    fg(p().error)
}
pub fn good() -> Style {
    fg(p().good)
}
pub fn bold(style: Style) -> Style {
    style.add_modifier(Modifier::BOLD)
}
/// Whether this terminal draws 24-bit color. Apple's Terminal does not, and
/// reads the codes for it as something else entirely.
pub fn truecolor() -> bool {
    let var = |name: &str| std::env::var(name).unwrap_or_default().to_lowercase();
    if var("TERM_PROGRAM") == "apple_terminal" {
        return false;
    }
    let colorterm = var("COLORTERM");
    if colorterm.contains("truecolor") || colorterm.contains("24bit") {
        return true;
    }
    let term = var("TERM");
    ["kitty", "alacritty", "wezterm", "ghostty", "foot", "konsole", "direct"].iter().any(|t| term.contains(t))
        || ["iterm.app", "wezterm", "vscode", "ghostty", "tabby", "hyper"].contains(&var("TERM_PROGRAM").as_str())
        || std::env::var_os("KONSOLE_VERSION").is_some()
        || std::env::var_os("WT_SESSION").is_some()
        || var("VTE_VERSION").parse::<u32>().is_ok_and(|v| v >= 3600)
}

fn nearest(color: Color) -> Color {
    let Color::Rgb(r, g, b) = color else { return color };
    let cube = |v: u8| -> u8 {
        if v < 48 {
            0
        } else if v < 115 {
            1
        } else {
            (v - 35) / 40
        }
    };
    let level = |n: u8| -> i32 {
        if n == 0 {
            0
        } else {
            55 + 40 * n as i32
        }
    };
    let (cr, cg, cb) = (cube(r), cube(g), cube(b));
    let distance = |x: i32, y: i32, z: i32| {
        (r as i32 - x).pow(2) + (g as i32 - y).pow(2) + (b as i32 - z).pow(2)
    };
    let in_cube = distance(level(cr), level(cg), level(cb));
    let average = (r as i32 + g as i32 + b as i32) / 3;
    let step = ((average - 8).max(0) / 10).min(23);
    let grey = 8 + step * 10;
    if distance(grey, grey, grey) < in_cube {
        Color::Indexed(232 + step as u8)
    } else {
        Color::Indexed(16 + 36 * cr + 6 * cg + cb)
    }
}

/// Every color in the frame, as the nearest of the 256 this terminal has.
pub fn downgrade(buffer: &mut Buffer) {
    for cell in &mut buffer.content {
        cell.fg = nearest(cell.fg);
        cell.bg = nearest(cell.bg);
    }
}
