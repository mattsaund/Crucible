// SPDX-License-Identifier: MIT
//
// The text in a PDF.
//
// A PDF does not contain text so much as instructions to draw glyphs: pick a
// font, move here, show these codes. Reading it back means following those
// instructions far enough to know which characters the codes stand for and
// roughly where lines break -- not rendering anything.
//
// What this handles, which is what a document exported from a word processor
// or LaTeX uses: objects anywhere in the file including inside compressed
// object streams, deflate-compressed streams, the page tree with inherited
// resources, fonts with a ToUnicode map (every modern exporter writes one),
// simple fonts in a Latin encoding, form XObjects drawn from a page, and the
// text operators Tj, TJ, ' and ". What it does not handle is a PDF that is a
// picture of text -- a scan -- because there is no text in one to read, and
// encryption, because reading an encrypted document is the point of
// encrypting it.
//
// It errs toward reading something rather than nothing. A malformed object is
// skipped, a stream that will not inflate is skipped, and the pages that can
// be read are.
#include "crucible/tools/attachments.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace crucible::attach::detail {
namespace {

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

struct Obj {
    enum class Type { Null, Bool, Number, String, Name, Array, Dict, Ref, Keyword };
    Type        type = Type::Null;
    double      number = 0.0;
    std::string text;   ///< a string's bytes, a name, or a keyword
    std::vector<Obj> items;                            ///< an array's
    std::vector<std::pair<std::string, Obj>> entries;  ///< a dictionary's
    int ref = 0;

