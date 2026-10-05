// SPDX-License-Identifier: MIT
//
// The text in the Office files from before 2007: .doc, .xls and .ppt.
//
// Each is a compound file -- a little FAT file system in one file -- holding
// streams in a format of its own. Reading the text out of them takes far less
// than reading them: Word keeps its characters in one stream and a table of
// where the pieces are in another; PowerPoint keeps each text box as one
// record; Excel keeps every string once and each cell as a record that names
// it. What is here is that much, and nothing else -- no formatting, no
// formulas, no pictures.
//
// It is what a machine with no converter on it (Windows, nearly always) can
// read these with. attachments.cpp tries it first and falls back to the
// printable runs when it finds nothing.
#include "crucible/tools/attachments.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace crucible::attach::detail {
namespace {

std::uint16_t u16(std::string_view data, std::size_t at) {
    if (at + 2 > data.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[at])
                                      | (static_cast<unsigned char>(data[at + 1]) << 8));
}

std::uint32_t u32(std::string_view data, std::size_t at) {
    if (at + 4 > data.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(u16(data, at)) | (static_cast<std::uint32_t>(u16(data, at + 2)) << 16);
}

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

/// A byte in Windows-1252, which is what "compressed" text in these formats
/// is. Its 0x80 to 0x9F are not Latin-1's.
std::uint32_t cp1252(unsigned char c) {
    static constexpr std::array<std::uint16_t, 32> kHigh{
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    return c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80] : c;
}

/// UTF-16LE to UTF-8, pairs joined, lone halves dropped.
void append_utf16le(std::string& out, std::string_view bytes) {
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        std::uint32_t unit = u16(bytes, i);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes.size()) {
            const std::uint32_t low = u16(bytes, i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (unit < 0xD800 || unit >= 0xE000) {
            append_utf8(out, unit);
        }
    }
}

// ---------------------------------------------------------------------------
// The compound file
// ---------------------------------------------------------------------------

constexpr std::uint32_t kEnd  = 0xFFFFFFFE;
constexpr std::uint32_t kFree = 0xFFFFFFFF;

class Compound {
public:
    explicit Compound(std::string_view file) : file_(file) {
        if (file.size() < 512 || file.substr(0, 8) != std::string_view("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) {
            return;
        }
        const std::uint16_t shift = u16(file, 0x1E);
        const std::uint16_t mini_shift = u16(file, 0x20);
        if (shift < 7 || shift > 16 || mini_shift > shift) {
            return;
        }
        sector_ = std::size_t{1} << shift;
        mini_sector_ = std::size_t{1} << mini_shift;
        cutoff_ = u32(file, 0x38);

        // The FAT's own sectors: the first 109 listed in the header, the rest
        // in a chain of DIFAT sectors.
        std::vector<std::uint32_t> fat_sectors;
        for (std::size_t i = 0; i < 109; ++i) {
            const std::uint32_t s = u32(file, 0x4C + i * 4);
            if (s != kFree) {
                fat_sectors.push_back(s);
            }
        }
        std::uint32_t difat = u32(file, 0x44);
        for (int guard = 0; difat != kEnd && difat != kFree && guard < 4096; ++guard) {
            const std::string_view block = sector(difat);
            if (block.empty()) {
                break;
            }
            for (std::size_t i = 0; i + 4 < block.size(); i += 4) {
                const std::uint32_t s = u32(block, i);
                if (s != kFree) {
                    fat_sectors.push_back(s);
                }
            }
            difat = u32(block, block.size() - 4);
        }
        for (const std::uint32_t s : fat_sectors) {
            const std::string_view block = sector(s);
            for (std::size_t i = 0; i + 3 < block.size(); i += 4) {
                fat_.push_back(u32(block, i));
            }
        }

        const std::string directory = chain(u32(file, 0x30), fat_, false, 0);
        for (std::size_t at = 0; at + 128 <= directory.size(); at += 128) {
            Entry entry;
            const std::size_t name_bytes = std::min<std::size_t>(u16(directory, at + 0x40), 64);
            append_utf16le(entry.name, std::string_view(directory).substr(at, name_bytes >= 2 ? name_bytes - 2 : 0));
            entry.type  = static_cast<unsigned char>(directory[at + 0x42]);
            entry.start = u32(directory, at + 0x74);
            entry.size  = u32(directory, at + 0x78);
            entries_.push_back(entry);
        }
        if (entries_.empty() || entries_.front().type != 5) {
            entries_.clear();
            return;
        }
        // Small streams live in the mini stream, which is the root's own
        // data, cut into 64-byte sectors with a FAT of its own.
        mini_stream_ = chain(entries_.front().start, fat_, false, entries_.front().size);
        const std::string mini_fat = chain(u32(file, 0x3C), fat_, false, 0);
        for (std::size_t i = 0; i + 3 < mini_fat.size(); i += 4) {
            mini_fat_.push_back(u32(mini_fat, i));
        }
        ok_ = true;
    }

    bool ok() const { return ok_; }

    /// A stream by name, anywhere in the file; empty when there is none.
    std::string stream(std::string_view name) const {
        for (const Entry& entry : entries_) {
            if (entry.type == 2 && entry.name == name) {
                return entry.size < cutoff_ ? chain(entry.start, mini_fat_, true, entry.size)
                                            : chain(entry.start, fat_, false, entry.size);
            }
        }
        return {};
    }

private:
    struct Entry {
        std::string   name;
        unsigned char type  = 0;
        std::uint32_t start = 0;
        std::uint32_t size  = 0;
    };

    std::string_view sector(std::uint32_t index) const {
        const std::size_t at = (static_cast<std::size_t>(index) + 1) * sector_;
        if (at >= file_.size()) {
            return {};
        }
        return file_.substr(at, std::min(sector_, file_.size() - at));
    }

    std::string chain(std::uint32_t start, const std::vector<std::uint32_t>& table, bool mini,
                      std::size_t size) const {
        std::string out;
        const std::size_t unit = mini ? mini_sector_ : sector_;
        std::uint32_t at = start;
        // A loop in the chain is a broken file, not a reason to hang.
        for (std::size_t guard = 0; at != kEnd && at != kFree && at < table.size() && guard <= table.size(); ++guard) {
            if (mini) {
                const std::size_t offset = static_cast<std::size_t>(at) * unit;
                if (offset >= mini_stream_.size()) {
                    break;
                }
                out.append(mini_stream_, offset, std::min(unit, mini_stream_.size() - offset));
            } else {
                const std::string_view block = sector(at);
                if (block.empty()) {
                    break;
                }
                out.append(block);
            }
            if (size != 0 && out.size() >= size) {
                break;
            }
            at = table[at];
        }
        if (size != 0 && out.size() > size) {
            out.resize(size);
        }
        return out;
    }

    std::string_view           file_;
    std::size_t                sector_      = 512;
    std::size_t                mini_sector_ = 64;
    std::uint32_t              cutoff_      = 4096;
    std::vector<std::uint32_t> fat_;
    std::vector<std::uint32_t> mini_fat_;
    std::vector<Entry>         entries_;
    std::string                mini_stream_;
    bool                       ok_ = false;
};

// ---------------------------------------------------------------------------
// Word 97 and later
// ---------------------------------------------------------------------------

std::string word_text(const Compound& file) {
    const std::string word = file.stream("WordDocument");
    if (word.size() < 0x200 || u16(word, 0) != 0xA5EC) {
        return {};
    }
    // The FIB: a fixed base, then three arrays whose lengths it gives.
    const std::uint16_t flags = u16(word, 0x0A);
    if ((flags & 0x0100) != 0) {
        return {};   // fEncrypted
    }
    const std::size_t rg_w  = 32 + 2;
    const std::size_t csw   = u16(word, 32);
    const std::size_t rg_lw = rg_w + csw * 2 + 2;
    const std::size_t cslw  = u16(word, rg_w + csw * 2);
    const std::size_t rg_fc = rg_lw + cslw * 4 + 2;
    const std::uint32_t ccp_text = u32(word, rg_lw + 3 * 4);
    const std::uint32_t fc_clx   = u32(word, rg_fc + 33 * 8);
    const std::uint32_t lcb_clx  = u32(word, rg_fc + 33 * 8 + 4);

    const std::string table = file.stream((flags & 0x0200) != 0 ? "1Table" : "0Table");
    if (lcb_clx == 0 || static_cast<std::size_t>(fc_clx) + lcb_clx > table.size()) {
        return {};
    }
    const std::string_view clx = std::string_view(table).substr(fc_clx, lcb_clx);

    // The piece table: where each run of the document's characters sits in
    // the WordDocument stream, and whether it is one byte a character or two.
    std::size_t at = 0;
    while (at < clx.size() && clx[at] == 0x01) {
        at += 3 + u16(clx, at + 1);   // a Prc: formatting, skipped
    }
    if (at >= clx.size() || clx[at] != 0x02) {
        return {};
    }
    const std::uint32_t lcb = u32(clx, at + 1);
    const std::string_view plc = clx.substr(at + 5, std::min<std::size_t>(lcb, clx.size() - at - 5));
    const std::size_t pieces = plc.size() >= 4 ? (plc.size() - 4) / 12 : 0;

    std::string raw;   // as UTF-8, with Word's control characters still in it
    for (std::size_t i = 0; i < pieces; ++i) {
        const std::uint32_t cp_start = u32(plc, i * 4);
        const std::uint32_t cp_end   = std::min(u32(plc, (i + 1) * 4), ccp_text);
        if (cp_start >= cp_end) {
            continue;
        }
        const std::uint32_t fc = u32(plc, (pieces + 1) * 4 + i * 8 + 2);
        const bool compressed  = (fc & 0x40000000U) != 0;
        const std::size_t count = cp_end - cp_start;
        if (compressed) {
            const std::size_t offset = (fc & 0x3FFFFFFFU) / 2;
            for (std::size_t k = 0; k < count && offset + k < word.size(); ++k) {
                append_utf8(raw, cp1252(static_cast<unsigned char>(word[offset + k])));
            }
        } else {
            const std::size_t offset = fc & 0x3FFFFFFFU;
            if (offset < word.size()) {
                append_utf16le(raw, std::string_view(word).substr(offset, std::min(count * 2, word.size() - offset)));
            }
        }
    }

    // Word's marks to text: paragraph ends to line ends, cell ends to tabs,
    // a field's code dropped and its result kept.
    std::string out;
    int in_code = 0;
    std::vector<bool> fields;   // per open field: still in its code part
    for (const char c : raw) {
        switch (c) {
            case 0x13:   // field begins
                fields.push_back(true);
                ++in_code;
                continue;
            case 0x14:   // field separator: the result follows
                if (!fields.empty() && fields.back()) {
                    fields.back() = false;
                    --in_code;
                }
                continue;
            case 0x15:   // field ends
                if (!fields.empty()) {
                    if (fields.back()) {
                        --in_code;
                    }
                    fields.pop_back();
                }
                continue;
            default:
                break;
        }
        if (in_code > 0) {
            continue;
        }
        if (c == '\r' || c == 0x0B || c == 0x0C) {
            out += '\n';
        } else if (c == 0x07) {
            out += '\t';   // the end of a cell, or of a row
        } else if (c == 0x1E) {
            out += '-';
        } else if (c == '\t' || static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    // A row ends with a cell mark of its own, after the last cell's: a tab
    // followed by a tab, then the next paragraph.
    std::string tidy;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\t' && i + 1 < out.size() && out[i + 1] == '\t') {
            tidy += '\n';
            ++i;
            continue;
        }
        tidy += out[i];
    }
    return tidy;
}

// ---------------------------------------------------------------------------
// PowerPoint 97 and later
// ---------------------------------------------------------------------------

void ppt_records(std::string_view data, std::string& out, int depth) {
    std::size_t at = 0;
    while (at + 8 <= data.size() && depth < 32) {
        const std::uint16_t ver_instance = u16(data, at);
        const std::uint16_t type = u16(data, at + 2);
        const std::uint32_t length = u32(data, at + 4);
        const std::size_t body = at + 8;
        if (length > data.size() - body) {
            break;
        }
        const std::string_view content = data.substr(body, length);
        std::string text;
        if (type == 0x03F8 || type == 0x0FC9) {
            // The master slide and the handout master: "Click to edit Master
            // title style", and the placeholders for the slide number.
        } else if ((ver_instance & 0x0F) == 0x0F) {
            ppt_records(content, out, depth + 1);   // a container
        } else if (type == 0x0FA0) {                // TextCharsAtom: UTF-16LE
            append_utf16le(text, content);
        } else if (type == 0x0FA8) {                // TextBytesAtom: one byte each
            for (const char c : content) {
                append_utf8(text, cp1252(static_cast<unsigned char>(c)));
            }
        }
        if (!text.empty() && text != "*") {
            out += text + "\n";
        }
        if (type == 0x03F3) {   // SlidePersistAtom: a slide begins
            out += '\n';
        }
        at = body + length;
    }
}

std::string ppt_text(const Compound& file) {
    const std::string document = file.stream("PowerPoint Document");
    if (document.empty()) {
        return {};
    }
    std::string raw;
    ppt_records(document, raw, 0);
    std::string out;
    for (const char c : raw) {
        out += c == '\r' || c == 0x0B ? '\n' : c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Excel 97 and later
// ---------------------------------------------------------------------------

/// A BIFF8 string: a count, a flag byte, then one or two bytes a character.
/// Strings that cross into a CONTINUE record restart their flag there, which
/// `pieces` -- the record and the CONTINUEs after it -- is for.
class BiffReader {
public:
    explicit BiffReader(std::vector<std::string_view> pieces) : pieces_(std::move(pieces)) {}

    bool done() const { return piece_ >= pieces_.size(); }

    std::uint8_t byte() {
        settle();
        if (done()) {
            return 0;
        }
        return static_cast<std::uint8_t>(pieces_[piece_][at_++]);
    }
    std::uint16_t word() {
        const std::uint16_t lo = byte();
        return static_cast<std::uint16_t>(lo | (byte() << 8));
    }
    std::uint32_t dword() {
        const std::uint32_t lo = word();
        return lo | (static_cast<std::uint32_t>(word()) << 16);
    }
    void skip(std::size_t count) {
        for (std::size_t i = 0; i < count && !done(); ++i) {
            byte();
        }
    }

    std::string string(std::size_t chars, bool has_flags = true) {
        std::uint8_t flags = has_flags ? byte() : 0;
        std::size_t runs = 0;
        std::size_t extended = 0;
        if ((flags & 0x08) != 0) {
            runs = word();
        }
        if ((flags & 0x04) != 0) {
            extended = dword();
        }
        std::string out;
        for (std::size_t i = 0; i < chars && !done(); ++i) {
            if (at_ >= pieces_[piece_].size()) {
                // Into a CONTINUE: it starts with the flag byte again.
                settle();
                if (done()) {
                    break;
                }
                flags = static_cast<std::uint8_t>(pieces_[piece_][at_++]);
            }
            if ((flags & 0x01) != 0) {
                append_utf8(out, word());
            } else {
                append_utf8(out, cp1252(byte()));
            }
        }
        skip(runs * 4 + extended);
        return out;
    }

private:
    void settle() {
        while (piece_ < pieces_.size() && at_ >= pieces_[piece_].size()) {
            ++piece_;
            at_ = 0;
        }
    }

    std::vector<std::string_view> pieces_;
    std::size_t piece_ = 0;
    std::size_t at_    = 0;
};

std::string number_text(double value) {
    char buffer[32];
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    }
    return buffer;
}

double rk_value(std::uint32_t rk) {
    double value = 0.0;
    if ((rk & 0x02) != 0) {
        value = static_cast<double>(static_cast<std::int32_t>(rk) >> 2);
    } else {
        const std::uint64_t bits = static_cast<std::uint64_t>(rk & 0xFFFFFFFCU) << 32;
        std::memcpy(&value, &bits, sizeof(value));
    }
    return (rk & 0x01) != 0 ? value / 100.0 : value;
}

std::string xls_text(const Compound& file) {
    std::string book = file.stream("Workbook");
    if (book.empty()) {
        book = file.stream("Book");
    }
    if (book.empty()) {
        return {};
    }
    struct Record {
        std::uint16_t    type;
        std::string_view body;
    };
    std::vector<Record> records;
    for (std::size_t at = 0; at + 4 <= book.size();) {
        const std::uint16_t type = u16(book, at);
        const std::uint16_t length = u16(book, at + 2);
        if (at + 4 + length > book.size()) {
            break;
        }
        records.push_back({type, std::string_view(book).substr(at + 4, length)});
        at += 4 + static_cast<std::size_t>(length);
    }

    std::vector<std::string> shared;
    std::vector<std::string> sheet_names;
    // Each sheet: row -> column -> text.
    std::vector<std::map<std::uint16_t, std::map<std::uint16_t, std::string>>> sheets;
    int bof_depth = 0;

    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        switch (r.type) {
            case 0x0809:   // BOF: the globals, then one per sheet
                if (++bof_depth == 1 && i > 0) {
                    sheets.emplace_back();
                }
                break;
            case 0x000A:   // EOF
                --bof_depth;
                break;
            case 0x0085: {   // BOUNDSHEET: a sheet's name
                if (r.body.size() >= 8) {
                    BiffReader reader({r.body.substr(6)});
                    const std::size_t chars = reader.byte();
                    sheet_names.push_back(reader.string(chars));
                }
                break;
            }
            case 0x00FC: {   // SST: every string, once
                std::vector<std::string_view> pieces{r.body};
                for (std::size_t k = i + 1; k < records.size() && records[k].type == 0x003C; ++k) {
                    pieces.push_back(records[k].body);
                }
                BiffReader reader(std::move(pieces));
                reader.skip(4);
                const std::uint32_t count = reader.dword();
                for (std::uint32_t k = 0; k < count && !reader.done() && k < 1000000; ++k) {
                    const std::size_t chars = reader.word();
                    shared.push_back(reader.string(chars));
                }
                break;
            }
            default:
                break;
        }
        if (sheets.empty() || bof_depth < 1 || r.body.size() < 6) {
            continue;
        }
        auto& cells = sheets.back();
        const std::uint16_t row = u16(r.body, 0);
        const std::uint16_t col = u16(r.body, 2);
        switch (r.type) {
            case 0x00FD:   // LABELSST
                if (r.body.size() >= 10) {
                    const std::uint32_t index = u32(r.body, 6);
                    cells[row][col] = index < shared.size() ? shared[index] : std::string();
                }
                break;
            case 0x0204: {   // LABEL
                BiffReader reader({r.body.substr(6)});
                const std::size_t chars = reader.word();
                cells[row][col] = reader.string(chars);
                break;
            }
            case 0x0203:   // NUMBER
                if (r.body.size() >= 14) {
                    double value = 0.0;
                    std::memcpy(&value, r.body.data() + 6, sizeof(value));
                    cells[row][col] = number_text(value);
                }
                break;
            case 0x027E:   // RK
                if (r.body.size() >= 10) {
                    cells[row][col] = number_text(rk_value(u32(r.body, 6)));
                }
                break;
            case 0x00BD: {   // MULRK: a run of RKs along a row
                const std::size_t count = (r.body.size() - 6) / 6;
                for (std::size_t k = 0; k < count; ++k) {
                    cells[row][static_cast<std::uint16_t>(col + k)]
                        = number_text(rk_value(u32(r.body, 4 + k * 6 + 2)));
                }
                break;
            }
            case 0x0006:   // FORMULA: its cached result, when a number
                if (r.body.size() >= 14 && u16(r.body, 12) != 0xFFFF) {
                    double value = 0.0;
                    std::memcpy(&value, r.body.data() + 6, sizeof(value));
                    cells[row][col] = number_text(value);
                }
                break;
            default:
                break;
        }
    }

    std::string out;
    for (std::size_t s = 0; s < sheets.size(); ++s) {
        out += "## " + (s < sheet_names.size() ? sheet_names[s] : "Sheet " + std::to_string(s + 1)) + "\n";
        for (const auto& [row, columns] : sheets[s]) {
            std::string line;
            std::uint16_t next = 0;
            for (const auto& [col, text] : columns) {
                for (; next < col; ++next) {
                    line += '\t';
                }
                line += text;
                line += '\t';
                next = static_cast<std::uint16_t>(col + 1);
            }
            while (!line.empty() && line.back() == '\t') {
                line.pop_back();
            }
            out += line + "\n";
        }
        out += "\n";
    }
    return out;
}

}  // namespace

std::string legacy_office_text(std::string_view bytes) {
    const Compound file(bytes);
    if (!file.ok()) {
        return {};
    }
    std::string text = word_text(file);
    if (text.empty()) {
        text = ppt_text(file);
    }
    if (text.empty()) {
        text = xls_text(file);
    }
    return text;
}

}  // namespace crucible::attach::detail
