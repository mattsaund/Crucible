// SPDX-License-Identifier: MIT
//
// A GGUF file's header, read without llama.cpp.
//
// The program reads model files to size them, place them and describe them,
// and does that whether or not a runtime is installed, and whichever llama.cpp
// the runtime in use was built from. So the header is read here: every key
// and every tensor's name, shape and place, none of the data. A tensor's size
// comes from where the next one starts, which adds up for a quantization type
// newer than this program as well as for one it knows.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::gguf {

/// What a value is, as the file numbers it.
enum class Type : std::uint32_t {
    U8 = 0, I8 = 1, U16 = 2, I16 = 3, U32 = 4, I32 = 5, F32 = 6, Bool = 7,
    String = 8, Array = 9, U64 = 10, I64 = 11, F64 = 12,
};

/// One key's value.
struct Value {
    Type          type    = Type::U8;
    Type          element = Type::U8;  ///< what an array holds
    std::uint64_t count   = 0;         ///< an array's length

    /// An integer or a bool, or an array of them. A very long array -- a
    /// vocabulary's token types -- has only its count kept.
    std::vector<std::int64_t> integers;
    /// A float, or an array of them, kept as for integers.
    std::vector<double> reals;
    /// A string. An array of strings has only its count kept.
    std::string text;
};

/// One tensor's description.
struct Tensor {
    std::string               name;
    std::vector<std::int64_t> ne;           ///< its dimensions, as llama.cpp numbers them
    std::uint32_t             type   = 0;   ///< ggml's number for its type
    std::uint64_t             offset = 0;   ///< from the start of the data
    std::uint64_t             size   = 0;   ///< bytes, to where the next one starts
};

class File {
public:
    /// The header of `path`, or nothing when it is not a GGUF or is cut
    /// short; `error`, when given, says which.
    static std::optional<File> read(const std::filesystem::path& path, std::string* error = nullptr);

    std::uint32_t version() const { return version_; }

    /// The value of `key`, or null.
    const Value* find(std::string_view key) const;

    /// A string's text, or empty when `key` is missing or not a string.
    std::string text(std::string_view key) const;

    /// An integer, or an array of them, as unsigned: a negative one reads
    /// as zero. Empty when `key` is missing or holds something else.
    std::vector<std::uint64_t> unsigneds(std::string_view key) const;

    /// An array's length, or zero when `key` is missing or not an array.
    std::uint64_t length(std::string_view key) const;

    const std::vector<Tensor>& tensors() const { return tensors_; }

private:
    std::uint32_t                               version_ = 0;
    std::map<std::string, Value, std::less<>>   values_;
    std::vector<Tensor>                         tensors_;
};

}  // namespace crucible::gguf
