// SPDX-License-Identifier: MIT
//
// The lab: what a custom model is made of, and whether this machine can make it.
//
// The arithmetic here is what the Create tab promises a user before they spend
// an hour on a run, so it is pinned: a size that is wrong by a factor of two
// is the difference between "this fits" and a run that dies at epoch one. The
// hub replies are canned, because a test that needs Hugging Face to be up is a
// test that fails for reasons that have nothing to do with this code.
#include "test_helpers.hpp"

#include "crucible/lab/condense.hpp"
#include "crucible/lab/hub.hpp"
#include "crucible/lab/imatrix.hpp"
#include "crucible/lab/recipe.hpp"

using namespace crucible::lab;

namespace {

Recipe started(const std::string& name) {
    Recipe recipe;
    recipe.name = name;
    recipe.id   = slug_of(name);
    return recipe;
}

Asset hub_asset(const std::string& id) {
    Asset asset;
    asset.source = Source::Hub;
    asset.id     = id;
    return asset;
}

/// A model as read_source would find it: what it averages, what its name
/// says it is and what its header does.
Original measured(double bits, const std::string& name = {}, const std::string& header = {}) {
    Original source;
    source.ok           = true;
    source.bits         = bits;
    source.quantization = name.empty() ? header : name;
    source.type         = header;
    return source;
}

/// A GGUF with one small tensor and a file type in its header, which is all
/// read_source needs. The tensor's data is not written, and nothing here
/// loads it.
void write_typed_gguf(const std::filesystem::path& file, std::uint32_t file_type) {
    ggml_init_params init{};
    init.mem_size   = 4 * ggml_tensor_overhead();
    init.mem_buffer = nullptr;
    init.no_alloc   = true;
    ggml_context* ctx = ggml_init(init);
    gguf_context* gguf = gguf_init_empty();
    gguf_set_val_str(gguf, "general.architecture", "llama");
    gguf_set_val_u32(gguf, "general.file_type", file_type);
    ggml_tensor* tensor = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, 256, 64);
    ggml_set_name(tensor, "token_embd.weight");
    gguf_add_tensor(gguf, tensor);
    gguf_write_to_file(gguf, file.string().c_str(), /*only_meta=*/true);
    gguf_free(gguf);
    ggml_free(ctx);
}

