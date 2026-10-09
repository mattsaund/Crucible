// SPDX-License-Identifier: MIT
//
// See attachments.hpp for what this is for. The PDF reader is pdf_text.cpp.
#include "crucible/tools/attachments.hpp"
#include "crucible/tools/computer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include <miniz.h>

#include "crucible/util/format.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::attach {
namespace {

/// The most of one file that is ever read. A prompt cannot hold more than a
/// few hundred thousand characters on any model there is, and a document of
/// several hundred megabytes is not going to be summarized by pasting it.
constexpr std::uintmax_t kMaxRead = 64ULL << 20;

/// The most of a picture that is sent. Anthropic's limit is five megabytes
/// encoded; the window shrinks a larger one before it gets here.
constexpr std::uintmax_t kMaxImage = 3750ULL << 10;

std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = format::to_lower(path.extension().string());
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    return ext;
}

bool read_file(const std::filesystem::path& path, std::string& out, std::uintmax_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(static_cast<std::size_t>(limit), '\0');
    in.read(out.data(), static_cast<std::streamsize>(limit));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

// --- what it is ----------------------------------------------------------------

const std::map<std::string, std::string>& image_types() {
    static const std::map<std::string, std::string> kTypes{
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"webp", "image/webp"}, {"bmp", "image/bmp"},
    };
    return kTypes;
}

/// Formats with text inside them, and how to get it out.
enum class Format { None, Pdf, Docx, Pptx, Xlsx, OpenDocument, Epub, Rtf, Html, LegacyOffice };

Format format_of(const std::string& ext) {
    if (ext == "pdf") { return Format::Pdf; }
    if (ext == "docx" || ext == "docm" || ext == "dotx") { return Format::Docx; }
    if (ext == "pptx" || ext == "pptm") { return Format::Pptx; }
    if (ext == "xlsx" || ext == "xlsm") { return Format::Xlsx; }
    if (ext == "odt" || ext == "ods" || ext == "odp" || ext == "ott") { return Format::OpenDocument; }
    if (ext == "epub") { return Format::Epub; }
    if (ext == "rtf") { return Format::Rtf; }
    if (ext == "html" || ext == "htm" || ext == "xhtml") { return Format::Html; }
    if (ext == "doc" || ext == "xls" || ext == "ppt") { return Format::LegacyOffice; }
    return Format::None;
}

/// Plain text, by looking: no NUL bytes, and UTF-8 that decodes.
bool looks_like_text(std::string_view sample) {
    if (sample.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t i = 0;
    while (i < sample.size()) {
        const auto c = static_cast<unsigned char>(sample[i]);
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0) {
            return false;
        }
        if (i + static_cast<std::size_t>(extra) >= sample.size()) {
            break;   // a character cut off by the end of the sample is fine
        }
        for (int k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(sample[i + static_cast<std::size_t>(k)]) >> 6) != 0x2) {
                return false;
            }
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

/// Folders a person attaching a project does not mean to send: dependencies,
/// build output, version control, caches.
bool skipped_folder(const std::string& name) {
    static const std::array<const char*, 14> kSkip{
        "node_modules", "build", "dist", "target", "out", "__pycache__", "venv", ".venv",
        "vendor", "bin", "obj", "Pods", "DerivedData", "coverage"};
    return name.empty() || name.front() == '.'
        || std::find(kSkip.begin(), kSkip.end(), name) != kSkip.end();
}

// --- zip archives -------------------------------------------------------------

class Zip {
public:
    explicit Zip(const std::string& bytes) {
        std::memset(&zip_, 0, sizeof(zip_));
        open_ = mz_zip_reader_init_mem(&zip_, bytes.data(), bytes.size(), 0) != 0;
    }
    ~Zip() {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
    }
    Zip(const Zip&)            = delete;
    Zip& operator=(const Zip&) = delete;

    bool open() const { return open_; }

    std::vector<std::string> names() {
        std::vector<std::string> out;
        if (!open_) {
            return out;
        }
        const mz_uint count = mz_zip_reader_get_num_files(&zip_);
        for (mz_uint i = 0; i < count; ++i) {
            char name[1024];
            mz_zip_reader_get_filename(&zip_, i, name, sizeof(name));
            out.emplace_back(name);
        }
        return out;
    }

    std::string read(const std::string& name) {
        if (!open_) {
            return {};
        }
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&zip_, name.c_str(), &size, 0);
        if (data == nullptr) {
            return {};
        }
        std::string out(static_cast<const char*>(data), size);
        mz_free(data);
        return out;
    }

private:
    mz_zip_archive zip_{};
    bool           open_ = false;
};

/// "slide12.xml" sorts after "slide2.xml" the way a person would expect.
bool numbered_before(const std::string& a, const std::string& b) {
    const auto number = [](const std::string& s) {
        std::size_t end = s.find_last_of("0123456789");
        if (end == std::string::npos) {
            return -1L;
        }
        std::size_t start = end;
        while (start > 0 && std::isdigit(static_cast<unsigned char>(s[start - 1])) != 0) {
            --start;
        }
        return std::strtol(s.substr(start, end - start + 1).c_str(), nullptr, 10);
    };
    const long na = number(a);
    const long nb = number(b);
    return na != nb ? na < nb : a < b;
}

