// SPDX-License-Identifier: MIT
//
// What stands in for localStorage and sessionStorage in a page Crucible runs.
//
// A page in the preview (ui/preview.js) and a page made into an app
// (tools/app_runner.cpp) both run without an origin of their own, one in a
// sandboxed frame and the other handed to its window as a string. A browser
// gives a page like that no storage at all: reading localStorage throws. A
// page that keeps its data there, which is most pages written anywhere but
// here, then cannot save anything, and its forms seem dead. On Linux an
// app's page is served instead, from an address of the program's own, and
// has the stand-ins all the same, so that what it keeps is in the same file
// on every system.
//
// So both are replaced, before the page's own scripts run, by ones that
// behave as a browser's do (strings in and strings out, null for a name
// never set, length and key(n), removeItem and clear, names read, set and
// deleted as properties, an error past the quota) and that hand what they
// hold to whatever is running the page, to be kept beside what the page saves
// with window.crucible.save. The preview and an app each keep it from one
// start to the next, as each keeps that. sessionStorage is kept as a tab keeps
// it: by the preview for as long as the window is open, and by an app for as
// long as it is.
//
// It is the text of a function, because it runs in the page and is put there
// as source: `(<this>)(kept, keep)`. `kept` is JSON, {"local": {...},
// "session": {...}}, what each held when the page last ran; keep(kind, items)
// is handed everything one of them holds, "local" or "session", once a change
// to it is over.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace crucible::tools {

inline constexpr std::string_view kPageStorage = R"JS(function (kept, keep) {
  // About what a browser gives a page: five million characters, names and
  // values together. A page that wants more is told so, as it would be there,
  // rather than filling the file it is kept in.
  var QUOTA = 5000000;
  var held = {};
  try { held = JSON.parse(kept) || {}; } catch (e) { /* nothing kept yet */ }

  // What a full one throws: a page that checks for room tests the name.
  function full() {
    try {
      return new DOMException('The quota has been exceeded.', 'QuotaExceededError');
    } catch (e) {
      var error = new Error('The quota has been exceeded.');
      error.name = 'QuotaExceededError';
      return error;
    }
  }

  // One of the two, holding `start` to begin with.
  function storage(kind, start) {
    var items = Object.create(null);
    var names = [];   // in the order they were first set, which key(n) counts in
    var used = 0;
    var due = false;
    if (start && typeof start === 'object') {
      Object.keys(start).forEach(function (name) {
        items[name] = String(start[name]);
        names.push(name);
        used += name.length + items[name].length;
      });
    }
    function has(name) { return Object.prototype.hasOwnProperty.call(items, name); }

    // Handed over once whatever is changing it has finished: a loop of
    // setItem calls is one handing over, not one per call.
    function changed() {
      if (due) return;
      due = true;
      Promise.resolve().then(function () {
        due = false;
        var all = Object.create(null);
        names.forEach(function (name) { all[name] = items[name]; });
        try { keep(kind, all); } catch (e) { /* kept in the page, at least */ }
      });
    }

    var methods = Object.create(typeof Storage === 'function' ? Storage.prototype : Object.prototype);
    methods.getItem = function (name) {
      name = String(name);
      return has(name) ? items[name] : null;
    };
    methods.setItem = function (name, value) {
      name = String(name);
      value = String(value);
      var before = has(name) ? items[name] : null;
      if (before === value) return;
      var more = before === null ? name.length + value.length : value.length - before.length;
      if (used + more > QUOTA) throw full();
      if (before === null) names.push(name);
      items[name] = value;
      used += more;
      changed();
    };
    methods.removeItem = function (name) {
      name = String(name);
      if (!has(name)) return;
      used -= name.length + items[name].length;
      delete items[name];
      names.splice(names.indexOf(name), 1);
      changed();
    };
    methods.clear = function () {
      if (!names.length) return;
      items = Object.create(null);
      names = [];
      used = 0;
      changed();
    };
    methods.key = function (n) {
      n = Number(n) >>> 0;
      return n < names.length ? names[n] : null;
    };
    Object.defineProperty(methods, 'length', {
      configurable: true, enumerable: true, get: function () { return names.length; }
    });

    var store = Object.create(methods);
    if (typeof Proxy !== 'function') return store;
    // What is held can be read, set and deleted as properties as well
    // (localStorage.theme, Object.keys(localStorage)), except under a name
    // the Storage has already, getItem or length, which stays what it was.
    function shown(name) { return typeof name === 'string' && has(name) && !(name in methods); }
    var proxy = new Proxy(store, {
      get: function (target, name, receiver) {
        return shown(name) ? items[name] : Reflect.get(target, name, receiver);
      },
      set: function (target, name, value, receiver) {
        if (typeof name !== 'string' || receiver !== proxy) return Reflect.set(target, name, value, receiver);
        methods.setItem(name, value);
        return true;
      },
      has: function (target, name) { return shown(name) || Reflect.has(target, name); },
      deleteProperty: function (target, name) {
        if (!shown(name)) return Reflect.deleteProperty(target, name);
        methods.removeItem(name);
        return true;
      },
      ownKeys: function (target) { return names.filter(shown).concat(Reflect.ownKeys(target)); },
      getOwnPropertyDescriptor: function (target, name) {
        return shown(name) ? { value: items[name], writable: true, enumerable: true, configurable: true }
                           : Reflect.getOwnPropertyDescriptor(target, name);
      }
    });
    return proxy;
  }

  // In place of the window's own, which only throw here.
  var stores = { localStorage: storage('local', held.local), sessionStorage: storage('session', held.session) };
  Object.keys(stores).forEach(function (name) {
    try {
      Object.defineProperty(window, name, {
        configurable: true, enumerable: true, get: function () { return stores[name]; }
      });
    } catch (e) { /* a window that will not have it replaced keeps its own */ }
  });
})JS";