std::string read_all(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// The recipe
// ---------------------------------------------------------------------------

TEST(a_name_becomes_something_a_filesystem_can_hold) {
    CHECK_EQ(slug_of("Kitchen Physicist"), std::string("kitchen-physicist"));
    CHECK_EQ(slug_of("  C++ / Rust helper!  "), std::string("c-rust-helper"));
    CHECK_EQ(slug_of("already-a-slug"), std::string("already-a-slug"));
    CHECK_EQ(slug_of("!!!"), std::string(""));
}

TEST(a_recipe_says_what_it_is_still_missing_in_the_order_it_is_asked_for) {
    Recipe recipe;
    const std::vector<std::string> empty_recipe = missing(recipe);
    CHECK_EQ(empty_recipe.size(), std::size_t{3});
    CHECK_EQ(empty_recipe[0], std::string("a name"));

    recipe = started("Kitchen Physicist");
    CHECK_EQ(missing(recipe).size(), std::size_t{2});

    recipe.base = hub_asset("unsloth/Llama-3.2-1B");
    const std::vector<std::string> last = missing(recipe);
    CHECK_EQ(last.size(), std::size_t{1});
    CHECK_EQ(last[0], std::string("training data"));

    recipe.data.push_back(hub_asset("wikitext"));
    CHECK(missing(recipe).empty());
}

TEST(the_export_size_is_what_the_quantization_actually_weighs) {
    // A 1B model at Q4_K_M is a little over half a gigabyte, and at F16 two
    // gigabytes. These are the numbers the tab puts beside each choice, and
    // choosing a quantization is choosing a size.
    const std::uint64_t small = export_bytes(1.0, "Q4_K_M");
    CHECK(small > 550ULL * 1000 * 1000);
    CHECK(small < 700ULL * 1000 * 1000);

    const std::uint64_t half = export_bytes(1.0, "F16");
    CHECK(half > 1900ULL * 1000 * 1000);
    CHECK(half < 2100ULL * 1000 * 1000);

    // Bigger quantization, bigger file, every time.
    CHECK(export_bytes(7.0, "Q4_K_M") < export_bytes(7.0, "Q5_K_M"));
    CHECK(export_bytes(7.0, "Q5_K_M") < export_bytes(7.0, "Q8_0"));

    // Nothing known about it is nothing claimed about it.
    CHECK_EQ(export_bytes(7.0, "Q4_K_S_MADE_UP"), std::uint64_t{0});
    CHECK_EQ(export_bytes(0.0, "Q4_K_M"), std::uint64_t{0});
}

TEST(qlora_is_what_puts_a_seven_billion_model_on_one_card) {
    // A four-bit base and its activations: a 7B lands near 7 GB, which is the
    // whole point of the method -- it fits a card people already own.
    const Fit seven = estimate_fit(7.0, Method::Qlora, 12 * kGiB, 32 * kGiB);
    CHECK(seven.possible);
    CHECK(seven.needed > 5ULL * kGiB);
    CHECK(seven.needed < 10ULL * kGiB);

    // The same model in sixteen bits does not fit that card, and the estimate
    // says which way out there is rather than only refusing.
    const Fit lora = estimate_fit(7.0, Method::Lora, 12 * kGiB, 32 * kGiB);
    CHECK(!lora.possible);
    CHECK(lora.note.find("QLoRA") != std::string::npos);
}

TEST(a_small_model_fits_either_way_and_a_huge_one_fits_neither) {
    CHECK(estimate_fit(1.0, Method::Lora, 8 * kGiB, 16 * kGiB).possible);
    CHECK(estimate_fit(1.0, Method::Qlora, 8 * kGiB, 16 * kGiB).possible);

    // Seventy billion is a data-center model whatever the method: an adapter
    // makes the trainable part small, not the frozen base it sits on.
    CHECK(!estimate_fit(70.0, Method::Qlora, 40 * kGiB, 46 * kGiB).possible);
}

TEST(with_no_base_model_chosen_the_estimate_asks_for_one_rather_than_guessing) {
    const Fit nothing = estimate_fit(0.0, Method::Qlora, 40 * kGiB, 46 * kGiB);
    CHECK(!nothing.possible);
    CHECK(nothing.note.find("pick a base model") != std::string::npos);
}

TEST(a_recipe_survives_the_round_trip_through_disk) {
    Recipe recipe   = started("Kitchen Physicist");
    recipe.purpose  = "answers questions about induction hobs";
    recipe.base     = hub_asset("unsloth/Llama-3.2-1B");
    recipe.base.file  = "model.safetensors";
    recipe.base.bytes = 2500000000ULL;
    recipe.data.push_back(hub_asset("wikitext"));
    recipe.tools.push_back(hub_asset("crucible/tool-calls"));
    recipe.tools.back().source = Source::Local;
    recipe.method       = Method::Lora;
    recipe.format       = Export::Mlx;
    recipe.quantization = "Q5_K_M";
    recipe.parameters_b = 1.24;
    recipe.epochs       = 3;
    recipe.context      = 1024;

    Recipe      back;
    std::string error;
    CHECK(parse(serialize(recipe), back, error));
    CHECK(error.empty());
    CHECK_EQ(back.name, recipe.name);
    CHECK_EQ(back.id, std::string("kitchen-physicist"));
    CHECK_EQ(back.purpose, recipe.purpose);
    CHECK_EQ(back.base.id, recipe.base.id);
    CHECK_EQ(back.base.file, recipe.base.file);
    CHECK_EQ(back.base.bytes, recipe.base.bytes);
    CHECK_EQ(back.data.size(), std::size_t{1});
    CHECK_EQ(back.tools.size(), std::size_t{1});
    CHECK(back.tools[0].source == Source::Local);
    CHECK(back.method == Method::Lora);
    CHECK(back.format == Export::Mlx);
    CHECK_EQ(back.quantization, std::string("Q5_K_M"));
    CHECK_EQ(back.epochs, 3);
    CHECK_EQ(back.context, 1024);
}

TEST(nonsense_on_disk_is_refused_rather_than_loaded_as_an_empty_recipe) {
    Recipe      recipe;
    std::string error;
    CHECK(!parse("this is not json", recipe, error));
    CHECK(!error.empty());
    CHECK(!parse("[1, 2, 3]", recipe, error));
}

TEST(a_saved_recipe_is_found_again) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    Recipe recipe = started("Kitchen Physicist");
    recipe.base   = hub_asset("unsloth/Llama-3.2-1B");
    std::string error;
    CHECK(save(recipe, error));
    CHECK(error.empty());

    const std::vector<Recipe> found = saved_recipes();
    CHECK_EQ(found.size(), std::size_t{1});
    if (!found.empty()) {
        CHECK_EQ(found[0].name, std::string("Kitchen Physicist"));
        CHECK_EQ(found[0].base.id, std::string("unsloth/Llama-3.2-1B"));
    }

    // And a recipe with nothing to name it is refused, rather than writing a
    // directory called "".
    Recipe unnamed;
    CHECK(!save(unnamed, error));
    CHECK(!error.empty());
}

