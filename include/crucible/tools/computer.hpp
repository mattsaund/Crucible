// SPDX-License-Identifier: MIT
//
// Using the computer: the screen, the mouse and the keyboard.
//
// An expert that can take a screenshot, click where it sees a button and
// type into what opens can do the parts of a job that happen outside the
// project folder -- try the program it just built, read a page a browser
// shows, fill a form. This is that, done with what each platform already
// has: `screencapture` and `osascript` on a Mac, `xdotool` with an X11
// screenshot tool on Linux, PowerShell on Windows. Nothing is installed and
// nothing is linked in; where a tool is missing the attempt says which one.
//
// It is off until it is switched on, and it is the one tool that is not
// about the project. A folder bounds a WRITE; nothing bounds a click. The
// settings screen says that in as many words, and the first use on a Mac
// is where the system asks for the Accessibility and Screen Recording
// permissions it will not grant quietly.
//
// A screenshot is a picture, and a model on this machine reads text only.
// Where tesseract is installed the picture's words are read off it, so a
// local expert can see at least what the screen says; a provider's model
// that sees pictures is sent the picture itself.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::tools::computer {

/// What a screen action came to.
struct Outcome {
    bool        ok = false;
    std::string summary;   ///< one line for the journal
    std::string error;     ///< why not, when not -- including which program is missing
};

/// Is there a way to take a screenshot here, and which program is it?
/// Empty when there is none, with `why` saying what to install.
std::string screenshot_tool(std::string& why);

/// Is there a way to move the mouse and type here? Empty with `why` when not.
std::string input_tool(std::string& why);

/// Take a screenshot into `file` (PNG), shrunk to at most `max_side` pixels
/// on its longer side where the platform can shrink one. `display` picks a
/// screen on a machine with several; 0 is the main one.
Outcome screenshot(const std::filesystem::path& file, int max_side = 1568, int display = 0);

/// Move the mouse to (x, y) in screen pixels.
Outcome move(int x, int y);

/// Click at (x, y): `button` is "left", "right" or "middle"; `count` is 1 or 2.
Outcome click(int x, int y, std::string_view button = "left", int count = 1);

/// Type `text` as keystrokes into whatever has the focus.
Outcome type_text(std::string_view text);

/// Press a key or a combination: "enter", "tab", "escape", "ctrl+c",
/// "cmd+shift+s", "alt+f4", "f5", "down"... Written the way a person names
/// it; each platform spells it its own way underneath.
Outcome press(std::string_view combination);

/// Scroll by `dy` notches (negative is up) and `dx` (negative is left), at the
/// pointer's position.
Outcome scroll(int dx, int dy);

/// The words in a picture, when tesseract is installed; empty otherwise, with
/// `why` saying so.
std::string read_text(const std::filesystem::path& picture, std::string& why);

/// Whether tesseract is on the machine.
bool ocr_available();

/// The screen's size in pixels, "1920x1080", or empty when it cannot be asked.
std::string screen_size();

// --- exposed for the tests -------------------------------------------------

namespace detail {

/// A combination like "ctrl+shift+s" as its pieces, lower case: the
/// modifiers and the key.
struct Keys {
    bool        ctrl  = false;
    bool        shift = false;
    bool        alt   = false;
    bool        cmd   = false;   ///< the command key on a Mac, the Windows key elsewhere
    std::string key;             ///< "s", "enter", "f5"
};
Keys parse_keys(std::string_view combination);

/// The macOS key code for a named key, or -1 when the name is not one macOS
/// names and should be typed as a character instead.
int mac_key_code(std::string_view key);

/// The xdotool name for a key: "Return" for enter, "ctrl+c" as is.
std::string xdotool_key(const Keys& keys);

/// The SendKeys string for a key on Windows: "{ENTER}", "^c".
std::string sendkeys(const Keys& keys);

}  // namespace detail

}  // namespace crucible::tools::computer
