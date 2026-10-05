// SPDX-License-Identifier: MIT
//
// Reading what a person attaches to a prompt.
//
// Every document here is built by hand, byte by byte: a zip with its entries
// stored rather than compressed, a PDF with its objects written out, the
// compound file an old Word or Excel or PowerPoint file is. That keeps the
// fixtures in the source and shows exactly which part of a format each
// reader depends on -- which is the part that will break when a file from
// the wild does something else.
#include "test_helpers.hpp"

#include <chrono>
#include <cstring>
#include <iterator>

#include "crucible/tools/attachments.hpp"
#include "crucible/util/format.hpp"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // GetACP
#endif

using namespace crucible;

namespace {

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

void set32(std::string& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
}

void set16(std::string& out, std::size_t at, std::uint32_t value) {
    out[at]     = static_cast<char>(value & 0xFF);
    out[at + 1] = static_cast<char>((value >> 8) & 0xFF);
}

std::uint32_t crc32_of(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char c : data) {
        crc ^= static_cast<unsigned char>(c);
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/// A zip with each entry stored as it is: enough for a .docx, a .xlsx, an
/// .odt, which are zips of XML.
std::string stored_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    std::string central;
    for (const auto& [name, data] : entries) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32_of(data);
        put32(out, 0x04034B50U);
        put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0);
        put32(out, crc);
        put32(out, static_cast<std::uint32_t>(data.size()));
        put32(out, static_cast<std::uint32_t>(data.size()));
        put16(out, static_cast<std::uint32_t>(name.size())); put16(out, 0);
        out += name;
        out += data;

        put32(central, 0x02014B50U);
        put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0);
        put32(central, crc);
        put32(central, static_cast<std::uint32_t>(data.size()));
        put32(central, static_cast<std::uint32_t>(data.size()));
        put16(central, static_cast<std::uint32_t>(name.size()));
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central += name;
    }
    const auto central_at = static_cast<std::uint32_t>(out.size());
    out += central;
    put32(out, 0x06054B50U);
    put16(out, 0); put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, central_at);
    put16(out, 0);
    return out;
}

/// A PDF with one page per content stream, objects found by scanning --
/// which is how the reader finds them, so no cross-reference table.
std::string pdf_with(const std::vector<std::string>& pages, const std::string& extra_objects = {}) {
    std::string out = "%PDF-1.4\n";
    out += "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n";
    std::string kids;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        kids += std::to_string(10 + i * 2) + " 0 R ";
    }
    out += "2 0 obj << /Type /Pages /Count " + std::to_string(pages.size()) + " /Kids [" + kids
         + "] /Resources << /Font << /F1 3 0 R >> >> >> endobj\n";
    out += "3 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n";
    out += extra_objects;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::size_t page = 10 + i * 2;
        out += std::to_string(page) + " 0 obj << /Type /Page /Parent 2 0 R /Contents "
             + std::to_string(page + 1) + " 0 R >> endobj\n";
        out += std::to_string(page + 1) + " 0 obj << /Length " + std::to_string(pages[i].size())
             + " >>\nstream\n" + pages[i] + "\nendstream\nendobj\n";
    }
    out += "trailer << /Root 1 0 R >>\n%%EOF\n";
    return out;
}

