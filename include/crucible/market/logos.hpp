// SPDX-License-Identifier: MIT
//
// The logo beside each model in the Models tab.
//
// LM Studio shows a model with the logo of whoever made it: Qwen's beside a
// GGUF of Qwen3 that unsloth or bartowski packed, Google's beside Gemma.
// Hugging Face says who that is through a repository's card, which names the
// model it was made from, and it keeps a picture for every organization and
// user. So the logo is the picture of the base model's owner, or of the
// repository's own owner when its card names no base model.
//
// A card can name a picture of its own too, its thumbnail, and a repository
// whose card does is shown with that: what its author chose to show it by.
// It is taken from the hub alone, a file of the repository or an address of
// the hub's, so that a list of results tells no other server who looked at
// it. One that cannot be had, or is no small picture, leaves the owner's.
//
// Crucible fetches the pictures, not the page: one for each owner and one for
// each picture a card names, kept in data_dir()/logos. Copies of a model
// most often carry its card, picture and all, and share the one kept. A
// search that shows the same dozen logos again asks nothing, and a logo once
// seen is there without a network. Only pictures are taken, and only small
// ones. What reaches the hub is an owner's name, or the name of a picture a
// card gave, while the Models tab is searching it anyway.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::market {

struct Listing;

/// A logo: whose it is, and the picture when there is one.
struct Logo {
    std::string owner;   ///< "Qwen", or a repository's id for a picture of its own, which
                         ///< is kept under its address; empty when a repository's id names nobody
    std::string mime;    ///< "image/png", "image/jpeg", "image/gif" or "image/webp"; empty for none
    std::string bytes;   ///< the picture
};

/// Whose logo goes beside `repo`: the owner of `base_model`, the model its
/// card says it was made from, and otherwise the repository's own owner.
/// Empty when neither is a name the hub could have.
std::string logo_owner(std::string_view repo, std::string_view base_model);

/// Where the picture `thumbnail` that repository `repo`'s card names is
/// fetched from: a file of the repository at the address the hub serves it
/// at, or an address of the hub's own as it is. Empty for anything else, an
/// address of any other server included.
std::string thumbnail_url(std::string_view repo, std::string_view thumbnail);

/// What `listing`'s logo is known by: its own id, when its card names a
/// picture the hub serves, and otherwise its owner's, as logo_owner() says.
/// Repositories with the same key show the same logo.
std::string logo_key(const Listing& listing);

/// A picture's type, read from its first bytes: one of the four every
/// webview draws, or empty. What a server calls a file is not believed.
std::string_view picture_type(std::string_view bytes);

/// Where the hub answers with an organization's picture, or a user's, by
/// sending the asker on to it.
std::string avatar_url(std::string_view owner, bool user);

/// data_dir()/logos, where they are kept.
std::filesystem::path logos_dir();

/// What `folder` keeps for `owner`, an owner's name or the address of a
/// picture a card names: true with the picture, or with an empty `mime` for
/// one known to have none. `fresh` is false once it is old enough to be asked
/// for again: a month for a picture, a week for none.
bool kept_logo(const std::filesystem::path& folder, std::string_view owner, Logo& out, bool& fresh);

/// Keep `logo` in `folder`, as fetched now. Past a few megabytes the oldest
/// go, so the folder never grows past that.
void keep_logo(const std::filesystem::path& folder, const Logo& logo);

/// Note whose logo each repository of a search is, and the address of a
/// picture of its own, in logos_dir(), so the logo can be asked for by a
/// repository's id alone without asking the hub what the search already said.
void remember_logos(const std::vector<Listing>& listings);

/// Whose logo logos_dir() says `repo` has, or empty when nothing has said.
std::string remembered_owner(std::string_view repo);

/// The address of the picture of its own logos_dir() says `repo` has, or
/// empty when it has none or nothing has said.
std::string remembered_thumbnail(std::string_view repo);

/// The logo beside repository `repo`, "unsloth/Qwen3-8B-GGUF": the picture
/// of its own its card names, or else its owner's, each kept or fetched once
/// and kept. The hub is asked only for what is not kept, or kept long enough
/// to have changed, and when it cannot be reached an old logo is better than
/// none. False, with `error`, only when there is no logo to give and no
/// telling whether there is one.
bool logo(std::string_view repo, Logo& out, std::string& error);

}  // namespace crucible::market