    const Obj* get(std::string_view key) const {
        for (const auto& [name, value] : entries) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }
    bool is(Type t) const { return type == t; }
};

bool is_white(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0';
}

bool is_delim(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{'
        || c == '}' || c == '/' || c == '%';
}

/// Reads PDF tokens and values out of a span of bytes.
class Lexer {
public:
    explicit Lexer(std::string_view data, std::size_t at = 0) : data_(data), at_(at) {}

    std::size_t at() const { return at_; }
    bool done() { skip(); return at_ >= data_.size(); }

    void skip() {
        while (at_ < data_.size()) {
            if (is_white(data_[at_])) {
                ++at_;
            } else if (data_[at_] == '%') {
                while (at_ < data_.size() && data_[at_] != '\n' && data_[at_] != '\r') {
                    ++at_;
                }
            } else {
                break;
            }
        }
    }

    /// One value. Operators -- anything that is not a value -- come back as a
    /// Keyword, which is what a content stream is made of.
    Obj value(int depth = 0) {
        skip();
        Obj out;
        if (at_ >= data_.size() || depth > 64) {
            return out;
        }
        const char c = data_[at_];
        if (c == '<' && peek(1) == '<') {
            at_ += 2;
            out.type = Obj::Type::Dict;
            for (;;) {
                skip();
                if (at_ >= data_.size()) {
                    break;
                }
                if (data_[at_] == '>' && peek(1) == '>') {
                    at_ += 2;
                    break;
                }
                Obj key = value(depth + 1);
                if (!key.is(Obj::Type::Name)) {
                    if (key.is(Obj::Type::Null) && at_ >= data_.size()) {
                        break;
                    }
                    continue;   // malformed; keep going rather than give up
                }
                out.entries.emplace_back(std::move(key.text), value(depth + 1));
            }
            return out;
        }
        if (c == '<') {
            ++at_;
            out.type = Obj::Type::String;
            std::string hex;
            while (at_ < data_.size() && data_[at_] != '>') {
                if (std::isxdigit(static_cast<unsigned char>(data_[at_])) != 0) {
                    hex += data_[at_];
                }
                ++at_;
            }
            ++at_;
            if (hex.size() % 2 != 0) {
                hex += '0';
            }
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                out.text += static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
            }
            return out;
        }
        if (c == '(') {
            ++at_;
            out.type = Obj::Type::String;
            int level = 1;
            while (at_ < data_.size()) {
                char ch = data_[at_++];
                if (ch == '\\' && at_ < data_.size()) {
                    char e = data_[at_++];
                    switch (e) {
                        case 'n': out.text += '\n'; break;
                        case 'r': out.text += '\r'; break;
                        case 't': out.text += '\t'; break;
                        case 'b': out.text += '\b'; break;
                        case 'f': out.text += '\f'; break;
                        case '\r':
                            if (at_ < data_.size() && data_[at_] == '\n') {
                                ++at_;
                            }
                            break;
                        case '\n': break;   // a line continued
                        default:
                            if (e >= '0' && e <= '7') {
                                int code = e - '0';
                                for (int k = 0; k < 2 && at_ < data_.size() && data_[at_] >= '0'
                                                && data_[at_] <= '7';
                                     ++k) {
                                    code = code * 8 + (data_[at_++] - '0');
                                }
                                out.text += static_cast<char>(code & 0xFF);
                            } else {
                                out.text += e;
                            }
                    }
                    continue;
                }
                if (ch == '(') {
                    ++level;
                } else if (ch == ')' && --level == 0) {
                    break;
                }
                out.text += ch;
            }
            return out;
        }
        if (c == '/') {
            ++at_;
            out.type = Obj::Type::Name;
            while (at_ < data_.size() && !is_white(data_[at_]) && !is_delim(data_[at_])) {
                if (data_[at_] == '#' && at_ + 2 < data_.size()) {
                    out.text += static_cast<char>(
                        std::strtol(std::string(data_.substr(at_ + 1, 2)).c_str(), nullptr, 16));
                    at_ += 3;
                } else {
                    out.text += data_[at_++];
                }
            }
            return out;
        }
        if (c == '[') {
            ++at_;
            out.type = Obj::Type::Array;
            for (;;) {
                skip();
                if (at_ >= data_.size()) {
                    break;
                }
                if (data_[at_] == ']') {
                    ++at_;
                    break;
                }
                out.items.push_back(value(depth + 1));
            }
            return out;
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
            ++at_;   // stray: skipped
            out.type = Obj::Type::Keyword;
            return out;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.') {
            const std::size_t start = at_;
            ++at_;
            while (at_ < data_.size()
                   && (std::isdigit(static_cast<unsigned char>(data_[at_])) != 0 || data_[at_] == '.')) {
                ++at_;
            }
            out.type   = Obj::Type::Number;
            out.number = std::strtod(std::string(data_.substr(start, at_ - start)).c_str(), nullptr);
            // "12 0 R" is a reference; anything else was just a number.
            const std::size_t after = at_;
            Lexer ahead(data_, at_);
            ahead.skip();
            std::size_t p = ahead.at_;
            std::size_t q = p;
            while (q < data_.size() && std::isdigit(static_cast<unsigned char>(data_[q])) != 0) {
                ++q;
            }
            if (q > p) {
                Lexer more(data_, q);
                more.skip();
                if (more.at_ < data_.size() && data_[more.at_] == 'R'
                    && (more.at_ + 1 >= data_.size() || is_white(data_[more.at_ + 1])
                        || is_delim(data_[more.at_ + 1]))) {
                    out.type = Obj::Type::Ref;
                    out.ref  = static_cast<int>(out.number);
                    at_      = more.at_ + 1;
                    return out;
                }
            }
            at_ = after;
            return out;
        }
        // A keyword: true, false, null, or an operator.
        const std::size_t start = at_;
        while (at_ < data_.size() && !is_white(data_[at_]) && !is_delim(data_[at_])) {
            ++at_;
        }
        if (at_ == start) {
            ++at_;
        }
        const std::string_view word = data_.substr(start, at_ - start);
        if (word == "true" || word == "false") {
            out.type   = Obj::Type::Bool;
            out.number = word == "true" ? 1.0 : 0.0;
        } else if (word == "null") {
            out.type = Obj::Type::Null;
        } else {
            out.type = Obj::Type::Keyword;
            out.text = std::string(word);
        }
        return out;
    }

    /// After an inline image's ID: skip its bytes to the EI that ends them.
    void skip_inline_image() {
        while (at_ + 2 < data_.size()) {
            if (is_white(data_[at_]) && data_[at_ + 1] == 'E' && data_[at_ + 2] == 'I'
                && (at_ + 3 >= data_.size() || is_white(data_[at_ + 3]))) {
                at_ += 3;
                return;
            }
            ++at_;
        }
        at_ = data_.size();
    }

private:
    char peek(std::size_t ahead) const {
        return at_ + ahead < data_.size() ? data_[at_ + ahead] : '\0';
    }

    std::string_view data_;
    std::size_t      at_;
};

// ---------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------

struct Stored {
    Obj         value;
    std::string stream;   ///< raw, still encoded
    bool        has_stream = false;
};

/// Inflate when the stream says it is deflated; the bytes as they are when it
/// has no filter; nothing for a filter this does not read.
std::string decode(const Stored& stored) {
    const Obj* filter = stored.value.get("Filter");
    if (filter == nullptr) {
        return stored.stream;
    }
    std::string name;
    if (filter->is(Obj::Type::Name)) {
        name = filter->text;
    } else if (filter->is(Obj::Type::Array) && filter->items.size() == 1
               && filter->items[0].is(Obj::Type::Name)) {
        name = filter->items[0].text;
    }
    if (name == "FlateDecode" || name == "Fl") {
        return inflate(stored.stream, true);
    }
    return {};
}

class Document {
public:
    explicit Document(std::string_view bytes) {
        scan(bytes);
        unpack_object_streams();
    }