/// A compound file -- the container .doc, .xls and .ppt are -- holding the
/// given streams. Each is padded past the mini-stream cutoff so it lives in
/// ordinary sectors, which keeps this to one FAT sector and one directory
/// sector.
std::string compound_with(const std::vector<std::pair<std::string, std::string>>& streams) {
    constexpr std::size_t kSector = 512;
    std::vector<std::string> bodies;
    for (const auto& [name, data] : streams) {
        std::string body = data;
        if (body.size() < 4096) {
            body.resize(4096, '\0');
        }
        body.resize((body.size() + kSector - 1) / kSector * kSector, '\0');
        bodies.push_back(body);
    }

    std::string header(512, '\0');
    std::memcpy(header.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    set16(header, 0x18, 0x3E); set16(header, 0x1A, 3); set16(header, 0x1C, 0xFFFE);
    set16(header, 0x1E, 9); set16(header, 0x20, 6);
    set32(header, 0x2C, 1);            // one FAT sector
    set32(header, 0x30, 1);            // the directory starts at sector 1
    set32(header, 0x38, 4096);         // the mini stream cutoff
    set32(header, 0x3C, 0xFFFFFFFE);   // no mini FAT
    set32(header, 0x44, 0xFFFFFFFE);   // no DIFAT sectors
    for (std::size_t i = 0; i < 109; ++i) {
        set32(header, 0x4C + i * 4, 0xFFFFFFFF);
    }
    set32(header, 0x4C, 0);            // the FAT is sector 0

    std::vector<std::uint32_t> fat(kSector / 4, 0xFFFFFFFF);
    fat[0] = 0xFFFFFFFD;   // the FAT's own sector
    fat[1] = 0xFFFFFFFE;   // the directory, one sector
    std::uint32_t next = 2;
    std::vector<std::uint32_t> starts;
    for (const std::string& body : bodies) {
        starts.push_back(next);
        const auto count = static_cast<std::uint32_t>(body.size() / kSector);
        for (std::uint32_t k = 0; k < count; ++k) {
            fat[next + k] = k + 1 < count ? next + k + 1 : 0xFFFFFFFE;
        }
        next += count;
    }
    std::string fat_sector;
    for (const std::uint32_t entry : fat) {
        put32(fat_sector, entry);
    }

    std::string directory(kSector, '\0');
    const auto entry = [&](std::size_t index, const std::string& name, int type, std::uint32_t start,
                           std::uint32_t size) {
        const std::size_t at = index * 128;
        for (std::size_t i = 0; i < name.size(); ++i) {
            directory[at + i * 2] = name[i];
        }
        set16(directory, at + 0x40, static_cast<std::uint32_t>((name.size() + 1) * 2));
        directory[at + 0x42] = static_cast<char>(type);
        set32(directory, at + 0x44, 0xFFFFFFFF);
        set32(directory, at + 0x48, 0xFFFFFFFF);
        set32(directory, at + 0x4C, index == 0 && streams.size() > 0 ? 1 : 0xFFFFFFFF);
        set32(directory, at + 0x74, start);
        set32(directory, at + 0x78, size);
    };
    entry(0, "Root Entry", 5, 0xFFFFFFFE, 0);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        entry(i + 1, streams[i].first, 2, starts[i], static_cast<std::uint32_t>(bodies[i].size()));
    }

    std::string out = header + fat_sector + directory;
    for (const std::string& body : bodies) {
        out += body;
    }
    return out;
}

/// The smallest Word 97 document: a FIB that says where the piece table
/// is, the piece table, and the text it points at, one byte a character.
std::string word_document(const std::string& text) {
    std::string word(1024, '\0');
    set16(word, 0, 0xA5EC);
    set16(word, 0x0A, 0x0200);         // the table stream is 1Table
    set16(word, 32, 14);               // csw
    set16(word, 62, 22);               // cslw
    set32(word, 64 + 3 * 4, static_cast<std::uint32_t>(text.size()));   // ccpText
    set16(word, 152, 93);              // cbRgFcLcb
    word += text;

    std::string table;
    table += '\x02';
    put32(table, 4 * 2 + 8);
    put32(table, 0);
    put32(table, static_cast<std::uint32_t>(text.size()));
    put16(table, 0);
    put32(table, (1024U * 2U) | 0x40000000U);   // compressed, at byte 1024
    put16(table, 0);
    set32(word, 154 + 33 * 8, 0);                                      // fcClx
    set32(word, 154 + 33 * 8 + 4, static_cast<std::uint32_t>(table.size()));   // lcbClx
    return compound_with({{"WordDocument", word}, {"1Table", table}});
}

std::string ppt_record(std::uint16_t ver_instance, std::uint16_t type, const std::string& body) {
    std::string out;
    put16(out, ver_instance);
    put16(out, type);
    put32(out, static_cast<std::uint32_t>(body.size()));
    return out + body;
}

std::string biff(std::uint16_t type, const std::string& body) {
    std::string out;
    put16(out, type);
    put16(out, static_cast<std::uint32_t>(body.size()));
    return out + body;
}

}  // namespace

TEST(inflate_reads_a_zlib_stream) {
    const std::string packed("\x78\x9c\xcb\x48\xcd\xc9\xc9\x57\xc8\x40\x27\x01\x68\x03\x08\xb1", 16);
    CHECK_EQ(attach::detail::inflate(packed, true), std::string("hello hello hello hello"));
    CHECK(attach::detail::inflate("not compressed at all", true).empty());
}

