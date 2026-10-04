// SPDX-License-Identifier: MIT
//
// Turning text into the HTML that shows it.
//
// Markdown, syntax coloring and line diffs: everything that takes a string
// and returns markup, and nothing that touches the document. That split is
// the point. These are the functions worth being sure about -- they are what
// replaced util/markdown.cpp, util/syntax.cpp and util/code_lines.cpp -- and
// a function that reaches for `document` cannot be run by a test.
//
// tests/test_ui_js.cpp runs this file in JavaScriptCore, which is the engine
// the webview itself uses here.

/// Text to markup, safe in element content and in an attribute both.
///
/// The quotes are escaped as well as the tags, because most callers of this
/// interpolate into `value="..."` and one of them is showing names that came
/// back from Huggingface. Escaping only `<` and `>` is correct for a text
/// node and wrong in an attribute, and the difference is a closing quote
/// somebody else chose.
const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const escape = (text) => String(text ?? '').replace(/[&<>"']/g, (c) => ESCAPES[c]);

// --- markdown -------------------------------------------------------------
//
// Headings, bullets, numbered items, quotes, fences with a language, rules,
// tables, and bold / italic / code inline. This was util/markdown.cpp, which
// parsed into blocks and spans for the window's renderer to draw; it goes
// straight to HTML here because that is what draws it now, and there is one
// parser again rather than two to keep in step.
//
// Everything is escaped before a single tag is inserted, and no tag ever
// comes from the model: a reply is text that chose some emphasis, not a
// document that gets to bring its own HTML.

function inline(text) {
  // Code spans first and separately, so backticked content is never looked at
  // again -- `a ** b` is code containing asterisks, not emphasis.
  return text.split(/(`[^`]+`)/).map((part) => {
    if (part.startsWith('`') && part.endsWith('`') && part.length > 1) {
      return `<code>${escape(part.slice(1, -1))}</code>`;
    }
    return escape(part)
      .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
      .replace(/(^|[\s(])[*_]([^*_\n]+)[*_](?=$|[\s).,!?;:])/g, '$1<em>$2</em>');
  }).join('');
}

function cells(line) {
  return line.trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map((c) => c.trim());
}

/// What the |:---|---:|:---:| row under a header asks for, per column.
///
/// A column of numbers reads wrong ragged-left, and a model that took the
/// trouble to say so meant it.
function alignments(rule) {
  return cells(rule).map((c) => {
    const left = c.startsWith(':');
    const right = c.endsWith(':');
    if (left && right) return 'center';
    if (right) return 'right';
    return 'left';
  });
}

const aligned = (align, i) =>
  align[i] && align[i] !== 'left' ? ` style="text-align:${align[i]}"` : '';

function markdown(src) {
  const lines = String(src ?? '').split('\n');
  const out = [];
  let i = 0;

  while (i < lines.length) {
    const line = lines[i];

    // A fence runs to its closing fence, or to the end -- a reply still
    // streaming has an opening fence and no closing one for a while, and it
    // should read as code the whole time rather than flickering.
    const fence = line.match(/^\s*```(\w*)\s*$/);
    if (fence) {
      const body = [];
      i += 1;
      while (i < lines.length && !/^\s*```/.test(lines[i])) { body.push(lines[i]); i += 1; }
      i += 1;
      const lang = fence[1] ? `<span class="lang">${escape(fence[1])}</span>` : '';
      out.push(`<pre>${lang}<code>${highlight(body.join('\n'), fence[1])}</code></pre>`);
      continue;
    }

    if (!line.trim()) { i += 1; continue; }
    if (/^\s*(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) { out.push('<hr>'); i += 1; continue; }

    const heading = line.match(/^(#{1,6})\s+(.*)$/);
    if (heading) {
      const level = heading[1].length;
      out.push(`<h${level}>${inline(heading[2])}</h${level}>`);
      i += 1;
      continue;
    }

    // A table is a row whose next line is the |---|---| delimiter. Without
    // that line it is a paragraph with pipes in it, which is usually what a
    // model meant.
    if (line.includes('|') && i + 1 < lines.length &&
        /^\s*\|?[\s:-]*-[\s:|-]*\|?\s*$/.test(lines[i + 1]) && lines[i + 1].includes('-')) {
      const head = cells(line);
      const align = alignments(lines[i + 1]);
      i += 2;
      const rows = [];
      while (i < lines.length && lines[i].includes('|') && lines[i].trim()) {
        rows.push(cells(lines[i])); i += 1;
      }
      const headRow = head.map((c, n) => `<th${aligned(align, n)}>${inline(c)}</th>`).join('');
      const body = rows.map((r) =>
        `<tr>${r.map((c, n) => `<td${aligned(align, n)}>${inline(c)}</td>`).join('')}</tr>`).join('');
      out.push(`<table><thead><tr>${headRow}</tr></thead><tbody>${body}</tbody></table>`);
      continue;
    }

    if (/^\s*>\s?/.test(line)) {
      const body = [];
      while (i < lines.length && /^\s*>\s?/.test(lines[i])) {
        body.push(lines[i].replace(/^\s*>\s?/, '')); i += 1;
      }
      out.push(`<blockquote>${markdown(body.join('\n'))}</blockquote>`);
      continue;
    }

    // Consecutive items become one list, so three bullets are one block with
    // one margin rather than three lists stacked.
    const bullet = /^\s*[-*+]\s+(.*)$/;
    const numbered = /^\s*\d+[.)]\s+(.*)$/;
    if (bullet.test(line) || numbered.test(line)) {
      const ordered = !bullet.test(line);
      const pattern = ordered ? numbered : bullet;
      const items = [];
      while (i < lines.length && pattern.test(lines[i])) {
        items.push(lines[i].match(pattern)[1]); i += 1;
      }
      const tag = ordered ? 'ol' : 'ul';
      out.push(`<${tag}>${items.map((t) => `<li>${inline(t)}</li>`).join('')}</${tag}>`);
      continue;
    }

    const body = [];
    while (i < lines.length && lines[i].trim()
           && !/^\s*```/.test(lines[i]) && !/^(#{1,6})\s/.test(lines[i])
           && !/^\s*>/.test(lines[i]) && !bullet.test(lines[i]) && !numbered.test(lines[i])
           && !/^\s*(-{3,}|\*{3,}|_{3,})\s*$/.test(lines[i])) {
      body.push(lines[i]); i += 1;
    }
    if (body.length) { out.push(`<p>${inline(body.join('\n'))}</p>`); }
  }
  return out.join('');
}

// --- coloring the code a model wrote ---------------------------------------
//
// A lexer, not a parser. It splits a line into runs of "this is a string",
// "this is a comment", "this is a keyword" and gets those right often enough
// that a forty-line function reads as structure instead of as a gray block. A
// real grammar per language would be a thousand lines each and would still be
// wrong on the half-written snippets models actually emit.
//
// This was 900 lines of C++ serving the window's renderer. It is table-driven
// here because the languages differ in about five ways -- what starts a
// comment, what quotes a string, which words are reserved -- and writing those
// five things down per language is shorter than writing a lexer per language.

const QUOTES    = '"\'';     // the two string quotes
const QUOTES_JS = '"\'`';    // and the backtick
const TRIPLE_D  = '"""';
const TRIPLE_S  = "'''";

const KEYWORDS = {
  c: 'alignas alignof asm auto break case catch class co_await co_return co_yield concept const consteval constexpr constinit const_cast continue decltype default delete do dynamic_cast else enum explicit export extern false final for friend goto if inline module mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union using virtual volatile while',
  rust: 'as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self static struct super trait true type unsafe use where while',
  go: 'break case chan const continue default defer else fallthrough false for func go goto if import interface map nil package range return select struct switch true type var',
  java: 'abstract assert base break case catch class const continue default do else enum extends false final finally for fun get if implements import in init inline interface internal is lateinit let native new null object open operator out override package private protected public return sealed set static super suspend switch synchronized this throw throws transient true try typealias val var when where while',
  js: 'as async await break case catch class const continue debugger default delete do else enum export extends false finally for from function get if implements import in instanceof interface let new null of private protected public readonly return set static super switch this throw true try type typeof undefined var void while yield',
  python: 'and as assert async await break class continue def del elif else except False finally for from global if import in is lambda None nonlocal not or pass raise return True try while with yield',
  shell: 'case do done elif else esac fi for function if in local readonly return select then time until while',
  ruby: 'alias and begin break case class def defined? do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield',
  php: 'abstract and array as break callable case catch class clone const continue declare default do echo else elseif empty endfor endforeach endif endswitch endwhile enum extends final finally fn for foreach function global goto if implements include instanceof insteadof interface isset list match namespace new or print private protected public readonly require return static switch throw trait try unset use var while xor yield',
  lua: 'and break do else elseif end false for function goto if in local nil not or repeat return then true until while',
  sql: 'ALTER AND AS ASC BY CASE CREATE CROSS DELETE DESC DISTINCT DROP ELSE END EXISTS FROM FULL GROUP HAVING IN INDEX INNER INSERT INTO IS JOIN LEFT LIKE LIMIT NOT NULL OFFSET ON OR ORDER OUTER RIGHT SELECT SET TABLE THEN UNION UPDATE VALUES VIEW WHEN WHERE WITH',
  cmake: 'add_custom_command add_executable add_library add_subdirectory else elseif endforeach endfunction endif endmacro find_package foreach function if include install macro message option project return set target_compile_definitions target_include_directories target_link_libraries',
  css: 'and from important media import keyframes not only supports to',
  yaml: 'false no null true yes',
};

const TYPES = {
  c: 'bool char char16_t char32_t char8_t double float int int16_t int32_t int64_t int8_t long short signed size_t ssize_t std string uint16_t uint32_t uint64_t uint8_t unsigned vector void wchar_t',
  rust: 'Box Option Result Self String Vec bool char f32 f64 i128 i16 i32 i64 i8 isize str u128 u16 u32 u64 u8 usize',
  go: 'bool byte complex128 complex64 error float32 float64 int int16 int32 int64 int8 rune string uint uint16 uint32 uint64 uint8 uintptr',
  java: 'Boolean Byte Char Double Float Int Integer Long Object Short String Unit boolean byte char double float int long short void',
  js: 'Array Boolean Date Error JSON Map Math Number Object Promise RegExp Set String Symbol WeakMap any bigint boolean never number object string symbol unknown void',
  python: 'bool bytes complex dict float frozenset int list object set str tuple type',
};

const words = (list, fold) => {
  const all = (list || '').split(' ').filter(Boolean);
  return new Set(fold ? all.map((w) => w.toLowerCase()) : all);
};

/// What a fence marker names. The aliases matter more than the list: a model
/// writes ```c++ and ```cpp and ```C++ for the same thing.
function langSpec(name) {
  const n = String(name || '').toLowerCase();
  const family = {
    c: 'c', h: 'c', cc: 'c', cpp: 'c', 'c++': 'c', cxx: 'c', hpp: 'c', objc: 'c',
    rust: 'rust', rs: 'rust',
    go: 'go', golang: 'go',
    java: 'java', kotlin: 'java', kt: 'java', cs: 'java', csharp: 'java', swift: 'java', scala: 'java',
    js: 'js', javascript: 'js', jsx: 'js', ts: 'js', typescript: 'js', tsx: 'js',
    py: 'python', python: 'python', python3: 'python',
    sh: 'shell', bash: 'shell', zsh: 'shell', shell: 'shell', console: 'shell',
    rb: 'ruby', ruby: 'ruby',
    php: 'php',
    lua: 'lua',
    sql: 'sql', postgres: 'sql', psql: 'sql', mysql: 'sql', sqlite: 'sql',
    json: 'json', jsonc: 'json',
    yaml: 'yaml', yml: 'yaml',
    toml: 'toml', ini: 'toml', cfg: 'toml', conf: 'toml',
    cmake: 'cmake',
    css: 'css', scss: 'css', less: 'css',
    diff: 'diff', patch: 'diff',
  }[n];
  if (!family) return null;

  const C    = { line: ['//'], block: ['/*', '*/'], quotes: QUOTES };
  const HASH = { line: ['#'],  block: null,         quotes: QUOTES };
  const by = {
    c:      { ...C, keywords: KEYWORDS.c, types: TYPES.c, preproc: true },
    rust:   { ...C, keywords: KEYWORDS.rust, types: TYPES.rust },
    go:     { ...C, keywords: KEYWORDS.go, types: TYPES.go },
    java:   { ...C, keywords: KEYWORDS.java, types: TYPES.java },
    js:     { ...C, quotes: QUOTES_JS, keywords: KEYWORDS.js, types: TYPES.js },
    python: { ...HASH, triple: true, keywords: KEYWORDS.python, types: TYPES.python },
    shell:  { ...HASH, keywords: KEYWORDS.shell, dollar: true },
    ruby:   { ...HASH, keywords: KEYWORDS.ruby },
    php:    { ...C, line: ['//', '#'], keywords: KEYWORDS.php, dollar: true },
    lua:    { line: ['--'], block: ['--[[', ']]'], quotes: QUOTES, keywords: KEYWORDS.lua },
    sql:    { line: ['--'], block: ['/*', '*/'], quotes: QUOTES, keywords: KEYWORDS.sql, fold: true },
    json:   { line: [], block: null, quotes: '"', keywords: 'false null true' },
    yaml:   { ...HASH, keywords: KEYWORDS.yaml },
    toml:   { ...HASH, keywords: 'false true' },
    cmake:  { ...HASH, keywords: KEYWORDS.cmake, fold: true },
    css:    { line: [], block: ['/*', '*/'], quotes: QUOTES, keywords: KEYWORDS.css },
    diff:   { diff: true },
  }[family];

  return {
    ...by,
    keywords: words(by.keywords, by.fold),
    types: words(by.types, by.fold),
  };
}

const isWordChar = (c) => /[A-Za-z0-9_$?]/.test(c);

/// A diff is colored by its first column and nothing else. Lexing the language
/// underneath would mean lexing two half-files that do not parse.
function highlightDiff(code) {
  return code.split('\n').map((line) => {
    const cls = /^(\+\+\+|---|@@)/.test(line) ? 'tok-com'
              : line.startsWith('+') ? 'tok-add'
              : line.startsWith('-') ? 'tok-del' : '';
    return cls ? `<span class="${cls}">${escape(line)}</span>` : escape(line);
  }).join('\n');
}

function highlight(code, langName) {
  const spec = langSpec(langName);
  if (!spec) return escape(code);
  if (spec.diff) return highlightDiff(code);

  let out = '';
  let i = 0;
  const n = code.length;
  const at = (s) => code.startsWith(s, i);
  const span = (cls, text) => { out += `<span class="${cls}">${escape(text)}</span>`; };
  // Anything not recognized accumulates here and goes out unwrapped, which is
  // one span for a run of plain code instead of one per character.
  let plain = '';
  const flush = () => { if (plain) { out += escape(plain); plain = ''; } };

  while (i < n) {
    const ch = code[i];

    // A block comment, which is the one thing that outlives its line.
    if (spec.block && at(spec.block[0])) {
      const close = code.indexOf(spec.block[1], i + spec.block[0].length);
      const end = close < 0 ? n : close + spec.block[1].length;
      flush(); span('tok-com', code.slice(i, end)); i = end; continue;
    }
    const lineMark = (spec.line || []).find(at);
    if (lineMark) {
      const nl = code.indexOf('\n', i);
      const end = nl < 0 ? n : nl;
      flush(); span('tok-com', code.slice(i, end)); i = end; continue;
    }
    // Python's triple quotes: a string that spans lines, and also how most
    // Python writes its documentation.
    if (spec.triple && (at(TRIPLE_D) || at(TRIPLE_S))) {
      const mark = code.slice(i, i + 3);
      const close = code.indexOf(mark, i + 3);
      const end = close < 0 ? n : close + 3;
      flush(); span('tok-str', code.slice(i, end)); i = end; continue;
    }
    if (spec.quotes && spec.quotes.includes(ch)) {
      let j = i + 1;
      while (j < n && code[j] !== ch) {
        if (code[j] === '\\') { j += 2; continue; }   // an escaped quote is not the end
        if (code[j] === '\n' && ch !== '`') break;    // an unterminated string ends at the line
        j += 1;
      }
      const end = Math.min(j + 1, n);
      flush(); span('tok-str', code.slice(i, end)); i = end; continue;
    }
    if (spec.dollar && ch === '$') {
      let j = i + 1;
      if (code[j] === '{') { const c = code.indexOf('}', j); j = c < 0 ? n : c + 1; }
      else while (j < n && isWordChar(code[j])) j += 1;
      flush(); span('tok-var', code.slice(i, j)); i = j; continue;
    }
    // A preprocessor line, which is a '#' that starts one.
    if (spec.preproc && ch === '#' && /(^|\n)[ \t]*$/.test(code.slice(Math.max(0, i - 40), i))) {
      let j = i + 1;
      while (j < n && /[a-z]/.test(code[j])) j += 1;
      flush(); span('tok-key', code.slice(i, j)); i = j; continue;
    }
    if (/[0-9]/.test(ch) && !(i > 0 && isWordChar(code[i - 1]))) {
      let j = i;
      while (j < n && /[0-9a-fA-FxXoObB._]/.test(code[j])) j += 1;
      flush(); span('tok-num', code.slice(i, j)); i = j; continue;
    }
    if (/[A-Za-z_]/.test(ch)) {
      let j = i;
      while (j < n && isWordChar(code[j])) j += 1;
      const word = code.slice(i, j);
      // SQL and CMake are written in either case and mean the same thing.
      const look = spec.fold ? word.toLowerCase() : word;
      const cls = spec.keywords.has(look) ? 'tok-key' : spec.types.has(word) ? 'tok-typ' : '';
      if (cls) { flush(); span(cls, word); } else { plain += word; }
      i = j; continue;
    }
    plain += ch;
    i += 1;
  }
  flush();
  return out;
}

// --- what changed between two texts ---------------------------------------
//
// Shown as the file is beside as it would be, with the lines that moved
// marked. Two buttons and no third option: it stays as it is, or it becomes
// the other one.
//
// The diff is computed here rather than sent, because what the engine has is
// the two texts and anything else would be a second opinion about them.

/// Longest common subsequence over lines, walked back into a line-by-line
/// edit script. Quadratic, and that is fine: these are the lines of one file
/// an expert touched, not a repository.
function diffLines(before, after) {
  const a = before.split('\n');
  const b = after.split('\n');
  const n = a.length, m = b.length;
  // A guard rather than an optimisation: a very large file would make the
  // table enormous, and showing it whole is more useful than hanging.
  if (n * m > 4000000) {
    return [{ kind: 'note', text: `${n} lines before, ${m} after` }];
  }
  const table = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
  for (let i = n - 1; i >= 0; i -= 1) {
    for (let j = m - 1; j >= 0; j -= 1) {
      table[i][j] = a[i] === b[j] ? table[i + 1][j + 1] + 1
                                  : Math.max(table[i + 1][j], table[i][j + 1]);
    }
  }
  const out = [];
  let i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] === b[j]) { out.push({ kind: 'same', text: a[i] }); i += 1; j += 1; }
    else if (table[i + 1][j] >= table[i][j + 1]) { out.push({ kind: 'del', text: a[i] }); i += 1; }
    else { out.push({ kind: 'add', text: b[j] }); j += 1; }
  }
  while (i < n) { out.push({ kind: 'del', text: a[i] }); i += 1; }
  while (j < m) { out.push({ kind: 'add', text: b[j] }); j += 1; }
  return out;
}
