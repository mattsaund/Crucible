// SPDX-License-Identifier: MIT
//
// The lexer.
//
// One scanner drives every language. What changes between them is a small
// table: which strings start a line comment, which pair brackets a block
// comment, which quotes open a string and whether they may run past the end of
// the line, and two word lists. That is genuinely all the lexical difference
// there is between C and Go, or between Python and Ruby, once you stop trying
// to parse them.
//
// Two exceptions get their own scanners because their rules are not the
// C-family's at all: Markdown, where the interesting thing is the line's first
// character, and Diff, where it is the column-one marker.
#include "crucible/util/syntax.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace crucible::syntax {
namespace {

// ---------------------------------------------------------------------------
// The word lists
// ---------------------------------------------------------------------------
//
// Sorted, so a lookup is a binary search rather than a walk. Kept as flat
// arrays of literals: a std::set of std::string built at startup for a table
// that never changes is a page of allocations to answer a question a memcmp
// answers.

using Words = std::vector<std::string_view>;

const Words& c_keywords() {
    static const Words words = {
        "alignas", "alignof", "and", "asm", "auto", "break", "case", "catch",
        "class", "co_await", "co_return", "co_yield", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "final", "for", "friend",
        "goto", "if", "import", "inline", "module", "mutable", "namespace",
        "new", "noexcept", "not", "nullptr", "operator", "or", "override",
        "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "using", "virtual", "volatile", "while", "xor",
    };
    return words;
}

const Words& c_types() {
    static const Words words = {
        "NULL", "bool", "char", "char16_t", "char32_t", "char8_t", "double",
        "float", "int", "int16_t", "int32_t", "int64_t", "int8_t", "long",
        "short", "signed", "size_t", "ssize_t", "std", "string", "uint16_t",
        "uint32_t", "uint64_t", "uint8_t", "unsigned", "void", "wchar_t",
    };
    return words;
}

const Words& rust_keywords() {
    static const Words words = {
        "as", "async", "await", "break", "const", "continue", "crate", "dyn",
        "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
        "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
        "self", "static", "struct", "super", "trait", "true", "type", "unsafe",
        "use", "where", "while",
    };
    return words;
}

const Words& rust_types() {
    static const Words words = {
        "Box", "Option", "Result", "Self", "String", "Vec", "bool", "char",
        "f32", "f64", "i128", "i16", "i32", "i64", "i8", "isize", "str", "u128",
        "u16", "u32", "u64", "u8", "usize",
    };
    return words;
}

const Words& go_keywords() {
    static const Words words = {
        "break", "case", "chan", "const", "continue", "default", "defer",
        "else", "fallthrough", "false", "for", "func", "go", "goto", "if",
        "import", "interface", "map", "nil", "package", "range", "return",
        "select", "struct", "switch", "true", "type", "var",
    };
    return words;
}

const Words& go_types() {
    static const Words words = {
        "any", "bool", "byte", "complex128", "complex64", "error", "float32",
        "float64", "int", "int16", "int32", "int64", "int8", "rune", "string",
        "uint", "uint16", "uint32", "uint64", "uint8", "uintptr",
    };
    return words;
}

const Words& java_keywords() {
    static const Words words = {
        "abstract", "as", "assert", "break", "case", "catch", "class",
        "companion", "const", "continue", "data", "default", "do", "else",
        "enum", "extends", "false", "final", "finally", "for", "fun", "guard",
        "if", "implements", "import", "in", "init", "instanceof", "interface",
        "internal", "is", "let", "namespace", "native", "new", "null",
        "object", "open", "operator", "out", "override", "package", "private",
        "protected", "public", "readonly", "return", "sealed", "static",
        "super", "suspend", "switch", "synchronized", "this", "throw",
        "throws", "transient", "true", "try", "typealias", "using", "val",
        "var", "when", "where", "while", "yield",
    };
    return words;
}

const Words& java_types() {
    static const Words words = {
        "Any", "Boolean", "Double", "Float", "Int", "Integer", "List", "Long",
        "Map", "Object", "Set", "String", "Unit", "bool", "boolean", "byte",
        "char", "decimal", "double", "float", "int", "long", "object", "sbyte",
        "short", "string", "uint", "ulong", "ushort", "void",
    };
    return words;
}

const Words& js_keywords() {
    static const Words words = {
        "abstract", "as", "async", "await", "break", "case", "catch", "class",
        "const", "continue", "debugger", "declare", "default", "delete", "do",
        "else", "enum", "export", "extends", "false", "finally", "for", "from",
        "function", "get", "if", "implements", "import", "in", "instanceof",
        "interface", "keyof", "let", "namespace", "new", "null", "of",
        "private", "protected", "public", "readonly", "return", "satisfies",
        "set", "static", "super", "switch", "this", "throw", "true", "try",
        "type", "typeof", "undefined", "var", "void", "while", "with", "yield",
    };
    return words;
}

const Words& js_types() {
    static const Words words = {
        "Array", "Boolean", "Date", "Error", "JSON", "Map", "Math", "Number",
        "Object", "Promise", "RegExp", "Set", "String", "Symbol", "any",
        "bigint", "boolean", "never", "number", "string", "symbol", "unknown",
    };
    return words;
}

const Words& python_keywords() {
    static const Words words = {
        "and", "as", "assert", "async", "await", "break", "class", "continue",
        "def", "del", "elif", "else", "except", "finally", "for", "from",
        "global", "if", "import", "in", "is", "lambda", "match", "nonlocal",
        "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
    };
    return words;
}

const Words& python_types() {
    static const Words words = {
        "False", "None", "True", "bool", "bytes", "dict", "float", "int",
        "list", "self", "set", "str", "tuple",
    };
    return words;
}

const Words& shell_keywords() {
    static const Words words = {
        "case", "do", "done", "elif", "else", "esac", "exit", "export", "fi",
        "for", "function", "if", "in", "local", "return", "set", "shift",
        "then", "unset", "until", "while",
    };
    return words;
}

const Words& shell_types() {
    static const Words words = {
        "awk", "cat", "cd", "chmod", "cp", "curl", "cut", "echo", "find",
        "git", "grep", "head", "ls", "make", "mkdir", "mv", "printf", "read",
        "rm", "sed", "sort", "tail", "test", "tr", "wc",
    };
    return words;
}

const Words& ruby_keywords() {
    static const Words words = {
        "alias", "and", "begin", "break", "case", "class", "def", "defined?",
        "do", "else", "elsif", "end", "ensure", "false", "for", "if", "in",
        "module", "next", "nil", "not", "or", "redo", "require", "rescue",
        "retry", "return", "self", "super", "then", "true", "undef", "unless",
        "until", "when", "while", "yield",
    };
    return words;
}

const Words& php_keywords() {
    static const Words words = {
        "abstract", "and", "array", "as", "break", "case", "catch", "class",
        "const", "continue", "declare", "default", "do", "echo", "else",
        "elseif", "extends", "false", "final", "finally", "fn", "for",
        "foreach", "function", "global", "if", "implements", "include",
        "instanceof", "interface", "isset", "namespace", "new", "null", "or",
        "print", "private", "protected", "public", "require", "return",
        "static", "switch", "throw", "trait", "true", "try", "unset", "use",
        "var", "while", "yield",
    };
    return words;
}

const Words& lua_keywords() {
    static const Words words = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
        "return", "then", "true", "until", "while",
    };
    return words;
}