/// The named characters HTML writers actually use. The full list is two
/// thousand; past these, a page says &#233; anyway.
const std::map<std::string, std::uint32_t>& html_entities() {
    static const std::map<std::string, std::uint32_t> kNamed{
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"bull", 0x2022},
        {"middot", 0xB7}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"copy", 0xA9}, {"reg", 0xAE},
        {"trade", 0x2122}, {"deg", 0xB0}, {"times", 0xD7}, {"divide", 0xF7}, {"plusmn", 0xB1},
        {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7},
        {"para", 0xB6}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193},
        {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"shy", 0xAD}, {"zwj", 0x200D},
        {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003},
        {"aacute", 0xE1}, {"agrave", 0xE0}, {"acirc", 0xE2}, {"auml", 0xE4}, {"atilde", 0xE3},
        {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7}, {"eacute", 0xE9}, {"egrave", 0xE8},
        {"ecirc", 0xEA}, {"euml", 0xEB}, {"iacute", 0xED}, {"igrave", 0xEC}, {"icirc", 0xEE},
        {"iuml", 0xEF}, {"ntilde", 0xF1}, {"oacute", 0xF3}, {"ograve", 0xF2}, {"ocirc", 0xF4},
        {"ouml", 0xF6}, {"otilde", 0xF5}, {"oslash", 0xF8}, {"uacute", 0xFA}, {"ugrave", 0xF9},
        {"ucirc", 0xFB}, {"uuml", 0xFC}, {"szlig", 0xDF}, {"Aacute", 0xC1}, {"Agrave", 0xC0},
        {"Auml", 0xC4}, {"Ccedil", 0xC7}, {"Eacute", 0xC9}, {"Egrave", 0xC8}, {"Ntilde", 0xD1},
        {"Oacute", 0xD3}, {"Ouml", 0xD6}, {"Uuml", 0xDC},
    };
    return kNamed;
}

std::string decode_entities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const std::size_t semi = text.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        const std::string_view name = text.substr(i + 1, semi - i - 1);
        std::uint32_t cp = 0;
        if (name == "amp") { cp = '&'; }
        else if (name == "lt") { cp = '<'; }
        else if (name == "gt") { cp = '>'; }
        else if (name == "quot") { cp = '"'; }
        else if (name == "apos") { cp = '\''; }
        else if (name == "nbsp") { cp = ' '; }
        else if (const auto named = html_entities().find(std::string(name)); named != html_entities().end()) {
            cp = named->second;
        }
        else if (!name.empty() && name.front() == '#') {
            cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
                     ? static_cast<std::uint32_t>(std::strtoul(std::string(name.substr(2)).c_str(), nullptr, 16))
                     : static_cast<std::uint32_t>(std::strtoul(std::string(name.substr(1)).c_str(), nullptr, 10));
        }
        if (cp == 0) {
            out += '&';
            continue;
        }
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
        i = semi;
    }
    return out;
}

/// A spreadsheet's cells, a row to a line and a tab between cells.
std::string xlsx_text(Zip& zip) {
    // The strings are stored once and referred to by number.
    std::vector<std::string> shared;
    const std::string strings = zip.read("xl/sharedStrings.xml");
    for (std::size_t at = 0; (at = strings.find("<si", at)) != std::string::npos;) {
        const std::size_t end = strings.find("</si>", at);
        if (end == std::string::npos) {
            break;
        }
        shared.push_back(detail::ooxml_text(std::string_view(strings).substr(at, end - at), "t", ""));
        at = end;
    }

    // The sheets' names, in the workbook's order.
    std::vector<std::string> titles;
    const std::string workbook = zip.read("xl/workbook.xml");
    for (std::size_t at = 0; (at = workbook.find("<sheet ", at)) != std::string::npos; ++at) {
        const std::size_t name = workbook.find("name=\"", at);
        const std::size_t close = workbook.find('>', at);
        if (name != std::string::npos && name < close) {
            const std::size_t end = workbook.find('"', name + 6);
            titles.push_back(decode_entities(std::string_view(workbook).substr(name + 6, end - name - 6)));
        }
    }

    std::vector<std::string> sheets;
    for (const std::string& name : zip.names()) {
        if (name.rfind("xl/worksheets/sheet", 0) == 0 && name.size() > 4
            && name.compare(name.size() - 4, 4, ".xml") == 0) {
            sheets.push_back(name);
        }
    }
    std::sort(sheets.begin(), sheets.end(), numbered_before);

    std::string out;
    for (std::size_t s = 0; s < sheets.size(); ++s) {
        const std::string xml = zip.read(sheets[s]);
        out += "## " + (s < titles.size() ? titles[s] : "Sheet " + std::to_string(s + 1)) + "\n";
        for (std::size_t row = 0; (row = xml.find("<row", row)) != std::string::npos;) {
            const std::size_t row_end = xml.find("</row>", row);
            if (row_end == std::string::npos) {
                break;
            }
            std::string line;
            for (std::size_t c = row; (c = xml.find("<c ", c)) != std::string::npos && c < row_end;) {
                const std::size_t head_end = xml.find('>', c);
                const std::string head = xml.substr(c, head_end - c);
                const bool self_closing = head_end > 0 && xml[head_end - 1] == '/';
                std::string value;
                if (!self_closing) {
                    const std::size_t cell_end = xml.find("</c>", head_end);
                    const std::string body = xml.substr(head_end + 1, cell_end - head_end - 1);
                    if (head.find("t=\"s\"") != std::string::npos) {
                        const std::size_t v = body.find("<v>");
                        if (v != std::string::npos) {
                            const auto index = static_cast<std::size_t>(std::strtoul(body.c_str() + v + 3, nullptr, 10));
                            value = index < shared.size() ? shared[index] : std::string();
                        }
                    } else if (head.find("t=\"inlineStr\"") != std::string::npos) {
                        value = detail::ooxml_text(body, "t", "");
                    } else {
                        const std::size_t v = body.find("<v>");
                        if (v != std::string::npos) {
                            value = decode_entities(body.substr(v + 3, body.find("</v>", v) - v - 3));
                        }
                    }
                    c = cell_end;
                } else {
                    c = head_end;
                }
                if (!line.empty() || !value.empty()) {
                    line += (line.empty() ? "" : "\t") + value;
                }
            }
            while (!line.empty() && line.back() == '\t') {
                line.pop_back();
            }
            if (!line.empty()) {
                out += line + "\n";
            }
            row = row_end;
        }
        out += "\n";
    }
    return out;
}

