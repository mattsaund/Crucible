// SPDX-License-Identifier: MIT
//
// The settings screen.
//
// Everything in config.json is editable here, so a working Crucible never
// requires dropping out to a text editor: assign a model to each expert seat
// and to the delegator, move the models directory, and tune sampling.
//
// This type owns the list of rows and decides what each one means. The two
// dialogs and the text editor are separate widgets, so the logic here stays
// about "what is editable" rather than "how a file picker behaves".
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "crucible/config/config.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/ui/settings/directory_browser.hpp"
#include "crucible/ui/settings/line_editor.hpp"
#include "crucible/ui/settings/model_picker.hpp"

namespace crucible::ui {

/// What the settings screen wants the application to do after a key.
enum class SettingsAction {
    None,
    Close,          ///< leave the settings screen
    Apply,          ///< configuration changed: save it and hand it to the engine
    OpenRuntimes,   ///< hand over to the runtimes panel
    OpenGpuOrder,   ///< hand over to the GPU priority panel
    OpenModels,     ///< hand over to the model manager panel
};

class SettingsView {
public:
    explicit SettingsView(Config config);

    /// Re-read the models directory and rebuild the rows. Call on open, and
    /// whenever the models directory changes.
    void refresh();

    const Config& config() const { return config_; }
    void          set_config(Config config);

    bool dirty() const { return dirty_; }
    void mark_saved() { dirty_ = false; }

    ftxui::Element render() const;

    /// Apply one key. `consumed` reports whether the screen used it, so the
    /// application can fall through to its own bindings when it did not.
    SettingsAction handle(const ftxui::Event& event, bool& consumed);

    const std::string& status() const { return status_; }
    void set_status(std::string status) { status_ = std::move(status); }

    /// Called by the GPU priority panel, which owns the arrangement while it
    /// is open and hands back the finished order.
    void set_gpu_priority(std::vector<int> order);

    /// Every model file the config currently points at, for the model manager
    /// panel -- which marks those rows and warns before deleting one.
    std::vector<std::string> models_in_use() const;

    /// Empty every seat that named one of `names`, after the files behind them
    /// were deleted. A seat left pointing at a file that is gone would show as
    /// "(missing)" and fail at load time; clearing it says the same thing at
    /// the moment it becomes true.
    void forget_models(const std::vector<std::string>& names);

private:
    /// What a row edits, which decides what Enter does to it.
    enum class Kind {
        Header,     ///< a section label; never selectable
        ModelRef,   ///< opens the model picker
        Directory,  ///< opens the directory browser
        Text,
        Int,
        Float,
        Bool,       ///< Enter toggles; there is nothing to type
        Enum,       ///< Enter cycles through `options`
        Panel,      ///< Enter hands off to a screen of its own
        Action,     ///< Enter does something once; there is no value to edit
    };

    /// What an Action row does. Named rather than matched on the label, so
    /// rewording a row cannot quietly break it.
    enum class ActionId {
        None,
        ResetModelsDir,
    };

    /// Which screen a Panel row hands off to. Same reasoning as ActionId.
    enum class PanelId {
        None,
        Runtimes,
        GpuOrder,
        Models,
    };

    /// One line of the screen.
    ///
    /// A row points straight at the field it edits inside `config_`, so
    /// committing an edit is an assignment rather than a lookup. Exactly one
    /// pointer is non-null, chosen by `kind`.
    struct Row {
        Kind         kind = Kind::Header;
        std::string  label;
        std::string  help;
        std::string* text    = nullptr;
        int*         integer = nullptr;
        float*       real    = nullptr;
        bool*        flag    = nullptr;
        std::vector<std::string> options;  ///< for Enum rows
        ActionId     action = ActionId::None;  ///< for Action rows
        PanelId      panel  = PanelId::None;   ///< for Panel rows

        /// Why this row cannot be changed right now, or empty when it can.
        ///
        /// A setting whose runtime cannot honor it is worse than a missing
        /// one: it reads as configured, it saves, and nothing happens. So the
        /// row stays visible and selectable -- you have to be able to land on
        /// it to find out why -- and says what would have to change.
        std::string  inert = {};
    };

    void build_rows();

    /// Fill in Row::inert for every setting whose runtime cannot act on it.
    /// Called at the end of build_rows, so it sees the finished list.
    void mark_inert_rows();
    void move_selection(int delta);

    /// The cards behind Config::gpu.priority, named, for the row's value
    /// column -- "RTX 4070 > RTX 3060".
    std::string gpu_priority_summary() const;

    /// Enter on the selected row: toggle, cycle, open a dialog, or start typing.
    void activate_selection();
    void run_action(ActionId action);
    void begin_typing();
    void commit_edit();

    std::string value_of(const Row& row) const;

    ftxui::Element render_row(const Row& row, std::size_t index) const;
    ftxui::Element footer_hint() const;

    Config                 config_;
    std::vector<Row>       rows_;
    std::vector<ModelFile> models_;

    std::size_t selected_ = 0;
    bool        dirty_    = false;
    std::string status_;

    LineEditor       editor_;
    ModelPicker      picker_;
    DirectoryBrowser browser_;
    /// Which row the open dialog is filling.
    std::size_t      dialog_target_ = 0;
};

}  // namespace crucible::ui
