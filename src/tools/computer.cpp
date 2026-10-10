// SPDX-License-Identifier: MIT
//
// The screen, the mouse and the keyboard, through each platform's own
// programs. See computer.hpp.
#include "crucible/tools/computer.hpp"

#include "crucible/kit/kit.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>

#include "crucible/config/paths.hpp"
#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"

namespace crucible::tools::computer {
namespace {

/// Run a program, wait for it (at most `timeout` seconds), and return what
/// it printed. `status` is its exit code, or -1 when it could not start.
std::string run(const std::vector<std::string>& argv, int& status, int timeout = 30,
                const std::vector<std::string>& env = {}) {
    util::Subprocess child;
    std::string error;
    status = -1;
    if (!child.start(argv, {}, env, error)) {
        return error;
    }
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (!finished.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() >= deadline) {
                child.terminate();
                return;
            }
        }
    });
    std::string out;
    std::string line;
    while (child.read_line(line)) {
        out += line;
        out += '\n';
    }
    status = child.wait();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();
    return crucible::detail::console_to_utf8(out);
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trimmed(std::string text) {
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text.erase(last == std::string::npos ? 0 : last + 1);
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    text.erase(0, first == std::string::npos ? text.size() : first);
    return text;
}

Outcome failed(std::string why) {
    Outcome out;
    out.error = std::move(why);
    return out;
}

Outcome done(std::string summary) {
    Outcome out;
    out.ok      = true;
    out.summary = std::move(summary);
    return out;
}

/// A JavaScript string literal, for the JXA that macOS runs.
std::string js_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out + "\"";
}

#if defined(_WIN32)
/// A PowerShell single-quoted literal: only the quote needs doubling.
std::string ps_string(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        out += c;
        if (c == '\'') {
            out += '\'';
        }
    }
    return out + "'";
}
#endif

#if defined(__APPLE__)

/// Run JavaScript for Automation. What macOS gives every machine for driving
/// the mouse and keyboard without anything installed: CoreGraphics events
/// through ObjC bridging, which need the Accessibility permission, and the
/// system says so itself when it is missing.
Outcome jxa(const std::string& script, std::string summary) {
    int status = 0;
    const std::string out = run({"osascript", "-l", "JavaScript", "-e", script}, status, 30);
    if (status != 0) {
        std::string why = trimmed(out);
        if (why.find("not allowed") != std::string::npos || why.find("1002") != std::string::npos
            || why.find("assistive") != std::string::npos) {
            why += " -- allow Crucible under System Settings, Privacy & Security, Accessibility";
        }
        return failed("could not control the computer: " + (why.empty() ? "osascript failed" : why));
    }
    return done(std::move(summary));
}

const std::string kBridge =
    "ObjC.import('Cocoa');"
    "function post(e){$.CGEventPost($.kCGHIDEventTap,e);}"
    "function sleep(ms){$.usleep(ms*1000);}";

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Key names
// ---------------------------------------------------------------------------