TEST(a_word_document_reads_as_its_paragraphs_and_table_rows) {
    // Runs split across formatting join up; paragraphs end lines; a table's
    // cells read across a row; tab stops in the paragraph's properties are
    // not tabs in its text.
    const std::string xml =
        R"(<w:document><w:body>)"
        R"(<w:p><w:pPr><w:tabs><w:tab w:val="left" w:pos="720"/></w:tabs></w:pPr>)"
        R"(<w:r><w:t>The </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>launch</w:t></w:r>)"
        R"(<w:r><w:t xml:space="preserve"> moved &amp; slipped</w:t></w:r></w:p>)"
        R"(<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Servers</w:t></w:r></w:p></w:tc>)"
        R"(<w:tc><w:p><w:r><w:t>3,000</w:t></w:r></w:p></w:tc></w:tr></w:tbl>)"
        R"(<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t><w:br/><w:t>c</w:t></w:r></w:p>)"
        R"(</w:body></w:document>)";
    CHECK_EQ(attach::detail::ooxml_text(xml, "w:t", "w:p"),
             std::string("The launch moved & slipped\nServers\t3,000\na\tb\nc\n"));
}

TEST(a_docx_on_disk_is_read_and_labeled) {
    TempDir dir;
    const auto path = dir.path() / "Report.docx";
    write_bytes(path, stored_zip({
        {"[Content_Types].xml", "<Types/>"},
        {"word/document.xml", "<w:document><w:body><w:p><w:r><w:t>Quarterly notes</w:t></w:r></w:p>"
                              "<w:p><w:r><w:t>Caf\xC3\xA9 costs rose.</w:t></w:r></w:p></w:body></w:document>"},
    }));
    const attach::Info info = attach::inspect(path);
    CHECK(info.kind == attach::Kind::Document);
    CHECK_EQ(info.label, std::string("DOCX"));
    CHECK_EQ(info.name, std::string("Report.docx"));
    CHECK(info.error.empty());
    CHECK_EQ(attach::read(path, 10000).body, std::string("Quarterly notes\nCaf\xC3\xA9 costs rose."));
}

TEST(a_spreadsheet_reads_a_row_to_a_line_under_its_sheet_name) {
    TempDir dir;
    const auto path = dir.path() / "costs.xlsx";
    write_bytes(path, stored_zip({
        {"xl/workbook.xml", R"(<workbook><sheets><sheet name="Budget" sheetId="1" r:id="rId1"/></sheets></workbook>)"},
        {"xl/sharedStrings.xml", R"(<sst><si><t>Item</t></si><si><t>Servers</t></si></sst>)"},
        {"xl/worksheets/sheet1.xml",
         R"(<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="inlineStr"><is><t>Cost</t></is></c></row>)"
         R"(<row r="2"><c r="A2" t="s"><v>1</v></c><c r="B2"><v>3000</v></c></row></sheetData></worksheet>)"},
    }));
    CHECK_EQ(attach::read(path, 10000).body, std::string("## Budget\nItem\tCost\nServers\t3000"));
}

TEST(html_reads_as_text_without_its_scripts_or_its_indentation) {
    const std::string html =
        "<html><head><style>p{color:red}</style><script>var x = '<p>no</p>';</script></head>\n"
        "<body>\n  <h1>Notes</h1>\n  <p>The <b>launch</b>\n     moved &mdash; again &amp; again.</p>\n"
        "  <table>\n    <tr>\n      <td>Caf&eacute;</td>\n      <td>1,200</td>\n    </tr>\n  </table>\n"
        "  <pre>keep   this\n  as is</pre>\n<!-- <p>a comment</p> --></body></html>";
    CHECK_EQ(attach::detail::markup_text(html),
             std::string("Notes\nThe launch moved \xE2\x80\x94 again & again.\nCaf\xC3\xA9\t1,200\n"
                         "keep   this\n  as is\n"));
}

