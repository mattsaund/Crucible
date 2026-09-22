// SPDX-License-Identifier: MIT
//
// The window and the frame: opening it, one pass of drawing, and the actions
// the panels call back into.
//
// The panels themselves are in gui/panels/, one file each, and the helpers
// they are written with are in gui/widgets.hpp. What stays here is everything
// that owns state rather than draws it -- which is why an action like
// open_project or update_config lives in this file and nothing in panels/
// touches config_ directly.
#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <thread>
#include <utility>

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include "crucible/config/paths.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace crucible::gui {

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

App::App(Config config, std::vector<std::string> warnings, bool skip_trust)
    : config_(std::move(config)),
      trust_(paths::trust_file()),
      skip_trust_(skip_trust) {
    for (std::string& warning : warnings) {
        notices_.push_back(std::move(warning));
    }

    state_.configure_seats(config_);

    engine_ = std::make_unique<Engine>(config_, state_, [this] {
        // The engine runs on its own thread and the window may be parked in
        // glfwWaitEvents. Without this the screen would not repaint until the
        // mouse moved, which during a model load is most of a minute.
        glfwPostEmptyEvent();
    });
    // No project, so no root and no history folder. The root is the permission
    // an expert acts under, and there is nothing to act on yet: a WRITE in this
    // state is refused, which is the right answer to "before you opened one".
    engine_->set_project({}, {});

    // The browser has to start somewhere, and the most recent project is the
    // best guess at where this person keeps their work -- offered as a starting
    // directory, not opened.
    const std::vector<Project> recent = recent_projects(1);
    browse_      = recent.empty() ? paths::expand_user("~") : recent.front().root;
    browse_text_ = browse_.string();
    refresh_models();
}

std::filesystem::path App::project_root() const {
    return store_ ? store_->project().root : std::filesystem::path{};
}

std::filesystem::path App::project_dir() const {
    return store_ ? store_->project().dir : std::filesystem::path{};
}

App::~App() {
    if (engine_) {
        engine_->stop();
    }
}

void App::say(std::string message) {
    notices_.push_back(std::move(message));
    // Only the last few. This is a status channel, not a log; the log is on
    // disk and the transcript is above it.
    if (notices_.size() > 6) {
        notices_.erase(notices_.begin());
    }
}

void App::refresh_models() {
    models_      = scan_models(config_.resolved_models_dir());
    lab_made_    = lab::finished_models();
    // Asked here rather than every frame: both are the same question -- what is
    // on this machine that a prompt could actually be run on -- and both change
    // only when the user goes and changes them.
    any_runtime_ = RuntimeRegistry::any_installed();
}

void App::update_config(const std::function<void(Config&)>& change) {
    change(config_);
    config_.resolve_models();
    state_.configure_seats(config_);
    if (!save_config(config_)) {
        say("could not write " + paths::config_file().string());
    }
    engine_->apply_config(config_);
}

void App::persist_session() {
    // Nothing to write a history into. A conversation cannot have happened
    // without a project -- the composer is closed until one is open -- but this
    // is called from the frame loop and from open_project, so it says so rather
    // than trusting that.
    if (!store_) {
        return;
    }
    const Snapshot snapshot = state_.snapshot();
    std::size_t finished = 0;
    for (const Turn& turn : snapshot.turns) {
        finished += turn.streaming ? 0 : 1;
    }
    if (finished == persisted_turns_) {
        return;
    }
    std::string error;
    if (store_->save(snapshot.turns, snapshot.session_usage, error)) {
        persisted_turns_ = finished;
    }
}

void App::absorb_written_examples() {
    const std::vector<std::pair<ExpertId, std::vector<std::string>>> written =
        engine_->take_written_examples();
    if (written.empty()) {
        return;
    }
    update_config([&written](Config& config) {
        for (const auto& [id, examples] : written) {
            if (const std::optional<std::size_t> seat = config.roster.find(id)) {
                Expert expert = config.roster.at(*seat);
                expert.examples = examples;
                config.roster.update(id, expert);
            }
        }
    });
    for (const auto& [id, examples] : written) {
        say(expert_label(config_.roster, id) + ": the delegator wrote "
            + std::to_string(examples.size()) + " example questions to route on");
    }
}