/// One attribute's value out of a tag: `attribute(R"(<item id="a">)", "id")`
/// is "a".
std::string attribute(std::string_view tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const std::size_t at = tag.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t start = at + key.size();
    const std::size_t end = tag.find('"', start);
    return end == std::string_view::npos ? std::string() : decode_entities(tag.substr(start, end - start));
}

/// Every tag named `name` in `xml`, whole.
std::vector<std::string_view> tags(std::string_view xml, std::string_view name) {
    std::vector<std::string_view> out;
    const std::string open = "<" + std::string(name) + " ";
    for (std::size_t at = 0; (at = xml.find(open, at)) != std::string_view::npos;) {
        const std::size_t end = xml.find('>', at);
        if (end == std::string_view::npos) {
            break;
        }
        out.push_back(xml.substr(at, end - at + 1));
        at = end;
    }
    return out;
}

std::vector<std::string> epub_spine(Zip& zip) {
    const std::string container = zip.read("META-INF/container.xml");
    std::string package;
    for (const std::string_view tag : tags(container, "rootfile")) {
        package = attribute(tag, "full-path");
        break;
    }
    if (package.empty()) {
        return {};
    }
    const std::string opf = zip.read(package);
    const std::string base = package.find('/') == std::string::npos
                                 ? std::string()
                                 : package.substr(0, package.rfind('/') + 1);
    std::map<std::string, std::string> hrefs;
    for (const std::string_view tag : tags(opf, "item")) {
        hrefs[attribute(tag, "id")] = attribute(tag, "href");
    }
    std::vector<std::string> pages;
    for (const std::string_view tag : tags(opf, "itemref")) {
        const auto found = hrefs.find(attribute(tag, "idref"));
        if (found != hrefs.end() && !found->second.empty()) {
            pages.push_back(base + found->second);
        }
    }
    return pages;
}

std::string office_text(const std::string& bytes, Format format) {
    Zip zip(bytes);
    if (!zip.open()) {
        return {};
    }
    switch (format) {
        case Format::Docx: {
            std::string out = detail::ooxml_text(zip.read("word/document.xml"), "w:t", "w:p");
            // Footnotes are text the document says; the body refers to them.
            const std::string notes = detail::ooxml_text(zip.read("word/footnotes.xml"), "w:t", "w:p");
            if (!notes.empty()) {
                out += "\n\nFootnotes:\n" + notes;
            }
            return out;
        }
        case Format::Pptx: {
            std::vector<std::string> slides;
            for (const std::string& name : zip.names()) {
                if (name.rfind("ppt/slides/slide", 0) == 0 && name.find(".xml") != std::string::npos
                    && name.find("_rels") == std::string::npos) {
                    slides.push_back(name);
                }
            }
            std::sort(slides.begin(), slides.end(), numbered_before);
            std::string out;
            for (std::size_t i = 0; i < slides.size(); ++i) {
                out += "## Slide " + std::to_string(i + 1) + "\n"
                     + detail::ooxml_text(zip.read(slides[i]), "a:t", "a:p") + "\n\n";
            }
            return out;
        }
        case Format::Xlsx:
            return xlsx_text(zip);
        case Format::OpenDocument:
            return detail::markup_text(zip.read("content.xml"));
        case Format::Epub: {
            // The chapters in the order the book's spine gives them; in name
            // order when it cannot be found.
            std::vector<std::string> pages = epub_spine(zip);
            if (pages.empty()) {
                for (const std::string& name : zip.names()) {
                    const std::string ext = lower_extension(name);
                    if (ext == "xhtml" || ext == "html" || ext == "htm") {
                        pages.push_back(name);
                    }
                }
                std::sort(pages.begin(), pages.end(), numbered_before);
            }
            std::string out;
            for (const std::string& page : pages) {
                out += detail::markup_text(zip.read(page)) + "\n\n";
            }
            return out;
        }
        default:
            return {};
    }
}

