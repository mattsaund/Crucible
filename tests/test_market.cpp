// SPDX-License-Identifier: MIT
//
// The Models tab: what Hugging Face says, read; a repository's files made
// into the options a person chooses between; whether each will run; and a
// download carried on from where it stopped. The replies here are cut down
// from the real ones as they were on 2026-10-23. The network itself is
// asked only when CRUCIBLE_LIVE_HUB is set.
#include "test_helpers.hpp"

#include "crucible/market/downloads.hpp"
#include "crucible/market/hub.hpp"
#include "crucible/util/sha256.hpp"
#include "crucible/util/http.hpp"

#include <atomic>
#include <fstream>

namespace {

using namespace crucible::market;

const char* const kSearch = R"([
  {"id": "unsloth/Qwen3.8-27B-GGUF", "downloads": 5751118, "likes": 5294,
   "lastModified": "2026-08-20T12:04:25.000Z",
   "gguf": {"total": 27320697856, "architecture": "qwen35", "context_length": 262144,
            "chat_template": "{% if tools %}<tool_call>{% endif %}<think>"},
   "tags": ["gguf", "license:apache-2.0", "conversational"], "gated": false},
  {"id": "someone/Vision-Model-GGUF", "downloads": 10, "likes": 1, "pipeline_tag": "image-text-to-text",
   "gated": "manual", "tags": ["gguf"]},
  {"modelId": "bare/Llama-3.2-1B-Instruct-GGUF"},
  {"no id": true},
  "not even an object"
])";

const char* const kLink =
    R"(<https://huggingface.co/api/models?search=qwen&cursor=abc%3D>; rel="next")";

const char* const kModel = R"({
  "id": "unsloth/Qwen3.8-27B-GGUF", "downloads": 5751118, "likes": 5294,
  "lastModified": "2026-08-20T12:04:25.000Z", "gated": false,
  "cardData": {"base_model": ["Qwen/Qwen3.8-27B"], "license": "apache-2.0"},
  "gguf": {"total": 27320697856, "architecture": "qwen35", "context_length": 262144},
  "siblings": [
    {"rfilename": ".gitattributes", "size": 4175},
    {"rfilename": "BF16/Qwen3.8-27B-BF16-00001-of-00002.gguf", "size": 49986159616},
    {"rfilename": "BF16/Qwen3.8-27B-BF16-00002-of-00002.gguf", "size": 4671576000},
    {"rfilename": "MTP/mtp-Qwen3.8-27B-Q4_0.gguf", "size": 1369590656},
    {"rfilename": "Qwen3.8-27B-Q4_0.gguf", "size": 1024,
     "lfs": {"size": 16056478688, "sha256": "5F4020D9C59B329AAA082A690743E760411DFBB5C4CCD1EF49FB0FD20A6B6A7C"}},
    {"rfilename": "Qwen3.8-27B-UD-Q4_K_XL.gguf", "size": 17559178144},
    {"rfilename": "Qwen3.8-27B-Q8_0.gguf", "size": 29047086048},
    {"rfilename": "README.md", "size": 7455},
    {"rfilename": "imatrix_unsloth.gguf", "size": 13642656},
    {"rfilename": "mmproj-BF16.gguf", "size": 931146432},
    {"rfilename": "mmproj-F16.gguf", "size": 927607488}
  ]
})";

const Option* named(const std::vector<Option>& options, const std::string& name) {
    for (const Option& option : options) {
        if (option.name == name) {
            return &option;
        }
    }
    return nullptr;
}

constexpr std::uint64_t kGiB = 1ULL << 30;

}  // namespace