const Words& sql_keywords() {
    static const Words words = {
        "ALTER", "AND", "AS", "ASC", "BY", "CREATE", "DELETE", "DESC",
        "DISTINCT", "DROP", "EXISTS", "FROM", "GROUP", "HAVING", "IN", "INDEX",
        "INNER", "INSERT", "INTO", "JOIN", "LEFT", "LIMIT", "NOT", "NULL", "ON",
        "OR", "ORDER", "OUTER", "PRIMARY", "SELECT", "SET", "TABLE", "UNION",
        "UPDATE", "VALUES", "WHERE", "WITH",
    };
    return words;
}

const Words& sql_types() {
    static const Words words = {
        "BIGINT", "BLOB", "BOOLEAN", "CHAR", "DATE", "DECIMAL", "DOUBLE",
        "FLOAT", "INT", "INTEGER", "KEY", "REAL", "TEXT", "TIMESTAMP",
        "VARCHAR",
    };
    return words;
}

const Words& cmake_keywords() {
    static const Words words = {
        "add_executable", "add_library", "add_subdirectory", "else", "endif",
        "endforeach", "endfunction", "find_package", "foreach", "function",
        "if", "include", "install", "message", "option", "project", "return",
        "set", "target_compile_options", "target_include_directories",
        "target_link_libraries", "cmake_minimum_required",
    };
    return words;
}