void App::open_project(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        project_error_ = root.string() + " is not a directory";
        return;
    }
    if (engine_->cooking()) {
        // A cook is about the directory it started in, and its journal is keyed
        // to it. Moving the ground under it would produce a record of work done
        // somewhere it was not.
        say("finish or stop the cook before opening another project");
        return;
    }

    // Asked once per directory, and remembered. `--no-trust` is the way past
    // it for a scripted run, where there is nobody to answer a modal.
    if (!skip_trust_ && !trust_.is_trusted(root)) {
        pending_trust_ = root;
        return;
    }

    const Project project = Project::at(root);
    persist_session();

    store_ = std::make_unique<SessionStore>(project);
    engine_->set_project(project.root, project.dir);
    engine_->reset_history();
    state_.clear_turns();
    state_.clear_notices();
    state_.set_cook(nullptr);
    state_.set_project_usage(store_->project_usage());
    persisted_turns_ = 0;
    expanded_.clear();
    notices_.clear();
    follow_      = true;
    browse_      = project.root;
    browse_text_ = browse_.string();

    remember_project(project.root);
    project_error_.clear();
    say("opened " + project.root.string());
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::submit_prompt() {
    const std::string text = format::trim(prompt_);
    if (text.empty()) {
        return;
    }
    prompt_.clear();
    follow_ = true;

    // A cook waiting on a question takes the next thing typed as its answer.
    // The screen is showing a question; nothing else would be a reasonable
    // reading of a line typed under it.
    if (const std::shared_ptr<const Cook> cook = state_.cook();
        cook && cook->state == CookState::Asking) {
        engine_->answer_cook(text);
        return;
    }
    engine_->submit(text);
}

void App::stop_work() {
    engine_->cancel();
    say("stopping");
}

void App::retry_turn(std::size_t index) {
    if (engine_->is_busy()) {
        return;   // the buttons are not offered then; this is the belt to that brace
    }
    const Snapshot snapshot = state_.snapshot();
    if (index >= snapshot.turns.size()) {
        return;
    }
    const std::string prompt = snapshot.turns[index].prompt;
    state_.truncate_turns(index);
    rebuild_history();
    expanded_.clear();
    follow_ = true;
    engine_->submit(prompt);
}

void App::delete_turn(std::size_t index) {
    if (engine_->is_busy()) {
        return;
    }
    state_.remove_turn(index);
    rebuild_history();
    persist_session();
}

void App::rebuild_history() {
    const Snapshot snapshot = state_.snapshot();
    std::vector<ChatMessage> history;
    for (const Turn& turn : snapshot.turns) {
        // The same rule the session resume uses: only exchanges that actually
        // produced an answer go back to the expert. A failed or canceled turn
        // in the context teaches it that not answering is a thing that happens
        // here.
        if (!turn.failed && !turn.canceled && !turn.reply.empty()) {
            history.push_back({"user", turn.prompt});
            history.push_back({"assistant", turn.reply});
        }
    }
    engine_->restore_history(std::move(history));

    // The session file is written from the turn count, so a shorter transcript
    // has to reset it or the next save believes it has already stored turns
    // that are no longer there.
    persisted_turns_ = 0;
}