TEST(a_search_reads_what_each_repository_is_and_can_do) {
    const Page page = parse_search(kSearch, kLink);
    CHECK_EQ(page.listings.size(), std::size_t{3});
    CHECK_EQ(page.next, std::string("https://huggingface.co/api/models?search=qwen&cursor=abc%3D"));
    if (page.listings.size() < 3) {
        return;
    }
    const Listing& qwen = page.listings[0];
    CHECK_EQ(qwen.author, std::string("unsloth"));
    CHECK_EQ(qwen.name, std::string("Qwen3.8-27B-GGUF"));
    CHECK_EQ(qwen.downloads, std::uint64_t{5751118});
    CHECK(qwen.params_b > 27.0 && qwen.params_b < 28.0);
    CHECK_EQ(qwen.architecture, std::string("qwen35"));
    CHECK_EQ(qwen.context, std::uint64_t{262144});
    CHECK_EQ(qwen.license, std::string("apache-2.0"));
    // Told from its chat template, the way LM Studio tells.
    CHECK(qwen.tools);
    CHECK(qwen.reasoning);
    CHECK(!qwen.vision);
    // A license to accept first; pictures read; a size from the name alone.
    CHECK(page.listings[1].gated);
    CHECK(page.listings[1].vision);
    CHECK(page.listings[2].params_b > 0.9 && page.listings[2].params_b < 1.1);
    // The hub counted a draft head; the name says what the model is.
    const Page counted = parse_search(R"([{"id": "a/Big-27B-MTP-GGUF", "gguf": {"total": 1900000000}}])", "");
    CHECK(!counted.listings.empty() && counted.listings[0].params_b == 27.0);
    // No next page is no next page, not a crash.
    CHECK(parse_search("[]", "").next.empty());
    CHECK(parse_search("not json", "").listings.empty());
}

TEST(best_match_keeps_what_is_in_use_now_first_and_orders_the_rest_by_use) {
    std::vector<Listing> listings(4);
    listings[0].id = "new/tiny";     listings[0].trending = 2;  listings[0].downloads = 29;    listings[0].likes = 2;
    listings[1].id = "some/one";     listings[1].downloads = 304;
    listings[2].id = "unsloth/x";    listings[2].downloads = 66408; listings[2].likes = 27;
    listings[3].id = "hot/new";      listings[3].trending = 300; listings[3].downloads = 1000;
    rank_best(listings);
    CHECK_EQ(listings[0].id, std::string("hot/new"));
    CHECK_EQ(listings[1].id, std::string("unsloth/x"));
}

TEST(a_quantization_is_read_from_a_name_at_its_boundaries) {
    CHECK_EQ(quant_of("Qwen3.8-27B-Q4_K_M.gguf"), std::string("Q4_K_M"));
    CHECK_EQ(quant_of("Qwen3.8-27B-UD-Q4_K_XL.gguf"), std::string("UD-Q4_K_XL"));
    CHECK_EQ(quant_of("Qwen3.8-27B-BF16-00001-of-00002.gguf"), std::string("BF16"));
    CHECK_EQ(quant_of("gpt-oss-20b-MXFP4.gguf"), std::string("MXFP4"));
    CHECK_EQ(quant_of("model.IQ2_XXS.gguf"), std::string("IQ2_XXS"));
    CHECK_EQ(quant_of("mmproj-F16.gguf"), std::string("F16"));
    CHECK_EQ(quant_of("Qwen3.8-27B-MLX-4bit"), std::string("4bit"));
    CHECK_EQ(quant_of("Qwen3-8B-6bit-DWQ"), std::string("6bit"));
    // "Qwen" starts with a Q and "BF16" ends in F16; neither is the answer.
    CHECK(quant_of("Qwen2.5-0.5B-Instruct").empty());
    CHECK_EQ(quant_of("x-BF16.gguf"), std::string("BF16"));
    // A kind of K quant this was not written knowing keeps its last letter.
    CHECK_EQ(quant_of("Qwen3.8-27B-Aggressive-Q8_K_P.gguf"), std::string("Q8_K_P"));
}

TEST(a_draft_head_is_not_offered_but_a_model_with_mtp_layers_is) {
    const std::vector<Option> options = gguf_options("a/b",
        {{"Qwen3.8-27B-Aggressive-FastMTP-32K.gguf", 900, {}}, {"Qwen3.8-27B-Aggressive-Q4_K_P.gguf", 17000, {}},
         {"x-draft.gguf", 50, {}}, {"Qwen3.8-27B-MTP-Q8_0.gguf", 30000, {}}});
    CHECK_EQ(options.size(), std::size_t{2});
    CHECK(named(options, "Q4_K_P") != nullptr);
    CHECK(named(options, "Q8_0") != nullptr);
}