/// Run a program and take what it prints, when the machine has it.
std::string program_output(const std::vector<std::string>& argv) {
    if (argv.empty() || !util::on_path(argv.front())) {
        return {};
    }
    util::Subprocess child;
    std::string      error;
    if (!child.start(argv, {}, {}, error)) {
        return {};
    }
    std::string out;
    std::string line;
    while (child.read_line(line)) {
        out += line;
        out += '\n';
        if (out.size() > kMaxRead) {
            break;
        }
    }
    return child.wait() == 0 ? out : std::string();
}

/// The printable runs in a file nothing here can parse: an old binary Word
/// file, mostly. Rough, and said to be rough; a document whose words come
/// back with some debris around them is still more use than a refusal.
std::string strings_of(std::string_view bytes) {
    std::string out;
    std::string run;
    const auto flush = [&]() {
        if (run.size() >= 6) {
            out += run;
            out += '\n';
        }
        run.clear();
    };
    // UTF-16LE runs first, which is how Word stores text that is not plain
    // Latin; then single-byte ones.
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(bytes[i]);
        if (bytes[i + 1] == '\0' && (std::isprint(lo) != 0 || lo == '\t')) {
            run += static_cast<char>(lo);
        } else if (bytes[i + 1] == '\0' && (lo == '\r' || lo == '\n')) {
            run += '\n';
        } else {
            flush();
        }
    }
    flush();
    if (out.size() < bytes.size() / 200) {
        for (const char c : bytes) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isprint(u) != 0 || u == '\t') {
                run += c;
            } else if (u == '\r' || u == '\n') {
                run += '\n';
            } else {
                flush();
            }
        }
        flush();
    }
    return out;
}

std::string extract(const std::filesystem::path& path, std::string& note);

/// The whole text of one file, uncapped, without the blank lines a format
/// leaves at its end. `note` says when it was read roughly, or not at all.
std::string text_of_file(const std::filesystem::path& path, std::string& note) {
    std::string text = extract(path, note);
    const std::size_t last = text.find_last_not_of(" \t\r\n\f");
    text.erase(last == std::string::npos ? 0 : last + 1);
    const std::size_t first = text.find_first_not_of("\r\n\f");
    text.erase(0, first == std::string::npos ? text.size() : first);
    return text;
}

std::string extract(const std::filesystem::path& path, std::string& note) {
    const std::string ext = lower_extension(path);
    const Format format = format_of(ext);
    std::string bytes;
    if (!read_file(path, bytes, kMaxRead)) {
        note = "could not be read";
        return {};
    }
    switch (format) {
        case Format::Pdf: {
            // pdftotext reads more PDFs, and more of each, than anything that
            // fits in a file here -- so it is used when the machine has it.
            std::string text = program_output({"pdftotext", "-q", "-layout", "-enc", "UTF-8",
                                               path.string(), "-"});
            if (text.find_first_not_of(" \n\t\f") == std::string::npos) {
                text = detail::pdf_text(bytes);
            }
            if (text.find_first_not_of(" \n\t\f") == std::string::npos) {
                note = "no text could be read from it -- a scanned page is a picture of text, "
                       "and an encrypted one cannot be read";
            }
            return text;
        }
        case Format::Docx:
        case Format::Pptx:
        case Format::Xlsx:
        case Format::OpenDocument:
        case Format::Epub:
            return office_text(bytes, format);
        case Format::Rtf:
            return detail::rtf_text(bytes);
        case Format::Html:
            return detail::markup_text(bytes);
        case Format::LegacyOffice: {
            // An old binary Office file: read here; with the machine's own
            // converter when that finds nothing (Word 95, mostly); and as the
            // printable runs in it when there is no converter either.
            std::string text = detail::legacy_office_text(bytes);
#if defined(__APPLE__)
            if (text.find_first_not_of(" \n\t") == std::string::npos) {
                text = program_output({"textutil", "-convert", "txt", "-stdout", path.string()});
            }
#else
            if (text.find_first_not_of(" \n\t") == std::string::npos) {
                text = ext == "doc" ? program_output({"catdoc", "-w", path.string()})
                     : ext == "xls" ? program_output({"xls2csv", path.string()})
                                    : program_output({"catppt", path.string()});
            }
#endif
            if (text.find_first_not_of(" \n\t") == std::string::npos) {
                note = "read roughly -- this is an old binary Office format";
                text = strings_of(bytes);
            }
            return text;
        }
        case Format::None:
            break;
    }
    if (looks_like_text(std::string_view(bytes).substr(0, 65536))) {
        // A byte-order mark is not part of the text.
        if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) {
            bytes.erase(0, 3);
        }
        return bytes;
    }
    note = "not something that can be read as text";
    return {};
}

/// Cut `text` to `limit` characters at a line end where there is one near.
std::string cut_to(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t at = text.rfind('\n', limit);
    if (at == std::string::npos || at < limit * 3 / 4) {
        at = limit;
    }
    // Not through the middle of a UTF-8 character.
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    return text.substr(0, at);
}

}  // namespace

// ---------------------------------------------------------------------------
// The text inside markup
// ---------------------------------------------------------------------------