const Words& empty_words() {
    static const Words words;
    return words;
}

bool holds(const Words& words, std::string_view word) {
    return std::binary_search(words.begin(), words.end(), word);
}

// ---------------------------------------------------------------------------
// The rules one language is
// ---------------------------------------------------------------------------

struct Rules {
    std::string_view line_comment;       ///< "//" or "#" or "--"
    std::string_view line_comment_alt;   ///< a second one, where a language has two
    std::string_view block_open;         ///< "/*"
    std::string_view block_close;        ///< "*/"
    std::string_view quotes = "\"'";     ///< what opens a string
    bool             backtick     = false;  ///< `template literals`
    bool             triple_quote = false;  ///< python's ''' and """
    bool             preproc_hash = false;  ///< a # in column one is a directive
    bool             dollar_names = false;  ///< $var is one token, colored apart
    bool             capital_types = false; ///< Capitalised means a type here
    const Words*     keywords = nullptr;
    const Words*     types    = nullptr;
};

Rules rules_for(Lang lang) {
    Rules r;
    switch (lang) {
        case Lang::C:
            r = {"//", "", "/*", "*/", "\"'", false, false, true, false, false,
                 &c_keywords(), &c_types()};
            break;
        case Lang::Rust:
            r = {"//", "", "/*", "*/", "\"'", false, false, false, false, true,
                 &rust_keywords(), &rust_types()};
            break;
        case Lang::Go:
            r = {"//", "", "/*", "*/", "\"'", true, false, false, false, true,
                 &go_keywords(), &go_types()};
            break;
        case Lang::Java:
            r = {"//", "", "/*", "*/", "\"'", false, false, false, false, true,
                 &java_keywords(), &java_types()};
            break;
        case Lang::JavaScript:
            r = {"//", "", "/*", "*/", "\"'", true, false, false, false, true,
                 &js_keywords(), &js_types()};
            break;
        case Lang::Python:
            r = {"#", "", "", "", "\"'", false, true, false, false, true,
                 &python_keywords(), &python_types()};
            break;
        case Lang::Shell:
            r = {"#", "", "", "", "\"'", true, false, false, true, false,
                 &shell_keywords(), &shell_types()};
            break;
        case Lang::Ruby:
            r = {"#", "", "=begin", "=end", "\"'", true, false, false, true,
                 true, &ruby_keywords(), &empty_words()};
            break;
        case Lang::Php:
            r = {"//", "#", "/*", "*/", "\"'", false, false, false, true, true,
                 &php_keywords(), &empty_words()};
            break;
        case Lang::Lua:
            r = {"--", "", "--[[", "]]", "\"'", false, false, false, false,
                 false, &lua_keywords(), &empty_words()};
            break;
        case Lang::Sql:
            r = {"--", "#", "/*", "*/", "\"'", false, false, false, false,
                 false, &sql_keywords(), &sql_types()};
            break;
        case Lang::Json:
            r = {"", "", "", "", "\"", false, false, false, false, false,
                 &empty_words(), &empty_words()};
            break;
        case Lang::Yaml:
        case Lang::Toml:
        case Lang::Ini:
            r = {"#", ";", "", "", "\"'", false, false, false, false, false,
                 &empty_words(), &empty_words()};
            break;
        case Lang::Cmake:
            r = {"#", "", "", "", "\"", false, false, false, true, false,
                 &cmake_keywords(), &empty_words()};
            break;
        case Lang::Css:
            r = {"//", "", "/*", "*/", "\"'", false, false, false, false, false,
                 &empty_words(), &empty_words()};
            break;
        case Lang::Html:
            r = {"", "", "<!--", "-->", "\"'", false, false, false, false,
                 false, &empty_words(), &empty_words()};
            break;
        case Lang::None:
        case Lang::Markdown:
        case Lang::Diff:
            r = {"", "", "", "", "", false, false, false, false, false,
                 &empty_words(), &empty_words()};
            break;
    }
    if (r.keywords == nullptr) { r.keywords = &empty_words(); }
    if (r.types == nullptr)    { r.types    = &empty_words(); }
    return r;
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool ident_body(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

void push(std::vector<Piece>& out, std::string_view text, Token token) {
    if (text.empty()) {
        return;
    }
    // Runs of the same color are merged as they are appended, because the
    // renderer measures and draws once per piece and a line of forty
    // single-character Punct pieces is forty draw calls saying one thing.
    if (!out.empty() && out.back().token == token) {
        out.back().text.append(text);
        return;
    }
    out.push_back(Piece{std::string(text), token});
}

bool starts_with(std::string_view line, std::size_t at, std::string_view what) {
    return !what.empty() && line.size() - at >= what.size()
        && line.compare(at, what.size(), what) == 0;
}

/// Consume a string that started with `quote`, from `i` (which is on the
/// opening delimiter). Returns the index past the close, or npos when the
/// string runs off the end of the line.
std::size_t scan_string(std::string_view line, std::size_t i, std::string_view quote,
                        bool escapes) {
    std::size_t j = i + quote.size();
    while (j < line.size()) {
        if (escapes && line[j] == '\\' && j + 1 < line.size()) {
            j += 2;
            continue;
        }
        if (starts_with(line, j, quote)) {
            return j + quote.size();
        }
        ++j;
    }
    return std::string_view::npos;
}

/// The general scanner: everything that is not Markdown and not a diff.
std::vector<Piece> scan(std::string_view line, const Rules& rules, Carry& carry) {
    std::vector<Piece> out;
    std::size_t i = 0;

    // A line that begins inside something has to close it before anything else
    // on it means what it looks like.
    if (carry.in_comment) {
        const std::size_t close = rules.block_close.empty()
            ? std::string_view::npos
            : line.find(rules.block_close);
        if (close == std::string_view::npos) {
            push(out, line, Token::Comment);
            return out;
        }
        i = close + rules.block_close.size();
        push(out, line.substr(0, i), Token::Comment);
        carry.in_comment = false;
    } else if (carry.in_string) {
        // The delimiter is looked for from the top of the line rather than with
        // scan_string, which expects to be standing on the opening one.
        const std::size_t at = line.find(carry.quote);
        if (at == std::string_view::npos) {
            push(out, line, Token::String);
            return out;
        }
        i = at + carry.quote.size();
        push(out, line.substr(0, i), Token::String);
        carry.in_string = false;
        carry.quote.clear();
    }

    while (i < line.size()) {
        const char c = line[i];

        // Whitespace, kept as Text so the pieces still concatenate to the line.
        if (c == ' ' || c == '\t') {
            const std::size_t start = i;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                ++i;
            }
            push(out, line.substr(start, i - start), Token::Text);
            continue;
        }

        // A preprocessor directive, or a YAML/shell comment, or a heading --
        // whichever this language spends its '#' on.
        if (rules.preproc_hash && c == '#') {
            const std::size_t start = i;
            ++i;
            while (i < line.size() && ident_body(line[i])) {
                ++i;
            }
            push(out, line.substr(start, i - start), Token::Preproc);
            continue;
        }

        if (starts_with(line, i, rules.line_comment)
            || starts_with(line, i, rules.line_comment_alt)) {
            push(out, line.substr(i), Token::Comment);
            return out;
        }

        if (starts_with(line, i, rules.block_open)) {
            const std::size_t close = line.find(rules.block_close,
                                                i + rules.block_open.size());
            if (close == std::string_view::npos) {
                push(out, line.substr(i), Token::Comment);
                carry.in_comment = true;
                return out;
            }
            const std::size_t end = close + rules.block_close.size();
            push(out, line.substr(i, end - i), Token::Comment);
            i = end;
            continue;
        }

        // A triple quote opens a string that may run for pages. Checked before
        // the single quote, or """ reads as an empty string followed by one.
        if (rules.triple_quote && (c == '"' || c == '\'')
            && line.size() - i >= 3 && line[i + 1] == c && line[i + 2] == c) {
            const std::string_view quote = line.substr(i, 3);
            const std::size_t      end   = scan_string(line, i, quote, false);
            if (end == std::string_view::npos) {
                push(out, line.substr(i), Token::String);
                carry.in_string = true;
                carry.quote     = std::string(quote);
                return out;
            }
            push(out, line.substr(i, end - i), Token::String);
            i = end;
            continue;
        }

        const bool quoted = rules.quotes.find(c) != std::string_view::npos
                         || (rules.backtick && c == '`');
        if (quoted) {
            const std::string_view quote = line.substr(i, 1);
            const std::size_t      end   = scan_string(line, i, quote, true);
            if (end == std::string_view::npos) {
                // A quote that never closes is far more often an apostrophe in
                // a comment than a string running onto the next line, so only
                // the backtick -- which really is multi-line -- is carried.
                push(out, line.substr(i), c == '`' ? Token::String : Token::Text);
                if (c == '`') {
                    carry.in_string = true;
                    carry.quote     = "`";
                }
                return out;
            }
            push(out, line.substr(i, end - i), Token::String);
            i = end;
            continue;
        }

        if (rules.dollar_names && c == '$') {
            const std::size_t start = i;
            ++i;
            if (i < line.size() && (line[i] == '{' || line[i] == '(')) {
                ++i;
            }
            while (i < line.size() && ident_body(line[i])) {
                ++i;
            }
            push(out, line.substr(start, i - start), Token::Preproc);
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            const std::size_t start = i;
            while (i < line.size()
                   && (std::isalnum(static_cast<unsigned char>(line[i])) != 0
                       || line[i] == '.' || line[i] == '_')) {
                ++i;
            }
            push(out, line.substr(start, i - start), Token::Number);
            continue;
        }

        if (ident_start(c)) {
            const std::size_t start = i;
            while (i < line.size() && ident_body(line[i])) {
                ++i;
            }
            // Ruby and Lua let a name end in ? or !, and coloring `empty?` as
            // `empty` plus a stray punctuation mark reads as a typo.
            if (i < line.size() && (line[i] == '?' || line[i] == '!')
                && (rules.keywords == &ruby_keywords())) {
                ++i;
            }
            const std::string_view word = line.substr(start, i - start);

            // A ( immediately after the name means it is being called, which is
            // the only thing here that is worth knowing and cannot be looked up
            // in a table. Immediately: `foo (x)` in Python is a call too, but
            // requiring the bracket to touch keeps `if (x)` from turning `if`
            // blue in every C-like language on earth.
            const bool called = i < line.size() && line[i] == '(';

            Token token = Token::Text;
            if (holds(*rules.keywords, word)) {
                token = Token::Keyword;
            } else if (holds(*rules.types, word)) {
                token = Token::Type;
            } else if (called) {
                token = Token::Function;
            } else if (rules.capital_types
                       && std::isupper(static_cast<unsigned char>(word.front())) != 0) {
                token = Token::Type;
            }
            push(out, word, token);
            continue;
        }

        // Everything else is punctuation, gathered in runs so `->` and `::` and
        // `!==` come out as one piece rather than as two or three.
        const std::size_t start = i;
        while (i < line.size() && !ident_start(line[i])
               && std::isdigit(static_cast<unsigned char>(line[i])) == 0
               && line[i] != ' ' && line[i] != '\t'
               && rules.quotes.find(line[i]) == std::string_view::npos
               && line[i] != '`'
               && !starts_with(line, i, rules.line_comment)
               && !starts_with(line, i, rules.block_open)) {
            ++i;
        }
        if (i == start) {
            ++i;  // never stall: one byte, whatever it was
        }
        push(out, line.substr(start, i - start), Token::Punct);
    }
    return out;
}

/// Markdown, which is about the start of the line and the inline runs in it.
std::vector<Piece> scan_markdown(std::string_view line) {
    std::vector<Piece> out;
    const std::size_t indent = line.find_first_not_of(" \t");
    if (indent == std::string_view::npos) {
        push(out, line, Token::Text);
        return out;
    }
    const std::string_view body = line.substr(indent);
    push(out, line.substr(0, indent), Token::Text);

    if (body.front() == '#') {
        push(out, body, Token::Keyword);
        return out;
    }
    if (body.front() == '>') {
        push(out, body, Token::Comment);
        return out;
    }
    if (body.rfind("- ", 0) == 0 || body.rfind("* ", 0) == 0
        || body.rfind("+ ", 0) == 0) {
        push(out, body.substr(0, 1), Token::Punct);
        push(out, body.substr(1), Token::Text);
        return out;
    }
    if (body.rfind("```", 0) == 0) {
        push(out, body, Token::Preproc);
        return out;
    }
    push(out, body, Token::Text);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

Lang lang_from(std::string_view marker) {
    // Both a fence marker and a file name arrive here, because both answer the
    // same question: "```python" and "+++ b/main.py" each say what language the
    // lines under them are. So three spellings are tried, most specific first --
    // the whole word, the file name, and the extension -- which is what makes
    // "python", "CMakeLists.txt" and "src/gui/syntax.cpp" all land somewhere.
    std::string word;
    word.reserve(marker.size());
    for (const char c : marker) {
        word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // A fence sometimes carries more than the language: ```python title="x"
    if (const std::size_t space = word.find_first_of(" \t{"); space != std::string::npos) {
        word = word.substr(0, space);
    }

    std::string base = word;
    if (const std::size_t slash = base.find_last_of("/\\"); slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    std::string ext;
    if (const std::size_t dot = base.find_last_of('.');
        dot != std::string::npos && dot + 1 < base.size()) {
        ext = base.substr(dot + 1);
    }

    struct Entry { std::string_view name; Lang lang; };
    static constexpr Entry kTable[] = {
        {"c", Lang::C},           {"h", Lang::C},          {"cc", Lang::C},
        {"cpp", Lang::C},         {"cxx", Lang::C},        {"hpp", Lang::C},
        {"hxx", Lang::C},         {"c++", Lang::C},        {"objc", Lang::C},
        {"m", Lang::C},           {"mm", Lang::C},         {"cuda", Lang::C},
        {"cu", Lang::C},          {"glsl", Lang::C},
        {"rs", Lang::Rust},       {"rust", Lang::Rust},
        {"go", Lang::Go},         {"golang", Lang::Go},
        {"java", Lang::Java},     {"kt", Lang::Java},      {"kotlin", Lang::Java},
        {"cs", Lang::Java},       {"csharp", Lang::Java},  {"swift", Lang::Java},
        {"scala", Lang::Java},
        {"js", Lang::JavaScript}, {"jsx", Lang::JavaScript},
        {"ts", Lang::JavaScript}, {"tsx", Lang::JavaScript},
        {"javascript", Lang::JavaScript}, {"typescript", Lang::JavaScript},
        {"mjs", Lang::JavaScript}, {"cjs", Lang::JavaScript},
        {"py", Lang::Python},     {"python", Lang::Python}, {"pyi", Lang::Python},
        {"sh", Lang::Shell},      {"bash", Lang::Shell},   {"zsh", Lang::Shell},
        {"shell", Lang::Shell},   {"console", Lang::Shell},
        {"ps1", Lang::Shell},     {"powershell", Lang::Shell},
        {"makefile", Lang::Shell}, {"dockerfile", Lang::Shell},
        {"rb", Lang::Ruby},       {"ruby", Lang::Ruby},
        {"php", Lang::Php},
        {"lua", Lang::Lua},
        {"sql", Lang::Sql},
        {"json", Lang::Json},     {"jsonc", Lang::Json},
        {"yaml", Lang::Yaml},     {"yml", Lang::Yaml},
        {"toml", Lang::Toml},
        {"ini", Lang::Ini},       {"cfg", Lang::Ini},      {"conf", Lang::Ini},
        {"cmake", Lang::Cmake},   {"cmakelists.txt", Lang::Cmake},
        {"html", Lang::Html},     {"htm", Lang::Html},     {"xml", Lang::Html},
        {"svg", Lang::Html},
        {"css", Lang::Css},       {"scss", Lang::Css},
        {"md", Lang::Markdown},   {"markdown", Lang::Markdown},
        {"diff", Lang::Diff},     {"patch", Lang::Diff},
    };

    for (const std::string& candidate : {word, base, ext}) {
        if (candidate.empty()) {
            continue;
        }
        for (const Entry& entry : kTable) {
            if (candidate == entry.name) {
                return entry.lang;
            }
        }
    }
    return Lang::None;
}

std::string_view lang_name(Lang lang) {
    switch (lang) {
        case Lang::C:          return "c++";
        case Lang::Rust:       return "rust";
        case Lang::Go:         return "go";
        case Lang::Java:       return "java";
        case Lang::JavaScript: return "javascript";
        case Lang::Python:     return "python";
        case Lang::Shell:      return "shell";
        case Lang::Ruby:       return "ruby";
        case Lang::Php:        return "php";
        case Lang::Lua:        return "lua";
        case Lang::Sql:        return "sql";
        case Lang::Json:       return "json";
        case Lang::Yaml:       return "yaml";
        case Lang::Toml:       return "toml";
        case Lang::Ini:        return "ini";
        case Lang::Cmake:      return "cmake";
        case Lang::Html:       return "html";
        case Lang::Css:        return "css";
        case Lang::Markdown:   return "markdown";
        case Lang::Diff:       return "diff";
        case Lang::None:       break;
    }
    return "";
}

namespace {

/// Does `body` contain `mark` at the start of some line?
bool at_line_start(std::string_view body, std::string_view mark) {
    std::size_t at = 0;
    while (at <= body.size()) {
        std::size_t start = at;
        while (start < body.size() && (body[start] == ' ' || body[start] == '\t')) {
            ++start;
        }
        if (body.compare(start, mark.size(), mark) == 0) {
            return true;
        }
        const std::size_t end = body.find('\n', at);
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    return false;
}

bool holds_text(std::string_view body, std::string_view mark) {
    return body.find(mark) != std::string_view::npos;
}

}  // namespace

Lang sniff(std::string_view body) {
    if (body.empty()) {
        return Lang::None;
    }

    // A shebang settles it outright, which is the whole point of a shebang.
    if (body.rfind("#!", 0) == 0) {
        const std::size_t end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        if (holds_text(line, "python")) { return Lang::Python; }
        if (holds_text(line, "node"))   { return Lang::JavaScript; }
        if (holds_text(line, "ruby"))   { return Lang::Ruby; }
        if (holds_text(line, "perl"))   { return Lang::None; }
        return Lang::Shell;  // sh, bash, zsh, env with anything else
    }
    if (body.rfind("<?php", 0) == 0)      { return Lang::Php; }
    if (body.rfind("<?xml", 0) == 0)      { return Lang::Html; }
    if (body.rfind("<!DOCTYPE", 0) == 0)  { return Lang::Html; }

    // A diff announces itself in column one, and mistaking one for the language
    // it patches would color every marker as code.
    if (at_line_start(body, "@@ ") || at_line_start(body, "+++ ")
        || at_line_start(body, "diff --git")) {
        return Lang::Diff;
    }

    // JSON is the one shape rather than the one keyword: brackets at the top
    // and quoted keys under them.
    {
        const std::size_t first = body.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos
            && (body[first] == '{' || body[first] == '[')
            && holds_text(body, "\":")
            && !holds_text(body, ";")) {
            return Lang::Json;
        }
    }

    struct Mark { Lang lang; std::string_view text; int weight; bool line_start; };
    static constexpr Mark kMarks[] = {
        {Lang::C,          "#include",       4, true},
        {Lang::C,          "std::",          3, false},
        {Lang::C,          "int main(",      4, false},
        {Lang::C,          "nullptr",        2, false},
        {Lang::C,          "->",             1, false},
        {Lang::Python,     "def ",           3, true},
        {Lang::Python,     "elif ",          3, false},
        {Lang::Python,     "self.",          3, false},
        {Lang::Python,     "import ",        2, true},
        {Lang::Python,     "__name__",       4, false},
        {Lang::Python,     "print(",         1, false},
        {Lang::Rust,       "fn ",            3, true},
        {Lang::Rust,       "let mut ",       4, false},
        {Lang::Rust,       "impl ",          3, true},
        {Lang::Rust,       "pub fn",         4, false},
        {Lang::Rust,       "println!",       4, false},
        {Lang::Go,         "func ",          3, true},
        {Lang::Go,         "package ",       3, true},
        {Lang::Go,         ":= ",            3, false},
        {Lang::Go,         "fmt.",           3, false},
        {Lang::JavaScript, "function ",      2, false},
        {Lang::JavaScript, "const ",         2, true},
        {Lang::JavaScript, "=> ",            2, false},
        {Lang::JavaScript, "console.log",    4, false},
        {Lang::JavaScript, "require(",       3, false},
        {Lang::Java,       "public class",   4, false},
        {Lang::Java,       "System.out",     4, false},
        {Lang::Shell,      "echo ",          2, true},
        {Lang::Shell,      "sudo ",          3, true},
        {Lang::Shell,      "$(",             2, false},
        {Lang::Shell,      "fi\n",           2, true},
        {Lang::Shell,      "apt-get",        3, false},
        {Lang::Ruby,       "puts ",          3, true},
        {Lang::Ruby,       "end\n",          1, true},
        {Lang::Lua,        "local ",         2, true},
        {Lang::Sql,        "SELECT ",        4, true},
        {Lang::Sql,        "INSERT INTO",    4, false},
        {Lang::Sql,        "CREATE TABLE",   4, false},
        {Lang::Cmake,      "cmake_minimum",  4, false},
        {Lang::Cmake,      "target_link",    4, false},
        {Lang::Html,       "</",             2, false},
        {Lang::Css,        "margin:",        3, false},
        {Lang::Markdown,   "## ",            2, true},
    };

    std::map<int, int> score;   // Lang as int -> points
    for (const Mark& mark : kMarks) {
        const bool found = mark.line_start ? at_line_start(body, mark.text)
                                           : holds_text(body, mark.text);
        if (found) {
            score[static_cast<int>(mark.lang)] += mark.weight;
        }
    }
    if (score.empty()) {
        return Lang::None;
    }

    int best  = 0;
    int most  = 0;
    int equal = 0;
    for (const auto& [lang, points] : score) {
        if (points > most) {
            most  = points;
            best  = lang;
            equal = 1;
        } else if (points == most) {
            ++equal;
        }
    }
    // A weak guess is worse than none: a paragraph of English with one "const"
    // in it is not JavaScript, and coloring it as such is a confident wrong
    // answer where "plain text" was the right one.
    if (most < 3 || equal > 1) {
        return Lang::None;
    }
    return static_cast<Lang>(best);
}

std::vector<Piece> highlight(std::string_view line, Lang lang, Carry& carry) {
    if (lang == Lang::Markdown) {
        return scan_markdown(line);
    }
    if (lang == Lang::None || lang == Lang::Diff) {
        std::vector<Piece> out;
        push(out, line, Token::Text);
        return out;
    }
    const Rules rules = rules_for(lang);
    return scan(line, rules, carry);
}

}  // namespace crucible::syntax