void App::begin_cook() {
    // A cook is an hour of work on a directory, and its journal is keyed to
    // one. Without a project there is nothing for it to be about.
    if (!project_open()) {
        say("open a project first -- a cook works on a folder");
        return;
    }
    const std::string goal = format::trim(cook_goal_);
    if (goal.empty()) {
        say("a cook needs a goal");
        return;
    }
    cook_goal_.clear();
    follow_ = true;
    view_   = View::Cook;
    expanded_.clear();
    // No budget. A cook runs until it finishes or until one of the two Stop
    // buttons is pressed; the minutes slider that used to set this is gone.
    engine_->start_cook(goal, 0, project_root());
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

float App::composer_wanted_height(const Snapshot& snapshot) {
    if (view_ != View::Chat && view_ != View::Cook) {
        return 0.0F;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    // The child's own padding, plus the border it draws inside it. Leaving the
    // border out is two pixels of overflow, which the child answers with a
    // scrollbar down the side of the box you type in.
    const float pad   = style.WindowPadding.y * 2.0F + style.ChildBorderSize * 2.0F;
    const float frame = ImGui::GetFrameHeight();

    // The width the box will actually be given, so the wrapping measured here
    // is the wrapping that gets drawn. The reading column, not the window: the
    // composer is capped with the transcript above it.
    const float column = reading_column(ImGui::GetContentRegionAvail().x);
    const float inner  = std::max(column - style.WindowPadding.x * 2.0F, em(8.0F));
    const float beside = std::max(inner - em(5.0F) - style.ItemSpacing.x, em(6.0F));

    const std::shared_ptr<const Cook> cook = snapshot.cook;
    const bool asking = cook && cook->state == CookState::Asking;

    // Both composers carry a line under the box -- tokens in and out, and how
    // full the context is -- and each box only gets the height asked for here.
    // Leave that line out and it is drawn past the bottom edge of the child and
    // clipped away, which looks exactly like a readout that was never written.
    const float readout = ImGui::GetTextLineHeightWithSpacing();

    if (view_ == View::Chat || asking) {
        return pad + grow_input_height(prompt_, beside, kComposerLines) + readout;
    }
    if (engine_->cooking()) {
        return pad + frame + readout;   // the two stop buttons, and the count
    }
    // The goal box with the Cook button beside it -- one row, the same shape as
    // the chat bar. It used to be two, with a minutes slider and a checkbox on
    // the second; reserving room for that row after it was removed left an
    // empty strip under the box.
    return pad + grow_input_height(cook_goal_, beside, kComposerLines) + readout;
}

/// The composer's height, kept to something the window can actually spare.
///
/// Two sources, in order. A height the user has dragged the splitter to wins,
/// because they said so. Otherwise it is measured from what has been typed, as
/// it always was.
///
/// Either way it is capped: a box grown to its full height in a short window
/// would leave the pane above it nothing, so the transcript you are typing into
/// would disappear as you typed. A measured box may take half the window; one
/// dragged by hand may take four fifths, since at that point it is a deliberate
/// choice rather than a side effect of a long paste.
float App::composer_height(const Snapshot& snapshot) {
    const float wanted = composer_wanted_height(snapshot);
    if (wanted <= 0.0F) {
        return 0.0F;   // this view has no composer
    }
    const float room = ImGui::GetContentRegionAvail().y;
    if (room <= 0.0F) {
        return wanted;
    }
    if (composer_height_ > 0.0F) {
        const float floor_at = ImGui::GetFrameHeight()
                             + ImGui::GetStyle().WindowPadding.y * 2.0F
                             + ImGui::GetStyle().ChildBorderSize * 2.0F;
        return std::clamp(composer_height_, floor_at, room * 0.8F);
    }
    return std::min(wanted, room * 0.5F);
}

float App::composer_input_height() const {
    return composer_input_height_;
}

/// The grab bar between the transcript and the composer.
///
/// The sidebar's splitter turned sideways, and deliberately the same thing to
/// use: hover it, hold it, move the mouse. Dragging up makes the box taller,
/// which is the direction that matches the edge being moved.
void App::draw_composer_splitter() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::to_vec(theme::kFlame));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::to_vec(theme::kFlameBright));
    ImGui::Button("##composer-splitter", ImVec2(-FLT_MIN, em(0.35F)));
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGui::SetItemTooltip("Drag to resize the box you type in");

    if (ImGui::IsItemActive()) {
        // Seeded from whatever the box is right now, so the first pixel of drag
        // moves the edge that is on screen rather than jumping to some
        // remembered height from earlier in the session.
        if (composer_height_ <= 0.0F) {
            composer_height_ = composer_drawn_height_;
        }
        composer_height_ -= ImGui::GetIO().MouseDelta.y;
        // Kept explicit and at least one row tall. Letting it fall through zero
        // would hand the composer back to its measured height mid-drag, which
        // reads as the box snapping away from the mouse.
        composer_height_ = std::max(composer_height_,
                                    ImGui::GetFrameHeight()
                                        + ImGui::GetStyle().WindowPadding.y * 2.0F);
    }
}

