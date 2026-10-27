// SPDX-License-Identifier: MIT
//
// What stands in for localStorage and sessionStorage in a page Crucible runs.
//
// A page in the preview (ui/preview.js) and a page made into an app
// (tools/app_runner.cpp) both run without an origin of their own, one in a
// sandboxed frame and the other handed to its window as a string. A browser
// gives a page like that no storage at all: reading localStorage throws. A
// page that keeps its data there, which is most pages written anywhere but
// here, then cannot save anything, and its forms seem dead.
//
// So both are replaced, before the page's own scripts run, by ones that
// behave as a browser's do (strings in and strings out, null for a name
// never set, length and key(n), removeItem and clear, names read, set and
// deleted as properties, an error past the quota) and that hand what they
// hold to whatever is running the page, to be kept beside what the page saves
// with window.crucible.save. The preview keeps it for as long as the window
// is open, and an app from one start to the next, as each keeps that.
// sessionStorage is kept by the preview too, as a tab keeps it, and by an app
// for as long as it is open.
//
// It is the text of a function, because it runs in the page and is put there
// as source: `(<this>)(kept, keep)`. `kept` is JSON, {"local": {...},
// "session": {...}}, what each held when the page last ran; keep(kind, items)
// is handed everything one of them holds, "local" or "session", once a change
// to it is over.
#pragma once

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

}  // namespace crucible::tools