/// `text` as a JavaScript string that can stand inside a script element:
/// quoted and escaped, with every `<` written as an escape so that nothing in
/// it can end the element, and the two line separators an older engine ends
/// a line at written out too.
inline std::string script_string(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += text[i];
        } else if (c == '<' || c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        } else if (c == 0xE2 && i + 2 < text.size() && text[i + 1] == '\x80'
                   && (text[i + 2] == '\xA8' || text[i + 2] == '\xA9')) {
            out += text[i + 2] == '\xA8' ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out + "\"";
}

/// The stand-ins put in place, as a script: kPageStorage called with `kept`,
/// the JSON of what the two held, and `keep`, the source of the function it
/// hands what one of them holds.
inline std::string page_storage_script(std::string_view kept, std::string_view keep) {
    return "(" + std::string(kPageStorage) + ")(" + script_string(kept) + ", " + std::string(keep) + ");";
}

/// `page` with `script` in an element of its own ahead of anything of the
/// page's: just inside its head, or after its doctype when it has no head, so
/// that a page written for standards mode is not put in quirks mode, or else
/// at the very start.
inline std::string with_script_first(std::string_view page, std::string_view script) {
    const auto is = [page](std::size_t at, std::string_view word) {
        if (page.size() - at < word.size()) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            const char c = page[at + i];
            if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != word[i]) {
                return false;
            }
        }
        return true;
    };
    std::size_t at = 0;
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (page[i] != '<') {
            continue;
        }
        // A head written after the body has begun is no head: the parser
        // has made one already, with nothing of ours in it.
        if (is(i, "<body")) {
            break;
        }
        // <head>, and not <header>.
        if (is(i, "<head") && i + 5 < page.size()
            && std::string_view(">/ \t\r\n\f").find(page[i + 5]) != std::string_view::npos) {
            const std::size_t end = page.find('>', i);
            at = end == std::string_view::npos ? 0 : end + 1;
            break;
        }
        if (at == 0 && is(i, "<!doctype")) {
            const std::size_t end = page.find('>', i);
            at = end == std::string_view::npos ? 0 : end + 1;
        }
    }
    return std::string(page.substr(0, at)) + "<script>" + std::string(script) + "</script>"
         + std::string(page.substr(at));
}

}  // namespace crucible::tools