TEST(a_finished_model_is_offered_to_the_roster_and_a_missing_one_is_not) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // One that was trained and is still there.
    const std::filesystem::path file = temp.path() / "math.Q4_K_M.gguf";
    { std::ofstream out(file); out << "not really a model"; }
    Recipe done         = started("Math");
    done.base           = hub_asset("unsloth/Llama-3.2-1B");
    done.purpose        = "algebra, calculus, proofs";
    done.trained_path   = file.string();
    done.stage          = Stage::Finished;
    std::string error;
    CHECK(save(done, error));

    // One still being assembled, and one whose file has been deleted since.
    Recipe building = started("Physics");
    CHECK(save(building, error));
    Recipe gone       = started("Chemistry");
    gone.trained_path = (temp.path() / "went-away.gguf").string();
    gone.stage        = Stage::Finished;
    CHECK(save(gone, error));

    // And one that trained into a real file nobody has tested yet. It is not
    // a finished model: pressing Finish is the user saying it works, and no
    // amount of looking at the file makes that judgment for them.
    const std::filesystem::path candidate = temp.path() / "biology.Q4_K_M.gguf";
    { std::ofstream out(candidate); out << "not really a model either"; }
    Recipe untested       = started("Biology");
    untested.trained_path = candidate.string();
    untested.stage        = Stage::Testing;
    CHECK(save(untested, error));

    const std::vector<Made> made = finished_models();
    CHECK_EQ(made.size(), std::size_t{1});
    if (!made.empty()) {
        CHECK_EQ(made[0].name, std::string("Math"));
        CHECK_EQ(made[0].purpose, std::string("algebra, calculus, proofs"));
        CHECK_EQ(made[0].path, file);
        CHECK(made[0].bytes > 0);
    }
}

