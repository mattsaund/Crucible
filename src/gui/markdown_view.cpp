// SPDX-License-Identifier: MIT
//
// Drawing parsed markdown with Dear ImGui.
//
// util/markdown.hpp does the parsing, both faces share it, and this is the
// desktop half of the rendering -- ui/widgets/markdown_view.cpp is the
// terminal's.
//
// The work here is wrapping. ImGui::TextWrapped wraps one string in one font
// and one colour, which is no use for a paragraph whose bold run continues
// mid-sentence: drawing each span separately breaks the line at every style
// change instead of at the width. So the spans are flattened into words that
// each remember their own face, and the wrapping is done a word at a time.
//
// Word::space_after is why `**Bold**:` renders tight. A space between every
// pair of words is nearly right and wrong exactly where punctuation follows a
// styled run, which is where it is most noticeable.
#include "markdown_view.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "crucible/util/markdown.hpp"
#include "code_lines.hpp"
#include "syntax.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace crucible::gui {
namespace {

/// One word, with the style it inherited from its span.
///
/// Wrapping has to happen at this granularity rather than per span: a paragraph
/// is one span of prose, one of `code`, and one more of prose, and the line
/// break can fall anywhere in any of them.
struct Word {
    std::string text;
    bool        code   = false;
    bool        bold   = false;
    bool        italic = false;
    float       width  = 0.0F;
    bool        space_after = false;
};

ImFont* face_for(const Word& word) {
    if (word.bold) {
        return theme::bold();
    }
    if (word.italic) {
        return theme::italic();
    }
    return theme::body();
}

/// Split styled spans into measured words.
std::vector<Word> words_of(const std::vector<markdown::Span>& spans) {
    std::vector<Word> words;
    for (const markdown::Span& span : spans) {
        std::size_t i = 0;
        while (i < span.text.size()) {
            while (i < span.text.size() && span.text[i] == ' ') {
                ++i;  // leading spaces are carried by the previous word
                if (!words.empty()) {
                    words.back().space_after = true;
                }
            }
            const std::size_t start = i;
            while (i < span.text.size() && span.text[i] != ' ') {
                ++i;
            }
            if (i == start) {
                continue;
            }
            Word word;
            word.text   = span.text.substr(start, i - start);
            word.code   = span.code;
            word.bold   = span.bold;
            word.italic = span.italic;

            ImGui::PushFont(face_for(word));
            word.width = ImGui::CalcTextSize(word.text.c_str()).x;
            ImGui::PopFont();
            words.push_back(std::move(word));
        }
    }
    return words;
}

/// Draw one word at the cursor, with a background if it is inline code.
void draw_word(const Word& word, ImU32 colour) {
    ImGui::PushFont(face_for(word));
    if (word.code) {
        // Painted behind rather than around: an inline span cannot use a child
        // window without breaking the line it is part of.
        const ImVec2 at   = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(word.text.c_str());
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(at.x - 2.0F, at.y - 1.0F),
            ImVec2(at.x + size.x + 2.0F, at.y + size.y + 1.0F),
            theme::kRaised, 2.0F);
    }
    ImGui::PushStyleColor(ImGuiCol_Text,
                          theme::to_vec(word.code ? theme::kFlameBright : colour));
    ImGui::TextUnformatted(word.text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// Lay out `spans` as wrapped prose, indented by `indent` pixels.
void draw_spans(const std::vector<markdown::Span>& spans, ImU32 colour, float indent) {
    const std::vector<Word> words = words_of(spans);
    if (words.empty()) {
        ImGui::NewLine();
        return;
    }

    const float left      = ImGui::GetCursorPosX() + indent;
    const float available = ImGui::GetContentRegionAvail().x - indent;
    const float space     = ImGui::CalcTextSize(" ").x;

    float used  = 0.0F;
    bool  first = true;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const Word& word = words[i];

        // A space goes in only where the source had one. Adding one between
        // every pair renders `**Bold**: text` as "Bold : text", because the
        // colon is a separate word in a separate span and nothing said there
        // was whitespace between them.
        const bool  spaced  = !first && words[i - 1].space_after;
        const float advance = (spaced ? space : 0.0F) + word.width;

        if (!first && used + advance > available) {
            used  = 0.0F;
            first = true;
        }
        if (first) {
            ImGui::SetCursorPosX(left);
        } else {
            ImGui::SameLine(0.0F, spaced ? space : 0.0F);
        }
        draw_word(word, colour);
        used += advance;
        first = false;
    }
}

std::string joined(const std::vector<markdown::Span>& spans) {
    std::string out;
    for (const markdown::Span& span : spans) {
        out += span.text;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Code blocks
// ---------------------------------------------------------------------------
//
// The interface is set in a monospace face, which is what makes all of this
// affordable: a column is a fixed number of pixels, so the width of a line is
// its length times one measurement, the gutter is a whole number of columns
// wide, and nothing has to be measured per glyph. Every position below is
// computed that way rather than by walking the cursor with SameLine, which
// could not put a background behind a row or a number in a gutter anyway.

namespace {

ImU32 colour_of(syntax::Token token) {
    switch (token) {
        case syntax::Token::Keyword:  return theme::kCodeKeyword;
        case syntax::Token::Type:     return theme::kCodeType;
        case syntax::Token::String:   return theme::kCodeString;
        case syntax::Token::Number:   return theme::kCodeNumber;
        case syntax::Token::Comment:  return theme::kCodeComment;
        case syntax::Token::Function: return theme::kCodeFunction;
        case syntax::Token::Punct:    return theme::kCodePunct;
        case syntax::Token::Preproc:  return theme::kCodePreproc;
        case syntax::Token::Text:     break;
    }
    return theme::kCodeText;
}

/// How many lines are shown before the block folds itself.
///
/// Generous on purpose. A model that writes a fifty-line function has written
/// one thing, and folding it at ten turns the answer into a filing cabinet. The
/// fold is for the four-hundred-line file, where scrolling past it to reach the
/// sentence underneath is the actual complaint.
constexpr std::size_t kFoldAbove = 46;
constexpr std::size_t kFoldTo    = 34;

std::string digits_of(int value) {
    return std::to_string(value);
}

}  // namespace

void draw_code_block(std::string_view text, std::string_view language,
                     std::string_view caption, int seq) {
    if (text.empty()) {
        return;
    }

    // --- what is this ------------------------------------------------------
    syntax::Lang lang = syntax::lang_from(language);
    const bool   diff = lang == syntax::Lang::Diff || looks_like_diff(text);
    std::string  path(caption);
    if (diff) {
        if (std::string from_header = diff_path(text); !from_header.empty()) {
            path = from_header;
        }
        // The language of a diff is the language of the file it patches, so the
        // added lines get coloured like the code they are.
        lang = syntax::lang_from(path);
    }
    if (lang == syntax::Lang::None && !path.empty()) {
        lang = syntax::lang_from(path);
    }

    int added   = 0;
    int removed = 0;
    std::vector<CodeRow> rows = code_rows(text, diff, added, removed);

    // --- fold --------------------------------------------------------------
    ImGui::PushID(seq);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key     = ImGui::GetID("open");
    const bool    foldable = rows.size() > kFoldAbove;
    const bool    open     = !foldable || storage->GetBool(key, false);
    const std::size_t shown = open ? rows.size() : kFoldTo;

    // --- the shape of it ---------------------------------------------------
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(theme::body());
    const float advance = ImGui::CalcTextSize("0").x;  // monospace: one column
    const float line_h  = ImGui::GetTextLineHeight();

    std::size_t widest = 0;
    int         last_old = 0;
    int         last_new = 0;
    for (const CodeRow& row : rows) {
        widest   = std::max(widest, columns(row.text));
        last_old = std::max(last_old, row.old_no);
        last_new = std::max(last_new, row.new_no);
    }
    const int old_width = diff ? static_cast<int>(digits_of(std::max(last_old, 1)).size()) : 0;
    const int new_width = static_cast<int>(digits_of(std::max(last_new, 1)).size());
    // Numbers, then one column of marker, then a space before the code starts.
    const float gutter = static_cast<float>(old_width + new_width + 3) * advance
                       + style.FramePadding.x * 2.0F;

    const float pad_x   = style.FramePadding.x;
    const float content = gutter + static_cast<float>(widest + 1) * advance + pad_x;
    const float avail   = ImGui::GetContentRegionAvail().x;
    const bool  overflow = content > avail;
    const float body_h  = static_cast<float>(shown) * line_h + style.FramePadding.y * 2.0F
                        + (overflow ? style.ScrollbarSize : 0.0F);

    // --- the header --------------------------------------------------------
    //
    // A strip of its own above the body, so the block says what it is before it
    // is read. Everything in it is a fact about the block: the language, the
    // file, how much moved, and the one action anybody wants.
    const float head_h = ImGui::GetFrameHeight();
    const ImVec2 head_at = ImGui::GetCursorScreenPos();
    ImDrawList*  draw    = ImGui::GetWindowDrawList();
    draw->AddRectFilled(head_at, ImVec2(head_at.x + avail, head_at.y + head_h),
                        theme::kPanel, style.ChildRounding,
                        ImDrawFlags_RoundCornersTop);
    draw->AddLine(ImVec2(head_at.x, head_at.y + head_h - 1.0F),
                  ImVec2(head_at.x + avail, head_at.y + head_h - 1.0F),
                  theme::kPanelEdge);

    {
        std::string left;
        if (const std::string_view name = diff ? std::string_view("diff")
                                               : syntax::lang_name(lang);
            !name.empty()) {
            left = std::string(name);
        }
        if (!path.empty()) {
            left += left.empty() ? path : "  \xC2\xB7  " + path;
        }
        if (left.empty()) {
            left = "text";
        }

        std::string right;
        if (diff) {
            right = "+" + std::to_string(added) + "  -" + std::to_string(removed);
        } else {
            right = std::to_string(rows.size())
                  + (rows.size() == 1 ? " line" : " lines");
        }

        const float text_y = head_at.y + (head_h - line_h) * 0.5F;
        draw->AddText(ImVec2(head_at.x + pad_x * 1.4F, text_y), theme::kTextDim,
                      left.c_str());
        const float right_w = ImGui::CalcTextSize(right.c_str()).x;
        const float copy_w  = head_h;
        draw->AddText(ImVec2(head_at.x + avail - right_w - copy_w - pad_x, text_y),
                      theme::kTextFaint, right.c_str());

        // The copy button, at the right end where every other program keeps it.
        ImGui::SetCursorScreenPos(ImVec2(head_at.x + avail - copy_w, head_at.y));
        ImGui::InvisibleButton("##copy", ImVec2(copy_w, head_h));
        const bool hot = ImGui::IsItemHovered();
        theme::draw_copy(draw,
                         ImVec2(head_at.x + avail - copy_w * 0.5F, head_at.y + head_h * 0.5F),
                         line_h * 0.62F, hot ? theme::kFlameBright : theme::kTextFaint);
        if (hot) {
            ImGui::SetTooltip("Copy");
        }
        if (ImGui::IsItemActivated()) {
            ImGui::SetClipboardText(std::string(text).c_str());
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(head_at.x, head_at.y + head_h));

    // --- the body ----------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::to_vec(theme::kRaised));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, style.FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##code", ImVec2(avail, body_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const float row_w = std::max(content, ImGui::GetContentRegionAvail().x);
    syntax::Carry carry;
    for (std::size_t i = 0; i < shown; ++i) {
        const CodeRow& row = rows[i];
        // Every line is lexed, drawn or not: a block comment opened on a line
        // that got folded away still has to be closed on one that did not.
        const std::vector<syntax::Piece> pieces = syntax::highlight(row.text, lang, carry);

        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList*  body = ImGui::GetWindowDrawList();

        // The wash goes the full content width rather than the visible width,
        // so a long added line stays green when the block is scrolled sideways.
        if (row.marker == '+') {
            body->AddRectFilled(at, ImVec2(at.x + row_w, at.y + line_h), theme::kAddedWash);
        } else if (row.marker == '-') {
            body->AddRectFilled(at, ImVec2(at.x + row_w, at.y + line_h), theme::kRemovedWash);
        }

        float x = at.x + pad_x;
        if (diff) {
            const std::string old_text = row.old_no > 0 ? digits_of(row.old_no) : std::string();
            const std::string new_text = row.new_no > 0 ? digits_of(row.new_no) : std::string();
            // Right-aligned in their columns, which is the only way a column of
            // numbers reads as a column.
            body->AddText(ImVec2(x + static_cast<float>(old_width - static_cast<int>(old_text.size())) * advance,
                                 at.y), theme::kTextFaint, old_text.c_str());
            x += static_cast<float>(old_width + 1) * advance;
            body->AddText(ImVec2(x + static_cast<float>(new_width - static_cast<int>(new_text.size())) * advance,
                                 at.y), theme::kTextFaint, new_text.c_str());
            x += static_cast<float>(new_width + 1) * advance;

            if (row.marker == '+' || row.marker == '-') {
                const char mark[2] = {row.marker, '\0'};
                body->AddText(ImVec2(x, at.y),
                              row.marker == '+' ? theme::kAdded : theme::kRemoved, mark);
            }
            x += advance;
        } else {
            const std::string number = digits_of(row.new_no);
            body->AddText(ImVec2(x + static_cast<float>(new_width - static_cast<int>(number.size())) * advance,
                                 at.y), theme::kTextFaint, number.c_str());
            x = at.x + gutter;
        }

        if (row.marker == '@') {
            // A hunk or file header: one colour, and the orange that means
            // "this is the structure" everywhere else in the interface.
            body->AddText(ImVec2(x, at.y), theme::kFlame, row.text.c_str());
        } else {
            for (const syntax::Piece& piece : pieces) {
                body->AddText(ImVec2(x, at.y), colour_of(piece.token),
                              piece.text.c_str(),
                              piece.text.c_str() + piece.text.size());
                x += static_cast<float>(columns(piece.text)) * advance;
            }
        }
        ImGui::Dummy(ImVec2(row_w, line_h));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // --- the fold ----------------------------------------------------------
    if (foldable) {
        const ImVec2 foot_at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##fold", ImVec2(avail, head_h));
        const bool hot = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated()) {
            storage->SetBool(key, !open);
        }
        ImDrawList* foot = ImGui::GetWindowDrawList();
        foot->AddRectFilled(foot_at, ImVec2(foot_at.x + avail, foot_at.y + head_h),
                            theme::kPanel, style.ChildRounding,
                            ImDrawFlags_RoundCornersBottom);
        const ImU32 ink = hot ? theme::kFlameBright : theme::kTextDim;
        theme::draw_chevron(foot, ImVec2(foot_at.x + pad_x * 2.2F, foot_at.y + head_h * 0.5F),
                            line_h * 0.7F, ink, open);
        const std::string label = open
            ? "fold"
            : "show all " + std::to_string(rows.size()) + " lines";
        foot->AddText(ImVec2(foot_at.x + pad_x * 3.6F, foot_at.y + (head_h - line_h) * 0.5F),
                      ink, label.c_str());
    }

    ImGui::PopFont();
    ImGui::PopID();
}

void draw_markdown(std::string_view text, ImU32 base) {
    const std::vector<markdown::Block> blocks = markdown::parse(text);

    // Fenced code arrives a line at a time. Gathering the run back up means one
    // block per fence rather than one per line, which is the difference between
    // a code block and a stack of them.
    //
    // `pending_lang` is the word after the opening ``` -- the only place a model
    // ever says what language it is writing, and the difference between coloured
    // code and grey code.
    std::string pending_code;
    std::string pending_lang;
    int         block_seq = 0;
    const auto flush_code = [&pending_code, &pending_lang, &block_seq]() {
        if (!pending_code.empty()) {
            draw_code_block(pending_code, pending_lang, {}, block_seq++);
            pending_code.clear();
            pending_lang.clear();
        }
    };

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const markdown::Block& block = blocks[i];
        if (block.kind != markdown::BlockKind::Code) {
            flush_code();
        }

        switch (block.kind) {
            case markdown::BlockKind::Code:
                if (pending_code.empty()) {
                    // Every line of one fence carries the same marker, so this
                    // is the fence's language and not the line's.
                    pending_lang = block.marker;
                }
                if (!pending_code.empty()) {
                    pending_code += '\n';
                }
                pending_code += joined(block.spans);
                continue;

            case markdown::BlockKind::Blank:
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35F));
                continue;

            case markdown::BlockKind::Rule:
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 4));
                continue;

            case markdown::BlockKind::Heading: {
                ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35F));
                // Only the top two levels get the larger face. Below that the
                // step in size stops meaning anything and just makes a reply
                // look like a ransom note.
                ImGui::PushFont(block.level <= 2 ? theme::heading() : theme::bold());
                ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kFlame));
                ImGui::TextUnformatted(joined(block.spans).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                continue;
            }

            case markdown::BlockKind::Quote: {
                // A bar down the left rather than an indent alone: a quote that
                // is only indented reads as a code block at a glance.
                const ImVec2 at = ImGui::GetCursorScreenPos();
                draw_spans(block.spans, theme::kTextDim, 14.0F);
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(at.x, at.y), ImVec2(at.x + 2.0F, after.y - 4.0F), theme::kFlame);
                continue;
            }

            case markdown::BlockKind::Bullet:
            case markdown::BlockKind::Numbered: {
                const float indent = 16.0F * static_cast<float>(block.level + 1);
                const std::string marker = block.kind == markdown::BlockKind::Bullet
                    ? std::string("\xE2\x80\xA2")  // •
                    : block.marker;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::to_vec(theme::kFlame));
                ImGui::TextUnformatted(marker.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0F, ImGui::CalcTextSize(" ").x);
                draw_spans(block.spans, base, 0.0F);
                continue;
            }

            case markdown::BlockKind::TableRule:
                continue;  // the ruled line under a header carries no text

            case markdown::BlockKind::TableRow: {
                // Gather the whole run of rows, so one table is one ImGui table
                // rather than one per line.
                std::vector<const markdown::Block*> rows;
                std::size_t j = i;
                for (; j < blocks.size(); ++j) {
                    if (blocks[j].kind == markdown::BlockKind::TableRow) {
                        rows.push_back(&blocks[j]);
                    } else if (blocks[j].kind != markdown::BlockKind::TableRule) {
                        break;
                    }
                }
                i = j - 1;

                std::size_t columns = 0;
                for (const markdown::Block* row : rows) {
                    columns = std::max(columns, row->cells.size());
                }
                if (columns == 0) {
                    continue;
                }
                if (ImGui::BeginTable("##md", static_cast<int>(columns),
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                          | ImGuiTableFlags_SizingStretchProp)) {
                    for (std::size_t r = 0; r < rows.size(); ++r) {
                        ImGui::TableNextRow();
                        for (std::size_t c = 0; c < columns; ++c) {
                            ImGui::TableSetColumnIndex(static_cast<int>(c));
                            if (c >= rows[r]->cells.size()) {
                                continue;
                            }
                            // The first row is the header, which markdown marks
                            // by the ruled line under it rather than in the row
                            // itself.
                            ImGui::PushFont(r == 0 ? theme::bold() : theme::body());
                            ImGui::PushStyleColor(
                                ImGuiCol_Text,
                                theme::to_vec(r == 0 ? theme::kFlame : base));
                            ImGui::TextUnformatted(joined(rows[r]->cells[c]).c_str());
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                        }
                    }
                    ImGui::EndTable();
                }
                continue;
            }

            case markdown::BlockKind::Paragraph:
                break;
        }

        draw_spans(block.spans, base, 0.0F);
    }
    flush_code();
}

}  // namespace crucible::gui