TEST(a_gguf_repository_is_offered_as_its_quantizations) {
    const Detail detail = parse_model(kModel, Format::Gguf);
    CHECK_EQ(detail.listing.id, std::string("unsloth/Qwen3.8-27B-GGUF"));
    CHECK_EQ(detail.base_model, std::string("Qwen/Qwen3.8-27B"));
    // A projector in the repository means it reads pictures, whatever its card.
    CHECK(detail.listing.vision);
    // The importance matrix and the MTP draft head are not things to talk to;
    // the split BF16 is one option of two parts.
    CHECK_EQ(detail.options.size(), std::size_t{4});
    const Option* q4  = named(detail.options, "Q4_0");
    const Option* bf  = named(detail.options, "BF16");
    const Option* ud  = named(detail.options, "UD-Q4_K_XL");
    CHECK(q4 != nullptr);
    CHECK(bf != nullptr);
    CHECK(ud != nullptr);
    if (q4 == nullptr || bf == nullptr) {
        return;
    }
    // Each option takes the F16 projector with it, and costs what it all costs.
    CHECK_EQ(q4->files.size(), std::size_t{2});
    CHECK_EQ(q4->files.back().path, std::string("mmproj-F16.gguf"));
    CHECK_EQ(q4->bytes, std::uint64_t{16056478688ULL + 927607488ULL});
    // The LFS record's size is the file's; its digest is kept, in lower case.
    CHECK_EQ(q4->files.front().sha256, std::string("5f4020d9c59b329aaa082a690743e760411dfbb5c4ccd1ef49fb0fd20a6b6a7c"));
    CHECK(q4->files.back().sha256.empty());
    CHECK_EQ(bf->files.size(), std::size_t{3});
    CHECK_EQ(bf->files[0].path, std::string("BF16/Qwen3.8-27B-BF16-00001-of-00002.gguf"));
    // Smallest first.
    for (std::size_t i = 1; i < detail.options.size(); ++i) {
        CHECK(detail.options[i - 1].bytes <= detail.options[i].bytes);
    }
    // A split set with a part missing is not offered.
    const std::vector<Option> broken = gguf_options("a/b", {{"x-00001-of-00003.gguf", 10, {}}, {"x-00002-of-00003.gguf", 10, {}}});
    CHECK(broken.empty());
}

TEST(an_mlx_repository_is_its_files_without_the_card) {
    const Option option = mlx_option("mlx-community/Qwen3-8B-4bit",
        {{"config.json", 100, {}}, {"model.safetensors", 4000, {}}, {"README.md", 50, {}}, {".gitattributes", 5, {}},
         {"images/banner.png", 900, {}}, {"tokenizer.json", 200, {}}, {"../escape.json", 1, {}}});
    CHECK_EQ(option.name, std::string("4bit"));
    CHECK_EQ(option.files.size(), std::size_t{3});
    CHECK_EQ(option.bytes, std::uint64_t{4300});
}

TEST(what_will_run_here_is_said_from_the_cards_and_the_memory) {
    const Machine workstation{40 * kGiB, 46 * kGiB, 3, false};
    CHECK(fit(16 * kGiB, Format::Gguf, workstation) == Fit::Gpu);
    CHECK(fit(50 * kGiB, Format::Gguf, workstation) == Fit::Partial);
    CHECK(fit(120 * kGiB, Format::Gguf, workstation) == Fit::TooLarge);
    CHECK(fit(4 * kGiB, Format::Mlx, workstation) == Fit::MacOnly);

    const Machine no_gpu{0, 32 * kGiB, 0, false};
    CHECK(fit(8 * kGiB, Format::Gguf, no_gpu) == Fit::Cpu);
    CHECK(fit(40 * kGiB, Format::Gguf, no_gpu) == Fit::TooLarge);

    const Machine mac{18 * kGiB, 24 * kGiB, 1, true};
    CHECK(fit(8 * kGiB, Format::Mlx, mac) == Fit::Gpu);
    CHECK(fit(30 * kGiB, Format::Mlx, mac) == Fit::TooLarge);
}