TEST(a_stage_survives_the_round_trip_and_is_named_in_the_file) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    Recipe recipe      = started("Kitchen Physicist");
    recipe.stage       = Stage::Training;
    recipe.started_at  = 1'700'000'000;
    recipe.finished_at = 0;

    Recipe      back;
    std::string error;
    CHECK(parse(serialize(recipe), back, error));
    CHECK(back.stage == Stage::Training);
    CHECK_EQ(back.started_at, std::int64_t{1'700'000'000});
    CHECK_EQ(back.finished_at, std::int64_t{0});

    // Written as a word rather than a number, so a recipe file can be read and
    // edited by hand without a table of what 2 means.
    CHECK(serialize(recipe).find("\"stage\": \"training\"") != std::string::npos);
}

TEST(every_stage_has_a_name_and_comes_back_as_itself) {
    const Stage all[] = {Stage::Draft, Stage::Training, Stage::Testing, Stage::Finished};
    for (const Stage stage : all) {
        CHECK(stage_from_id(stage_id(stage)) == stage);
        CHECK(std::string(stage_text(stage)).find(' ') != 0);
        CHECK(!std::string(stage_text(stage)).empty());
    }
    // Anything else is a draft. A recipe written by a newer Crucible with a
    // stage this one has never heard of is still a recipe, and the safe place
    // to put it is at the start rather than on the roster.
    CHECK(stage_from_id("sous-vide") == Stage::Draft);
    CHECK(stage_from_id("") == Stage::Draft);
}

TEST(a_recipe_written_before_stages_existed_reads_as_one_waiting_to_be_tested) {
    // The file it points at is the only evidence there is: something was
    // trained, and nobody has said whether it was any good.
    const std::string trained = R"({"name":"Math","id":"math","trained_path":"/tmp/m.gguf"})";
    Recipe            back;
    std::string       error;
    CHECK(parse(trained, back, error));
    CHECK(back.stage == Stage::Testing);

    const std::string bare = R"({"name":"Math","id":"math"})";
    CHECK(parse(bare, back, error));
    CHECK(back.stage == Stage::Draft);
}

// ---------------------------------------------------------------------------
// The hub
// ---------------------------------------------------------------------------

TEST(a_search_asks_for_the_most_downloaded_first) {
    const std::string url = hub::search_url(hub::Kind::Model, "physics tutor", 20);
    CHECK(url.find("https://huggingface.co/api/models") == 0);
    CHECK(url.find("search=physics%20tutor") != std::string::npos);
    CHECK(url.find("limit=20") != std::string::npos);
    CHECK(url.find("sort=downloads") != std::string::npos);
    // A plain search carries no "gated" flag, and a gated repository is a
    // download that fails an hour in. Expanding replaces the default fields,
    // so everything the list shows has to be asked for by name.
    CHECK(url.find("expand%5B%5D=gated") != std::string::npos);
    CHECK(url.find("expand%5B%5D=safetensors") != std::string::npos);
    // A dataset has neither weights nor a pipeline tag, and asking for them is
    // an error rather than an empty field.
    const std::string data_url = hub::search_url(hub::Kind::Dataset, "wiki", 5);
    CHECK(data_url.find("expand%5B%5D=gated") != std::string::npos);
    CHECK(data_url.find("safetensors") == std::string::npos);
    CHECK(data_url.find("pipeline_tag") == std::string::npos);

    // Datasets are a different shelf of the same library.
    CHECK(hub::search_url(hub::Kind::Dataset, "wiki", 5).find("/api/datasets") != std::string::npos);
    // And a download is a different host path again, with datasets prefixed.
    CHECK_EQ(hub::download_url(hub::Kind::Model, "a/b", "model.gguf"),
             std::string("https://huggingface.co/a/b/resolve/main/model.gguf"));
    CHECK_EQ(hub::download_url(hub::Kind::Dataset, "a/b", "train.jsonl"),
             std::string("https://huggingface.co/datasets/a/b/resolve/main/train.jsonl"));
}

TEST(a_hub_reply_becomes_a_list_to_choose_from) {
    const std::string reply = R"([
        {"id": "unsloth/Llama-3.2-1B", "downloads": 412000, "likes": 250,
         "pipeline_tag": "text-generation", "gated": false,
         "safetensors": {"total": 1235814400}},
        {"id": "meta-llama/Llama-3.1-8B", "downloads": 9000000, "likes": 4100,
         "pipeline_tag": "text-generation", "gated": "auto"},
        {"modelId": "tiny/whatever", "downloads": 3},
        {"nothing": "useful"}
    ])";
    const std::vector<hub::Item> items = hub::parse_search(reply);
    CHECK_EQ(items.size(), std::size_t{3});   // the one with no id is dropped

    CHECK_EQ(items[0].id, std::string("unsloth/Llama-3.2-1B"));
    CHECK_EQ(items[0].author, std::string("unsloth"));
    CHECK_EQ(items[0].downloads, std::uint64_t{412000});
    CHECK(!items[0].gated);
    // The metadata says 1.24 billion, and that beats reading the name.
    CHECK(items[0].parameters_b > 1.2);
    CHECK(items[0].parameters_b < 1.3);

    // Gated is anything but false: this one needs a browser and a license.
    CHECK(items[1].gated);
    // No metadata, so the name carries it.
    CHECK_EQ(items[1].parameters_b, 8.0);

    // An id under the older key is still an id.
    CHECK_EQ(items[2].id, std::string("tiny/whatever"));
}

TEST(a_hub_reply_that_is_not_a_list_yields_nothing_rather_than_breaking_the_step) {
    CHECK(hub::parse_search("").empty());
    CHECK(hub::parse_search("<html>rate limited</html>").empty());
    CHECK(hub::parse_search(R"({"error": "not found"})").empty());
}

TEST(a_file_listing_reports_what_a_download_will_actually_cost) {
    const std::string reply = R"([
        {"type": "file", "path": "README.md", "size": 1200},
        {"type": "directory", "path": "onnx"},
        {"type": "file", "path": "model.safetensors", "size": 135,
         "lfs": {"size": 2471645608}}
    ])";
    const std::vector<hub::File> files = hub::parse_tree(reply);
    CHECK_EQ(files.size(), std::size_t{2});   // the directory is not a file
    CHECK_EQ(files[0].path, std::string("README.md"));
    // The pointer file says 135 bytes; the model is two and a half gigabytes,
    // and that is the number worth showing.
    CHECK_EQ(files[1].bytes, std::uint64_t{2471645608});
}