namespace detail {

std::string inflate(std::string_view data, bool zlib) {
    if (data.empty()) {
        return {};
    }
    std::size_t out_size = 0;
    void* out = tinfl_decompress_mem_to_heap(data.data(), data.size(), &out_size,
                                             zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
    if (out == nullptr) {
        return {};
    }
    std::string result(static_cast<const char*>(out), out_size);
    mz_free(out);
    return result;
}

std::string ooxml_text(std::string_view xml, std::string_view run_tag, std::string_view paragraph_tag) {
    // Only what is inside a run's text element is text; everything else --
    // styles, field codes, revision marks -- is the document describing
    // itself. Paragraphs end lines; tabs and breaks are said as they are.
    std::string out;
    const std::string open  = "<" + std::string(run_tag);
    const std::string close = "</" + std::string(run_tag) + ">";
    const std::string para  = paragraph_tag.empty() ? std::string() : "</" + std::string(paragraph_tag) + ">";
    std::size_t at = 0;
    while (at < xml.size()) {
        const std::size_t tag = xml.find('<', at);
        if (tag == std::string_view::npos) {
            break;
        }
        const std::size_t end = xml.find('>', tag);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view head = xml.substr(tag, end - tag + 1);
        if (head.substr(0, open.size()) == open
            && (head.size() > open.size() && (head[open.size()] == '>' || head[open.size()] == ' '))) {
            if (head.back() == '/' || head[head.size() - 2] == '/') {
                at = end + 1;
                continue;   // an empty run
            }
            const std::size_t stop = xml.find(close, end);
            if (stop == std::string_view::npos) {
                break;
            }
            out += decode_entities(xml.substr(end + 1, stop - end - 1));
            at = stop + close.size();
            continue;
        }
        // The element's name, exactly: "<w:tab/>" is a tab in the text, and
        // "<w:tabs>" is the paragraph's list of tab stops.
        const std::size_t name_end = head.find_first_of(" />", head.size() > 1 && head[1] == '/' ? 2 : 1);
        const std::string_view name = head.substr(1, name_end == std::string_view::npos ? head.size() - 1 : name_end - 1);
        if (!para.empty() && head == para) {
            out += '\n';
        } else if (name == "w:tab" && head.find("w:val") == std::string_view::npos) {
            out += '\t';
        } else if (name == "w:br" || name == "a:br" || name == "w:cr") {
            out += '\n';
        } else if (name == "/w:tc") {
            // A table cell: its paragraphs' line ends become one tab, so a
            // row reads across.
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            out += '\t';
        } else if (name == "/w:tr") {
            while (!out.empty() && out.back() == '\t') {
                out.pop_back();
            }
            out += '\n';
        }
        at = end + 1;
    }
    return out;
}

std::string markup_text(std::string_view markup) {
    std::string out;
    bool        in_pre = false;
    const auto starts = [](std::string_view head, std::string_view name) {
        if (head.size() <= name.size() + 1 || head.substr(1, name.size()) != name) {
            return false;
        }
        const char after = head[name.size() + 1];
        return after == '>' || after == ' ' || after == '/' || after == '\n' || after == '\t';
    };
    // Text between tags. Outside <pre>, a run of white space is one space,
    // the way a browser draws it: markup is indented for its author, and a
    // line break in the source is not one on the page.
    const auto text = [&](std::string_view raw) {
        const std::string decoded = decode_entities(raw);
        for (const char c : decoded) {
            const bool white = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (!white || in_pre) {
                out += c;
            } else if (!out.empty() && out.back() != ' ' && out.back() != '\n' && out.back() != '\t') {
                out += ' ';
            }
        }
    };
    const auto trim_end = [&](std::string_view chars) {
        while (!out.empty() && chars.find(out.back()) != std::string_view::npos) {
            out.pop_back();
        }
    };
    std::size_t at = 0;
    while (at < markup.size()) {
        const std::size_t tag = markup.find('<', at);
        text(markup.substr(at, (tag == std::string_view::npos ? markup.size() : tag) - at));
        if (tag == std::string_view::npos) {
            break;
        }
        // Comments, scripts and styles are not text.
        if (markup.substr(tag, 4) == "<!--") {
            const std::size_t end = markup.find("-->", tag);
            at = end == std::string_view::npos ? markup.size() : end + 3;
            continue;
        }
        const std::size_t end = markup.find('>', tag);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string head = format::to_lower(markup.substr(tag, end - tag + 1));
        if (starts(head, "script") || starts(head, "style")) {
            const std::string close = starts(head, "script") ? "</script" : "</style";
            const std::string rest  = format::to_lower(markup.substr(end));
            const std::size_t stop  = rest.find(close);
            at = stop == std::string::npos ? markup.size() : end + stop;
            continue;
        }
        if (starts(head, "pre")) {
            in_pre = true;
        } else if (starts(head, "/pre")) {
            in_pre = false;
        }
        // Where a block ends, a line does -- in HTML and in OpenDocument's
        // own vocabulary both.
        static const std::array<const char*, 20> kBlocks{
            "/p", "br", "br/", "/div", "/li", "/h1", "/h2", "/h3", "/h4", "/h5", "/h6",
            "/title", "/text:p", "/text:h", "text:line-break", "text:line-break/",
            "/section", "/article", "/blockquote", "/pre"};
        bool block = false;
        for (const char* name : kBlocks) {
            block = block || starts(head, name);
        }
        if (block) {
            trim_end(" ");
            out += '\n';
        } else if (starts(head, "/td") || starts(head, "/th") || starts(head, "/table:table-cell")) {
            // A cell's own paragraphs do not each make a line; a row does.
            trim_end(" \n");
            out += '\t';
        } else if (starts(head, "/tr") || starts(head, "/table:table-row")) {
            trim_end(" \t");
            out += '\n';
        } else if (starts(head, "text:tab") || starts(head, "text:tab/")) {
            out += '\t';
        } else if (starts(head, "text:s") || starts(head, "text:s/")) {
            out += ' ';
        }
        at = end + 1;
    }
    // At most one blank line in a row, and none at the start.
    std::string tidy;
    std::size_t start = 0;
    int blank = 0;
    while (start < out.size()) {
        std::size_t stop = out.find('\n', start);
        if (stop == std::string::npos) {
            stop = out.size();
        }
        std::string line = out.substr(start, stop - start);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            if (++blank == 1 && !tidy.empty()) {
                tidy += '\n';
            }
        } else {
            blank = 0;
            tidy += line + "\n";
        }
        start = stop + 1;
    }
    return tidy;
}

std::string rtf_text(std::string_view rtf) {
    std::string out;
    // \u writes UTF-16 one unit at a time; the units are gathered until
    // something else is written, so a pair makes one character.
    std::string utf16;
    const auto flush = [&]() {
        if (!utf16.empty()) {
            out += utf16be_to_utf8(utf16);
            utf16.clear();
        }
    };
    const auto put = [&](std::string_view text) {
        flush();
        out += text;
    };
    const auto put_code = [&](std::uint32_t cp) {
        flush();
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    // \'xx is a byte in the document's code page, which is Windows-1252 for
    // nearly every RTF there is. Its 0x80 to 0x9F are not Latin-1's.
    static constexpr std::array<std::uint16_t, 32> kCp1252{
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

    // Groups that are not text -- the font table, colors, pictures, anything
    // marked \* -- are skipped whole by tracking the depth they started at.
    // How many fallback characters follow a \u is set per group by \uc.
    int depth = 0;
    int skip_from = -1;
    std::vector<int> fallback{1};
    int to_skip = 0;   // fallback characters still to drop after a \u
    std::size_t i = 0;
    while (i < rtf.size()) {
        const char c = rtf[i];
        if (c == '{') {
            ++depth;
            fallback.push_back(fallback.back());
            ++i;
            to_skip = 0;
            if (skip_from < 0) {
                const std::string_view ahead = rtf.substr(i, 12);
                if (ahead.rfind("\\*", 0) == 0 || ahead.rfind("\\fonttbl", 0) == 0
                    || ahead.rfind("\\colortbl", 0) == 0 || ahead.rfind("\\stylesheet", 0) == 0
                    || ahead.rfind("\\info", 0) == 0 || ahead.rfind("\\pict", 0) == 0) {
                    skip_from = depth;
                }
            }
            continue;
        }
        if (c == '}') {
            if (depth == skip_from) {
                skip_from = -1;
            }
            --depth;
            if (fallback.size() > 1) {
                fallback.pop_back();
            }
            ++i;
            to_skip = 0;
            continue;
        }
        if (c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c != '\\') {
            if (to_skip > 0) {
                --to_skip;
            } else if (skip_from < 0) {
                put(std::string_view(&rtf[i], 1));
            }
            ++i;
            continue;
        }

        ++i;
        if (i >= rtf.size()) {
            break;
        }
        const char n = rtf[i];
        if (n == '\'') {
            const auto code = i + 2 < rtf.size()
                ? static_cast<unsigned char>(std::strtol(std::string(rtf.substr(i + 1, 2)).c_str(), nullptr, 16))
                : 0;
            i += 3;
            if (to_skip > 0) {
                --to_skip;
            } else if (skip_from < 0 && code != 0) {
                put_code(code >= 0x80 && code < 0xA0 ? kCp1252[code - 0x80] : code);
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(n)) == 0) {
            // A control symbol: an escaped character, or one of the few
            // spaces and hyphens that have their own.
            ++i;
            if (skip_from >= 0) {
                continue;
            }
            if (n == '\\' || n == '{' || n == '}') {
                put(std::string(1, n));
            } else if (n == '~') {
                put(" ");
            } else if (n == '_') {
                put("-");
            }
            continue;
        }
        std::string word;
        while (i < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[i])) != 0) {
            word += rtf[i++];
        }
        std::string number;
        if (i < rtf.size() && (rtf[i] == '-' || std::isdigit(static_cast<unsigned char>(rtf[i])) != 0)) {
            number += rtf[i++];
            while (i < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[i])) != 0) {
                number += rtf[i++];
            }
        }
        if (i < rtf.size() && rtf[i] == ' ') {
            ++i;   // the space that ends a control word is not text
        }
        if (word == "uc" && !number.empty()) {
            fallback.back() = std::max(0, std::atoi(number.c_str()));
            continue;
        }
        if (skip_from >= 0) {
            continue;
        }
        if (word == "u" && !number.empty()) {
            long cp = std::strtol(number.c_str(), nullptr, 10);
            if (cp < 0) {
                cp += 65536;
            }
            utf16 += static_cast<char>((cp >> 8) & 0xFF);
            utf16 += static_cast<char>(cp & 0xFF);
            to_skip = fallback.back();
            continue;
        }
        to_skip = 0;
        if (word == "par" || word == "line" || word == "page" || word == "sect") {
            put("\n");
        } else if (word == "row") {
            flush();
            while (!out.empty() && out.back() == '\t') {
                out.pop_back();
            }
            out += '\n';
        } else if (word == "tab" || word == "cell") {
            put("\t");
        } else if (word == "emdash") {
            put("\xE2\x80\x94");
        } else if (word == "endash") {
            put("\xE2\x80\x93");
        } else if (word == "bullet") {
            put("\xE2\x80\xA2");
        } else if (word == "lquote") {
            put("\xE2\x80\x98");
        } else if (word == "rquote") {
            put("\xE2\x80\x99");
        } else if (word == "ldblquote") {
            put("\xE2\x80\x9C");
        } else if (word == "rdblquote") {
            put("\xE2\x80\x9D");
        }
    }
    flush();
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