TEST(a_download_lands_where_the_catalog_looks) {
    const std::filesystem::path models = "/m";
    CHECK(destination(models, "unsloth/Qwen3-8B-GGUF") == models / "unsloth" / "Qwen3-8B-GGUF");
    CHECK(destination(models, "../..") == models / "_" / "_");
    const std::filesystem::path folder = models / "a" / "b";
    // A split set's parts sit together, out of the folder they came in.
    CHECK(landing(folder, {"BF16/x-00001-of-00002.gguf", 1, {}}, Format::Gguf) == folder / "x-00001-of-00002.gguf");
    CHECK(landing(folder, {"sub/config.json", 1, {}}, Format::Mlx) == folder / "sub" / "config.json");
    CHECK(landing(folder, {"../../etc/passwd", 1, {}}, Format::Mlx) == folder / "passwd");
    CHECK_EQ(file_url("a/b", "dir/my file.gguf"),
             std::string("https://huggingface.co/a/b/resolve/main/dir/my%20file.gguf"));
    // A next page is only ever the hub's.
    Page page;
    std::string error;
    CHECK(!search("https://example.com/api/models?x", page, error));
}

TEST(a_download_carries_on_from_where_it_stopped) {
    std::string why;
    if (!crucible::util::http::available(why)) {
        std::printf("      (no curl; skipped)\n");
        return;
    }
    TempDir dir;
    const std::filesystem::path source = dir.path() / "source.bin";
    std::string bytes(300000, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i % 251);
    }
    { std::ofstream out(source, std::ios::binary); out << bytes; }
    // The first third already fetched, from a try that stopped.
    const std::filesystem::path to = dir.path() / "models" / "a" / "model.gguf";
    std::filesystem::create_directories(to.parent_path());
    { std::ofstream out(to.string() + ".part", std::ios::binary); out << bytes.substr(0, 100000); }

    const std::string url = "file://" + source.generic_string();
    std::uint64_t     seen = 0;
    std::string       error;
    crucible::util::Sha256 digest;
    digest.update(bytes);
    const std::string sum = digest.hex();
    const bool fetched = fetch(url, to, bytes.size(), sum, [&seen](std::uint64_t n) { seen = n; },
                               [] { return false; }, error);
    CHECK(fetched);
    if (!fetched) {
        std::printf("      %s\n", error.c_str());
        return;
    }
    std::ifstream in(to, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(got == bytes);
    CHECK_EQ(seen, std::uint64_t{bytes.size()});
    CHECK(!std::filesystem::exists(to.string() + ".part"));

    // Already here and whole: left alone, nothing fetched.
    CHECK(fetch("file:///nowhere/at/all", to, bytes.size(), sum, [](std::uint64_t) {}, [] { return false; }, error));

    // A part file that came out the wrong size is said to be cut short.
    const std::filesystem::path short_one = dir.path() / "models" / "a" / "short.gguf";
    CHECK(!fetch(url, short_one, bytes.size() + 10, "", [](std::uint64_t) {}, [] { return false; }, error));
    CHECK(error.find("cut short") != std::string::npos);

    // A part file from some other file, carried on from, comes out the right
    // size and the wrong bytes: the checksum is what tells, and it goes.
    const std::filesystem::path spliced = dir.path() / "models" / "a" / "spliced.gguf";
    { std::ofstream out(spliced.string() + ".part", std::ios::binary); out << std::string(100000, 'x'); }
    CHECK(!fetch(url, spliced, bytes.size(), sum, [](std::uint64_t) {}, [] { return false; }, error));
    CHECK(error.find("checksum") != std::string::npos);
    CHECK(!std::filesystem::exists(spliced.string() + ".part"));
    CHECK(!std::filesystem::exists(spliced));
}