TEST(a_size_in_a_model_name_is_read_from_the_right) {
    CHECK_EQ(hub::parameters_from_name("unsloth/Llama-3.2-1B"), 1.0);
    CHECK_EQ(hub::parameters_from_name("Qwen2.5-7B-Instruct"), 7.0);
    CHECK_EQ(hub::parameters_from_name("HuggingFaceTB/SmolLM-135M"), 0.135);
    CHECK_EQ(hub::parameters_from_name("google/gemma-2-2b-it"), 2.0);
    CHECK_EQ(hub::parameters_from_name("mlx-community/Qwen3.8-27B-4bit"), 27.0);
    CHECK_EQ(hub::parameters_from_name("lmstudio-community/Qwen3.8-27B-MLX-8bit"), 27.0);
    // A version number is not a size, and reading left to right made Llama 3.2
    // a three-billion model.
    CHECK_EQ(hub::parameters_from_name("meta/Llama-3.2"), 0.0);
    CHECK_EQ(hub::parameters_from_name("mistralai/Mistral-Nemo"), 0.0);
    CHECK_EQ(hub::parameters_from_name(""), 0.0);
}

// ---------------------------------------------------------------------------
// Condensing: the same model in fewer bits
// ---------------------------------------------------------------------------

TEST(a_file_name_says_which_quantization_it_is_at) {
    CHECK_EQ(quantization_in_name("flint-Q6_K.gguf"), std::string("Q6_K"));
    CHECK_EQ(quantization_in_name("LFM2.5-1.2B-Instruct-Q8_0.gguf"), std::string("Q8_0"));
    CHECK_EQ(quantization_in_name("gpt-oss-20b-MXFP4.gguf"), std::string("MXFP4"));
    CHECK_EQ(quantization_in_name("git-tutor.Q4_K_M.gguf"), std::string("Q4_K_M"));
    CHECK_EQ(quantization_in_name("flint-UD-Q6_K_XL.gguf"), std::string("UD-Q6_K_XL"));
    CHECK_EQ(quantization_in_name("Model-IQ4_XS-00001-of-00003.gguf"), std::string("IQ4_XS"));
    CHECK_EQ(quantization_in_name("qwen2.5-coder-7b-instruct-q4_k_m.gguf"), std::string("Q4_K_M"));
    // A version number is not a quantization.
    CHECK_EQ(quantization_in_name("Qwen3.8-27B.gguf"), std::string());
}

TEST(a_condensed_copy_is_named_after_its_original_and_its_new_size) {
    CHECK_EQ(condensed_name("flint-Q6_K.gguf", "Q4_K_M"), std::string("flint-Q4_K_M.gguf"));
    CHECK_EQ(condensed_name("flint-UD-Q6_K_XL.gguf", "Q3_K_M"), std::string("flint-Q3_K_M.gguf"));
    CHECK_EQ(condensed_name("git-tutor.Q8_0.gguf", "Q4_K_S"), std::string("git-tutor-Q4_K_S.gguf"));
    // No quantization in the name to replace: the new one is added.
    CHECK_EQ(condensed_name("my-model.gguf", "Q4_K_M"), std::string("my-model-Q4_K_M.gguf"));
    // A split model's first part becomes one file.
    CHECK_EQ(condensed_name("Big-Q8_0-00001-of-00004.gguf", "Q4_K_M"), std::string("Big-Q4_K_M.gguf"));
}

TEST(a_condensed_copy_goes_beside_its_original_inside_the_models_folder) {
    const std::filesystem::path models = "/models";
    CHECK(condensed_path("/models/unsloth/flint/flint-Q6_K.gguf", models, "Q4_K_M")
          == std::filesystem::path("/models/unsloth/flint/flint-Q4_K_M.gguf"));
    // From somewhere else, it lands at the top of the models folder, where
    // the pickers look.
    CHECK(condensed_path("/elsewhere/flint-Q6_K.gguf", models, "Q4_K_M")
          == std::filesystem::path("/models/flint-Q4_K_M.gguf"));
}

