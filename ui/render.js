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

/// Parts of a line side by side, set apart by space rather than by a
/// character between them: each in a span of its own, the gap the
/// stylesheet's. Takes markup, so escape text first; empty parts are left out.
const apart = (...parts) => parts
  .filter((p) => p !== '' && p !== null && p !== undefined && p !== false)
  .map((p) => `<span class="part">${p}</span>`).join('');

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

    // A script's first line written above its fence rather than inside it --
    // gpt-oss does this with "#!/usr/bin/env python3" -- belongs to the code.
    if (/^#!\S/.test(line) && /^\s*```\s*$/.test(lines[i + 1] || '')) {
      lines.splice(i, 2, lines[i + 1], line);
      continue;
    }

    // A fence runs to its closing fence, or to the end -- a reply still
    // streaming has an opening fence and no closing one for a while, and it
    // should read as code the whole time rather than flickering. One that
    // names no language is given the one its code is written in, when that
    // is plain, so it is colored like any other.
    const fence = line.match(/^\s*```(\w*)\s*$/);
    if (fence) {
      const body = [];
      i += 1;
      while (i < lines.length && !/^\s*```/.test(lines[i])) { body.push(lines[i]); i += 1; }
      i += 1;
      out.push(codeBlock(body.join('\n'), fence[1] || guessLanguage(body)));
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

/// A fenced block, drawn the way code is read: a header saying what language
/// and how long, and a numbered gutter beside the lines.
///
/// The line numbers are the point. A model that says "line 12 is the problem"
/// is talking about something you can find, and a block without them is a
/// wall you have to count down by hand.
/// What to call a language on a block's header.
///
/// A fence says ```py and a file name says .py; both mean Python, and the
/// header is a label for a person rather than the key the lexer looks up.
const LANG_NAMES = {
  py: 'python', py3: 'python', python3: 'python', pyw: 'python', pyi: 'python',
  js: 'javascript', jsx: 'javascript', mjs: 'javascript', cjs: 'javascript',
  ts: 'typescript', tsx: 'typescript', mts: 'typescript', cts: 'typescript',
  rs: 'rust', kt: 'kotlin', kts: 'kotlin', rb: 'ruby', sh: 'shell', bash: 'shell', zsh: 'shell',
  yml: 'yaml', md: 'markdown', mdx: 'markdown', h: 'c', hpp: 'c++', hh: 'c++', cc: 'c++', cxx: 'c++',
  cpp: 'c++', cs: 'c#', fs: 'f#', golang: 'go', psql: 'sql', mysql: 'sql', sqlite: 'sql',
  htm: 'html', xhtml: 'html', svg: 'svg', ps1: 'powershell', psm1: 'powershell', bat: 'batch',
  cmd: 'batch', mk: 'makefile', ex: 'elixir', exs: 'elixir', hs: 'haskell', jl: 'julia',
  pl: 'perl', pm: 'perl', tf: 'terraform', hcl: 'terraform', gql: 'graphql', proto: 'protobuf',
  m: 'objective-c', mm: 'objective-c', erl: 'erlang', clj: 'clojure', scm: 'scheme', el: 'lisp',
  ml: 'ocaml', gradle: 'groovy',
};

