# Third-party licenses

Crucible links these projects statically. Each is MIT-licensed, the same as
Crucible itself, so a distributed `crucible` binary carries no obligations beyond
retaining the copyright notices below.

They are fetched at build time by `cmake/CrucibleDependencies.cmake`, pinned to
exact tags. No third-party source is vendored into this repository.

| project | version | license | what it does here |
|---|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | `b10678` | MIT | loads GGUF models and runs inference |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | MIT | reads and writes the config file |

**Models are not covered by any of this.** Crucible ships no model weights and
downloads none. Whatever GGUF files you put in your models directory carry
their own licenses, which are between you and whoever trained them.