namespace detail {

Keys parse_keys(std::string_view combination) {
    Keys keys;
    std::string text = lower(combination);
    // The plus key on its own, which the loop below would read as a separator
    // with nothing either side of it.
    if (trimmed(text) == "+") {
        keys.key = "+";
        return keys;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t plus = text.find('+', start);
        // A trailing "+" is the plus key itself: "ctrl++".
        if (plus == start && start > 0) {
            plus = std::string::npos;
        }
        const std::string piece = trimmed(text.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (piece == "ctrl" || piece == "control") {
            keys.ctrl = true;
        } else if (piece == "shift") {
            keys.shift = true;
        } else if (piece == "alt" || piece == "option" || piece == "opt") {
            keys.alt = true;
        } else if (piece == "cmd" || piece == "command" || piece == "super" || piece == "win"
                   || piece == "meta") {
            keys.cmd = true;
        } else if (!piece.empty()) {
            keys.key = piece;
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
    if (keys.key == "return") { keys.key = "enter"; }
    if (keys.key == "esc")    { keys.key = "escape"; }
    if (keys.key == "del")    { keys.key = "delete"; }
    if (keys.key == "bksp" || keys.key == "backspace" || keys.key == "back") { keys.key = "backspace"; }
    if (keys.key == "pgup")   { keys.key = "pageup"; }
    if (keys.key == "pgdn" || keys.key == "pgdown") { keys.key = "pagedown"; }
    return keys;
}

int mac_key_code(std::string_view key) {
    static const std::map<std::string, int> codes{
        {"enter", 36}, {"tab", 48}, {"space", 49}, {"delete", 117}, {"backspace", 51},
        {"escape", 53}, {"left", 123}, {"right", 124}, {"down", 125}, {"up", 126},
        {"home", 115}, {"end", 119}, {"pageup", 116}, {"pagedown", 121},
        {"f1", 122}, {"f2", 120}, {"f3", 99}, {"f4", 118}, {"f5", 96}, {"f6", 97},
        {"f7", 98}, {"f8", 100}, {"f9", 101}, {"f10", 109}, {"f11", 103}, {"f12", 111},
        // Letters and digits, so a combination can be sent as a key event
        // rather than typed: a typed "c" with command down is not copy.
        {"a", 0}, {"s", 1}, {"d", 2}, {"f", 3}, {"h", 4}, {"g", 5}, {"z", 6}, {"x", 7},
        {"c", 8}, {"v", 9}, {"b", 11}, {"q", 12}, {"w", 13}, {"e", 14}, {"r", 15},
        {"y", 16}, {"t", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22},
        {"5", 23}, {"=", 24}, {"9", 25}, {"7", 26}, {"-", 27}, {"8", 28}, {"0", 29},
        {"]", 30}, {"o", 31}, {"u", 32}, {"[", 33}, {"i", 34}, {"p", 35}, {"l", 37},
        {"j", 38}, {"'", 39}, {"k", 40}, {";", 41}, {"\\", 42}, {",", 43}, {"/", 44},
        {"n", 45}, {"m", 46}, {".", 47}, {"`", 50},
    };
    const auto found = codes.find(std::string(key));
    return found == codes.end() ? -1 : found->second;
}

std::string xdotool_key(const Keys& keys) {
    static const std::map<std::string, std::string> names{
        {"enter", "Return"}, {"tab", "Tab"}, {"space", "space"}, {"escape", "Escape"},
        {"backspace", "BackSpace"}, {"delete", "Delete"}, {"left", "Left"}, {"right", "Right"},
        {"up", "Up"}, {"down", "Down"}, {"home", "Home"}, {"end", "End"},
        {"pageup", "Prior"}, {"pagedown", "Next"},
    };
    std::string out;
    if (keys.ctrl)  { out += "ctrl+"; }
    if (keys.shift) { out += "shift+"; }
    if (keys.alt)   { out += "alt+"; }
    if (keys.cmd)   { out += "super+"; }
    const auto named = names.find(keys.key);
    if (named != names.end()) {
        out += named->second;
    } else if (keys.key.size() == 2 && keys.key[0] == 'f' && std::isdigit(static_cast<unsigned char>(keys.key[1]))) {
        out += "F" + keys.key.substr(1);
    } else if (keys.key.size() == 3 && keys.key[0] == 'f') {
        out += "F" + keys.key.substr(1);
    } else {
        out += keys.key;
    }
    return out;
}

std::string sendkeys(const Keys& keys) {
    static const std::map<std::string, std::string> names{
        {"enter", "{ENTER}"}, {"tab", "{TAB}"}, {"space", " "}, {"escape", "{ESC}"},
        {"backspace", "{BACKSPACE}"}, {"delete", "{DELETE}"}, {"left", "{LEFT}"}, {"right", "{RIGHT}"},
        {"up", "{UP}"}, {"down", "{DOWN}"}, {"home", "{HOME}"}, {"end", "{END}"},
        {"pageup", "{PGUP}"}, {"pagedown", "{PGDN}"},
    };
    std::string out;
    if (keys.ctrl)  { out += "^"; }
    if (keys.shift) { out += "+"; }
    if (keys.alt)   { out += "%"; }
    const auto named = names.find(keys.key);
    if (named != names.end()) {
        out += named->second;
    } else if (!keys.key.empty() && keys.key[0] == 'f' && keys.key.size() <= 3
               && std::all_of(keys.key.begin() + 1, keys.key.end(),
                              [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        out += "{F" + keys.key.substr(1) + "}";
    } else {
        // SendKeys reads + ^ % ~ ( ) { } as its own syntax; a literal one is
        // braced.
        for (const char c : keys.key) {
            if (std::string("+^%~(){}[]").find(c) != std::string::npos) {
                out += "{";
                out += c;
                out += "}";
            } else {
                out += c;
            }
        }
    }
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// What is here to use
// ---------------------------------------------------------------------------

std::string screenshot_tool(std::string& why) {
    why.clear();
#if defined(__APPLE__)
    return "screencapture";
#elif defined(_WIN32)
    return "powershell";
#else
    for (const char* tool : {"grim", "gnome-screenshot", "spectacle", "scrot", "import", "xwd"}) {
        if (util::on_path(tool)) {
            return tool;
        }
    }
    why = "no screenshot program is installed -- grim (Wayland), gnome-screenshot, spectacle, "
          "scrot or ImageMagick's import would do";
    return {};
#endif
}

std::string input_tool(std::string& why) {
    why.clear();
#if defined(__APPLE__)
    return "osascript";
#elif defined(_WIN32)
    return "powershell";
#else
    if (util::on_path("xdotool")) {
        return "xdotool";
    }
    if (util::on_path("ydotool")) {
        return "ydotool";
    }
    why = "no program to drive the mouse and keyboard is installed -- xdotool (X11) or "
          "ydotool (Wayland)";
    return {};
#endif
}

// --- the words in a picture ------------------------------------------------
//
// Read by what the machine has, so nothing need be installed on two of the
// three: a Mac's Vision framework, asked through JavaScript for Automation
// the way the screen verbs ask for the mouse; Windows' own OCR engine, asked
// through PowerShell's view of WinRT; and on Linux, which has neither,
// tesseract when it is there and the kit's RapidOCR when it is not.

#if defined(__APPLE__)
/// Vision's text recognizer, the one Live Text uses, on the file given.
constexpr const char* kVisionScript = R"JXA(
ObjC.import('Foundation');
ObjC.import('Vision');
function run(argv) {
  var handler = $.VNImageRequestHandler.alloc.initWithURLOptions($.NSURL.fileURLWithPath(argv[0]), $({}));
  var request = $.VNRecognizeTextRequest.alloc.init;
  request.recognitionLevel = 0;
  request.usesLanguageCorrection = true;
  var error = $();
  if (!handler.performRequestsError($([request]), error)) { throw new Error('Vision could not read it'); }
  var lines = [];
  for (var i = 0; i < request.results.count; i++) {
    var best = request.results.objectAtIndex(i).topCandidates(1);
    if (best.count > 0) lines.push(ObjC.unwrap(best.objectAtIndex(0).string));
  }
  return lines.join('\n');
}
)JXA";
#elif defined(_WIN32)
/// PowerShell, asked to run `script` and to stop at the first thing that
/// fails. Left to itself it carries on past an error and exits 0, and what it
/// said about the failure is read back as though it were the answer: an OCR
/// that could not open a picture came back as the words in it that way. The
/// OCR script below says the same thing on its first line.
std::vector<std::string> powershell(const std::string& script) {
    return {"powershell", "-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference = 'Stop'; " + script};
}

/// Windows.Media.Ocr, from Windows PowerShell -- 5.1, which has WinRT; the
/// newer PowerShell does not.
constexpr const char* kWindowsOcrScript = R"PS(
param([string]$Path)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) { $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result }
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Path)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if ($engine -eq $null) { throw 'no OCR language is installed' }
$result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
$result.Lines | ForEach-Object { $_.Text }
)PS";
#else
/// RapidOCR in the kit's Python, for a Linux machine without tesseract.
constexpr const char* kRapidOcrScript =
    "import sys\n"
    "from rapidocr_onnxruntime import RapidOCR\n"
    "result, _ = RapidOCR()(sys.argv[1])\n"
    "print('\\n'.join(line[1] for line in (result or [])))\n";
#endif

bool ocr_available() {
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return util::on_path("tesseract") || !kit::ocr_python().empty();
#endif
}

std::string read_text(const std::filesystem::path& picture, std::string& why) {
    why.clear();
    int status = 0;
    std::string text;
#if defined(__APPLE__)
    text = run({"osascript", "-l", "JavaScript", "-e", kVisionScript, picture.string()}, status, 60);
#elif defined(_WIN32)
    const std::filesystem::path script = std::filesystem::temp_directory_path() / "crucible-ocr.ps1";
    std::ofstream(script, std::ios::binary | std::ios::trunc) << kWindowsOcrScript;
    text = run({"powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                script.string(), picture.string()}, status, 60);
#else
    if (util::on_path("tesseract")) {
        text = run({"tesseract", picture.string(), "-", "--psm", "3"}, status, 60);
    } else if (const std::filesystem::path python = kit::ocr_python(); !python.empty()) {
        text = run({python.string(), "-I", "-c", kRapidOcrScript, picture.string()}, status, 120);
    } else {
        why = "nothing on this machine reads the words in a picture yet -- Crucible fetches a reader "
              "as it starts, or install tesseract";
        return {};
    }
#endif
    if (status != 0) {
        why = "the words in it could not be read: " + trimmed(text);
        return {};
    }
    return trimmed(text);
}

std::string screen_size() {
    int status = 0;
#if defined(__APPLE__)
    const std::string out = run({"osascript", "-l", "JavaScript", "-e",
                                 "ObjC.import('Cocoa'); var f=$.NSScreen.mainScreen.frame; "
                                 "String(Math.round(f.size.width))+'x'+String(Math.round(f.size.height))"},
                                status, 20);
#elif defined(_WIN32)
    const std::string out = run(powershell("Add-Type -AssemblyName System.Windows.Forms; "
                                 "$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "
                                 "\"$($b.Width)x$($b.Height)\""),
                                status, 20);
#else
    std::string out;
    if (util::on_path("xdotool")) {
        out = run({"xdotool", "getdisplaygeometry"}, status, 20);
        // "1920 1080"
        const std::size_t space = out.find(' ');
        if (status == 0 && space != std::string::npos) {
            out = trimmed(out.substr(0, space)) + "x" + trimmed(out.substr(space + 1));
        }
    }
#endif
    return status == 0 ? trimmed(out) : std::string();
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

Outcome screenshot(const std::filesystem::path& file, int max_side, int display) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::remove(file, ec);
    int status = 0;
    std::string out;
#if defined(__APPLE__)
    (void)display;
    out = run({"screencapture", "-x", "-t", "png", file.string()}, status, 30);
    if (status != 0 || !std::filesystem::exists(file, ec)) {
        std::string why = trimmed(out);
        return failed("could not take a screenshot: " + (why.empty() ? "screencapture failed" : why)
                      + " -- allow Crucible under System Settings, Privacy & Security, Screen Recording");
    }
    if (max_side > 0) {
        // sips is part of macOS, and shrinks in place.
        run({"sips", "--resampleHeightWidthMax", std::to_string(max_side), file.string()}, status, 30);
    }
#elif defined(_WIN32)
    (void)display;
    const std::string script =
        "Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing; "
        "$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "
        "$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height; "
        "$g=[System.Drawing.Graphics]::FromImage($bmp); "
        "$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size); "
        "$max=" + std::to_string(std::max(max_side, 1)) + "; "
        "$scale=[Math]::Min(1,[Math]::Min($max/$b.Width,$max/$b.Height)); "
        "if ($scale -lt 1) { $w=[int]($b.Width*$scale); $h=[int]($b.Height*$scale); "
        "$small=New-Object System.Drawing.Bitmap $w,$h; "
        "$g2=[System.Drawing.Graphics]::FromImage($small); $g2.DrawImage($bmp,0,0,$w,$h); "
        "$small.Save(" + ps_string(file.string()) + ",[System.Drawing.Imaging.ImageFormat]::Png) } "
        "else { $bmp.Save(" + ps_string(file.string()) + ",[System.Drawing.Imaging.ImageFormat]::Png) }";
    out = run(powershell(script), status, 60);
    if (status != 0 || !std::filesystem::exists(file, ec)) {
        return failed("could not take a screenshot: " + trimmed(out));
    }
#else
    std::string why;
    const std::string tool = screenshot_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    std::vector<std::string> argv;
    if (tool == "grim")               { argv = {"grim", file.string()}; }
    else if (tool == "gnome-screenshot") { argv = {"gnome-screenshot", "-f", file.string()}; }
    else if (tool == "spectacle")     { argv = {"spectacle", "-b", "-n", "-o", file.string()}; }
    else if (tool == "scrot")         { argv = {"scrot", "-o", file.string()}; }
    else if (tool == "import")        { argv = {"import", "-window", "root", file.string()}; }
    else                              { argv = {"xwd", "-root", "-out", file.string()}; }
    (void)display;
    out = run(argv, status, 30);
    if (status != 0 || !std::filesystem::exists(file, ec)) {
        return failed("could not take a screenshot with " + tool + ": " + trimmed(out));
    }
    if (max_side > 0 && util::on_path("convert")) {
        const std::string size = std::to_string(max_side) + "x" + std::to_string(max_side) + ">";
        run({"convert", file.string(), "-resize", size, file.string()}, status, 30);
    }
#endif
    return done("took a screenshot");
}

// ---------------------------------------------------------------------------
// The mouse and the keyboard
// ---------------------------------------------------------------------------

Outcome move(int x, int y) {
#if defined(__APPLE__)
    return jxa(kBridge + "var p=$.CGPointMake(" + std::to_string(x) + "," + std::to_string(y) + ");"
               "post($.CGEventCreateMouseEvent(null,$.kCGEventMouseMoved,p,$.kCGMouseButtonLeft));",
               "moved the mouse to " + std::to_string(x) + ", " + std::to_string(y));
#elif defined(_WIN32)
    int status = 0;
    const std::string out = run(powershell("Add-Type -AssemblyName System.Windows.Forms; "
                                 "[System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point("
                                 + std::to_string(x) + "," + std::to_string(y) + ")"), status, 20);
    return status == 0 ? done("moved the mouse to " + std::to_string(x) + ", " + std::to_string(y))
                       : failed("could not move the mouse: " + trimmed(out));
#else
    std::string why;
    const std::string tool = input_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    int status = 0;
    const std::string out = run({tool, "mousemove", std::to_string(x), std::to_string(y)}, status, 20);
    return status == 0 ? done("moved the mouse to " + std::to_string(x) + ", " + std::to_string(y))
                       : failed("could not move the mouse: " + trimmed(out));
#endif
}

Outcome click(int x, int y, std::string_view button, int count) {
    const std::string which = lower(button);
    count = std::clamp(count, 1, 3);
    const std::string summary = (count == 2 ? "double-clicked" : count == 3 ? "triple-clicked" : "clicked")
                              + std::string(which == "left" ? "" : " the " + which + " button")
                              + " at " + std::to_string(x) + ", " + std::to_string(y);
#if defined(__APPLE__)
    const std::string down = which == "right" ? "kCGEventRightMouseDown" : which == "middle"
                           ? "kCGEventOtherMouseDown" : "kCGEventLeftMouseDown";
    const std::string up   = which == "right" ? "kCGEventRightMouseUp" : which == "middle"
                           ? "kCGEventOtherMouseUp" : "kCGEventLeftMouseUp";
    const std::string btn  = which == "right" ? "kCGMouseButtonRight" : which == "middle"
                           ? "kCGMouseButtonCenter" : "kCGMouseButtonLeft";
    std::string script = kBridge + "var p=$.CGPointMake(" + std::to_string(x) + "," + std::to_string(y) + ");"
        "post($.CGEventCreateMouseEvent(null,$.kCGEventMouseMoved,p,$." + btn + "));sleep(60);";
    for (int n = 1; n <= count; ++n) {
        script += "var d=$.CGEventCreateMouseEvent(null,$." + down + ",p,$." + btn + ");"
                  "$.CGEventSetIntegerValueField(d,$.kCGMouseEventClickState," + std::to_string(n) + ");post(d);"
                  "var u=$.CGEventCreateMouseEvent(null,$." + up + ",p,$." + btn + ");"
                  "$.CGEventSetIntegerValueField(u,$.kCGMouseEventClickState," + std::to_string(n) + ");post(u);"
                  "sleep(80);";
    }
    return jxa(script, summary);
#elif defined(_WIN32)
    // mouse_event is old, but it is in every Windows and needs no package.
    const std::string flags = which == "right" ? "0x0008,0x0010" : which == "middle" ? "0x0020,0x0040" : "0x0002,0x0004";
    std::string script =
        "Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern void mouse_event(uint f, uint x, uint y, uint d, int e);' -Name M -Namespace W; "
        "[System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(" + std::to_string(x) + "," + std::to_string(y) + "); "
        "Start-Sleep -Milliseconds 60; ";
    for (int n = 0; n < count; ++n) {
        script += "[W.M]::mouse_event(" + flags.substr(0, flags.find(',')) + ",0,0,0,0); "
                  "[W.M]::mouse_event(" + flags.substr(flags.find(',') + 1) + ",0,0,0,0); Start-Sleep -Milliseconds 80; ";
    }
    int status = 0;
    const std::string out = run(powershell(script), status, 20);
    return status == 0 ? done(summary) : failed("could not click: " + trimmed(out));
#else
    std::string why;
    const std::string tool = input_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    const std::string number = which == "right" ? "3" : which == "middle" ? "2" : "1";
    int status = 0;
    std::string out = run({tool, "mousemove", std::to_string(x), std::to_string(y)}, status, 20);
    if (status == 0) {
        std::vector<std::string> argv{tool, "click"};
        if (count > 1) {
            argv.insert(argv.end(), {"--repeat", std::to_string(count), "--delay", "80"});
        }
        argv.push_back(number);
        out = run(argv, status, 20);
    }
    return status == 0 ? done(summary) : failed("could not click: " + trimmed(out));
#endif
}

Outcome type_text(std::string_view text) {
    if (text.empty()) {
        return failed("nothing to type");
    }
    const std::string summary = "typed " + std::to_string(text.size()) + " characters";
#if defined(__APPLE__)
    // System Events types text as the keyboard would, in whatever has the
    // focus; CGEventKeyboardSetUnicodeString would too, a character at a
    // time, and this is one line.
    return jxa("var se=Application('System Events'); se.keystroke(" + js_string(text) + ");", summary);
#elif defined(_WIN32)
    // SendKeys reads + ^ % ~ ( ) { } as its own syntax, so each is braced.
    std::string braced;
    for (const char c : text) {
        if (std::string("+^%~(){}[]").find(c) != std::string::npos) {
            braced += "{";
            braced += c;
            braced += "}";
        } else if (c == '\n') {
            braced += "{ENTER}";
        } else if (c != '\r') {
            braced += c;
        }
    }
    int status = 0;
    const std::string out = run(powershell("Add-Type -AssemblyName System.Windows.Forms; "
                                 "[System.Windows.Forms.SendKeys]::SendWait(" + ps_string(braced) + ")"),
                                status, 60);
    return status == 0 ? done(summary) : failed("could not type: " + trimmed(out));
#else
    std::string why;
    const std::string tool = input_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    int status = 0;
    const std::string out = run({tool, "type", "--delay", "12", std::string(text)}, status, 120);
    return status == 0 ? done(summary) : failed("could not type: " + trimmed(out));
#endif
}

Outcome press(std::string_view combination) {
    const detail::Keys keys = detail::parse_keys(combination);
    if (keys.key.empty()) {
        return failed("no key named in \"" + std::string(combination) + "\"");
    }
    const std::string summary = "pressed " + lower(combination);
#if defined(__APPLE__)
    const int code = detail::mac_key_code(keys.key);
    if (code < 0 && (keys.ctrl || keys.cmd || keys.alt)) {
        return failed("macOS has no key named \"" + keys.key + "\"");
    }
    std::string flags;
    if (keys.cmd)   { flags += "|$.kCGEventFlagMaskCommand"; }
    if (keys.ctrl)  { flags += "|$.kCGEventFlagMaskControl"; }
    if (keys.alt)   { flags += "|$.kCGEventFlagMaskAlternate"; }
    if (keys.shift) { flags += "|$.kCGEventFlagMaskShift"; }
    if (code < 0) {
        return jxa("var se=Application('System Events'); se.keystroke(" + js_string(keys.key) + ");", summary);
    }
    return jxa(kBridge + "var d=$.CGEventCreateKeyboardEvent(null," + std::to_string(code) + ",true);"
               "var u=$.CGEventCreateKeyboardEvent(null," + std::to_string(code) + ",false);"
               + (flags.empty() ? "" : "$.CGEventSetFlags(d,0" + flags + ");$.CGEventSetFlags(u,0" + flags + ");")
               + "post(d);sleep(30);post(u);", summary);
#elif defined(_WIN32)
    if (keys.cmd) {
        return failed("the Windows key cannot be sent this way");
    }
    int status = 0;
    const std::string out = run(powershell("Add-Type -AssemblyName System.Windows.Forms; "
                                 "[System.Windows.Forms.SendKeys]::SendWait(" + ps_string(detail::sendkeys(keys)) + ")"),
                                status, 20);
    return status == 0 ? done(summary) : failed("could not press " + lower(combination) + ": " + trimmed(out));
#else
    std::string why;
    const std::string tool = input_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    int status = 0;
    const std::string out = run({tool, "key", detail::xdotool_key(keys)}, status, 20);
    return status == 0 ? done(summary) : failed("could not press " + lower(combination) + ": " + trimmed(out));
#endif
}

Outcome scroll(int dx, int dy) {
    const std::string summary = "scrolled " + (dy != 0 ? std::string(dy < 0 ? "up " : "down ") + std::to_string(std::abs(dy))
                                                       : std::string(dx < 0 ? "left " : "right ") + std::to_string(std::abs(dx)));
#if defined(__APPLE__)
    // A notch is a line here; the sign is the opposite of the screen's.
    return jxa(kBridge + "post($.CGEventCreateScrollWheelEvent(null,$.kCGScrollEventUnitLine,2,"
               + std::to_string(-dy * 3) + "," + std::to_string(-dx * 3) + "));", summary);
#elif defined(_WIN32)
    int status = 0;
    const std::string out = run(powershell("Add-Type -MemberDefinition '[DllImport(\"user32.dll\")] public static extern void mouse_event(uint f, uint x, uint y, int d, int e);' -Name M -Namespace W; "
                                 "[W.M]::mouse_event(0x0800,0,0," + std::to_string(-dy * 120) + ",0); "
                                 + (dx != 0 ? "[W.M]::mouse_event(0x1000,0,0," + std::to_string(dx * 120) + ",0);" : "")),
                                status, 20);
    return status == 0 ? done(summary) : failed("could not scroll: " + trimmed(out));
#else
    std::string why;
    const std::string tool = input_tool(why);
    if (tool.empty()) {
        return failed(why);
    }
    int status = 0;
    std::string out;
    for (int n = 0; n < std::abs(dy) && status == 0; ++n) {
        out = run({tool, "click", dy < 0 ? "4" : "5"}, status, 20);
    }
    for (int n = 0; n < std::abs(dx) && status == 0; ++n) {
        out = run({tool, "click", dx < 0 ? "6" : "7"}, status, 20);
    }
    return status == 0 ? done(summary) : failed("could not scroll: " + trimmed(out));
#endif
}

}  // namespace crucible::tools::computer