function codeBlock(code, lang, extra) {
  const lines = code.split('\n');
  // A trailing newline is a fence artifact, not an empty last line.
  if (lines.length > 1 && lines[lines.length - 1] === '') lines.pop();
  const gutter = lines.map((_, i) => i + 1).join('\n');
  return `<div class="code${extra ? ' ' + extra : ''}">
      <div class="code-head">
        <span class="lang">${escape(
          LANG_NAMES[String(lang || '').toLowerCase()] || lang || 'text')}</span>
        <span class="code-count">${lines.length} line${lines.length === 1 ? '' : 's'}</span>
        <button class="copy" data-act="copy" title="Copy" aria-label="Copy">
          <svg viewBox="0 0 16 16" width="13" height="13" fill="none"
               stroke="currentColor" stroke-width="1.3" aria-hidden="true">
            <rect x="5.5" y="5.5" width="8" height="8" rx="1.3"/>
            <path d="M10.5 5.5v-2a1.3 1.3 0 0 0-1.3-1.3H3.8a1.3 1.3 0 0 0-1.3 1.3v5.4a1.3 1.3 0 0 0 1.3 1.3h2"/>
          </svg>
        </button>
      </div>
      <div class="code-body">
        <pre class="code-gutter" aria-hidden="true">${gutter}</pre>
        <pre class="code-text"><code>${highlight(lines.join('\n'), lang)}</code></pre>
      </div>
    </div>`;
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
  js: 'abstract as asserts async await break case catch class const continue debugger declare default delete do else enum export extends false finally for from function get if implements import in infer instanceof interface is keyof let namespace new null of override private protected public readonly return satisfies set static super switch this throw true try type typeof undefined var void while yield',
  python: 'and as assert async await break class continue def del elif else except False finally for from global if import in is lambda None nonlocal not or pass raise return True try while with yield',
  shell: 'case do done elif else esac fi for function if in local readonly return select then time until while',
  ruby: 'alias and begin break case class def defined? do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield',
  php: 'abstract and array as break callable case catch class clone const continue declare default do echo else elseif empty endfor endforeach endif endswitch endwhile enum extends final finally fn for foreach function global goto if implements include instanceof insteadof interface isset list match namespace new or print private protected public readonly require return static switch throw trait try unset use var while xor yield',
  lua: 'and break do else elseif end false for function goto if in local nil not or repeat return then true until while',
  sql: 'ALTER AND AS ASC BY CASE CREATE CROSS DELETE DESC DISTINCT DROP ELSE END EXISTS FROM FULL GROUP HAVING IN INDEX INNER INSERT INTO IS JOIN LEFT LIKE LIMIT NOT NULL OFFSET ON OR ORDER OUTER RIGHT SELECT SET TABLE THEN UNION UPDATE VALUES VIEW WHEN WHERE WITH',
  cmake: 'add_custom_command add_executable add_library add_subdirectory else elseif endforeach endfunction endif endmacro find_package foreach function if include install macro message option project return set target_compile_definitions target_include_directories target_link_libraries',
  yaml: 'false no null true yes',
  csharp: 'abstract as async await base break case catch checked class const continue default delegate do dynamic else enum event explicit extern false finally fixed for foreach get goto if implicit in init interface internal is lock nameof namespace new null operator out override params partial private protected public readonly record ref return sealed set sizeof stackalloc static struct switch this throw true try typeof unchecked unsafe using var virtual volatile when where while yield',
  swift: 'Any Self actor as associatedtype async await break case catch class continue default defer deinit do else enum extension fallthrough false fileprivate for func guard if import in init inout internal is let nil open operator private protocol public repeat rethrows return self some static struct subscript super switch throw throws true try typealias var where while',
  kotlin: 'abstract annotation as break by catch class companion const constructor continue crossinline data do else enum expect external false final finally for fun get if import in infix init inline inner interface internal is lateinit noinline null object open operator out override package private protected public reified return sealed set super suspend tailrec this throw true try typealias val var vararg when where while',
  scala: 'abstract case catch class def do else enum export extends false final finally for given if implicit import lazy match new null object override package private protected return sealed super then this throw trait true try type using val var while with yield',
  dart: 'abstract as assert async await break case catch class const continue covariant default deferred do dynamic else enum export extends extension external factory false final finally for get if implements import in interface is late library mixin new null on operator part required rethrow return set static super switch sync this throw true try typedef var while with yield',
  zig: 'align allowzero and anyframe anytype asm async await break callconv catch comptime const continue defer else enum errdefer error export extern false fn for if inline noalias nosuspend null opaque or orelse packed pub resume return struct suspend switch test threadlocal true try undefined union unreachable usingnamespace var volatile while',
  haskell: 'as case class data default deriving do else family forall foreign hiding if import in infix infixl infixr instance let module newtype of qualified then type where',
  elixir: 'after alias and catch cond def defimpl defmacro defmacrop defmodule defp defprotocol defstruct do else end false fn for if import in nil not or quote raise receive require rescue true try unless unquote use when with',
  erlang: 'after and andalso band begin bnot bor bsl bsr bxor case catch cond div end fun if let not of or orelse receive rem try when xor',
  julia: 'abstract baremodule begin break catch const continue do else elseif end export false finally for function global if import in let local macro module mutable primitive quote return struct true try type using where while',
  r: 'FALSE Inf NA NULL NaN TRUE break else for function if in library next repeat require return while',
  perl: 'and cmp do else elsif eq for foreach ge gt if last le local lt my ne next no not or our package print redo require return sub unless until use while',
  powershell: 'begin break catch class continue data do dynamicparam else elseif end enum exit filter finally for foreach from function hidden if in param process return static switch throw trap try until using while',
  batch: 'call cd cls copy defined del do echo else endlocal errorlevel exist exit for goto if in md mkdir move not pause popd pushd rd ren rmdir set setlocal shift start title type',
  dockerfile: 'add arg as cmd copy entrypoint env expose from healthcheck label maintainer onbuild run shell stopsignal user volume workdir',
  makefile: 'define else endef endif export ifdef ifeq ifndef ifneq include override unexport vpath',
  graphql: 'directive enum extend false fragment implements input interface mutation null on query scalar schema subscription true type union',
  protobuf: 'enum extend false import map message oneof option optional package repeated required reserved returns rpc service stream syntax to true',
  terraform: 'count data depends_on dynamic false for for_each if in lifecycle locals module null output provider resource terraform true variable',
  ocaml: 'and as begin class do done downto else end exception false for fun function functor if in include inherit let match method module mutable new object of open or private rec sig struct then to true try type val virtual when while with',
  lisp: 'cond def defmacro defn define defun do false fn if lambda let loop nil ns quote recur require true when',
};