std::string kind_name(Kind kind) {
    switch (kind) {
        case Kind::Image:    return "image";
        case Kind::Document: return "document";
        case Kind::Text:     return "text";
        case Kind::Folder:   return "folder";
        case Kind::Binary:
        case Kind::Missing:  break;
    }
    return "file";
}

Kind kind_from_name(std::string_view name) {
    if (name == "image")    { return Kind::Image; }
    if (name == "document") { return Kind::Document; }
    if (name == "text")     { return Kind::Text; }
    if (name == "folder")   { return Kind::Folder; }
    return Kind::Binary;
}

Tile tile_of(const Attachment& attachment) {
    const std::filesystem::path path(attachment.path);
    return Tile{attachment.path,
                attachment.name.empty() ? path.filename().string() : attachment.name,
                attachment.label.empty() ? label_for(path) : attachment.label,
                kind_name(attachment.kind)};
}

Attachment from_tile(const Tile& tile) {
    return Attachment{tile.path, tile.name, tile.label, kind_from_name(tile.kind), {}};
}

std::string recalled(const std::vector<Tile>& tiles) {
    if (tiles.empty()) {
        return {};
    }
    std::string names;
    for (const Tile& tile : tiles) {
        names += (names.empty() ? "" : ", ") + tile.name + " (" + tile.label + ")";
    }
    return "[Attached to this message: " + names + ". What was in "
         + (tiles.size() == 1 ? "it" : "them") + " is not repeated here.]\n\n";
}

