// SPDX-License-Identifier: MIT
#include "crucible/app/setup_trainer.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

#include "crucible/lab/pyenv.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"

namespace crucible {
namespace {

namespace pyenv = lab::pyenv;

/// Overwrite one line, or print a new one when there is no terminal to
/// overwrite on. A log file full of eight hundred progress lines is not a
/// log file, and a terminal that only ever shows the first one is not
/// progress -- so which it is depends on whether anybody is watching.
class Line {
public:
    explicit Line(bool interactive) : interactive_(interactive) {}

    void show(const std::string& text) {
        if (text == last_) {
            return;
        }
        last_ = text;
        std::printf(interactive_ ? "\r\033[2K    %s" : "    %s\n", text.c_str());
        // Flushed either way. Redirected to a file -- which is what an
        // installer's log is -- stdout is block buffered, and four kilobytes
        // of progress lines is most of a download's worth of silence.
        std::fflush(stdout);
    }

    void done() const {
        if (interactive_) {
            std::printf("\r\033[2K");
            std::fflush(stdout);
        }
    }

private:
    bool        interactive_;
    std::string last_;
};

std::string describe(const pyenv::Status& status) {
    if (!status.present) {
        return "not installed";
    }
    if (!status.ready) {
        return "installed but not usable -- " + status.note;
    }
    std::string line = std::string(pyenv::flavor_id(status.flavor));
    if (!status.torch_version.empty()) {
        line += ", torch " + status.torch_version;
    }
    if (!status.python_version.empty()) {
        line += ", Python " + status.python_version;
    }
    return line;
}

}  // namespace

int run_trainer_status() {
    const pyenv::Status status = pyenv::status();
    std::cout << "training environment: " << describe(status) << "\n";
    // The card list, and the cards that do not count. A machine where two of
    // three GPUs work trains perfectly well and says "ready", and the third
    // sitting idle is still worth a line -- it is the difference between a
    // card that is busy and a card that was silently skipped.
    if (!status.usable_gpus.empty()) {
        std::cout << "  training on:";
        for (const std::string& name : status.usable_gpus) {
            std::cout << " " << name << ";";
        }
        std::cout << "\n";
    }
    for (const std::string& name : status.unusable_gpus) {
        std::cout << "  cannot use " << name
                  << ": this PyTorch has no kernels for it\n";
    }
    if (status.present) {
        std::cout << "  " << pyenv::root().string() << "\n";
        if (status.bytes > 0) {
            std::cout << "  " << format::bytes(status.bytes) << " on disk\n";
        }
        if (!status.installed_at.empty()) {
            std::cout << "  installed " << status.installed_at << "\n";
        }
    } else {
        std::cout << "  install it with: crucible --install-trainer\n";
    }
    return status.ready ? 0 : 1;
}

int run_trainer_setup(const std::string& flavor_id, bool force, bool quiet) {
    const pyenv::Status before = pyenv::status();
    if (before.ready && !force) {
        if (!quiet) {
            std::cout << "  The training environment is already installed ("
                      << describe(before) << ").\n"
                      << "  Reinstall it with --install-trainer --force.\n";
        }
        return 2;
    }

    const pyenv::Flavor flavor =
        flavor_id.empty() ? pyenv::flavor_here() : pyenv::flavor_from_id(flavor_id);

    // Said before it starts rather than after it finishes. Several gigabytes
    // is a thing somebody on a metered connection is entitled to know about
    // while they can still press Ctrl-C.
    if (!quiet) {
        std::cout << "  Installing the training environment ("
                  << pyenv::flavor_id(flavor) << "): about "
                  << format::bytes(pyenv::download_bytes(flavor)) << " to download, "
                  << format::bytes(pyenv::installed_bytes(flavor)) << " on disk.\n"
                  << "  " << pyenv::flavor_note(flavor) << ".\n";
    }

    std::string                 version;
    const std::filesystem::path host = pyenv::host_python(version);
    if (host.empty()) {
        std::cerr << "  No usable Python found"
                  << (version.empty() ? "" : " (found " + version + ")") << ".\n"
                  << "  " << pyenv::python_hint() << "\n";
        return 1;
    }

    const bool  interactive = util::stdin_is_a_terminal() && !quiet;
    Line        line(interactive);
    pyenv::Installer installer;
    installer.start(flavor, {});

    for (;;) {
        const pyenv::Progress progress = installer.progress();
        if (!quiet) {
            line.show(progress.label());
        }
        if (progress.finished()) {
            line.done();
            if (progress.phase == pyenv::Progress::Phase::Done) {
                const pyenv::Status after = pyenv::status();
                if (!quiet) {
                    std::cout << "    training environment ready: " << describe(after) << "\n";
                }
                return 0;
            }
            std::cerr << "    training environment failed: " << progress.error << "\n";
            for (const std::string& entry : progress.log_tail) {
                std::cerr << "      " << entry << "\n";
            }
            std::cerr << "    full log: " << progress.log_file.string() << "\n";
            return 1;
        }
        // Polled rather than pushed: the callback would have to be safe
        // against a terminal that is being written to from another thread,
        // and a quarter of a second is a perfectly good refresh for a line
        // of text.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

}  // namespace crucible
