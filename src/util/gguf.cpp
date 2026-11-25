// SPDX-License-Identifier: MIT
//
// See gguf.hpp. The format is llama.cpp's gguf.cpp, read the same way: the
// magic, a version, the counts, the keys with their typed values, then each
// tensor's name, dimensions, type and offset, and the data aligned after.
#include "crucible/util/gguf.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace crucible::gguf {
namespace {

/// Longer arrays than this keep only their count: what is read from them is
/// per block, and a vocabulary's hundreds of thousands are not.
constexpr std::uint64_t kKeptElements = 1u << 16;

/// Sanity limits, past which a header is taken for a damaged one rather than
/// read until memory runs out.
constexpr std::uint64_t kMostKeys    = 1u << 20;
constexpr std::uint64_t kMostTensors = 1u << 24;
constexpr std::uint64_t kLongestText = 1u << 26;
constexpr std::uint32_t kMostDims    = 8;

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : in_(path, std::ios::binary) {}

    bool ok() const { return ok_ && static_cast<bool>(in_); }

    template <typename T>
    T number() {
        T value{};
        if (!in_.read(reinterpret_cast<char*>(&value), sizeof(T))) {
            ok_ = false;
        }
        return value;
    }

    std::string text() {
        const auto length = number<std::uint64_t>();
        if (!ok() || length > kLongestText) {
            ok_ = false;
            return {};
        }
        std::string out(length, '\0');
        if (length > 0 && !in_.read(out.data(), static_cast<std::streamsize>(length))) {
            ok_ = false;
        }
        return out;
    }

    void skip_text() {
        const auto length = number<std::uint64_t>();
        if (!ok() || length > kLongestText) {
            ok_ = false;
            return;
        }
        in_.seekg(static_cast<std::streamoff>(length), std::ios::cur);
    }

    void skip(std::uint64_t bytes) { in_.seekg(static_cast<std::streamoff>(bytes), std::ios::cur); }

    std::uint64_t where() { return static_cast<std::uint64_t>(in_.tellg()); }

private:
    std::ifstream in_;
    bool          ok_ = true;
};

/// Bytes one element of a scalar type takes, or zero for one that is not.
std::size_t width(Type type) {
    switch (type) {
        case Type::U8: case Type::I8: case Type::Bool: return 1;
        case Type::U16: case Type::I16: return 2;
        case Type::U32: case Type::I32: case Type::F32: return 4;
        case Type::U64: case Type::I64: case Type::F64: return 8;
        default: return 0;
    }
}

bool integral(Type type) {
    return type != Type::F32 && type != Type::F64 && width(type) != 0;
}

/// One scalar, onto `value` as an integer or a real.
void scalar(Reader& in, Type type, Value& value) {
    switch (type) {
        case Type::U8:   value.integers.push_back(in.number<std::uint8_t>()); break;
        case Type::I8:   value.integers.push_back(in.number<std::int8_t>()); break;
        case Type::Bool: value.integers.push_back(in.number<std::uint8_t>() != 0 ? 1 : 0); break;
        case Type::U16:  value.integers.push_back(in.number<std::uint16_t>()); break;
        case Type::I16:  value.integers.push_back(in.number<std::int16_t>()); break;
        case Type::U32:  value.integers.push_back(in.number<std::uint32_t>()); break;
        case Type::I32:  value.integers.push_back(in.number<std::int32_t>()); break;
        case Type::U64:  value.integers.push_back(static_cast<std::int64_t>(in.number<std::uint64_t>())); break;
        case Type::I64:  value.integers.push_back(in.number<std::int64_t>()); break;
        case Type::F32:  value.reals.push_back(in.number<float>()); break;
        case Type::F64:  value.reals.push_back(in.number<double>()); break;
        default: break;
    }
}

}  // namespace