const TYPES = {
  c: 'bool char char16_t char32_t char8_t double float int int16_t int32_t int64_t int8_t long short signed size_t ssize_t std string uint16_t uint32_t uint64_t uint8_t unsigned vector void wchar_t',
  rust: 'Box Option Result Self String Vec bool char f32 f64 i128 i16 i32 i64 i8 isize str u128 u16 u32 u64 u8 usize',
  go: 'bool byte complex128 complex64 error float32 float64 int int16 int32 int64 int8 rune string uint uint16 uint32 uint64 uint8 uintptr',
  java: 'Boolean Byte Char Double Float Int Integer Long Object Short String Unit boolean byte char double float int long short void',
  js: 'Array Boolean Date Error JSON Map Math Number Object Promise RegExp Set String Symbol WeakMap any bigint boolean never number object string symbol unknown void',
  python: 'bool bytes complex dict float frozenset int list object set str tuple type',
  csharp: 'bool byte char decimal double float int long object sbyte short string uint ulong ushort void',
  swift: 'Array Bool Character Dictionary Double Float Int Int64 Optional Set String UInt Void',
  kotlin: 'Any Array Boolean Byte Char Double Float Int List Long Map Nothing Set Short String Unit',
  dart: 'List Map Never Object Set String bool double int num void',
  zig: 'bool f32 f64 i8 i16 i32 i64 isize type u8 u16 u32 u64 usize void',
  protobuf: 'bool bytes double fixed32 fixed64 float int32 int64 sfixed32 sfixed64 sint32 sint64 string uint32 uint64',
};

const words = (list, fold) => {
  const all = (list || '').split(' ').filter(Boolean);
  return new Set(fold ? all.map((w) => w.toLowerCase()) : all);
};