TEST(rtf_reads_as_text_with_its_unicode_and_without_its_tables_of_fonts) {
    // \u with its fallback character after it (and a negative one, which is
    // how RTF writes past U+7FFF); \'xx in Windows-1252; a
    // group of fonts that is not text; a table's cells and rows.
    const std::string rtf =
        R"({\rtf1\ansi\ansicpg1252{\fonttbl{\f0 Times;}}{\*\generator Writer;})"
        R"(\pard Na\u239\'efve r\'e9sum\'e9 \u-21504?\par )"
        R"(It\rquote s \'93quoted\'94.\par )"
        R"(\trowd Servers\cell 3,000\cell\row})";
    CHECK_EQ(attach::detail::rtf_text(rtf),
             std::string("Na\xC3\xAFve r\xC3\xA9sum\xC3\xA9 \xEA\xB0\x80\n"
                         "It\xE2\x80\x99s \xE2\x80\x9Cquoted\xE2\x80\x9D.\n"
                         "Servers\t3,000\n"));
}

TEST(a_pdf_reads_its_words_in_order_across_pages) {
    // Words placed one by one on a line, a TJ with a kerning gap that is a
    // space, a run that starts where the last one stopped, and a second page.
    const std::string page_one =
        "BT /F1 12 Tf 72 700 Td (The) Tj ( launch moved) Tj ET\n"
        "BT /F1 12 Tf 72 680 Td [(Costs) -250 (rose)] TJ ET\n"
        "BT /F1 12 Tf 72 640 Td (A new paragraph) Tj ET";
    const std::string page_two = "BT /F1 12 Tf 72 700 Td (Second page) Tj ET";
    const std::string text = attach::detail::pdf_text(pdf_with({page_one, page_two}));
    CHECK_EQ(text, std::string("The launch moved\nCosts rose\n\nA new paragraph\n\nSecond page"));
}

TEST(a_pdf_reads_compressed_pages_and_mapped_fonts) {
    // A FlateDecode content stream, and a font whose codes mean what its
    // ToUnicode map says rather than what they are.
    const std::string packed(
        "\x78\x9c\x73\x0a\x51\xd0\x77\x33\x54\x30\x34\x52\x08\x49\x53\x30\x37\x52\x30\x37\x30\x50\x08"
        "\x49\x51\xd0\x70\xce\xcf\x2d\x28\x4a\x2d\x2e\x4e\x4d\x51\x28\x48\x4c\x4f\xd5\x54\x08\xc9\x52"
        "\x70\x0d\x01\x00\x0e\xf4\x0c\xac", 54);
    std::string pdf = "%PDF-1.5\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Type /Pages /Count 2 /Kids [10 0 R 12 0 R] >> endobj\n"
                      "3 0 obj << /Type /Font /Subtype /Type1 >> endobj\n"
                      "10 0 obj << /Type /Page /Parent 2 0 R /Resources << /Font << /F1 3 0 R >> >> "
                      "/Contents 11 0 R >> endobj\n"
                      "11 0 obj << /Length 54 /Filter /FlateDecode >>\nstream\n" + packed + "\nendstream\nendobj\n";
    const std::string cmap = "begincmap 1 begincodespacerange <0000> <FFFF> endcodespacerange "
                             "2 beginbfchar <0001> <0048> <0002> <00E9> endbfchar endcmap";
    pdf += "4 0 obj << /Type /Font /Subtype /Type0 /ToUnicode 5 0 R >> endobj\n"
           "5 0 obj << /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream\nendobj\n"
           "12 0 obj << /Type /Page /Parent 2 0 R /Resources << /Font << /F2 4 0 R >> >> "
           "/Contents 13 0 R >> endobj\n"
           "13 0 obj << /Length 41 >>\nstream\nBT /F2 12 Tf 72 700 Td <00010002> Tj ET\nendstream\nendobj\n"
           "trailer << /Root 1 0 R >>\n%%EOF\n";
    CHECK_EQ(attach::detail::pdf_text(pdf), std::string("Compressed page\n\nH\xC3\xA9"));
}

TEST(an_encrypted_or_imageonly_pdf_says_nothing_rather_than_noise) {
    const std::string encrypted = pdf_with({"BT /F1 12 Tf 72 700 Td (secret) Tj ET"})
                                + "trailer << /Root 1 0 R /Encrypt 9 0 R >>\n";
    CHECK(attach::detail::pdf_text(encrypted).empty());
    CHECK(attach::detail::pdf_text(pdf_with({"q 100 0 0 100 0 0 cm /Im1 Do Q"})).empty());
}