std::optional<File> File::read(const std::filesystem::path& path, std::string* error) {
    const auto fail = [error](std::string why) -> std::optional<File> {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return std::nullopt;
    };
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail("cannot read " + path.string());
    }
    Reader in(path);
    char magic[4] = {};
    for (char& c : magic) {
        c = in.number<char>();
    }
    if (!in.ok() || std::memcmp(magic, "GGUF", 4) != 0) {
        return fail(path.filename().string() + " is not a GGUF file");
    }

    File file;
    file.version_ = in.number<std::uint32_t>();
    if (file.version_ < 2) {
        return fail(path.filename().string() + " is a GGUF older than this reads");
    }
    const auto n_tensors = in.number<std::uint64_t>();
    const auto n_keys    = in.number<std::uint64_t>();
    if (!in.ok() || n_tensors > kMostTensors || n_keys > kMostKeys) {
        return fail(path.filename().string() + " has a damaged header");
    }

    for (std::uint64_t k = 0; k < n_keys && in.ok(); ++k) {
        std::string key = in.text();
        Value       value;
        value.type = static_cast<Type>(in.number<std::uint32_t>());
        if (value.type == Type::String) {
            value.text = in.text();
        } else if (value.type == Type::Array) {
            value.element = static_cast<Type>(in.number<std::uint32_t>());
            value.count   = in.number<std::uint64_t>();
            if (value.element == Type::String) {
                for (std::uint64_t i = 0; i < value.count && in.ok(); ++i) {
                    in.skip_text();
                }
            } else if (width(value.element) == 0) {
                return fail(path.filename().string() + " has an array of a kind this does not read");
            } else if (value.count > kKeptElements) {
                in.skip(value.count * width(value.element));
            } else {
                for (std::uint64_t i = 0; i < value.count && in.ok(); ++i) {
                    scalar(in, value.element, value);
                }
            }
        } else if (width(value.type) != 0) {
            scalar(in, value.type, value);
        } else {
            return fail(path.filename().string() + " has a value of a kind this does not read");
        }
        file.values_.emplace(std::move(key), std::move(value));
    }

    file.tensors_.reserve(static_cast<std::size_t>(n_tensors));
    for (std::uint64_t t = 0; t < n_tensors && in.ok(); ++t) {
        Tensor tensor;
        tensor.name     = in.text();
        const auto dims = in.number<std::uint32_t>();
        if (dims > kMostDims) {
            return fail(path.filename().string() + " has a damaged tensor table");
        }
        for (std::uint32_t d = 0; d < dims; ++d) {
            tensor.ne.push_back(in.number<std::int64_t>());
        }
        tensor.type   = in.number<std::uint32_t>();
        tensor.offset = in.number<std::uint64_t>();
        file.tensors_.push_back(std::move(tensor));
    }
    if (!in.ok()) {
        return fail(path.filename().string() + " is cut short");
    }

    // Where the data starts: the header's end, aligned as the file says.
    std::uint64_t alignment = 32;
    if (const Value* stated = file.find("general.alignment"); stated != nullptr && !stated->integers.empty()
        && stated->integers.front() > 0) {
        alignment = static_cast<std::uint64_t>(stated->integers.front());
    }
    const std::uint64_t header    = in.where();
    const std::uint64_t data      = (header + alignment - 1) / alignment * alignment;
    const std::uint64_t data_size = file_size > data ? file_size - data : 0;

    // Each tensor's size, to where the next one starts.
    std::vector<std::size_t> order(file.tensors_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&file](std::size_t a, std::size_t b) { return file.tensors_[a].offset < file.tensors_[b].offset; });
    for (std::size_t i = 0; i < order.size(); ++i) {
        Tensor&             tensor = file.tensors_[order[i]];
        const std::uint64_t end    = i + 1 < order.size() ? file.tensors_[order[i + 1]].offset : data_size;
        tensor.size = end > tensor.offset ? end - tensor.offset : 0;
    }
    return file;
}

const Value* File::find(std::string_view key) const {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
}

std::string File::text(std::string_view key) const {
    const Value* value = find(key);
    return value != nullptr && value->type == Type::String ? value->text : std::string();
}

std::vector<std::uint64_t> File::unsigneds(std::string_view key) const {
    const Value* value = find(key);
    if (value == nullptr) {
        return {};
    }
    const Type kind = value->type == Type::Array ? value->element : value->type;
    if (!integral(kind)) {
        return {};
    }
    std::vector<std::uint64_t> out;
    out.reserve(value->integers.size());
    for (const std::int64_t one : value->integers) {
        out.push_back(one < 0 ? 0 : static_cast<std::uint64_t>(one));
    }
    return out;
}

std::uint64_t File::length(std::string_view key) const {
    const Value* value = find(key);
    return value != nullptr && value->type == Type::Array ? value->count : 0;
}

}  // namespace crucible::gguf