/// The language of a block whose fence did not say, or '' when it is not plain.
///
/// A model leaves the language off often enough -- and an unlabeled block is
/// drawn as gray text -- that guessing is worth it, but only on evidence: a
/// shebang, or lines that could only be one language. Each sign is a whole
/// line's worth of syntax, so prose in a block is not mistaken for code.
function guessLanguage(lines) {
  const code = lines.join('\n');
  const bang = (lines[0] || '').match(/^#!.*?\b(python3?|bash|sh|zsh|node|ruby|php|lua)\b/);
  if (bang) return { node: 'javascript', zsh: 'bash', sh: 'bash' }[bang[1]] || bang[1];
  const signs = [
    ['html', /^\s*(<!DOCTYPE html|<html[\s>]|<(head|body|div|section|main|script|style|template)[\s>])/im],
    ['xml', /^\s*<\?xml\s/],
    ['python', /^\s*(def \w+\(.*\)\s*(->.*)?:|class \w+(\(.*\))?:|from [\w.]+ import |import [\w.]+(, [\w.]+)*$|if __name__ == ['"]__main__['"]:|elif .*:$)/m],
    ['rust', /^\s*(fn \w+(<.*>)?\(|let mut |use std::|impl\b|pub fn |println!\()/m],
    ['go', /^\s*(package \w+$|func (\(.*\) )?\w+\(|import \($|fmt\.Print)/m],
    ['cpp', /^\s*(#include\s*[<"]|int main\s*\(|std::|template\s*<|using namespace )/m],
    ['java', /^\s*(public (static |final )*(class|void|interface)|System\.out\.print|using System;)/m],
    ['javascript', /^\s*((const|let) \w+ = |function \w*\s*\(|console\.log\(|module\.exports|export (default |const |function )|import .* from ['"])/m],
    ['sql', /^\s*(SELECT .* FROM |INSERT INTO |CREATE TABLE |UPDATE \w+ SET |DELETE FROM )/im],
    ['bash', /^\s*(\$ |sudo |apt(-get)? |brew |npm |pip3? install |git |cd |export \w+=|echo )/m],
  ];
  for (const [lang, sign] of signs) {
    if (sign.test(code)) return lang;
  }
  const json = code.trim();
  if (/^[{[]/.test(json)) {
    try { JSON.parse(json); return 'json'; } catch (e) { /* not JSON */ }
  }
  return '';
}

/// What a fence marker names. The aliases matter more than the list: a model
/// writes ```c++ and ```cpp and ```C++ for the same thing.
function langSpec(name) {
  const n = String(name || '').toLowerCase();
  const family = {
    c: 'c', h: 'c', cc: 'c', cpp: 'c', 'c++': 'c', cxx: 'c', hpp: 'c', objc: 'c',
    rust: 'rust', rs: 'rust',
    go: 'go', golang: 'go',
    java: 'java', groovy: 'java', gradle: 'java',
    kotlin: 'kotlin', kt: 'kotlin', kts: 'kotlin',
    cs: 'csharp', csharp: 'csharp', 'c#': 'csharp',
    swift: 'swift', scala: 'scala', sc: 'scala', dart: 'dart', zig: 'zig',
    m: 'c', mm: 'c', 'objective-c': 'c', ino: 'c', cu: 'c', glsl: 'c', hlsl: 'c', metal: 'c',
    js: 'js', javascript: 'js', jsx: 'js', mjs: 'js', cjs: 'js',
    ts: 'js', typescript: 'js', tsx: 'js', mts: 'js', cts: 'js',
    py: 'python', python: 'python', python3: 'python', py3: 'python', pyw: 'python', pyi: 'python',
    sh: 'shell', bash: 'shell', zsh: 'shell', fish: 'shell', ksh: 'shell', shell: 'shell', console: 'shell',
    env: 'shell', dotenv: 'shell',
    rb: 'ruby', ruby: 'ruby', rake: 'ruby', gemspec: 'ruby',
    php: 'php',
    lua: 'lua',
    sql: 'sql', postgres: 'sql', postgresql: 'sql', psql: 'sql', mysql: 'sql', sqlite: 'sql', plsql: 'sql',
    json: 'json', jsonc: 'json', json5: 'json', jsonl: 'json', geojson: 'json', webmanifest: 'json',
    yaml: 'yaml', yml: 'yaml',
    toml: 'toml', ini: 'toml', cfg: 'toml', conf: 'toml', properties: 'toml', editorconfig: 'toml',
    gitignore: 'hash', dockerignore: 'hash', gitattributes: 'hash', npmrc: 'hash', nginx: 'hash',
    cmake: 'cmake',
    css: 'css', scss: 'css', sass: 'css', less: 'css', styl: 'css',
    diff: 'diff', patch: 'diff',
    html: 'markup', htm: 'markup', xhtml: 'markup', xml: 'markup', svg: 'markup', vue: 'markup',
    svelte: 'markup', astro: 'markup', xaml: 'markup', plist: 'markup', xsd: 'markup', xsl: 'markup',
    xslt: 'markup', rss: 'markup', atom: 'markup', csproj: 'markup', vbproj: 'markup', props: 'markup',
    targets: 'markup', resx: 'markup', storyboard: 'markup', xib: 'markup', jinja: 'markup',
    j2: 'markup', hbs: 'markup', handlebars: 'markup', ejs: 'markup', erb: 'markup',
    md: 'markdown', markdown: 'markdown', mdx: 'markdown', mkd: 'markdown',
    ps1: 'powershell', psm1: 'powershell', psd1: 'powershell', powershell: 'powershell', pwsh: 'powershell',
    bat: 'batch', cmd: 'batch', batch: 'batch',
    dockerfile: 'dockerfile', containerfile: 'dockerfile', docker: 'dockerfile',
    makefile: 'makefile', mk: 'makefile', make: 'makefile', mak: 'makefile',
    hs: 'haskell', haskell: 'haskell', lhs: 'haskell',
    ex: 'elixir', exs: 'elixir', elixir: 'elixir', heex: 'elixir',
    erl: 'erlang', hrl: 'erlang', erlang: 'erlang',
    jl: 'julia', julia: 'julia',
    r: 'r', rmd: 'r',
    pl: 'perl', pm: 'perl', perl: 'perl',
    graphql: 'graphql', gql: 'graphql',
    proto: 'protobuf', protobuf: 'protobuf',
    tf: 'terraform', tfvars: 'terraform', hcl: 'terraform', terraform: 'terraform',
    ml: 'ocaml', mli: 'ocaml', ocaml: 'ocaml', fs: 'ocaml', fsx: 'ocaml', fsharp: 'ocaml', 'f#': 'ocaml',
    clj: 'lisp', cljs: 'lisp', edn: 'lisp', clojure: 'lisp', lisp: 'lisp', el: 'lisp', scm: 'lisp',
    scheme: 'lisp', rkt: 'lisp', racket: 'lisp',
    asm: 'asm', s: 'asm', nasm: 'asm',
    tex: 'tex', latex: 'tex', sty: 'tex', bib: 'tex',
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
    css:    { css: true },
    diff:   { diff: true },
    markup: { markup: true },
    markdown: { markdown: true },
    hash:   { ...HASH },
    csharp: { ...C, keywords: KEYWORDS.csharp, types: TYPES.csharp, preproc: true },
    kotlin: { ...C, keywords: KEYWORDS.kotlin, types: TYPES.kotlin, dollar: true },
    swift:  { ...C, keywords: KEYWORDS.swift, types: TYPES.swift },
    scala:  { ...C, keywords: KEYWORDS.scala, types: TYPES.java },
    dart:   { ...C, keywords: KEYWORDS.dart, types: TYPES.dart },
    zig:    { ...C, block: null, keywords: KEYWORDS.zig, types: TYPES.zig },
    haskell: { line: ['--'], block: ['{-', '-}'], quotes: QUOTES, keywords: KEYWORDS.haskell },
    elixir: { ...HASH, keywords: KEYWORDS.elixir },
    erlang: { line: ['%'], block: null, quotes: QUOTES, keywords: KEYWORDS.erlang },
    julia:  { ...HASH, block: ['#=', '=#'], triple: true, keywords: KEYWORDS.julia },
    r:      { ...HASH, keywords: KEYWORDS.r },
    perl:   { ...HASH, keywords: KEYWORDS.perl, dollar: true },
    powershell: { line: ['#'], block: ['<#', '#>'], quotes: QUOTES, keywords: KEYWORDS.powershell,
                  dollar: true, fold: true },
    batch:  { line: ['::', 'REM ', 'rem ', 'Rem ', '@REM ', '@rem '], block: null, quotes: '"',
              keywords: KEYWORDS.batch, percent: true, fold: true },
    dockerfile: { ...HASH, keywords: KEYWORDS.dockerfile, dollar: true, fold: true },
    makefile: { ...HASH, keywords: KEYWORDS.makefile, dollar: true },
    graphql: { ...HASH, quotes: '"', keywords: KEYWORDS.graphql },
    protobuf: { ...C, keywords: KEYWORDS.protobuf, types: TYPES.protobuf },
    terraform: { line: ['#', '//'], block: ['/*', '*/'], quotes: '"', keywords: KEYWORDS.terraform, dollar: true },
    ocaml:  { line: ['//'], block: ['(*', '*)'], quotes: '"', keywords: KEYWORDS.ocaml },
    lisp:   { line: [';'], block: null, quotes: '"', keywords: KEYWORDS.lisp },
    asm:    { line: [';', '#', '//'], block: null, quotes: QUOTES, keywords: '' },
    tex:    { line: ['%'], block: null, quotes: '', keywords: '', backslash: true },
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
  if (spec.markup) return highlightMarkup(code);
  if (spec.css) return highlightCss(code);
  if (spec.markdown) return highlightMarkdown(code);

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
      const close = { '{': '}', '(': ')' }[code[j]];
      if (close) { const c = code.indexOf(close, j); j = c < 0 ? n : c + 1; }
      else if (/[@<^?*%+]/.test(code[j] || '')) j += 1;   // make's automatic variables
      else while (j < n && isWordChar(code[j])) j += 1;
      flush(); span('tok-var', code.slice(i, j)); i = j; continue;
    }
    // A batch file's %NAME%, and its %1 and %~dp0.
    if (spec.percent && ch === '%') {
      const m = /^%(~[a-z]*\d|\d|\*|[A-Za-z_][\w.:~=-]*%)/.exec(code.slice(i, i + 80));
      if (m) { flush(); span('tok-var', m[0]); i += m[0].length; continue; }
    }
    // TeX's commands: \section, \begin.
    if (spec.backslash && ch === '\\') {
      let j = i + 1;
      while (j < n && /[A-Za-z@]/.test(code[j])) j += 1;
      if (j === i + 1 && j < n) j += 1;   // \% and the other one-character ones
      flush(); span('tok-key', code.slice(i, j)); i = j; continue;
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

/// CSS, and the Sass and Less written like it: selectors, the properties a
/// rule sets and the values it gives them, at-rules, and numbers with their
/// units and colors in hex -- the parts a stylesheet is read by.
function highlightCss(code) {
  let out = '';
  let plain = '';
  const n = code.length;
  const flush = () => { if (plain) { out += escape(plain); plain = ''; } };
  const span = (cls, text) => { flush(); out += `<span class="${cls}">${escape(text)}</span>`; };
  let depth = 0;          // how many blocks in
  let value = false;      // after a property's colon, until its semicolon
  let i = 0;
  while (i < n) {
    const ch = code[i];
    if (code.startsWith('/*', i)) {
      const c = code.indexOf('*/', i + 2); const end = c < 0 ? n : c + 2;
      span('tok-com', code.slice(i, end)); i = end; continue;
    }
    if (code.startsWith('//', i) && (i === 0 || /\s/.test(code[i - 1]))) {   // Sass and Less
      const c = code.indexOf('\n', i); const end = c < 0 ? n : c;
      span('tok-com', code.slice(i, end)); i = end; continue;
    }
    if (ch === '"' || ch === "'") {
      let j = i + 1;
      while (j < n && code[j] !== ch && code[j] !== '\n') j += code[j] === '\\' ? 2 : 1;
      const end = Math.min(j + 1, n);
      span('tok-str', code.slice(i, end)); i = end; continue;
    }
    if (ch === '{') { depth += 1; value = false; plain += ch; i += 1; continue; }
    if (ch === '}') { depth = Math.max(0, depth - 1); value = false; plain += ch; i += 1; continue; }
    if (ch === ';') { value = false; plain += ch; i += 1; continue; }
    if (ch === '@' || (ch === '!' && /^!important/i.test(code.slice(i, i + 10)))) {
      let j = i + 1;
      while (j < n && /[\w-]/.test(code[j])) j += 1;
      span('tok-key', code.slice(i, j)); i = j; continue;
    }
    if (ch === '$' && /[\w-]/.test(code[i + 1] || '')) {   // a Sass variable
      let j = i + 1;
      while (j < n && /[\w-]/.test(code[j])) j += 1;
      span('tok-var', code.slice(i, j)); i = j; continue;
    }
    if (value && ch === '#' && /[\da-fA-F]/.test(code[i + 1] || '')) {
      let j = i + 1;
      while (j < n && /[\da-fA-F]/.test(code[j])) j += 1;
      span('tok-num', code.slice(i, j)); i = j; continue;
    }
    // A number with its unit: in a value, or in an at-rule's condition.
    const signed = (ch === '.' || ch === '-') && /\d/.test(code[i + 1] || '');
    if ((value && (/\d/.test(ch) || signed)) || (depth === 0 && /\d/.test(ch))) {
      let j = i + 1;
      while (j < n && /[\d.]/.test(code[j])) j += 1;
      while (j < n && /[a-zA-Z%]/.test(code[j])) j += 1;   // the unit
      span('tok-num', code.slice(i, j)); i = j; continue;
    }
    if (/[A-Za-z_-]/.test(ch)) {
      let j = i;
      while (j < n && /[\w-]/.test(code[j])) j += 1;
      const word = code.slice(i, j);
      if (value) {
        plain += word;
      } else if (depth > 0 && /^\s*:/.test(code.slice(j, j + 40)) && !/^\s*:[\w-]+\s*[{,]/.test(code.slice(j, j + 60))) {
        span('tok-typ', word);   // a property
      } else {
        span('tok-key', word);   // a selector's part
      }
      i = j; continue;
    }
    if (ch === ':' && depth > 0 && !value && !/^:[\w-]+\s*[{,]/.test(code.slice(i, i + 60))) {
      value = true;
    }
    plain += ch;
    i += 1;
  }
  flush();
  return out;
}

/// HTML, XML and the rest of the angle-bracket family: tag names, their
/// attributes and values, comments, entities -- and the inside of a script
/// or a style colored as the language it is, since that is most of a page.
function highlightMarkup(code) {
  let out = '';
  let plain = '';
  const n = code.length;
  const flush = () => { if (plain) { out += escape(plain); plain = ''; } };
  const span = (cls, text) => {
    if (!text) return;
    flush();
    out += cls ? `<span class="${cls}">${escape(text)}</span>` : escape(text);
  };
  const until = (from, mark) => { const c = code.indexOf(mark, from); return c < 0 ? n : c + mark.length; };
  let i = 0;
  while (i < n) {
    if (code.startsWith('<!--', i)) { const end = until(i + 4, '-->'); span('tok-com', code.slice(i, end)); i = end; continue; }
    if (code.startsWith('<![CDATA[', i)) { const end = until(i + 9, ']]>'); span('tok-str', code.slice(i, end)); i = end; continue; }
    if (code[i] === '<' && /[A-Za-z!?/]/.test(code[i + 1] || '')) {
      const start = i;
      let j = i + 1;
      const closing = code[j] === '/';
      if (/[/!?]/.test(code[j])) j += 1;
      let k = j;
      while (k < n && /[\w:.-]/.test(code[k])) k += 1;
      const name = code.slice(j, k);
      span('', code.slice(i, j));
      span('tok-key', name);
      i = k;
      // Its attributes, to the '>' -- or to the next '<', when one never closed.
      while (i < n && code[i] !== '>' && code[i] !== '<') {
        const ch = code[i];
        if (ch === '"' || ch === "'") { const end = until(i + 1, ch); span('tok-str', code.slice(i, end)); i = end; continue; }
        if (ch === '=') {
          plain += ch;
          i += 1;
          // A value written without quotes runs to the next space or the end.
          const bare = /^[^\s"'<>=`]+/.exec(code.slice(i, i + 200));
          if (bare) { span('tok-str', bare[0]); i += bare[0].length; }
          continue;
        }
        if (/[A-Za-z_:@#[(*-]/.test(ch)) {
          let m = i;
          while (m < n && /[\w:.@#[\]()*-]/.test(code[m])) m += 1;
          span(name.toLowerCase() === '!doctype' ? 'tok-key' : 'tok-typ', code.slice(i, m));
          i = m;
          continue;
        }
        plain += ch;
        i += 1;
      }
      if (code[i] === '>') { plain += '>'; i += 1; }
      // A script or a style: what is in it is JavaScript or CSS -- or JSON,
      // for the scripts that are data.
      const tag = name.toLowerCase();
      if (!closing && (tag === 'script' || tag === 'style')) {
        const opening = code.slice(start, i);
        const close = code.toLowerCase().indexOf(`</${tag}`, i);
        const end = close < 0 ? n : close;
        const lang = tag === 'style' ? 'css'
          : /type\s*=\s*["']?(application\/(ld\+)?json|importmap)/i.test(opening) ? 'json'
          : /type\s*=\s*["']?text\/(x-)?(template|html)/i.test(opening) ? 'html' : 'js';
        flush();
        out += highlight(code.slice(i, end), lang);
        i = end;
      }
      continue;
    }
    if (code[i] === '&') {
      const entity = /^&(#\d+|#x[\da-fA-F]+|[A-Za-z][A-Za-z\d]*);/.exec(code.slice(i, i + 40));
      if (entity) { span('tok-num', entity[0]); i += entity[0].length; continue; }
    }
    plain += code[i];
    i += 1;
  }
  flush();
  return out;
}

/// Markdown: a heading, a quote and a list item by how the line starts, code
/// by its backticks -- a fenced block colored as the language its fence
/// names -- and a link's address. Emphasis is left as it is written.
function highlightMarkdown(code) {
  const lines = code.split('\n');
  const out = [];
  const inline = (text) => {
    let html = '';
    let last = 0;
    const pattern = /(`+)([^`]|[^`][\s\S]*?[^`])\1(?!`)|\[([^\]\n]*)\]\(([^)\s]*)([^)]*)\)/g;
    for (let m = pattern.exec(text); m; m = pattern.exec(text)) {
      html += escape(text.slice(last, m.index));
      html += m[1] ? `<span class="tok-str">${escape(m[0])}</span>`
                   : `[${escape(m[3])}](<span class="tok-str">${escape(m[4])}</span>${escape(m[5])})`;
      last = m.index + m[0].length;
    }
    return html + escape(text.slice(last));
  };
  for (let at = 0; at < lines.length; at += 1) {
    const line = lines[at];
    const fence = /^\s{0,3}(`{3,}|~{3,})\s*([\w+#.-]*)/.exec(line);
    if (fence) {
      let end = at + 1;
      while (end < lines.length && !lines[end].trim().startsWith(fence[1])) end += 1;
      out.push(`<span class="tok-com">${escape(line)}</span>`);
      if (end > at + 1) out.push(highlight(lines.slice(at + 1, end).join('\n'), fence[2]));
      if (end < lines.length) out.push(`<span class="tok-com">${escape(lines[end])}</span>`);
      at = end;
      continue;
    }
    if (/^\s{0,3}#{1,6}(\s|$)/.test(line)) { out.push(`<span class="tok-key">${escape(line)}</span>`); continue; }
    if (/^\s{0,3}>/.test(line) || /^\s{0,3}([-*_])(\s*\1){2,}\s*$/.test(line)) {
      out.push(`<span class="tok-com">${escape(line)}</span>`);
      continue;
    }
    const item = /^(\s*)([-*+]|\d+[.)])(\s+)/.exec(line);
    out.push(item ? `${escape(item[1])}<span class="tok-num">${escape(item[2])}</span>${item[3]}${inline(line.slice(item[0].length))}`
                  : inline(line));
  }
  return out.join('\n');
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

/// A file an expert wants to write, before it is written.
///
/// Shown as the file would be: the same block the reply's code is drawn in,
/// tinted by what is happening to it, and two buttons with no third option.
/// A new file is shown whole; a change to one is shown as the lines that
/// move, because the rest of the file is not what is being decided.
/// The language a file's extension names: "py" for calc.py. Empty for a
/// file with none, which highlight() takes as plain text.
function languageOf(path) {
  const name = String(path || '').split(/[\\/]/).pop();
  const lower = name.toLowerCase();
  // The files named for what they are rather than by an extension.
  const named = {
    dockerfile: 'dockerfile', containerfile: 'dockerfile', makefile: 'makefile', gnumakefile: 'makefile',
    'cmakelists.txt': 'cmake', gemfile: 'ruby', rakefile: 'ruby', podfile: 'ruby', vagrantfile: 'ruby',
    brewfile: 'ruby', jenkinsfile: 'groovy', procfile: 'shell', '.env': 'shell', '.bashrc': 'shell',
    '.zshrc': 'shell', '.profile': 'shell', '.bash_profile': 'shell', '.gitignore': 'gitignore',
    '.dockerignore': 'gitignore', '.gitattributes': 'gitignore', '.editorconfig': 'editorconfig',
    '.npmrc': 'npmrc',
  }[lower];
  if (named) return named;
  if (/^(dockerfile|containerfile)\./.test(lower) || lower.endsWith('.dockerfile')) return 'dockerfile';
  if (lower.startsWith('.env.')) return 'shell';
  return (name.match(/\.([A-Za-z0-9+#]+)$/) || [, ''])[1];
}

/// Lines that changed, colored as what they are: the gutter says added or
/// removed, and the code on each changed line is highlighted as code.
/// `where` is said in the head when the rows are one part of a file.
function diffBlock(rows, lang, where) {
  const added = rows.filter((r) => r.kind === 'add').length;
  const removed = rows.filter((r) => r.kind === 'del').length;
  return `<div class="code"><div class="code-head">
      <span class="lang">${escape(LANG_NAMES[String(lang || '').toLowerCase()] || lang || 'diff')}</span>
      <span class="code-count">${apart(where ? escape(where) : '', `+${added}  −${removed}`)}</span>
    </div>
    <div class="diff">${rows.map((row) => {
      if (row.kind === 'note') return `<div class="dl dl-note">${escape(row.text)}</div>`;
      const sign = row.kind === 'add' ? '+' : row.kind === 'del' ? '-' : ' ';
      return `<div class="dl dl-${row.kind}"><span class="sign">${sign}</span>${
        row.kind === 'same' ? escape(row.text) : highlight(row.text, lang) || '&nbsp;'}</div>`;
    }).join('')}</div></div>`;
}

/// A write that has happened, drawn the way it was offered: a new file as the
/// code it is, an edit as the lines that moved. `diff` is what the session
/// keeps of it (util::unified_diff): "@@ line N @@", then a line each marked
/// ' ', '-' or '+', and a "... (...)" line when it was cut short.
function writtenBlock(diff, path) {
  const lang = languageOf(path);
  const lines = String(diff || '').split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  const head = /^@@ line (\d+) @@$/.exec(lines[0] || '');
  if (!head) return codeBlock(String(diff || ''), lang);
  const rows = [];
  let cut = '';
  for (const line of lines.slice(1)) {
    if (line.startsWith('... (')) { cut = line; continue; }
    const kind = line[0] === '+' ? 'add' : line[0] === '-' ? 'del' : 'same';
    rows.push({ kind, text: line.slice(1) });
  }
  const note = cut ? `<div class="status" style="padding:.4rem 0 0">${escape(cut.replace(/^\.\.\. /, ''))}</div>` : '';
  // Nothing but added lines from the top: a file that did not exist, or was
  // empty. Shown as the code itself, as it was when it was allowed.
  if (rows.length && rows.every((r) => r.kind === 'add') && head[1] === '1') {
    return codeBlock(rows.map((r) => r.text).join('\n'), lang, 'code-add') + note;
  }
  // What is kept is the whole changed stretch, every old line and then every
  // new one. Matched up line by line again, the way the edit was offered, a
  // line the change left alone reads as left alone.
  const lead = [];
  const tail = [];
  while (rows.length && rows[0].kind === 'same') lead.push(rows.shift());
  while (rows.length && rows[rows.length - 1].kind === 'same') tail.unshift(rows.pop());
  const before = rows.filter((r) => r.kind === 'del').map((r) => r.text);
  const after = rows.filter((r) => r.kind === 'add').map((r) => r.text);
  const middle = !cut && before.length && after.length && rows.every((r) => r.kind !== 'same')
    ? diffLines(before.join('\n'), after.join('\n')) : rows;
  const matched = middle.length === 1 && middle[0].kind === 'note' ? rows : middle;
  return diffBlock(lead.concat(matched, tail), lang, `from line ${head[1]}`) + note;
}

function pendingEdit(edit) {
  const isNew = !edit.before;
  const lang = languageOf(edit.path);

  let block;
  if (!(edit.after || '').trim()) {
    // An expert that asks to write nothing has got something wrong, and an
    // empty code block saying "1 line" reads as the interface being broken
    // rather than the request being odd.
    block = `<div class="status" style="padding:.5rem 0">${
      isNew ? 'an empty file' : 'this would empty the file'}</div>`;
  } else if (isNew) {
    block = codeBlock(edit.after || '', lang, 'code-add');
  } else {
    const rows = diffLines(edit.before || '', edit.after || '');
    const note = rows.length === 1 && rows[0].kind === 'note';
    block = note
      ? `<div class="status" style="padding:.6rem 1rem">${escape(rows[0].text)}</div>`
      : diffBlock(rows, lang);
  }

  return `<div class="edit">
      <div class="edit-head"><span class="${isNew ? 'new' : 'changed'}">${
        isNew ? 'New file' : 'Edit'}</span> ${escape(edit.path)}</div>
      ${block}
      <div class="row edit-feet">
        <button class="action yes" id="edit-yes" data-act="edit-allow">Allow</button>
        <button class="action no" id="edit-no" data-act="edit-deny">Deny</button>
      </div>
    </div>`;
}