namespace {

// Two of the hub's real files, as it described them on 2026-10-26: a model in
// one file, and the first of a split set's two parts.
const char* const kMxfp4  = "27cd6c432c7672cb812a92f611cf3ba7bbc35928262bb1e1253ff4ee6ae35901";
const char* const kFirst  = "b1d7474caa92717b8a7f75e3c1d1006eb40f3bd3591522e5a2b095f36b12757d";
const char* const kSecond = "b7cc4f7dd8dfd252900d79ab462abc0ad4bf06e4bcf0e59e491b6625bfee36e6";
constexpr std::uint64_t kMxfp4Bytes = 12109566624ULL;
constexpr std::uint64_t kSetBytes   = 49630904192ULL + 13137819360ULL;

const char* const kRepository = R"({
  "_id": "688dec3ebc727e7a313790d8", "id": "ggml-org/gpt-oss-20b-GGUF", "private": false, "gated": false,
  "disabled": false,
  "siblings": [
    {"rfilename": "README.md", "blobId": "e095fd94747f223e24e89440617c71099e0b98b3", "size": 501},
    {"rfilename": "gpt-oss-20b-MXFP4.gguf", "blobId": "deda3c25c5da45a23db1c5d52a49e6c509da659b", "size": 12109566624,
     "lfs": {"sha256": "27cd6c432c7672cb812a92f611cf3ba7bbc35928262bb1e1253ff4ee6ae35901", "size": 12109566624,
             "pointerSize": 136}},
    {"rfilename": "Q4_K_M/gpt-oss-120b-Q4_K_M-00001-of-00002.gguf", "size": 49630904192,
     "lfs": {"sha256": "b1d7474caa92717b8a7f75e3c1d1006eb40f3bd3591522e5a2b095f36b12757d", "size": 49630904192}},
    {"rfilename": "Q4_K_M/gpt-oss-120b-Q4_K_M-00002-of-00002.gguf", "size": 13137819360,
     "lfs": {"sha256": "b7cc4f7dd8dfd252900d79ab462abc0ad4bf06e4bcf0e59e491b6625bfee36e6", "size": 13137819360}}
  ]
})";

}  // namespace

TEST(a_model_file_is_found_on_the_hub_by_its_hash_and_size) {
    const Repository repo = parse_repository(kRepository);
    CHECK_EQ(repo.id, std::string("ggml-org/gpt-oss-20b-GGUF"));
    CHECK(repo.open);
    CHECK_EQ(repo.files.size(), std::size_t{4});
    // The name it has there, whatever it is called here.
    CHECK_EQ(matching_file(repo.files, kMxfp4, kMxfp4Bytes, "renamed.gguf"), std::string("gpt-oss-20b-MXFP4.gguf"));
    CHECK(matching_file(repo.files, kMxfp4, kMxfp4Bytes - 1, "").empty());
    CHECK(matching_file(repo.files, std::string(64, '0'), kMxfp4Bytes, "").empty());
    // A split set is hashed from its first part and is the size of all of them.
    CHECK_EQ(matching_file(repo.files, kFirst, kSetBytes, ""), std::string("gpt-oss-120b-Q4_K_M-00001-of-00002.gguf"));
    CHECK(matching_file(repo.files, kFirst, 49630904192ULL, "").empty());
    CHECK(matching_file(repo.files, kSecond, 13137819360ULL, "").empty());
    std::vector<File> short_set(repo.files.begin(), repo.files.end() - 1);
    CHECK(matching_file(short_set, kFirst, kSetBytes, "").empty());
    // Two copies of one file: the one named as it is here, else the first.
    const std::vector<File> twice{{"a/x.gguf", 10, kMxfp4}, {"b/y.gguf", 10, kMxfp4}};
    CHECK_EQ(matching_file(twice, kMxfp4, 10, "y.gguf"), std::string("y.gguf"));
    CHECK_EQ(matching_file(twice, kMxfp4, 10, "z.gguf"), std::string("x.gguf"));
    // Only a GGUF is a model, and only one kept in LFS has a hash to compare.
    CHECK(matching_file({{"x.bin", 10, kMxfp4}}, kMxfp4, 10, "").empty());
    CHECK(matching_file({{"x.gguf", 10, {}}}, "", 10, "").empty());

    // Private or disabled is not open to anybody; a refusal is no repository.
    CHECK(!parse_repository(R"({"id": "a/b", "private": true, "siblings": []})").open);
    CHECK(!parse_repository(R"({"id": "a/b", "disabled": true, "siblings": []})").open);
    CHECK(parse_repository(R"({"id": "a/b", "gated": "manual", "siblings": []})").open);
    CHECK(parse_repository(R"({"error": "Invalid username or password."})").id.empty());
    CHECK(parse_repository("not json").id.empty());
    CHECK_EQ(repository_url("ggml-org/gpt-oss-20b-GGUF"),
             std::string("https://huggingface.co/api/models/ggml-org/gpt-oss-20b-GGUF"
                         "?blobs=true&expand%5B%5D=siblings&expand%5B%5D=private&expand%5B%5D=disabled"));
    // A folder's name can hold anything, and goes in the address as a name.
    CHECK(repository_url("my models/a?b=c").find("/api/models/my%20models/a%3Fb%3Dc?blobs") != std::string::npos);
}