TEST(an_old_word_file_reads_its_text_without_field_codes) {
    // Paragraph marks, a table row (cell marks, then the row's own mark), and
    // a hyperlink field whose code is dropped and whose result is kept.
    const std::string text = std::string("Quarterly notes\r")
                           + "Servers\x07" + "3,000\x07\x07"
                           + "See \x13 HYPERLINK \"https://example.invalid\" \x14the site\x15 for more.\r";
    TempDir dir;
    const auto path = dir.path() / "old.doc";
    write_bytes(path, word_document(text));
    const attach::Text read = attach::read(path, 10000);
    CHECK_EQ(read.body, std::string("Quarterly notes\nServers\t3,000\nSee the site for more."));
    CHECK(read.note.empty());
}

TEST(an_old_powerpoint_file_reads_its_slides_and_not_its_master) {
    std::string inner = ppt_record(0x0F, 0x03F8, ppt_record(0, 0x0FA8, "Click to edit Master title style"))
                      + ppt_record(0, 0x0FA0, std::string("T\0i\0t\0l\0e\0", 10))
                      + ppt_record(0, 0x0FA8, "Body text")
                      + ppt_record(0, 0x0FA8, "*");
    const std::string document = ppt_record(0x0F, 0x03E8, inner);
    CHECK_EQ(attach::detail::legacy_office_text(compound_with({{"PowerPoint Document", document}})),
             std::string("Title\nBody text\n"));
}

TEST(an_old_excel_file_reads_its_cells_by_row_with_split_strings_joined) {
    // The shared strings, with the second one carried on into a CONTINUE
    // record -- which restarts with its own flag byte -- then a sheet of
    // string, number and RK cells.
    std::string sst;
    put32(sst, 2); put32(sst, 2);
    put16(sst, 4); sst += '\0'; sst += "Item";
    put16(sst, 6); sst += '\0'; sst += "Ser";
    std::string continued;
    continued += '\0'; continued += "ver";
    std::string sheet_name;
    put32(sheet_name, 0); put16(sheet_name, 0);
    sheet_name += '\x06'; sheet_name += '\0'; sheet_name += "Budget";
    std::string a1; put16(a1, 0); put16(a1, 0); put16(a1, 0); put32(a1, 0);
    std::string b1; put16(b1, 0); put16(b1, 1); put16(b1, 0);
    const double value = 3.5;
    b1.append(reinterpret_cast<const char*>(&value), sizeof(value));
    std::string a2; put16(a2, 1); put16(a2, 0); put16(a2, 0); put32(a2, 1);
    std::string b2; put16(b2, 1); put16(b2, 1); put16(b2, 0); put32(b2, (42U << 2) | 2U);
    std::string bof(16, '\0');

    const std::string book = biff(0x0809, bof) + biff(0x0085, sheet_name) + biff(0x00FC, sst)
                           + biff(0x003C, continued) + biff(0x000A, "")
                           + biff(0x0809, bof) + biff(0x00FD, a1) + biff(0x0203, b1)
                           + biff(0x00FD, a2) + biff(0x027E, b2) + biff(0x000A, "");
    CHECK_EQ(attach::detail::legacy_office_text(compound_with({{"Workbook", book}})),
             std::string("## Budget\nItem\t3.5\nServer\t42\n\n"));
}

TEST(a_folder_reads_its_text_files_and_skips_what_nobody_means_to_send) {
    TempDir dir;
    const auto root = dir.path() / "proj";
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "node_modules" / "left");
    std::filesystem::create_directories(root / ".git");
    write_bytes(root / "README.md", "# Proj\n");
    write_bytes(root / "src" / "main.c", "int main(void) { return 0; }\n");
    write_bytes(root / "node_modules" / "left" / "pad.js", "module.exports = 1;\n");
    write_bytes(root / ".git" / "HEAD", "ref: refs/heads/main\n");
    write_bytes(root / "logo.bin", std::string("\x89PNG\0\0\0", 7));

    const attach::Info info = attach::inspect(root);
    CHECK(info.kind == attach::Kind::Folder);
    CHECK_EQ(info.label, std::string("FOLDER"));
    CHECK_EQ(info.files, 3);

    const attach::Text text = attach::read(root, 100000);
    CHECK_EQ(text.body, std::string("### README.md\n# Proj\n\n### src/main.c\nint main(void) { return 0; }\n\n"));
    CHECK_EQ(text.note, std::string("1 not readable as text"));
}