void App::open_browse(BrowseFor what, const std::filesystem::path& start) {
    browse_for_ = what;
    browse_     = start;
    if (browse_.empty()) {
        browse_ = what == BrowseFor::ModelsDir ? config_.resolved_models_dir()
                                               : project_root();
    }
    // A path that has gone missing would leave the list empty with nothing to
    // click, so the browser falls back to somewhere that certainly exists.
    std::error_code ec;
    if (!std::filesystem::is_directory(browse_, ec)) {
        browse_ = paths::expand_user("~");
    }
    browse_text_       = browse_.string();
    project_error_.clear();
    browse_modal_open_ = true;
}

void App::begin_update_check() {
    // The cache first, and always: it is a file read, it is what the last check
    // found, and it is what the window shows until a new answer arrives.
    update::read_cache(update_);

    if (!config_.ui.check_updates || update_checking_ != nullptr) {
        return;
    }
    // On a thread, like every other errand that leaves the machine. A version
    // check that made the window wait for a network round trip at startup would
    // be a worse bug than the one it is trying to tell you about.
    auto check = std::make_shared<UpdateCheck>();
    update_checking_ = check;
    std::thread([check]() {
        update::State state = update::refresh(/*allowed_to_ask=*/true);
        {
            const std::lock_guard<std::mutex> lock(check->mutex);
            check->state = std::move(state);
            check->done  = true;
        }
        glfwPostEmptyEvent();
    }).detach();
}