TEST(a_model_file_is_searched_for_by_the_name_of_its_model) {
    CHECK_EQ(search_name("gpt-oss-20b-MXFP4.gguf"), std::string("gpt-oss-20b"));
    CHECK_EQ(search_name("Qwen3.8-27B-UD-Q4_K_XL.gguf"), std::string("Qwen3.8-27B"));
    CHECK_EQ(search_name("gpt-oss-120b-Q4_K_M-00001-of-00002.gguf"), std::string("gpt-oss-120b"));
    CHECK_EQ(search_name("openai_gpt-oss-20b-Q4_K_M.gguf"), std::string("openai_gpt-oss-20b"));
    CHECK_EQ(search_name("model.IQ2_XXS.gguf"), std::string("model"));
    CHECK_EQ(search_name("tiny-00001-of-00003.gguf"), std::string("tiny"));
    CHECK_EQ(search_name("tiny.gguf"), std::string("tiny"));
}

TEST(a_model_file_is_found_public_when_asked_for_real) {
    if (std::getenv("CRUCIBLE_LIVE_HUB") == nullptr) {
        return;   // the network, only when asked for
    }
    Found       found;
    std::string error;
    // In the repository its folder is named for.
    CHECK(find_public("ggml-org/gpt-oss-20b-GGUF", "gpt-oss-20b-MXFP4.gguf", kMxfp4, kMxfp4Bytes, found, error));
    CHECK_EQ(found.repo, std::string("ggml-org/gpt-oss-20b-GGUF"));
    CHECK_EQ(found.file, std::string("gpt-oss-20b-MXFP4.gguf"));
    // In other capitals, the hub's own spelling comes back.
    CHECK(find_public("GGML-ORG/gpt-oss-20b-gguf", "gpt-oss-20b-MXFP4.gguf", kMxfp4, kMxfp4Bytes, found, error));
    CHECK_EQ(found.repo, std::string("ggml-org/gpt-oss-20b-GGUF"));
    // With no folder to go by, a search by its name: any public copy will do.
    CHECK(find_public("", "gpt-oss-20b-MXFP4.gguf", kMxfp4, kMxfp4Bytes, found, error));
    CHECK_EQ(found.file, std::string("gpt-oss-20b-MXFP4.gguf"));
    CHECK(!found.repo.empty());
    // A split set, by its first part.
    CHECK(find_public("", "gpt-oss-120b-Q4_K_M-00001-of-00002.gguf", kFirst, kSetBytes, found, error));
    CHECK_EQ(found.repo, std::string("unsloth/gpt-oss-120b-GGUF"));
    // A file nobody published is found nowhere, and that is no error.
    CHECK(find_public("nobody-xyz-123/nothing-here-456", "gpt-oss-20b-MXFP4.gguf", std::string(64, '0'), 1, found,
                      error));
    CHECK(found.repo.empty());
    if (!error.empty()) {
        std::printf("      %s\n", error.c_str());
    }
}

TEST(the_hub_answers_when_asked_for_real) {
    if (std::getenv("CRUCIBLE_LIVE_HUB") == nullptr) {
        return;   // the network, only when asked for
    }
    Page        page;
    std::string error;
    CHECK(search(search_url("qwen", Format::Gguf, Sort::Downloads, 5), page, error));
    CHECK(!page.listings.empty());
    CHECK(!page.next.empty());
    Detail detail;
    CHECK(model("unsloth/Qwen3-0.6B-GGUF", Format::Gguf, detail, error));
    CHECK(!detail.options.empty());
    if (!error.empty()) {
        std::printf("      %s\n", error.c_str());
    }
}
