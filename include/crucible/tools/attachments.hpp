// SPDX-License-Identifier: MIT
//
// Things attached to a prompt: files, folders and pictures.
//
// A model reads text, so everything that is not a picture is turned into text
// here -- a PDF's words, a Word document's paragraphs, a spreadsheet's cells as
// rows, every readable file under a folder -- and the text goes into the
// prompt, above what was typed, with the name of what it came from. A picture
// goes as itself to a model that can see one, which is a provider's; a model
// on this machine is told one was attached and that it cannot see it.
//
// No conversion leaves the machine and nothing is installed for it. The
// formats that are zip files of XML -- .docx, .xlsx, .pptx, OpenDocument,
// EPUB -- are read with miniz, and a PDF's text is read here (pdf_text.cpp),
// or by pdftotext when the machine has it, which reads more of them better.
//
// The text is capped, and the cap is the model's: what fits beside the
// conversation in the context of whichever expert answers. A document that
// does not fit is cut, and the prompt says where and by how much.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::attach {

/// What something attached is, as far as reading it goes.
enum class Kind {
    Text,      ///< read as it is: code, Markdown, CSV, logs
    Document,  ///< has text inside a format: PDF, Word, slides, spreadsheets, RTF, HTML
    Image,     ///< a picture
    Folder,    ///< everything readable under it
    Binary,    ///< nothing to read
    Missing,   ///< not there
};

/// What a tile says about one: enough to draw it before anything is sent.
struct Info {
    std::string   path;
    std::string   name;
    Kind          kind  = Kind::Missing;
    std::string   label;    ///< "PDF", "DOCX", "PNG", "FOLDER" -- the badge
    std::uint64_t bytes = 0;
    int           files = 0;   ///< for a folder: how many of its files can be read
    std::string   preview;     ///< the start of its text, for a page-shaped tile
    std::string   mime;        ///< for a picture
    std::string   error;       ///< why it cannot be attached, when it cannot
};

/// Look at `path`: what it is, and the start of its text. Reads a document
/// through to get that, so it is for a worker thread.
Info inspect(const std::filesystem::path& path);

/// What `path` is by its name alone, and whether it is a folder. Reads
/// nothing, so it is fit for the thread a click arrives on.
Kind kind_of(const std::filesystem::path& path);

/// Whether `path` is markup somebody edits -- HTML, SVG, XML -- rather than
/// a document to take the words out of. Attached to a prompt, a web page is
/// read as its text; opened by an expert that is building it, or in the code
/// pane, it is its source, tags and all.
bool is_markup_source(const std::filesystem::path& path);


/// The readable text of `path`, at most `max_chars` of it.
struct Text {
    std::string body;
    std::size_t total = 0;    ///< how long the whole of it was
    bool        cut   = false;
    std::string note;         ///< anything worth saying: "read roughly", "3 files skipped"
};
Text read(const std::filesystem::path& path, std::size_t max_chars);

/// A picture, for a model that can see. `data` is base64.
struct Image {
    std::string mime;
    std::string data;
};

/// A picture's bytes as base64 and its type, when it is a picture of at most
/// `max_bytes`. Empty `data` otherwise.
Image picture(const std::filesystem::path& path, std::uintmax_t max_bytes);

/// One attachment as it is sent with a prompt.
///
/// `image` is filled by the window when it already has the picture -- it
/// shrinks a large one before sending, which a provider's size limit wants --
/// and left empty otherwise, in which case the file is read here.
struct Attachment {
    std::string path;
    std::string name;
    std::string label;
    Kind        kind = Kind::Binary;
    Image       image;
};

/// What the transcript keeps of an attachment: what to draw on its tile and
/// where it was. Not its contents -- those went to the model with the prompt.
struct Tile {
    std::string path;
    std::string name;
    std::string label;   ///< the badge: "PDF", "DOCX", "FOLDER"
    std::string kind;    ///< "image", "document", "text", "folder" or "file"
};

/// The word a Tile uses for `kind`, and back.
std::string kind_name(Kind kind);
Kind kind_from_name(std::string_view name);

/// The tile for an attachment, and an attachment to send again from a tile.
Tile tile_of(const Attachment& attachment);
Attachment from_tile(const Tile& tile);

/// What a past prompt's history says it had attached, when its contents are
/// not sent again: a conversation reopened from disk, or one with a turn
/// taken out. Empty when nothing was.
std::string recalled(const std::vector<Tile>& tiles);

/// What attachments become in a message: text above what was typed, and the
/// pictures to send beside it.
struct Composed {
    std::string        text;
    std::vector<Image> images;
};

/// Turn attachments into a message's text and pictures.
///
/// `budget` is how many characters of document text may go in, shared between
/// them. `sees_images` is whether the model can be sent a picture; when it
/// cannot, the text says one was attached, so it does not answer as if
/// nothing was.
Composed compose(const std::vector<Attachment>& attachments, std::size_t budget,
                 bool sees_images);

/// The badge for a path: its extension in capitals, or FOLDER.
std::string label_for(const std::filesystem::path& path);

/// Where a copy of something dropped on the window was kept.
struct Kept {
    std::string path;    ///< the file or folder written
    std::string top;     ///< the dropped item it belongs to: the file, or the folder at the top
    std::string error;   ///< why nothing was written, when nothing was
};

/// Keep (part of) a file dropped on a window whose webview hands the page a
/// dropped file's contents but not its path -- Windows' and macOS's, and
/// Linux's with GTK 4. The page sends the bytes, a piece at a time, and they
/// are written under `root`/`batch`/`relative`, which is then what is
/// attached.
///
/// `batch` names one drop and `relative` a path inside it ("notes.md", or
/// "project/src/main.c" for a file inside a dropped folder). Both are checked:
/// nothing is written outside `root`. `append` adds to what an earlier piece
/// wrote; `folder` makes `relative` a folder, for one that may hold nothing.
///
/// A drop's copies are kept for two weeks -- long enough for Ask again and a
/// reopened conversation to find them -- and removed when a later drop is
/// made after that.
Kept keep_dropped(const std::filesystem::path& root, std::string_view batch,
                  std::string_view relative, std::string_view bytes, bool append, bool folder);

// --- exposed for the tests ---------------------------------------------------

namespace detail {

/// The text of a PDF, from its bytes. Best effort: what the page draws as
/// text, in the order it draws it.
std::string pdf_text(std::string_view bytes);

/// The words inside a Word document's XML (word/document.xml).
std::string ooxml_text(std::string_view xml, std::string_view run_tag, std::string_view paragraph_tag);

/// The words inside an OpenDocument, HTML or similar markup.
std::string markup_text(std::string_view markup);

/// The words inside an RTF document.
std::string rtf_text(std::string_view rtf);

/// Inflate `data`; `zlib` says whether it carries a zlib header (a PDF's
/// streams do, a zip's members do not). Empty when it is not deflate.
std::string inflate(std::string_view data, bool zlib);

/// UTF-16BE to UTF-8, as a PDF writes text in its maps and strings.
std::string utf16be_to_utf8(std::string_view bytes);

/// The text of a .doc, .xls or .ppt -- the compound-file formats from before
/// 2007. Empty when it is none of those, or encrypted, or too old (Word 95).
std::string legacy_office_text(std::string_view bytes);

}  // namespace detail

}  // namespace crucible::attach