TEST(condensing_is_only_offered_where_it_makes_the_model_smaller) {
    CHECK(worth_condensing(measured(8.5), "Q4_K_M"));
    CHECK(worth_condensing(measured(6.58), "Q5_K_M"));
    // Already that small, or nearly: nothing to gain from another rounding.
    CHECK(!worth_condensing(measured(8.5), "Q8_0"));
    CHECK(!worth_condensing(measured(4.9), "Q4_K_M"));
    CHECK(!worth_condensing(measured(4.75), "Q4_K_S"));
    // A Q4_K_M squeezed to Q4_K_S is small but real: the last gigabyte is
    // sometimes the one that matters.
    CHECK(worth_condensing(measured(4.9), "Q4_K_S"));
    CHECK(worth_condensing(measured(4.9), "Q3_K_M"));
    CHECK(worth_condensing(measured(4.9, "Q4_K_M", "Q4_K_M"), "Q4_K_S"));
    CHECK(!worth_condensing(measured(8.5), "Q9_Z"));
    // Best first. The ones llama.cpp makes only with an importance matrix
    // say so: they sample the model before condensing it. IQ3_XXS sits
    // before Q2_K, as llama.cpp ranks them; the IQ2 and IQ1 sizes close it.
    const std::vector<Quantization>& offered = quantizations();
    CHECK_EQ(std::string(offered.front().name), std::string("Q8_0"));
    CHECK_EQ(std::string(offered.back().name), std::string("IQ1_S"));
    bool sampled = false;
    double bits = 99.0;
    for (const Quantization& q : offered) {
        const std::string name = q.name;
        const bool low = name.rfind("IQ1", 0) == 0 || name.rfind("IQ2", 0) == 0 || name == "IQ3_XXS";
        CHECK(q.samples == low);
        CHECK(samples_first(q.name) == low);
        sampled = sampled || q.samples;
        if (q.samples) {
            CHECK(q.bits < bits);       // each sampled one smaller than the one before
            bits = q.bits;
        }
        CHECK(known_quantization(q.name));
    }
    CHECK(sampled);
    CHECK(known_quantization("IQ2_XXS"));
    CHECK(worth_condensing(measured(4.9), "IQ2_XXS"));
    // Measured: gpt-oss-20b is 4.63 bits a weight, and its 2880-wide rows fall
    // back from IQ4_XS to IQ4_NL, 4.6 again; IQ2_XXS still saves a third of a bit.
    CHECK(!shrinks_enough(4.63, 4.60));
    CHECK(shrinks_enough(4.63, 4.29));
    CHECK(shrinks_enough(0.0, 4.29));
    CHECK(!samples_first("Q4_K_M"));
    CHECK(!samples_first("Q9_Z"));
    CHECK(known_quantization("F16"));
}

TEST(a_sampled_size_is_not_made_without_its_sample) {
    // A dry run sizes it without one; a real run refuses before reading a byte.
    const Quantized made = quantize("/nowhere/model.gguf", "/nowhere/model-IQ2_XXS.gguf", "IQ2_XXS", false);
    CHECK(!made.ok);
    CHECK(made.error.find("sampled") != std::string::npos);
}

TEST(an_importance_matrix_is_each_columns_mean_square) {
    // A plain weight: one run of three columns over four rows.
    const std::vector<float> plain = mean_squares({4.0F, 8.0F, 0.0F}, {4});
    CHECK_EQ(plain.size(), std::size_t{3});
    CHECK(plain[0] == 1.0F && plain[1] == 2.0F && plain[2] == 0.0F);

    // Three experts of two columns: the second never chosen, so ones.
    const std::vector<float> experts = mean_squares({6.0F, 3.0F, 9.0F, 9.0F, 10.0F, 20.0F}, {3, 0, 10});
    CHECK_EQ(experts.size(), std::size_t{6});
    CHECK(experts[0] == 2.0F && experts[1] == 1.0F);
    CHECK(experts[2] == 1.0F && experts[3] == 1.0F);
    CHECK(experts[4] == 1.0F && experts[5] == 2.0F);

    // Sums that do not divide into the experts are not guessed at.
    const std::vector<float> odd = mean_squares({1.0F, 2.0F, 3.0F}, {1, 1});
    CHECK(odd == std::vector<float>(3, 1.0F));
}

TEST(an_importance_matrix_is_kept_and_read_back_whole) {
    ImportanceMatrix matrix;
    matrix.chunks = 100;
    matrix.values["blk.0.attn_q.weight"]   = {0.5F, 1.25F, 3.0F};
    matrix.values["blk.1.ffn_up_exps.weight"] = std::vector<float>(64, 0.125F);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "crucible-test.imatrix";
    std::string error;
    CHECK(save_importance(matrix, file, error));

    ImportanceMatrix back;
    CHECK(load_importance(file, back));
    CHECK_EQ(back.chunks, 100);
    CHECK(back.values == matrix.values);

    // Cut short, or not one of ours: not read at all, so it is measured again.
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 5);
    ImportanceMatrix cut;
    CHECK(!load_importance(file, cut));
    { std::ofstream(file, std::ios::binary) << "GGUF and something else"; }
    CHECK(!load_importance(file, cut));
    std::filesystem::remove(file);

    // Kept by the model it measured: the same file twice, another file elsewhere.
    CHECK(importance_file("/models/a.gguf") == importance_file("/models/a.gguf"));
    CHECK(importance_file("/models/a.gguf") != importance_file("/models/b.gguf"));
    CHECK_EQ(importance_file("/models/a.gguf").extension().string(), std::string(".imatrix"));
}

