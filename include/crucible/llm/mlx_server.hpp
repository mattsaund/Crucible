// SPDX-License-Identifier: MIT
//
// MLX models: the format Apple Silicon runs natively, and the one LM Studio
// and the mlx-community uploads on Huggingface use for it.
//
// An MLX model is not a file but a folder -- a config.json and the weights as
// .safetensors -- and llama.cpp cannot read one. MLX can: mlx-lm, which the
// training environment already has on a Mac because the fine-tuner there is
// MLX, comes with a server that speaks the chat-completions API. So an MLX
// model is answered by that server, started from Crucible's own Python on a
// port nothing else is using, and asked through the same client a provider is
// asked through (remote::Hub::local_server). Nothing leaves the machine: the
// server listens on 127.0.0.1 and nowhere else.
//
// One at a time, like an expert loaded here. Starting one stops the last; a
// GGUF expert taking the seat stops it too.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace crucible::util {
class Subprocess;
}

namespace crucible::mlx {

/// Whether `path` is a folder holding a model MLX can run: a config.json and
/// the weights as one or more .safetensors files.
bool is_model_dir(const std::filesystem::path& path);

/// The size of one: its weights.
std::uintmax_t model_bytes(const std::filesystem::path& dir);

/// Empty when this machine can run an MLX model; why it cannot otherwise.
/// Asks Crucible's Python whether it has mlx-lm, and remembers the answer
/// for a minute -- the models list asks every time it is drawn.
std::string unavailable();

/// The server for one MLX model.
class Server {
public:
    Server();
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// Serve `dir`, and return once it answers. Whatever was served before is
    /// stopped first. `cancel` is asked while it starts; false with `error`
    /// "stopped" when it said to.
    bool serve(const std::filesystem::path& dir, const std::function<bool()>& cancel,
               std::string& error);

    /// Stop serving. Safe to call when nothing is.
    void stop();

    /// True while `dir` is the model being served and its server is up.
    bool serving(const std::filesystem::path& dir) const;

    /// Where to send requests: "http://127.0.0.1:<port>/v1".
    std::string base_url() const;

private:
    /// The last lines the server printed, for saying why it would not start.
    std::string tail() const;

    std::unique_ptr<util::Subprocess> child_;
    std::thread                       drain_;
    std::filesystem::path             dir_;
    int                               port_ = 0;

    mutable std::mutex      tail_mutex_;
    std::deque<std::string> tail_;
};

}  // namespace crucible::mlx
