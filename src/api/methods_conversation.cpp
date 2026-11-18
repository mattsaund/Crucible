// SPDX-License-Identifier: MIT
//
// Talking to an expert, and what has been said before.
#include "methods.hpp"

#include "crucible/session/store.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/tools/attachments.hpp"
#include "crucible/util/format.hpp"

#include <algorithm>

namespace crucible::api {
namespace {

// ---------------------------------------------------------------------------
// A turn
// ---------------------------------------------------------------------------

Reply snapshot(const json&, Host& host) {
    if (host.state() == nullptr) {
        return no_engine();
    }
    // Parsed back rather than assembled a second time, so the snapshot a
    // caller asks for and the one the window is pushed cannot differ.
    json out = json::parse(Surface(host).snapshot(), nullptr, false);
    return out.is_object() ? good(std::move(out)) : bad("the state could not be read");
}

/// `attachments`, as a caller sends them: a list of
/// `{"path": ..., "image": {"mime": ..., "data": ...}}`, where `image` is a
/// picture the caller has already read (and shrunk) and is optional.
///
/// Only paths that are there are taken; what each one is, is decided here
/// from its name rather than taken on the caller's word.
bool read_attachments(const json& params, std::vector<attach::Attachment>& out, std::string& error) {
    const auto list = params.find("attachments");
    if (list == params.end() || list->is_null()) {
        return true;
    }
    if (!list->is_array()) {
        error = "attachments is a list";
        return false;
    }
    for (const json& entry : *list) {
        const std::string path = entry.is_object() ? entry.value("path", std::string{}) : std::string{};
        if (path.empty()) {
            error = "an attachment needs a path";
            return false;
        }
        attach::Attachment one;
        one.path  = path;
        one.name  = std::filesystem::path(path).filename().string();
        one.kind  = attach::kind_of(path);
        one.label = attach::label_for(path);
        if (one.kind == attach::Kind::Missing) {
            error = one.name + " is not there any more";
            return false;
        }
        if (const auto image = entry.find("image");
            image != entry.end() && image->is_object() && one.kind == attach::Kind::Image) {
            one.image.mime = image->value("mime", std::string{});
            one.image.data = image->value("data", std::string{});
        }
        out.push_back(std::move(one));
    }
    return true;
}

Reply submit(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    const auto prompt = params.value("prompt", std::string{});
    std::vector<attach::Attachment> attachments;
    std::string error;
    if (!read_attachments(params, attachments, error)) {
        return bad(error);
    }
    if (prompt.empty() && attachments.empty()) {
        return bad("submit needs a prompt");
    }
    // Refused while a benchmark runs, before anything else here is done for
    // a turn that is not going to be.
    if (host.engine()->benching()) {
        return bad("benchmark running");
    }
    // No project open is no reason not to answer: the conversation goes in
    // the Scratchpad, which is somewhere for it to be kept and for anything
    // the expert makes to land.
    if (host.project_root().empty()) {
        if (const std::string refused = host.open_scratchpad(); !refused.empty()) {
            return bad(refused);
        }
    }
    // What the box is set to send to, said with every prompt: the engine
    // keeps that expert loaded between prompts. See Engine::choose.
    const auto route = params.find("route");
    if (route != params.end() && route->is_string()) {
        const std::string id = route->get<std::string>();
        host.engine()->choose(id.empty() ? std::nullopt : std::optional<ExpertId>(id), false);
    }
    // An expert named here skips routing, which is what `/physics ...` does.
    // An empty one is not an error: it is the normal case, and it means "let
    // the delegator decide".
    const auto expert = params.value("expert", std::string{});
    host.engine()->submit(prompt, expert.empty() ? std::nullopt : std::optional<ExpertId>(expert),
                          std::move(attachments));
    return good();
}

/// Where the box sends prompts: one expert, or the delegator (empty). With
/// "load", which is the default, that one is loaded now rather than when the
/// first prompt to it is sent. See Engine::choose.
Reply route_pick(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    const auto expert = params.find("expert");
    const auto load   = params.find("load");
    const std::string id = expert != params.end() && expert->is_string() ? expert->get<std::string>() : "";
    host.engine()->choose(id.empty() ? std::nullopt : std::optional<ExpertId>(id),
                          load == params.end() || !load->is_boolean() || load->get<bool>());
    return good();
}

/// What each of `paths` is, for its tile: the kind, the badge, the size and
/// the start of its text. A lookup, because reading a long PDF to find its
/// first lines takes a moment the window should not spend waiting.
Reply attach_inspect(const json& params, const Scene&) {
    const auto paths = params.find("paths");
    if (paths == params.end() || !paths->is_array()) {
        return bad("attach.inspect needs paths");
    }
    json out = json::array();
    for (const json& path : *paths) {
        if (!path.is_string()) {
            continue;
        }
        const attach::Info info = attach::inspect(path.get<std::string>());
        json one{{"path", info.path}, {"name", info.name}, {"kind", attach::kind_name(info.kind)},
                 {"label", info.label}, {"bytes", info.bytes}};
        if (info.kind == attach::Kind::Folder)  { one["files"] = info.files; }
        if (!info.preview.empty())              { one["preview"] = info.preview; }
        if (!info.mime.empty())                 { one["mime"] = info.mime; }
        if (!info.error.empty())                { one["error"] = info.error; }
        if (info.kind == attach::Kind::Missing) { one["kind"] = "missing"; }
        out.push_back(std::move(one));
    }
    return good(json{{"items", std::move(out)}});
}

/// Keep a piece of a file dropped on the window, for a webview that hands the
/// page a dropped file's contents and not its path. See attach::keep_dropped.
/// A lookup: it writes only into Crucible's own folder, never the session's.
Reply attach_store(const json& params, const Scene&) {
    std::string bytes;
    if (!format::from_base64(params.value("data", std::string{}), bytes)) {
        return bad("attach.store's data is base64");
    }
    const attach::Kept kept = attach::keep_dropped(
        paths::dropped_dir(), params.value("batch", std::string{}), params.value("path", std::string{}),
        bytes, params.value("append", false), params.value("folder", false));
    if (!kept.error.empty()) {
        return bad(kept.error);
    }
    return good(json{{"path", kept.path}, {"top", kept.top}});
}

/// A picture's bytes, for the window to draw its thumbnail from and to shrink
/// before sending. The window cannot read a file on its own.
Reply attach_image(const json& params, const Scene&) {
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        return bad("attach.image needs a path");
    }
    // Forty megabytes covers any photograph a camera takes; past that it is
    // not a picture anyone means to send to a model.
    const attach::Image image = attach::picture(path, 40ULL << 20);
    if (image.data.empty()) {
        return bad(std::filesystem::path(path).filename().string() + " is not a picture that can be sent");
    }
    return good(json{{"mime", image.mime}, {"data", image.data}});
}

Reply cancel(const json&, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    host.engine()->cancel();
    return good();
}

Reply release(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    if (params.value("all", false)) {
        host.engine()->release_all();
    } else {
        host.engine()->release_expert();
    }
    return good();
}

/// Ask it again, or take it away. Both are refused while something is
/// running, by the session rather than here: the buttons are not offered
/// then either, and this is the belt to that brace.
Reply change_turn(const json& params, Host& host, bool retry) {
    if (host.state() == nullptr) {
        return no_engine();
    }
    if (!params.contains("index") || !params["index"].is_number_unsigned()) {
        return bad("which turn?");
    }
    const auto index = params["index"].get<std::size_t>();
    if (index >= host.state()->snapshot().turns.size()) {
        return bad("there is no turn there");
    }
    if (!(retry ? host.retry_turn(index) : host.delete_turn(index))) {
        return bad("the transcript cannot be changed while something is running");
    }
    // Deleting changes the transcript and nothing else: no token arrives to
    // wake the screen afterwards, so the turn would stay visible until
    // something unrelated happened. Retrying wakes on its own, and waking
    // twice costs one redraw.
    host.wake();
    return good(json{{"index", index}});
}

Reply turn_retry(const json& params, Host& host)  { return change_turn(params, host, true); }
Reply turn_delete(const json& params, Host& host) { return change_turn(params, host, false); }

Reply edit_approve(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    host.engine()->approve_edit(params.value("approved", false));
    return good();
}

// ---------------------------------------------------------------------------
// What has been done before
// ---------------------------------------------------------------------------

Reply history(const json&, const Scene& scene) {
    if (scene.project.empty()) {
        return bad("no project is open, and history is per project");
    }
    const Project project = Project::at(scene.project);

    // Conversations only. A project's folder may still hold the records of
    // builds from before every prompt was worked in one loop; nothing lists
    // them, and they are left where they are.
    json sessions = json::array();
    for (const SessionSummary& one : SessionStore(project).list()) {
        sessions.push_back(json{{"id", one.id}, {"title", one.name.empty() ? one.title : one.name},
                                {"when", one.when()}, {"turns", one.turns}});
    }
    return good(json{{"sessions", sessions}});
}

/// Open a stored conversation -- in another project, when `project` names
/// one: that project is opened first, which is what makes the conversation's
/// files and history the ones the expert sees.
Reply history_open(const json& params, Host& host) {
    const auto what = params.value("id", std::string{});
    if (what.empty()) {
        return bad("history.open needs an id");
    }
    const auto where = params.value("project", std::string{});
    if (!where.empty() && Project::at(where).root != Project::at(host.project_root()).root) {
        if (const std::string error = host.open_project(where); !error.empty()) {
            return bad(error);
        }
        if (host.project_root().empty() || Project::at(host.project_root()).root != Project::at(where).root) {
            // Waiting on the folder question, which is on screen now.
            return bad("trust " + Project::at(where).name + " first, then open the conversation again");
        }
    }
    const std::string error = host.open_session(what);
    return error.empty() ? good() : bad(error);
}

/// Delete a conversation for good: `id` in the project at `project`.
Reply session_delete(const json& params, Host& host) {
    const auto what  = params.value("id", std::string{});
    const auto where = params.value("project", std::string{});
    if (what.empty() || where.empty()) {
        return bad("session.delete needs an id and a project");
    }
    const std::string error = host.delete_session(what, where);
    return error.empty() ? good() : bad(error);
}

Reply session_new(const json&, Host& host) {
    const std::string error = host.new_session();
    return error.empty() ? good() : bad(error);
}

/// The right-hand panel: the conversations had lately, across every project
/// and every chat's scratch folder, and the projects -- the folders somebody
/// opened, not the scratch ones. Newest first, and a lookup: it reads files.
Reply recents(const json& params, const Scene& scene) {
    const std::size_t want = std::clamp<std::size_t>(params.value("chats", std::size_t{24}), 1, 100);
    const std::filesystem::path current =
        scene.project.empty() ? std::filesystem::path() : Project::at(scene.project).root;

    json listed = json::array();
    for (const Project& project : recent_projects(12)) {
        if (is_scratch(project.root)) {
            continue;
        }
        listed.push_back(json{{"root", project.root.string()},
                              {"name", project.name},
                              {"display", format::short_path(project.root)},
                              {"current", project.root == current}});
    }

    json chats = json::array();
    for (const SessionSummary& chat : recent_chats(want)) {
        const bool scratch = is_scratch(chat.project);
        chats.push_back(json{{"id", chat.id},
                             {"title", chat.name.empty() ? chat.title : chat.name},
                             {"named", !chat.name.empty()},
                             {"when", chat.when()},
                             {"turns", chat.turns},
                             {"project", chat.project.string()},
                             {"project_name", scratch ? std::string("Scratchpad")
                                                      : chat.project.filename().string()},
                             {"scratch", scratch}});
    }
    return good(json{{"chats", std::move(chats)}, {"projects", std::move(listed)}});
}

}  // namespace

void conversation_methods(std::vector<Method>& table) {
    table.push_back({"snapshot",     nullptr, snapshot});
    table.push_back({"submit",       nullptr, submit});
    table.push_back({"route.pick",   nullptr, route_pick});
    table.push_back({"attach.inspect", attach_inspect, nullptr});
    table.push_back({"attach.image", attach_image, nullptr});
    table.push_back({"attach.store", attach_store, nullptr});
    table.push_back({"cancel",       nullptr, cancel});
    table.push_back({"release",      nullptr, release});
    table.push_back({"turn.retry",   nullptr, turn_retry});
    table.push_back({"turn.delete",  nullptr, turn_delete});
    table.push_back({"edit.approve", nullptr, edit_approve});
    table.push_back({"history",      history, nullptr});
    table.push_back({"history.open", nullptr, history_open});
    table.push_back({"session.new",  nullptr, session_new});
    table.push_back({"session.delete", nullptr, session_delete});
    table.push_back({"recents",      recents, nullptr});
}

}  // namespace crucible::api