TEST(a_condense_recipe_needs_only_a_name_and_a_model_on_this_machine) {
    Recipe recipe = started("Flint small");
    recipe.method = Method::Condense;
    std::vector<std::string> gaps = missing(recipe);
    CHECK_EQ(gaps.size(), std::size_t{1});
    CHECK(gaps[0].find("model on this machine") != std::string::npos);

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "crucible-test-condense.gguf";
    { std::ofstream(file) << "GGUF"; }
    recipe.base.source = Source::Local;
    recipe.base.id     = file.string();
    recipe.base.path   = file.string();
    CHECK(missing(recipe).empty());   // no data: a quantization has none

    // And it comes back as a condense recipe.
    Recipe back;
    std::string error;
    CHECK(parse(serialize(recipe), back, error));
    CHECK(back.method == Method::Condense);
    CHECK_EQ(std::string(method_id(back.method)), std::string("condense"));
    std::filesystem::remove(file);
}

TEST(a_model_is_never_offered_the_size_it_is_already_at) {
    // A 135M at Q4_K_M averages 5.2 bits a weight, its embeddings being kept
    // in more than four. Measured against that, Q4_K_M was a step down from
    // itself: offered, suggested, and named exactly as the original, which
    // the copy then replaced.
    const Original small = measured(5.2, "Q4_K_M", "Q4_K_M");
    CHECK(!worth_condensing(small, "Q4_K_M"));
    CHECK(!worth_condensing(small, "q4_k_m"));
    CHECK(!worth_condensing(small, "Q5_K_M"));   // larger, not smaller
    CHECK(worth_condensing(small, "Q3_K_M"));
    // Whichever of the two says what it is: a name that has none, or a name
    // that says one thing while the header says another.
    CHECK(!worth_condensing(measured(5.2, "", "Q4_K_M"), "Q4_K_M"));
    CHECK(!worth_condensing(measured(5.2, "Q8_0", "Q4_K_M"), "Q4_K_M"));
    CHECK(!worth_condensing(measured(5.2, "Q8_0", "Q4_K_M"), "Q8_0"));
    CHECK(!worth_condensing(measured(5.2, "q4_k_m"), "Q4_K_M"));
    // Unknown types still go by what the file averages: MXFP4 to four bits
    // is no saving, to three is.
    CHECK(!worth_condensing(measured(4.6, "MXFP4", "MXFP4"), "Q4_K_M"));
    CHECK(worth_condensing(measured(4.6, "MXFP4", "MXFP4"), "Q3_K_M"));
}

TEST(the_header_says_what_type_a_model_was_written_at) {
    TempDir temp;
    // Nothing in the name: the type is the header's, and so is the name.
    const std::filesystem::path plain = temp.path() / "plain.gguf";
    write_typed_gguf(plain, LLAMA_FTYPE_MOSTLY_Q4_K_M);
    const Original read = read_source(plain);
    CHECK(read.ok);
    CHECK_EQ(read.type, std::string("Q4_K_M"));
    CHECK_EQ(read.quantization, std::string("Q4_K_M"));
    CHECK(!worth_condensing(read, "Q4_K_M"));

    // A name that says otherwise is kept for showing; the header still counts.
    const std::filesystem::path misnamed = temp.path() / "model-Q8_0.gguf";
    write_typed_gguf(misnamed, LLAMA_FTYPE_MOSTLY_Q4_K_M);
    const Original other = read_source(misnamed);
    CHECK_EQ(other.quantization, std::string("Q8_0"));
    CHECK_EQ(other.type, std::string("Q4_K_M"));
    CHECK(!worth_condensing(other, "Q4_K_M"));
}