    const Obj* resolve(const Obj* obj, int depth = 0) const {
        while (obj != nullptr && obj->is(Obj::Type::Ref) && depth++ < 16) {
            const auto found = objects_.find(obj->ref);
            obj = found == objects_.end() ? nullptr : &found->second.value;
        }
        return obj;
    }

    const Stored* stored(const Obj* obj) const {
        if (obj == nullptr || !obj->is(Obj::Type::Ref)) {
            return nullptr;
        }
        const auto found = objects_.find(obj->ref);
        return found == objects_.end() ? nullptr : &found->second;
    }

    /// Every page, in reading order.
    std::vector<const Obj*> pages() const {
        std::vector<const Obj*> out;
        for (const auto& [number, entry] : objects_) {
            const Obj* type = entry.value.get("Type");
            if (type != nullptr && type->text == "Catalog") {
                std::set<const Obj*> seen;
                walk(resolve(entry.value.get("Pages")), out, seen, 0);
                break;
            }
        }
        if (out.empty()) {
            // No catalog found: every page object, in the order they were
            // numbered, which is the order most writers number them.
            for (const auto& [number, entry] : objects_) {
                const Obj* type = entry.value.get("Type");
                if (type != nullptr && type->text == "Page") {
                    out.push_back(&entry.value);
                }
            }
        }
        return out;
    }

    bool encrypted() const { return encrypted_; }

private:
    void walk(const Obj* node, std::vector<const Obj*>& out, std::set<const Obj*>& seen,
              int depth) const {
        if (node == nullptr || depth > 64 || !seen.insert(node).second) {
            return;
        }
        const Obj* type = node->get("Type");
        const Obj* kids = resolve(node->get("Kids"));
        if (kids != nullptr && kids->is(Obj::Type::Array)) {
            for (const Obj& kid : kids->items) {
                walk(resolve(&kid), out, seen, depth + 1);
            }
        } else if (type == nullptr || type->text == "Page") {
            out.push_back(node);
        }
    }