std::string label_for(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return "FOLDER";
    }
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext.empty() ? "FILE" : ext;
}

bool is_markup_source(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    return ext == "html" || ext == "htm" || ext == "xhtml" || ext == "svg" || ext == "xml";
}

Kind kind_of(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return Kind::Folder;
    }
    if (!std::filesystem::exists(path, ec)) {
        return Kind::Missing;
    }
    const std::string ext = lower_extension(path);
    if (image_types().count(ext) != 0) {
        return Kind::Image;
    }
    return format_of(ext) != Format::None ? Kind::Document : Kind::Text;
}

Image picture(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    Image out;
    const auto type = image_types().find(lower_extension(path));
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    std::string bytes;
    if (type == image_types().end() || ec || size > max_bytes || !read_file(path, bytes, max_bytes)) {
        return out;
    }
    out.mime = type->second;
    out.data = format::base64(bytes);
    return out;
}

Info inspect(const std::filesystem::path& path) {
    Info info;
    info.path  = path.string();
    info.name  = path.filename().string();
    info.label = label_for(path);
    if (info.name.empty()) {
        info.name = path.parent_path().filename().string();
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        info.kind  = Kind::Missing;
        info.error = "not there";
        return info;
    }
    if (std::filesystem::is_directory(path, ec)) {
        info.kind = Kind::Folder;
        int counted = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator() && counted < 2000; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_directory(ec) && skipped_folder(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec)) {
                ++counted;
                info.bytes += it->file_size(ec);
            }
        }
        info.files = counted;
        return info;
    }
    info.bytes = std::filesystem::file_size(path, ec);
    const std::string ext = lower_extension(path);
    if (const auto image = image_types().find(ext); image != image_types().end()) {
        info.kind = Kind::Image;
        info.mime = image->second;
        return info;
    }
    std::string note;
    const std::string text = text_of_file(path, note);
    info.kind = format_of(ext) != Format::None ? Kind::Document
              : text.empty()                    ? Kind::Binary
                                                : Kind::Text;
    info.preview = cut_to(text, 600);
    if (text.empty() && info.kind != Kind::Text) {
        info.error = note.empty() ? "nothing in it can be read as text" : note;
    }
    return info;
}

Text read(const std::filesystem::path& path, std::size_t max_chars) {
    Text out;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        // Every readable file under it, each under its own path, until the
        // budget is spent -- and a count of what did not fit.
        std::vector<std::filesystem::path> files;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator() && files.size() < 2000;
             it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_directory(ec) && skipped_folder(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && image_types().count(lower_extension(it->path())) == 0) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        int left_out = 0;
        int unreadable = 0;
        for (const std::filesystem::path& file : files) {
            std::string note;
            const std::string text = text_of_file(file, note);
            if (text.empty()) {
                ++unreadable;
                continue;
            }
            const std::string relative = std::filesystem::relative(file, path, ec).generic_string();
            const std::string block = "### " + relative + "\n" + text + (text.back() == '\n' ? "" : "\n") + "\n";
            out.total += block.size();
            if (out.body.size() + block.size() > max_chars) {
                ++left_out;
                out.cut = true;
                continue;
            }
            out.body += block;
        }
        if (left_out > 0) {
            out.note = std::to_string(left_out) + " file" + (left_out == 1 ? "" : "s")
                     + " left out for room";
        }
        if (unreadable > 0) {
            out.note += (out.note.empty() ? "" : "; ") + std::to_string(unreadable)
                      + " not readable as text";
        }
        return out;
    }
    std::string note;
    const std::string text = text_of_file(path, note);
    out.total = text.size();
    out.body  = cut_to(text, max_chars);
    out.cut   = out.body.size() < text.size();
    out.note  = note;
    return out;
}