TEST(a_condensed_copy_never_takes_a_name_already_in_use) {
    TempDir temp;
    const std::filesystem::path wanted = temp.path() / "flint-Q4_K_M.gguf";
    CHECK(free_path(wanted) == wanted);

    // Downloaded at that size already: the copy gets a number, before the
    // size so the name still ends in it.
    { std::ofstream(wanted) << "a model somebody downloaded"; }
    CHECK(free_path(wanted) == temp.path() / "flint-2-Q4_K_M.gguf");
    CHECK_EQ(quantization_in_name(free_path(wanted).filename().string()), std::string("Q4_K_M"));
    // A part file is a download, or another condense, on its way there.
    { std::ofstream(temp.path() / "flint-2-Q4_K_M.gguf.part") << "half"; }
    CHECK(free_path(wanted) == temp.path() / "flint-3-Q4_K_M.gguf");
    CHECK(free_path(temp.path() / "lone-Q8_0.gguf") == temp.path() / "lone-Q8_0.gguf");
    { std::ofstream(temp.path() / "lone-Q8_0.gguf.part") << "half"; }
    CHECK(free_path(temp.path() / "lone-Q8_0.gguf") == temp.path() / "lone-2-Q8_0.gguf");

    // The name's own separator, and a name with no size in it.
    { std::ofstream(temp.path() / "git-tutor.Q8_0.gguf") << "x"; }
    CHECK(free_path(temp.path() / "git-tutor.Q8_0.gguf") == temp.path() / "git-tutor.2.Q8_0.gguf");
    { std::ofstream(temp.path() / "my-model.gguf") << "x"; }
    CHECK(free_path(temp.path() / "my-model.gguf") == temp.path() / "my-model-2.gguf");

    // The original in another case. Whether that is the same file is the
    // disk's to say -- it is on Windows, a Mac and exFAT -- so the answer is
    // whatever is free there, and never the file that is.
    const std::filesystem::path original = temp.path() / "small-q4_k_m.gguf";
    { std::ofstream(original) << "the original"; }
    const std::filesystem::path other_case = temp.path() / "small-Q4_K_M.gguf";
    const std::filesystem::path chosen = free_path(other_case);
    std::error_code ec;
    CHECK(!chosen.empty());
    CHECK(!std::filesystem::exists(chosen, ec));
    if (std::filesystem::exists(other_case, ec)) {
        CHECK(chosen != other_case);
    }
    CHECK_EQ(read_all(original), std::string("the original"));
}

TEST(quantizing_never_writes_over_the_model_it_reads) {
    TempDir temp;
    const std::filesystem::path model = temp.path() / "small-Q4_K_M.gguf";
    { std::ofstream(model) << "the original"; }

    const Quantized same = quantize(model, model, "Q4_K_M", false);
    CHECK(!same.ok);
    CHECK(same.error.find("written over") != std::string::npos);
    CHECK_EQ(read_all(model), std::string("the original"));
    std::error_code ec;
    CHECK(!std::filesystem::exists(part_of(model), ec));
#if !defined(_WIN32)
    // Nor by way of another name for it.
    const std::filesystem::path link = temp.path() / "link-Q4_K_M.gguf";
    std::filesystem::create_symlink(model, link, ec);
    if (!ec) {
        CHECK(!quantize(model, link, "Q4_K_M", false).ok);
        CHECK_EQ(read_all(model), std::string("the original"));
    }
#endif
}

TEST(a_held_out_score_survives_the_round_trip_through_disk) {
    Recipe recipe                    = started("Kitchen Physicist");
    recipe.trained_path              = "/models/kitchen-physicist-Q4_K_M.gguf";
    recipe.held_out.records          = 12;
    recipe.held_out.base.loss        = 2.31;
    recipe.held_out.base.perplexity  = 10.07;
    recipe.held_out.tuned.loss       = 1.47;
    recipe.held_out.tuned.perplexity = 4.35;

    Recipe      back;
    std::string error;
    CHECK(parse(serialize(recipe), back, error));
    CHECK_EQ(back.held_out.records, 12);
    CHECK_EQ(back.held_out.base.loss, 2.31);
    CHECK_EQ(back.held_out.tuned.perplexity, 4.35);
    CHECK(back.held_out.measured());

    // Too little data to hold any out says so, and is not a score.
    Recipe small           = started("Tiny");
    small.held_out.skipped = "too little data";
    CHECK(parse(serialize(small), back, error));
    CHECK_EQ(back.held_out.skipped, std::string("too little data"));
    CHECK(!back.held_out.measured());

    // A recipe from before there were scores has none, and its file says
    // nothing about one.
    CHECK(parse(R"({"name":"Math","id":"math"})", back, error));
    CHECK(!back.held_out.measured());
    CHECK(serialize(started("Math")).find("held_out") == std::string::npos);
}