    /// Find "N G obj" anywhere in the file. A cross-reference table would say
    /// where each is, and a damaged or incrementally saved one says it wrongly
    /// often enough that reading the objects themselves is the safer course.
    /// Later definitions win, which is what an incremental save means.
    void scan(std::string_view bytes) {
        std::size_t at = 0;
        while ((at = bytes.find("obj", at)) != std::string_view::npos) {
            const std::size_t keyword = at;
            at += 3;
            if (keyword > 0 && !is_white(bytes[keyword - 1])) {
                continue;
            }
            if (at < bytes.size() && !is_white(bytes[at]) && !is_delim(bytes[at])) {
                continue;   // "objects", "endobj"
            }
            // Back over "N G ".
            std::size_t p = keyword;
            while (p > 0 && is_white(bytes[p - 1])) { --p; }
            std::size_t g_end = p;
            while (p > 0 && std::isdigit(static_cast<unsigned char>(bytes[p - 1])) != 0) { --p; }
            if (p == g_end) { continue; }
            while (p > 0 && is_white(bytes[p - 1])) { --p; }
            std::size_t n_end = p;
            while (p > 0 && std::isdigit(static_cast<unsigned char>(bytes[p - 1])) != 0) { --p; }
            if (p == n_end) { continue; }
            const int number = std::atoi(std::string(bytes.substr(p, n_end - p)).c_str());

            Lexer lexer(bytes, at);
            Stored entry;
            entry.value = lexer.value();
            lexer.skip();
            const std::size_t after = lexer.at();
            if (bytes.substr(after, 6) == "stream") {
                std::size_t start = after + 6;
                if (start < bytes.size() && bytes[start] == '\r') { ++start; }
                if (start < bytes.size() && bytes[start] == '\n') { ++start; }
                std::size_t end = std::string_view::npos;
                if (const Obj* length = entry.value.get("Length");
                    length != nullptr && length->is(Obj::Type::Number)) {
                    const auto n = static_cast<std::size_t>(length->number);
                    if (start + n <= bytes.size()
                        && bytes.substr(start + n, 32).find("endstream") != std::string_view::npos) {
                        end = start + n;
                    }
                }
                if (end == std::string_view::npos) {
                    end = bytes.find("endstream", start);
                }
                if (end != std::string_view::npos) {
                    entry.stream     = std::string(bytes.substr(start, end - start));
                    entry.has_stream = true;
                    at = end;
                }
            }
            if (entry.value.get("Encrypt") != nullptr) {
                encrypted_ = true;
            }
            objects_[number] = std::move(entry);
        }
        // The trailer names the encryption dictionary when there is one.
        if (bytes.find("/Encrypt") != std::string_view::npos
            && bytes.find("trailer") != std::string_view::npos) {
            const std::size_t trailer = bytes.rfind("trailer");
            Lexer lexer(bytes, trailer + 7);
            const Obj dict = lexer.value();
            if (dict.get("Encrypt") != nullptr) {
                encrypted_ = true;
            }
        }
    }

    /// Objects kept inside compressed object streams, which is where a modern
    /// writer puts every font and page dictionary.
    void unpack_object_streams() {
        std::vector<std::pair<int, Stored>> found;
        for (const auto& [number, entry] : objects_) {
            const Obj* type = entry.value.get("Type");
            if (!entry.has_stream || type == nullptr || type->text != "ObjStm") {
                continue;
            }
            const std::string data = decode(entry);
            const Obj* n     = entry.value.get("N");
            const Obj* first = entry.value.get("First");
            if (data.empty() || n == nullptr || first == nullptr) {
                continue;
            }
            Lexer header(data);
            std::vector<std::pair<int, std::size_t>> index;
            for (int i = 0; i < static_cast<int>(n->number); ++i) {
                const Obj num = header.value();
                const Obj off = header.value();
                if (!num.is(Obj::Type::Number) || !off.is(Obj::Type::Number)) {
                    break;
                }
                index.emplace_back(static_cast<int>(num.number),
                                   static_cast<std::size_t>(first->number + off.number));
            }
            for (const auto& [objnum, offset] : index) {
                if (offset >= data.size()) {
                    continue;
                }
                Lexer lexer(data, offset);
                Stored inner;
                inner.value = lexer.value();
                found.emplace_back(objnum, std::move(inner));
            }
        }
        for (auto& [number, inner] : found) {
            if (objects_.count(number) == 0) {
                objects_[number] = std::move(inner);
            }
        }
    }

    std::map<int, Stored> objects_;
    bool encrypted_ = false;
};

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

/// How one font's codes become characters.
struct Font {
    int bytes = 1;                          ///< how long one code is
    std::map<std::uint32_t, std::string> map;   ///< code -> UTF-8, from ToUnicode
    bool simple = true;                     ///< single-byte, Latin when not mapped
    /// How wide each code draws, in thousandths of the font size. Only what
    /// it takes to tell where one run of text ends, so that the gap to the
    /// next can be told apart from a space between words.
    std::map<std::uint32_t, double> widths;
    double fallback_width = 500.0;