TEST(a_file_that_is_not_text_says_so_instead_of_sending_bytes) {
    TempDir dir;
    write_bytes(dir.path() / "blob.dat", std::string("\x00\x01\x02\xff\xfe binary", 13));
    const attach::Info info = attach::inspect(dir.path() / "blob.dat");
    CHECK(info.kind == attach::Kind::Binary);
    CHECK(!info.error.empty());
    CHECK(attach::inspect(dir.path() / "gone.txt").kind == attach::Kind::Missing);

    // Latin-1 that is not UTF-8 is not mistaken for binary either way: it is
    // not sent as text, and it is not silently turned into mojibake.
    write_bytes(dir.path() / "notes.txt", "plain words\n");
    CHECK(attach::inspect(dir.path() / "notes.txt").kind == attach::Kind::Text);
    CHECK_EQ(attach::label_for(dir.path() / "notes.txt"), std::string("TXT"));
    CHECK_EQ(attach::label_for(dir.path() / "Makefile"), std::string("FILE"));
}

TEST(compose_shares_the_room_out_and_says_what_it_cut) {
    TempDir dir;
    write_bytes(dir.path() / "short.md", "a short note");
    std::string long_text;
    for (int i = 0; i < 2000; ++i) {
        long_text += "line " + std::to_string(i) + "\n";
    }
    write_bytes(dir.path() / "long.txt", long_text);
    const std::vector<attach::Attachment> files{
        {(dir.path() / "short.md").string(), "short.md", "MD", attach::Kind::Text, {}},
        {(dir.path() / "long.txt").string(), "long.txt", "TXT", attach::Kind::Text, {}},
    };
    const attach::Composed composed = attach::compose(files, 6000, false);
    CHECK(composed.images.empty());
    CHECK(composed.text.find("<attachment name=\"short.md\" type=\"MD\">\na short note\n</attachment>")
          != std::string::npos);
    CHECK(composed.text.find("<attachment name=\"long.txt\" type=\"TXT\">\nline 0\n") != std::string::npos);
    CHECK(composed.text.find("[cut: this is the first ") != std::string::npos);
    CHECK(composed.text.find("line 1999") == std::string::npos);
    CHECK(composed.text.size() < 6000 + 400);
}

TEST(a_picture_goes_to_a_model_that_sees_and_is_named_to_one_that_does_not) {
    TempDir dir;
    const std::string png("\x89PNG\r\n\x1a\n-not-really-a-png", 25);
    write_bytes(dir.path() / "chart.png", png);
    const std::vector<attach::Attachment> picture{
        {(dir.path() / "chart.png").string(), "chart.png", "PNG", attach::Kind::Image, {}},
    };
    const attach::Composed seen = attach::compose(picture, 10000, true);
    CHECK_EQ(seen.images.size(), std::size_t{1});
    CHECK_EQ(seen.images.front().mime, std::string("image/png"));
    CHECK_EQ(seen.images.front().data, format::base64(png));
    CHECK(seen.text.find("chart.png") != std::string::npos);

    const attach::Composed blind = attach::compose(picture, 10000, false);
    CHECK(blind.images.empty());
    CHECK(blind.text.find("cannot see it") != std::string::npos);

    // A picture the window already shrank arrives with its bytes, and those
    // are what is sent.
    std::vector<attach::Attachment> shrunk = picture;
    shrunk.front().image = {"image/jpeg", "c21hbGw="};
    const attach::Composed sent = attach::compose(shrunk, 10000, true);
    CHECK_EQ(sent.images.front().mime, std::string("image/jpeg"));
    CHECK_EQ(sent.images.front().data, std::string("c21hbGw="));
    CHECK(attach::inspect(dir.path() / "chart.png").kind == attach::Kind::Image);
}