void App::collect_update_check() {
    if (update_checking_ == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(update_checking_->mutex);
    if (!update_checking_->done) {
        return;
    }
    update_ = update_checking_->state;
    update_checking_.reset();
}

void App::draw() {
    const Snapshot snapshot = state_.snapshot();

    collect_update_check();

    // Negative means "never sized", not "closed". Zero is a width the user can
    // now reach by dragging the splitter to the edge, and testing for <= 0 here
    // would spring the sidebar back open on the very next frame.
    if (sidebar_width_ < 0.0F) {
        sidebar_width_ = em(17.0F);
    }

    // One window filling the viewport. Crucible is an application, not a
    // collection of floating panels, and a desktop app that opens with its own
    // windows scattered over the screen looks like a debug build.
    //
    // No padding on it: the top bar has to run edge to edge, and a window that
    // insets its children by sixteen pixels cannot have a bar at the top -- it
    // has a bar with a gutter around it, which reads as a floating strip rather
    // than as the top of the window.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("crucible", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    draw_topbar();

    // Everything below the bar. A child of its own so the sidebar and the main
    // pane both measure their height against what is left rather than against
    // the window, which is what stops the composer being pushed off the bottom
    // by exactly the height of the bar.
    //
    // It carries the margin the root window used to. The root cannot: a bar
    // that runs edge to edge needs a window with no padding, and the panels
    // under it still need to be held off the glass.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(0.75F), em(0.6F)));
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    draw_sidebar(snapshot);
    draw_splitter();

    ImGui::BeginChild("main", ImVec2(0, 0));
    {
        const float composer = composer_height(snapshot);
        const bool  has_composer = composer > 0.0F;
        // The splitter sits between the two, so the pane has to give up its
        // height as well as the composer's.
        const float bar = has_composer ? em(0.35F) + ImGui::GetStyle().ItemSpacing.y
                                       : 0.0F;
        composer_drawn_height_ = composer;

        ImGui::BeginChild("pane", ImVec2(0, -(composer + bar)),
                          ImGuiChildFlags_Borders);

        // A reading column inside the panel, rather than a narrow panel.
        //
        // The two are not the same picture. Narrowing the panel leaves a strip
        // of bare window on either side of a floating box; narrowing the text
        // inside a panel that still reaches both edges is a page with margins,
        // which is what every book and every document view is. The panel frames
        // the working area either way -- only the measure changes.
        //
        // AutoResizeY because the column has to be as tall as what is in it and
        // the panel behind it is what scrolls.
        //
        // Settings is the exception and stays full width: it is a form, not
        // prose. Its rows are a control and a label side by side, and squeezing
        // those into a measure meant for sentences puts a file path in a box
        // too narrow to read one in.
        if (view_ == View::Settings) {
            draw_settings();
        } else if (view_ == View::Create) {
            // Full width, for the same reason Settings is: a rail of steps with
            // a form beside it is not prose, and a reading measure squeezes
            // both into the middle of the window.
            draw_create(snapshot);
        } else {
            const float room = ImGui::GetContentRegionAvail().x;
            const float col  = reading_column(room);
            if (col < room) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - col) * 0.5F);
            }
            ImGui::BeginChild("column", ImVec2(col, 0), ImGuiChildFlags_AutoResizeY);
            switch (view_) {
                case View::Chat:     draw_chat(snapshot); break;
                case View::Cook:     draw_cook(snapshot); break;
                case View::Create:   break;               // handled above
                case View::History:  draw_history();      break;
                case View::Settings: break;               // handled above
            }
            ImGui::EndChild();
        }
        // Following the bottom, but only while the user is already there.
        // Yanking someone reading back through an hour-old cook to the end
        // every time a token arrives is the single most irritating thing a
        // streaming view can do.
        if (follow_ && (view_ == View::Chat || view_ == View::Cook)
            && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();

        if (has_composer) {
            draw_composer_splitter();
        }

        // The box fills a height the user chose, and sizes itself to the text
        // otherwise. Worked out here, where the composer's final height is
        // known, rather than inside each composer where it is not.
        composer_input_height_ = 0.0F;
        if (has_composer && composer_height_ > 0.0F) {
            const ImGuiStyle& style = ImGui::GetStyle();
            composer_input_height_ =
                std::max(composer - style.WindowPadding.y * 2.0F
                             - style.ChildBorderSize * 2.0F, 0.0F);
        }

        if (view_ == View::Chat) {
            draw_chat_composer(snapshot);
        } else if (view_ == View::Cook) {
            draw_cook_composer(snapshot);
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    draw_new_expert_modal();
    draw_browse_modal();
    draw_trust_modal();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

namespace {

/// Set by GLFW when the window lands on a display with another scale, and acted
/// on at the top of the next frame, where rebuilding the fonts is safe.
bool g_display_changed = false;

}  // namespace

void App::apply_display_scale(bool rebuild_texture) {
    float content_x = 1.0F;
    float content_y = 1.0F;
    glfwGetWindowContentScale(window_, &content_x, &content_y);
    int window_w = 0;
    int window_h = 0;
    int fb_w     = 0;
    int fb_h     = 0;
    glfwGetWindowSize(window_, &window_w, &window_h);
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    const util::DisplayScale scale = util::display_scale(content_x, window_w, fb_w);

    // A move between two displays of the same scale changes nothing, and the
    // rebuild is not free: every face is rasterized again.
    if (rebuild_texture && scale.layout == display_scale_.layout
        && scale.density == display_scale_.density) {
        return;
    }
    display_scale_ = scale;

    if (rebuild_texture) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
    }
    theme::load_fonts(scale.layout, scale.density);
    if (rebuild_texture) {
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }

    // The style from its defaults every time: ScaleAllSizes multiplies, so
    // scaling an already-scaled style would compound with every move.
    ImGui::GetStyle() = ImGuiStyle();
    theme::apply();
    ImGui::GetStyle().ScaleAllSizes(scale.layout);
}

int App::run() {
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "crucible: glfw error %d: %s\n", code, description);
    });
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "crucible: could not open a window. On Linux this "
                             "usually means there is no display, or no OpenGL driver.\n");
        return 1;
    }

    // GL 3.2 core: the oldest thing ImGui's backend is happy with, and old
    // enough that a decade-old integrated chip and a virtual machine both have
    // it. There is nothing here that wants a newer one.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    // The name the desktop identifies this window by, and it has to match
    // StartupWMClass in packaging/crucible.desktop.in. Without it the running
    // window is a different application from the icon that launched it: the
    // dock shows two entries, one of them generic, and the launcher never
    // stops looking like it is still starting up.
    //
    // Guarded because the hints arrived in different GLFW releases and the
    // system's GLFW is preferred over the vendored one where there is one.