Kept keep_dropped(const std::filesystem::path& root, std::string_view batch,
                  std::string_view relative, std::string_view bytes, bool append, bool folder) {
    Kept kept;
    const bool batch_ok = !batch.empty() && batch.size() <= 64
        && std::all_of(batch.begin(), batch.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
           });
    if (!batch_ok) {
        kept.error = "a drop is named with letters, digits and dashes";
        return kept;
    }
    // Each part of the path a name, never "." or ".." or empty: the page
    // builds these from what was dropped, and nothing it sends may climb out.
    std::vector<std::string> parts;
    std::string part;
    for (const char c : relative) {
        if (c == '/' || c == '\\') {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(part);
    if (folder && parts.size() > 1 && parts.back().empty()) {
        parts.pop_back();   // "project/" is the folder "project"
    }
    for (const std::string& name : parts) {
        if (name.empty() || name == "." || name == ".." || name.find('\0') != std::string::npos
            || name.size() > 255) {
            kept.error = "not a path inside the drop: " + std::string(relative);
            return kept;
        }
    }

    std::error_code ec;
    const std::filesystem::path drop = root / std::string(batch);
    if (!std::filesystem::exists(drop, ec)) {
        // A new drop is the moment to clear out the old ones.
        const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 14);
        for (auto it = std::filesystem::directory_iterator(root, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code old;
            if (it->is_directory(old) && it->last_write_time(old) < cutoff && !old) {
                std::filesystem::remove_all(it->path(), old);
            }
        }
        ec.clear();
    }
    std::filesystem::path target = drop;
    for (const std::string& name : parts) {
        target /= std::filesystem::path(name);
    }
    kept.top  = (drop / std::filesystem::path(parts.front())).string();
    kept.path = target.string();

    if (folder) {
        std::filesystem::create_directories(target, ec);
        if (ec) {
            kept.error = "could not make " + kept.path + ": " + ec.message();
        }
        return kept;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        kept.error = "could not write " + kept.path;
        return kept;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        kept.error = "could not write all of " + kept.path;
    }
    return kept;
}

Composed compose(const std::vector<Attachment>& attachments, std::size_t budget, bool sees_images) {
    Composed out;
    std::size_t documents = 0;
    for (const Attachment& one : attachments) {
        documents += one.kind == Kind::Image ? 0 : 1;
    }
    // Shared out evenly, with a floor so that a dozen files each get a page
    // rather than a line.
    const std::size_t share = documents == 0 ? 0 : std::max<std::size_t>(budget / documents, 2000);

    for (const Attachment& one : attachments) {
        const std::string name = one.name.empty() ? std::filesystem::path(one.path).filename().string()
                                                  : one.name;
        if (one.kind == Kind::Image) {
            if (!sees_images) {
                // The words off it, when tesseract is here to read them: a
                // screenshot of an error, a photograph of a page. Otherwise
                // said plainly, so the answer does not pretend to have looked.
                std::string why;
                const std::string words = one.path.empty() ? std::string()
                                                           : tools::computer::read_text(one.path, why);
                if (!words.empty()) {
                    out.text += "[A picture was attached: " + name + ". This model reads text only; "
                                "these are the words read off it, in reading order:]\n" + words
                              + "\n[end of the picture's words]\n\n";
                } else {
                    out.text += "[A picture was attached: " + name + ". This model reads text only and "
                                "cannot see it -- say so if the question depends on it.]\n\n";
                }
                continue;
            }
            Image image = one.image.data.empty() ? picture(one.path, kMaxImage) : one.image;
            if (image.data.empty()) {
                out.text += "[A picture was attached, " + name + ", but it is too large to send.]\n\n";
                continue;
            }
            out.text += "[Picture attached: " + name + "]\n";
            out.images.push_back(std::move(image));
            continue;
        }
        const Text text = read(one.path, share);
        const std::string label = one.label.empty() ? label_for(one.path) : one.label;
        out.text += "<attachment name=\"" + name + "\" type=\"" + label + "\">\n";
        if (text.body.empty()) {
            out.text += "(" + (text.note.empty() ? std::string("nothing in it could be read") : text.note) + ")\n";
        } else {
            out.text += text.body;
            if (out.text.back() != '\n') {
                out.text += '\n';
            }
            if (text.cut) {
                out.text += "[cut: this is the first " + std::to_string(text.body.size()) + " of "
                          + std::to_string(text.total) + " characters, which is what fits]\n";
            }
            if (!text.note.empty() && !text.cut) {
                out.text += "[" + text.note + "]\n";
            }
        }
        out.text += "</attachment>\n\n";
    }
    return out;
}

}  // namespace crucible::attach