TEST(a_dropped_file_is_kept_inside_its_drop_and_nowhere_else) {
    TempDir dir;
    const std::filesystem::path root = dir.path() / "dropped";

    // A file, sent in two pieces.
    attach::Kept kept = attach::keep_dropped(root, "k1x9", "notes.md", "first ", false, false);
    CHECK(kept.error.empty());
    kept = attach::keep_dropped(root, "k1x9", "notes.md", "second", true, false);
    CHECK(kept.error.empty());
    CHECK_EQ(kept.path, (root / "k1x9" / "notes.md").string());
    CHECK_EQ(kept.top, kept.path);
    std::ifstream in(kept.path, std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(written, std::string("first second"));

    // A folder, made before anything is in it, and a file deep inside it
    // that knows which dropped thing it belongs to.
    kept = attach::keep_dropped(root, "k1x9", "proj/", "", false, true);
    CHECK(kept.error.empty());
    CHECK(std::filesystem::is_directory(root / "k1x9" / "proj"));
    kept = attach::keep_dropped(root, "k1x9", "proj/src/main.c", "int main;", false, false);
    CHECK_EQ(kept.top, (root / "k1x9" / "proj").string());
    CHECK(std::filesystem::exists(root / "k1x9" / "proj" / "src" / "main.c"));

    // Nothing climbs out: not by "..", not by an absolute path, not by a
    // drop named like one.
    for (const char* wrong : {"../escape.txt", "proj/../../escape.txt", "/etc/passwd", "a//b", ""}) {
        CHECK(!attach::keep_dropped(root, "k1x9", wrong, "x", false, false).error.empty());
    }
    for (const char* wrong : {"..", "a/b", "", "x y"}) {
        CHECK(!attach::keep_dropped(root, wrong, "f.txt", "x", false, false).error.empty());
    }
    CHECK(!std::filesystem::exists(dir.path() / "escape.txt"));
}

TEST(an_old_drop_is_cleared_away_when_a_new_one_is_made) {
    TempDir dir;
    const std::filesystem::path root = dir.path() / "dropped";
    CHECK(attach::keep_dropped(root, "old", "a.txt", "a", false, false).error.empty());
    CHECK(attach::keep_dropped(root, "recent", "b.txt", "b", false, false).error.empty());
    std::filesystem::last_write_time(root / "old", std::filesystem::file_time_type::clock::now()
                                                       - std::chrono::hours(24 * 20));
    // More of a drop that is already there clears nothing; a new drop does.
    CHECK(attach::keep_dropped(root, "recent", "c.txt", "c", false, false).error.empty());
    CHECK(std::filesystem::exists(root / "old"));
    CHECK(attach::keep_dropped(root, "fresh", "d.txt", "d", false, false).error.empty());
    CHECK(!std::filesystem::exists(root / "old"));
    CHECK(std::filesystem::exists(root / "recent" / "b.txt"));
}

TEST(a_path_the_page_sends_as_utf8_finds_the_file_it_names) {
    // The file is made from UTF-16, the way a file dialog on Windows hands a
    // name over, and looked for by the UTF-8 the page sends. On Windows that
    // only meets in the middle when the program's code page is UTF-8, which
    // is packaging/windows/crucible.manifest -- linked into these tests too,
    // so this is the test of it there.
    TempDir dir;
    write_bytes(dir.path() / L"r\x00E9sum\x00E9.txt", "hello");
    const std::string sent = (dir.path() / "").string() + "r\xC3\xA9sum\xC3\xA9.txt";
    const attach::Info info = attach::inspect(sent);
    CHECK(info.kind == attach::Kind::Text);
    CHECK_EQ(info.name, std::string("r\xC3\xA9sum\xC3\xA9.txt"));
    CHECK_EQ(attach::read(sent, 100).body, std::string("hello"));
#if defined(_WIN32)
    CHECK_EQ(::GetACP(), 65001U);
#endif
}

TEST(base64_comes_back_as_it_went) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes += static_cast<char>(i);
    }
    std::string back;
    CHECK(format::from_base64(format::base64(bytes), back));
    CHECK_EQ(back, bytes);
    CHECK(format::from_base64("Zm9v\nYmFy", back));   // white space is skipped
    CHECK_EQ(back, std::string("foobar"));
    CHECK(format::from_base64("Zg", back));            // padding is optional
    CHECK_EQ(back, std::string("f"));
    CHECK(!format::from_base64("Zm9v!", back));
}

TEST(base64_pads_like_everyone_else) {
    CHECK_EQ(format::base64(""), std::string(""));
    CHECK_EQ(format::base64("f"), std::string("Zg=="));
    CHECK_EQ(format::base64("fo"), std::string("Zm8="));
    CHECK_EQ(format::base64("foo"), std::string("Zm9v"));
    CHECK_EQ(format::base64(std::string("\xff\x00\x10", 3)), std::string("/wAQ"));
}