    double width(std::uint32_t code) const {
        const auto found = widths.find(code);
        return found != widths.end() ? found->second : fallback_width;
    }
};

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// The characters Windows' Latin encoding puts at 0x80-0x9F, where Latin-1
/// has control codes. Word-processor PDFs use it for curly quotes and dashes.
std::uint32_t win_ansi(unsigned char c) {
    static constexpr std::uint32_t kHigh[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
        0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    if (c >= 0x80 && c <= 0x9F) {
        return kHigh[c - 0x80] != 0 ? kHigh[c - 0x80] : c;
    }
    return c;
}

std::uint32_t code_of(std::string_view bytes) {
    std::uint32_t code = 0;
    for (const char b : bytes) {
        code = (code << 8) | static_cast<unsigned char>(b);
    }
    return code;
}

/// Read a ToUnicode map: the code length from its codespace, and every
/// bfchar and bfrange in it.
void read_cmap(std::string_view cmap, Font& font) {
    Lexer lexer(cmap);
    std::vector<Obj> operands;
    while (!lexer.done()) {
        Obj token = lexer.value();
        if (!token.is(Obj::Type::Keyword)) {
            operands.push_back(std::move(token));
            if (operands.size() > 4096) {
                operands.clear();
            }
            continue;
        }
        if (token.text == "endcodespacerange") {
            if (!operands.empty() && operands.front().is(Obj::Type::String)) {
                font.bytes = std::max<int>(1, static_cast<int>(operands.front().text.size()));
            }
        } else if (token.text == "endbfchar") {
            for (std::size_t i = 0; i + 1 < operands.size(); i += 2) {
                if (operands[i].is(Obj::Type::String) && operands[i + 1].is(Obj::Type::String)) {
                    font.map[code_of(operands[i].text)] = utf16be_to_utf8(operands[i + 1].text);
                }
            }
        } else if (token.text == "endbfrange") {
            for (std::size_t i = 0; i + 2 < operands.size(); i += 3) {
                const Obj& lo = operands[i];
                const Obj& hi = operands[i + 1];
                const Obj& to = operands[i + 2];
                if (!lo.is(Obj::Type::String) || !hi.is(Obj::Type::String)) {
                    continue;
                }
                const std::uint32_t first = code_of(lo.text);
                const std::uint32_t last  = code_of(hi.text);
                if (last < first || last - first > 65535) {
                    continue;
                }
                if (to.is(Obj::Type::String)) {
                    // The destination counts up with the code: the last
                    // UTF-16 unit of it is incremented across the range.
                    std::string base = to.text;
                    for (std::uint32_t code = first; code <= last; ++code) {
                        font.map[code] = utf16be_to_utf8(base);
                        if (base.size() >= 2) {
                            std::uint32_t unit = (static_cast<unsigned char>(base[base.size() - 2]) << 8)
                                               | static_cast<unsigned char>(base.back());
                            ++unit;
                            base[base.size() - 2] = static_cast<char>((unit >> 8) & 0xFF);
                            base.back()           = static_cast<char>(unit & 0xFF);
                        }
                    }
                } else if (to.is(Obj::Type::Array)) {
                    std::uint32_t code = first;
                    for (const Obj& each : to.items) {
                        if (code > last) {
                            break;
                        }
                        if (each.is(Obj::Type::String)) {
                            font.map[code] = utf16be_to_utf8(each.text);
                        }
                        ++code;
                    }
                }
            }
        }
        operands.clear();
    }
}

Font load_font(const Document& doc, const Obj* ref) {
    Font font;
    const Obj* dict = doc.resolve(ref);
    if (dict == nullptr) {
        return font;
    }
    const Obj* subtype = dict->get("Subtype");
    if (subtype != nullptr && subtype->text == "Type0") {
        font.bytes  = 2;
        font.simple = false;
    }
    if (const Stored* cmap = doc.stored(dict->get("ToUnicode")); cmap != nullptr && cmap->has_stream) {
        read_cmap(decode(*cmap), font);
    }

    const auto number = [&](const Obj* value, double otherwise) {
        const Obj* resolved = doc.resolve(value);
        return resolved != nullptr && resolved->is(Obj::Type::Number) ? resolved->number : otherwise;
    };
    if (font.simple) {
        // A simple font: one width per code from FirstChar on.
        const Obj* widths = doc.resolve(dict->get("Widths"));
        const double first = number(dict->get("FirstChar"), 0.0);
        if (widths != nullptr && widths->is(Obj::Type::Array)) {
            for (std::size_t i = 0; i < widths->items.size(); ++i) {
                font.widths[static_cast<std::uint32_t>(first) + static_cast<std::uint32_t>(i)]
                    = number(&widths->items[i], 0.0);
            }
        }
        return font;
    }
    // A composite font keeps its widths on its one descendant, as runs:
    // `c [w1 w2 ...]` gives consecutive codes from c, `c1 c2 w` a range.
    const Obj* descendants = doc.resolve(dict->get("DescendantFonts"));
    const Obj* cid = descendants != nullptr && descendants->is(Obj::Type::Array) && !descendants->items.empty()
                         ? doc.resolve(&descendants->items.front())
                         : nullptr;
    if (cid == nullptr) {
        return font;
    }
    font.fallback_width = number(cid->get("DW"), 1000.0);
    const Obj* w = doc.resolve(cid->get("W"));
    if (w == nullptr || !w->is(Obj::Type::Array)) {
        return font;
    }
    const std::vector<Obj>& items = w->items;
    for (std::size_t i = 0; i + 1 < items.size() && font.widths.size() < 65536;) {
        const auto start = static_cast<std::uint32_t>(number(&items[i], 0.0));
        const Obj* next = doc.resolve(&items[i + 1]);
        if (next != nullptr && next->is(Obj::Type::Array)) {
            for (std::size_t k = 0; k < next->items.size(); ++k) {
                font.widths[start + static_cast<std::uint32_t>(k)] = number(&next->items[k], 0.0);
            }
            i += 2;
        } else if (i + 2 < items.size()) {
            const auto stop = static_cast<std::uint32_t>(number(&items[i + 1], 0.0));
            const double each = number(&items[i + 2], 0.0);
            for (std::uint32_t code = start; code <= stop && code - start < 65536; ++code) {
                font.widths[code] = each;
            }
            i += 3;
        } else {
            break;
        }
    }
    return font;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(const Document& doc) : doc_(doc) {}

    void page(const Obj* page) {
        const Obj* resources = inherited(page, "Resources");
        const Obj* contents  = page->get("Contents");
        std::string stream;
        if (const Obj* resolved = doc_.resolve(contents);
            resolved != nullptr && resolved->is(Obj::Type::Array)) {
            for (const Obj& part : resolved->items) {
                if (const Stored* stored = doc_.stored(&part); stored != nullptr) {
                    stream += decode(*stored);
                    stream += '\n';
                }
            }
        } else if (const Stored* stored = doc_.stored(contents); stored != nullptr) {
            stream = decode(*stored);
        }
        run(stream, doc_.resolve(resources), 0);
        newline();
        newline();
        have_last_ = false;
    }

    std::string text() const { return out_; }

private:
    const Obj* inherited(const Obj* node, const char* key) const {
        for (int depth = 0; node != nullptr && depth < 32; ++depth) {
            if (const Obj* found = node->get(key); found != nullptr) {
                return found;
            }
            node = doc_.resolve(node->get("Parent"));
        }
        return nullptr;
    }

    const Font& font_named(const Obj* resources, const std::string& name) {
        static const Font kNone;
        const Obj* fonts = resources != nullptr ? doc_.resolve(resources->get("Font")) : nullptr;
        if (fonts == nullptr) {
            return kNone;
        }
        const Obj* ref = fonts->get(name);
        if (ref == nullptr) {
            return kNone;
        }
        const int key = ref->is(Obj::Type::Ref) ? ref->ref : -static_cast<int>(cache_.size()) - 1;
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }
        return cache_.emplace(key, load_font(doc_, ref)).first->second;
    }

    /// Where text is being drawn: the text matrix, and the state that moves
    /// it along as glyphs are shown.
    struct Matrix {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };
    struct State {
        Matrix      line;              ///< where the current line started
        Matrix      text;              ///< where the next glyph goes
        double      size     = 0.0;
        double      spacing  = 0.0;    ///< Tc
        double      word     = 0.0;    ///< Tw
        double      scale    = 1.0;    ///< Tz, as a fraction
        double      leading  = 0.0;    ///< TL
        const Font* font     = nullptr;
    };

    /// Move to the start of the next line, `tx` along and `ty` up.
    static void move(State& state, double tx, double ty) {
        Matrix& m = state.line;
        m.e += tx * m.a + ty * m.c;
        m.f += tx * m.b + ty * m.d;
        state.text = m;
    }

    /// How tall text of this size is in the page's units: what a line break
    /// and a word gap are measured against.
    static double height(const State& state) {
        const double vertical = std::sqrt(state.text.c * state.text.c + state.text.d * state.text.d);
        const double h = std::abs(state.size) * (vertical > 0.0 ? vertical : 1.0);
        return h > 0.0 ? h : 1.0;
    }

    void show(const std::string& bytes, State& state) {
        static const Font kNone;
        const Font& font = state.font != nullptr ? *state.font : kNone;
        const double x = state.text.e;
        const double y = state.text.f;
        const double h = height(state);

        // A new line when the baseline moved by more than half a line; a
        // space when the text starts clear of where the last run ended, or
        // jumps back on the same line (a column, a table cell).
        if (have_last_) {
            const double line = std::max(h, last_height_);
            if (std::abs(y - last_y_) > 0.5 * line) {
                newline();
                if (std::abs(y - last_y_) > 2.0 * line) {
                    newline();   // a gap wider than a line: a new paragraph
                }
            } else if (x - last_end_ > 0.15 * h || last_end_ - x > 1.5 * h) {
                if (!out_.empty() && out_.back() != '\n' && out_.back() != ' ') {
                    out_ += ' ';
                }
            }
        }

        const auto step = static_cast<std::size_t>(std::max(1, font.bytes));
        for (std::size_t i = 0; i + step <= bytes.size(); i += step) {
            const std::uint32_t code = code_of(std::string_view(bytes).substr(i, step));
            if (const auto mapped = font.map.find(code); mapped != font.map.end()) {
                out_ += mapped->second;
            } else if (font.simple && step == 1) {
                const auto c = static_cast<unsigned char>(code);
                if (c >= 0x20 || c == '\t') {
                    append_utf8(out_, win_ansi(c));
                }
            }
            double advance = font.width(code) / 1000.0 * state.size + state.spacing;
            if (step == 1 && code == 32) {
                advance += state.word;
            }
            advance *= state.scale;
            state.text.e += advance * state.text.a;
            state.text.f += advance * state.text.b;
        }
        have_last_   = true;
        last_y_      = y;
        last_end_    = state.text.e;
        last_height_ = h;
    }

    void newline() {
        while (!out_.empty() && out_.back() == ' ') {
            out_.pop_back();
        }
        if (!out_.empty()) {
            out_ += '\n';
        }
    }

    void run(const std::string& stream, const Obj* resources, int depth) {
        if (depth > 6 || stream.empty()) {
            return;
        }
        Lexer lexer(stream);
        std::vector<Obj> operands;
        State state;
        const auto num = [&](std::size_t back) {
            const Obj& o = operands[operands.size() - back];
            return o.is(Obj::Type::Number) ? o.number : 0.0;
        };
        while (!lexer.done()) {
            Obj token = lexer.value();
            if (!token.is(Obj::Type::Keyword)) {
                operands.push_back(std::move(token));
                if (operands.size() > 256) {
                    operands.erase(operands.begin(), operands.begin() + 128);
                }
                continue;
            }
            const std::string& op = token.text;
            const std::size_t n = operands.size();
            if (op == "BT") {
                state.line = state.text = Matrix{};
            } else if (op == "Tf" && n >= 2 && operands[n - 2].is(Obj::Type::Name)) {
                state.font = &font_named(resources, operands[n - 2].text);
                state.size = num(1);
            } else if (op == "Tc" && n >= 1) {
                state.spacing = num(1);
            } else if (op == "Tw" && n >= 1) {
                state.word = num(1);
            } else if (op == "Tz" && n >= 1) {
                state.scale = num(1) / 100.0;
            } else if (op == "TL" && n >= 1) {
                state.leading = num(1);
            } else if (op == "Td" && n >= 2) {
                move(state, num(2), num(1));
            } else if (op == "TD" && n >= 2) {
                state.leading = -num(1);
                move(state, num(2), num(1));
            } else if (op == "Tm" && n >= 6) {
                state.line = Matrix{num(6), num(5), num(4), num(3), num(2), num(1)};
                state.text = state.line;
            } else if (op == "T*") {
                move(state, 0.0, -state.leading);
            } else if (op == "Tj" && n >= 1 && operands.back().is(Obj::Type::String)) {
                show(operands.back().text, state);
            } else if (op == "'" && n >= 1 && operands.back().is(Obj::Type::String)) {
                move(state, 0.0, -state.leading);
                show(operands.back().text, state);
            } else if (op == "\"" && n >= 3 && operands.back().is(Obj::Type::String)) {
                state.word    = num(3);
                state.spacing = num(2);
                move(state, 0.0, -state.leading);
                show(operands.back().text, state);
            } else if (op == "TJ" && n >= 1 && operands.back().is(Obj::Type::Array)) {
                for (const Obj& part : operands.back().items) {
                    if (part.is(Obj::Type::String)) {
                        show(part.text, state);
                    } else if (part.is(Obj::Type::Number)) {
                        // A kerning adjustment, in thousandths of the size,
                        // backwards. A large one is how many PDFs say "space".
                        const double advance = -part.number / 1000.0 * state.size * state.scale;
                        state.text.e += advance * state.text.a;
                        state.text.f += advance * state.text.b;
                    }
                }
            } else if (op == "ID") {
                lexer.skip_inline_image();
            } else if (op == "Do" && !operands.empty() && operands.back().is(Obj::Type::Name)) {
                const Obj* xobjects = resources != nullptr ? doc_.resolve(resources->get("XObject"))
                                                           : nullptr;
                const Obj* ref = xobjects != nullptr ? xobjects->get(operands.back().text) : nullptr;
                const Stored* form = doc_.stored(ref);
                const Obj* subtype = form != nullptr ? form->value.get("Subtype") : nullptr;
                if (form != nullptr && subtype != nullptr && subtype->text == "Form") {
                    const Obj* inner = doc_.resolve(form->value.get("Resources"));
                    run(decode(*form), inner != nullptr ? inner : resources, depth + 1);
                }
            }
            operands.clear();
        }
    }

    const Document&     doc_;
    std::map<int, Font> cache_;
    std::string         out_;
    bool                have_last_   = false;
    double              last_y_      = 0.0;
    double              last_end_    = 0.0;
    double              last_height_ = 0.0;
};

/// Tidy what came out: spaces at line ends, runs of blank lines.
std::string tidy(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    int blank = 0;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find('\n', start);
        if (end == std::string::npos) {
            end = raw.size();
        }
        std::string line = raw.substr(start, end - start);
        // Runs of spaces inside a line: one is enough.
        std::string squeezed;
        for (const char c : line) {
            if (c == ' ' && !squeezed.empty() && squeezed.back() == ' ') {
                continue;
            }
            squeezed += c;
        }
        while (!squeezed.empty() && squeezed.back() == ' ') {
            squeezed.pop_back();
        }
        if (squeezed.empty()) {
            if (++blank <= 1 && !out.empty()) {
                out += '\n';
            }
        } else {
            blank = 0;
            out += squeezed;
            out += '\n';
        }
        start = end + 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

}  // namespace

std::string utf16be_to_utf8(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        std::uint32_t unit = (static_cast<unsigned char>(bytes[i]) << 8)
                           | static_cast<unsigned char>(bytes[i + 1]);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            const std::uint32_t low = (static_cast<unsigned char>(bytes[i + 2]) << 8)
                                    | static_cast<unsigned char>(bytes[i + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (unit != 0 && unit != 0xFEFF) {
            append_utf8(out, unit);
        }
    }
    return out;
}

std::string pdf_text(std::string_view bytes) {
    if (bytes.substr(0, 5) != "%PDF-" && bytes.find("%PDF-") > 1024) {
        return {};
    }
    const Document doc(bytes);
    if (doc.encrypted()) {
        return {};
    }
    Reader reader(doc);
    for (const Obj* page : doc.pages()) {
        reader.page(page);
    }
    return tidy(reader.text());
}

}  // namespace crucible::attach::detail