#ifdef GLFW_X11_CLASS_NAME
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "crucible");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "crucible");
#endif
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "crucible");
#endif

    // Opened hidden and sized once it exists, because the size depends on the
    // display scale and only a real window can say what that is: whether its
    // units are pixels (Windows, X11) or points over a denser framebuffer (a
    // Mac, Wayland) is not something the monitor alone reports.
    //
    // SCALE_TO_MONITOR has Windows keep the window the same size in points when
    // it is dragged to a display with another scale, where it would otherwise
    // keep its pixels and grow or shrink.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef GLFW_SCALE_TO_MONITOR
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif
    window_ = glfwCreateWindow(1280, 820, "Crucible", nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "crucible: could not create the window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini litter beside the project
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Fonts and style at the scale of the display the window is on: the same
    // physical size on a Retina Mac, a 150% Windows laptop and a 1080p monitor.
    // The window's own scale, not the primary monitor's, which is a different
    // screen as often as not on a laptop with a monitor plugged in.
    apply_display_scale(/*rebuild_texture=*/false);

    // Then the size: decided in points, set in this display's window units.
    {
        int work_x = 0;
        int work_y = 0;
        int work_w = 0;
        int work_h = 0;
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor(); monitor != nullptr) {
            glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);
        }
        const util::WindowSize size =
            util::default_window_size(work_w, work_h, display_scale_.layout);
        glfwSetWindowSize(window_, size.width, size.height);
    }
    glfwShowWindow(window_);

    // Moved to a display with another scale: rebuilt at the top of the next
    // frame rather than here, in the middle of event handling.
    glfwSetWindowContentScaleCallback(window_, [](GLFWwindow*, float, float) {
        g_display_changed = true;
    });

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    engine_->start();

    // Ask, once, whether this is still the newest Crucible. The answer is a
    // mark on the gear and a line under Settings -> About; nothing waits on it.
    begin_update_check();

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        // Waiting rather than spinning. An idle Crucible should cost nothing,
        // and the engine posts an empty event whenever it has something new --
        // the timeout is only there so the cook clock keeps moving.
        glfwWaitEventsTimeout(state_.busy() ? 0.05 : 0.5);

        if (g_display_changed) {
            g_display_changed = false;
            apply_display_scale(/*rebuild_texture=*/true);
        }

        persist_session();
        absorb_written_examples();
        take_runtime_activation();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();

        int fb_width  = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        const ImVec4 ground = theme::to_vec(theme::kInk);
        glClearColor(ground.x, ground.y, ground.z, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    // The engine has a thread that calls back into this object, so it has to be
    // stopped before anything it might touch is torn down.
    engine_->stop();
    persist_session();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

}  // namespace crucible::gui
