// SPDX-License-Identifier: MIT
//
// The runtime underneath: which backends are available, what shape a model
// file turns out to be, and the resource monitor.
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

TEST(the_backend_table_is_indexed_by_the_enum) {
    const auto& backends = all_backends();
    CHECK_EQ(backends.size(), kBackendCount);
    for (std::size_t i = 0; i < backends.size(); ++i) {
        // backend_info() indexes straight into the array, so a table written
        // out of order would silently return the wrong entry for every lookup.
        CHECK_EQ(static_cast<std::size_t>(backends[i].kind), i);
    }
}

TEST(every_backend_has_a_unique_id_and_round_trips) {
    for (const BackendInfo& info : all_backends()) {
        const auto parsed = backend_from_id(info.id);
        CHECK(parsed.has_value());
        CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(info.kind));
        CHECK(!info.name.empty());
        CHECK(!info.blurb.empty());
        CHECK(!info.cmake_option.empty());
    }
    CHECK(!backend_from_id("opencl").has_value());
    CHECK(!backend_from_id("").has_value());
}

TEST(ggml_calls_the_metal_backend_something_else_entirely) {
    // ggml registers it as "MTL", not "Metal". Matching on the id would leave
    // an installed Metal runtime reported as inactive for ever, with its
    // devices working perfectly the whole time -- a wrong panel over a working
    // machine, which is the kind of bug nobody thinks to look for.
    CHECK(backend_from_reg_name("MTL").has_value());
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("MTL")),
             static_cast<int>(BackendKind::Metal));
    CHECK(!backend_from_reg_name("Metal").has_value());  // ggml never says this

    // And every entry's registry name resolves to itself, so a future backend
    // cannot be added with the field left at whatever looked plausible.
    for (const BackendInfo& info : all_backends()) {
        const auto parsed = backend_from_reg_name(info.reg_name);
        CHECK(parsed.has_value());
        if (parsed) {
            CHECK_EQ(static_cast<int>(*parsed), static_cast<int>(info.kind));
        }
    }
}

TEST(a_backend_the_platform_cannot_have_is_not_offered) {
    // Metal exists only on Apple hardware; CUDA has not existed on macOS since
    // 2018. Listing either in the wrong place offers a build that cannot
    // succeed, which is worse than not offering it.
#ifdef __APPLE__
    CHECK(backend_available_here(BackendKind::Metal));
    CHECK(!backend_available_here(BackendKind::Cuda));
#else
    CHECK(!backend_available_here(BackendKind::Metal));
    CHECK(backend_available_here(BackendKind::Cuda));
#endif
    // These two run anywhere, and the CPU one has to: nothing loads without it.
    CHECK(backend_available_here(BackendKind::Cpu));
    CHECK(backend_available_here(BackendKind::Vulkan));
}

TEST(the_install_hint_names_a_package_manager_this_machine_has) {
    // The field it reads from is chosen by what is on PATH rather than assumed,
    // because telling a Fedora user to run apt is worse than telling them
    // nothing at all. Whichever manager this machine has, the hint has to name
    // it and the packages have to be the ones for it.
    const std::string cuda = install_hint(backend_info(BackendKind::Cuda));
    if (!cuda.empty()) {
        const BackendInfo& info = backend_info(BackendKind::Cuda);
        const bool apt    = cuda == "sudo apt install " + std::string(info.apt_packages);
        const bool dnf    = cuda == "sudo dnf install " + std::string(info.dnf_packages);
        const bool pacman = cuda == "sudo pacman -S " + std::string(info.pacman_packages);
        CHECK(apt || dnf || pacman);
    }

    // Metal names no package anywhere: it arrives with the Xcode command line
    // tools, which Homebrew itself requires, so anyone who could run the
    // suggestion already has what it would install.
    CHECK(install_hint(backend_info(BackendKind::Metal)).empty());
}

TEST(ggml_registry_names_map_onto_backends_whatever_their_case) {
    // ggml reports "CUDA" and "Vulkan"; the ids are lower case. Getting this
    // wrong would make every installed runtime report itself as inactive.
    CHECK(backend_from_reg_name("CUDA").has_value());
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("CUDA")),
             static_cast<int>(BackendKind::Cuda));
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("Vulkan")),
             static_cast<int>(BackendKind::Vulkan));
    CHECK_EQ(static_cast<int>(*backend_from_reg_name("CPU")),
             static_cast<int>(BackendKind::Cpu));
    CHECK(!backend_from_reg_name("BLAS").has_value());
}

TEST(a_runtime_built_against_another_llama_cpp_is_reported_as_stale) {
    // Runtimes outlive the Crucible that built them: they survive an uninstall
    // that keeps your data, and a reinstall from newer source lands on top of
    // them. ggml is not ABI-stable across releases, so one built for a
    // different tag loads and then crashes on the first tensor.
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    const std::filesystem::path runtimes = paths::runtimes_dir();
    std::filesystem::create_directories(runtimes);
    // Vulkan, and named the way this platform names a module. CUDA is not
    // offered on a Mac at all, so a fake CUDA module there is never looked at;
    // and libggml-*.so is not a name Windows loads.
    const std::string module = std::string(module_prefix()) + "vulkan"
                             + std::string(module_suffix());
    { std::ofstream out(runtimes / module); out << "not really a module"; }

    const auto status_for = [&](BackendKind kind) {
        for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
            if (status.kind == kind) {
                return status;
            }
        }
        return RuntimeStatus{};
    };

    // The tag this binary wants: recorded, so not stale.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << R"({"vulkan":{"llama_tag":")"
                 << RuntimeStatus::required_llama_tag() << R"("}})";
    }
    CHECK(status_for(BackendKind::Vulkan).installed);
    CHECK(!status_for(BackendKind::Vulkan).stale);

    // Some other tag: stale, and the message needs the tag to name it.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << R"({"vulkan":{"llama_tag":"b0001"}})";
    }
    CHECK(status_for(BackendKind::Vulkan).stale);
    CHECK_EQ(status_for(BackendKind::Vulkan).llama_tag, std::string("b0001"));

    // No manifest entry at all means "cannot tell", which is not a reason to
    // tell someone their working runtime is broken.
    {
        std::ofstream manifest(runtimes / "manifest.json");
        manifest << "{}";
    }
    CHECK(status_for(BackendKind::Vulkan).installed);
    CHECK(!status_for(BackendKind::Vulkan).stale);

    // And a backend with no module is never stale, whatever the manifest says.
    CHECK(!status_for(BackendKind::Cpu).installed);
    CHECK(!status_for(BackendKind::Cpu).stale);
}

TEST(the_cpu_backend_is_the_one_every_other_runtime_needs) {
    // llama.cpp throws "no CPU backend found" whatever GPU is installed, which
    // is why the builder brings this one along with a CUDA or Vulkan install.
    CHECK(backend_info(BackendKind::Cpu).required);
    CHECK(!backend_info(BackendKind::Cuda).required);
    CHECK(!backend_info(BackendKind::Vulkan).required);
}

TEST(removing_a_runtime_that_is_not_installed_says_so) {
    // Scoped, and it has to be: RuntimeRegistry::remove deletes files under
    // the XDG data directory. Without this the test reaches into the runtimes
    // of whoever is running the suite and removes the one it is asking about.
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // Every runtime can be removed now, including the CPU one -- an install
    // with no runtimes at all is the state a fresh install starts in.
    std::string error;
    CHECK(!RuntimeRegistry::remove(BackendKind::Cuda, error));
    CHECK(error.find("not installed") != std::string::npos);
}

TEST(only_multi_device_backends_advertise_gpu_splitting) {
    CHECK(!backend_info(BackendKind::Cpu).multi_device);
    CHECK(backend_info(BackendKind::Cuda).multi_device);
    CHECK(backend_info(BackendKind::Vulkan).multi_device);
}

// ---------------------------------------------------------------------------
// Model shape
//
// The GPU split and the "Dedicated VRAM only" check both need to know how much
// room a model will want before it is loaded. Both used to guess at it; these
// pin the arithmetic that replaced the guess.
// ---------------------------------------------------------------------------

TEST(kv_cache_grows_with_the_context_it_is_asked_for) {
    ModelShape shape;
    shape.known        = true;
    shape.layers       = 32;
    shape.kv_per_token = 1024;

    CHECK_EQ(shape.kv_bytes(1000), std::uint64_t{1024 * 1000});
    // Twice the context is twice the cache, which is the whole reason the
    // context size is the number worth suggesting when a model will not fit.
    CHECK_EQ(shape.kv_bytes(2000), 2 * shape.kv_bytes(1000));
    CHECK_EQ(shape.kv_bytes(0), std::uint64_t{0});
}

TEST(a_model_with_no_readable_attention_shape_claims_no_cache) {
    // Better to under-state a cache we could not measure than to refuse a
    // model on the strength of a number we invented.
    ModelShape shape;
    shape.known = true;
    CHECK_EQ(shape.kv_bytes(8192), std::uint64_t{0});
}

TEST(the_compute_allowance_follows_the_vocabulary_and_the_batch) {
    // The logits buffer is the large part, and it is vocabulary times batch.
    ModelShape small;
    small.vocab = 32000;
    ModelShape large;
    large.vocab = 151936;

    CHECK(large.compute_bytes(512) > small.compute_bytes(512));
    CHECK(small.compute_bytes(1024) > small.compute_bytes(512));
    // A batch of zero is not a reason to return nothing: there is still a
    // graph to run.
    CHECK(small.compute_bytes(0) > 0);
}

TEST(the_total_is_the_weights_plus_the_cache_plus_the_compute) {
    ModelShape shape;
    shape.known        = true;
    shape.weights      = 8ULL * 1024 * 1024 * 1024;
    shape.layers       = 32;
    shape.vocab        = 32000;
    shape.kv_per_token = 2048;

    CHECK_EQ(shape.resident_bytes(4096), shape.weights + shape.kv_bytes(4096));
    CHECK_EQ(shape.total_bytes(4096, 512),
             shape.resident_bytes(4096) + shape.compute_bytes(512));
    // The split divides what follows the layers; the compute buffers do not.
    CHECK(shape.total_bytes(4096, 512) > shape.resident_bytes(4096));
}

TEST(a_file_that_is_not_a_gguf_reports_its_size_and_nothing_else) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "not-a-model.gguf";
    { std::ofstream out(file); out << "this is not a model"; }

    const ModelShape shape = read_model_shape(file);
    CHECK(!shape.known);
    CHECK(shape.weights > 0);   // the caller can still fall back on the size
    CHECK_EQ(shape.kv_per_token, std::uint64_t{0});
}

TEST(a_missing_file_has_no_shape_at_all) {
    TempDir dir;
    const ModelShape shape = read_model_shape(dir.path() / "gone.gguf");
    CHECK(!shape.known);
    CHECK_EQ(shape.weights, std::uint64_t{0});
}

TEST(a_gguf_header_is_read_for_its_attention_shape) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "plain.gguf";
    write_test_gguf(file, /*layers=*/32, /*embd=*/4096, /*heads=*/32,
                    /*kv_heads=*/{8}, /*key_len=*/128, /*vocab=*/32000);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{32});
    CHECK_EQ(shape.vocab, std::uint32_t{32000});
    // 32 layers x 8 KV heads x (128 + 128) x 2 bytes.
    CHECK_EQ(shape.kv_per_token, std::uint64_t{32} * 8 * 256 * 2);
}

TEST(a_hybrid_model_is_not_charged_for_the_layers_that_keep_no_cache) {
    // LFM2 and friends write head_count_kv as one value per layer, with a zero
    // for every layer that has no KV cache. Reading only the first value would
    // over-count such a model several times over -- and reading the scalar
    // form of the key would abort, since it is an array.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "hybrid.gguf";
    write_test_gguf(file, /*layers=*/4, /*embd=*/2048, /*heads=*/32,
                    /*kv_heads=*/{0, 0, 8, 0}, /*key_len=*/64, /*vocab=*/65536);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{4});
    // Only the third layer costs anything: 8 heads x (64 + 64) x 2 bytes.
    CHECK_EQ(shape.kv_per_token, std::uint64_t{8} * 128 * 2);
}

TEST(a_tensor_table_says_which_blocks_actually_cache) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "hybrid-tensors.gguf";
    // 12 blocks, every fourth one attending -- the shape of a qwen3next, whose
    // header states one KV head count for all 48 blocks when only 12 have a
    // cache at all. Reading the tensors is what tells them apart.
    write_test_gguf_with_tensors(file, /*layers=*/12, /*attention_every=*/4,
                                 /*embd=*/64, /*kv_width=*/16, /*vocab=*/128);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.layers, std::uint32_t{12});
    // 12 blocks plus the output.
    CHECK_EQ(shape.units.size(), std::size_t{13});

    int caching = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        if (shape.units[i].kv_per_token > 0) {
            ++caching;
            // K and V are 16 wide each, two bytes an element.
            CHECK_EQ(shape.units[i].kv_per_token, std::uint64_t{(16 + 16) * 2});
            CHECK_EQ(shape.units[i].state, std::uint64_t{0});
        } else {
            CHECK(shape.units[i].state > 0);  // recurrent instead
        }
    }
    CHECK_EQ(caching, 3);
    CHECK_EQ(shape.kv_per_token, std::uint64_t{3 * (16 + 16) * 2});
    // The output unit holds output.weight, and caches nothing.
    CHECK(shape.units.back().weights > 0);
    CHECK_EQ(shape.units.back().kv_per_token, std::uint64_t{0});
}

TEST(the_input_embedding_is_not_charged_to_a_card) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "embedding.gguf";
    write_test_gguf_with_tensors(file, /*layers=*/4, /*attention_every=*/1,
                                 /*embd=*/64, /*kv_width=*/16, /*vocab=*/1024);

    const ModelShape shape = read_model_shape(file);
    // llama.cpp keeps the input layer in system memory whatever the offload
    // settings say, so counting it against video memory refuses models that fit.
    CHECK_EQ(shape.host_weights, std::uint64_t{64} * 1024 * sizeof(float));

    // And it is not also counted among the units, which is what a card is
    // asked to divide. The output projection is the same size and *is* placed,
    // so the two are only distinguishable by name.
    std::uint64_t placed = 0;
    for (const ModelUnit& unit : shape.units) {
        placed += unit.weights;
    }
    CHECK_EQ(shape.units.back().weights,
             shape.host_weights + std::uint64_t{64} * sizeof(float));  // output + its norm
    CHECK(placed > shape.host_weights);
}

TEST(a_header_without_a_tensor_table_still_reports_its_cache) {
    // The metadata-only fixture: nothing to place layer by layer, so `units`
    // stays empty and the split falls back to dividing proportionally -- but
    // the cache is still worth knowing, and the header alone gives it.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "bare.gguf";
    write_test_gguf(file, /*layers=*/4, /*embd=*/64, /*heads=*/4,
                    /*kv_heads=*/{2}, /*key_len=*/16, /*vocab=*/128);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK(shape.units.empty());
    CHECK_EQ(shape.kv_per_token, std::uint64_t{4} * 2 * (16 + 16) * 2);
}

TEST(a_stated_head_dimension_beats_dividing_the_embedding_by_the_heads) {
    // qwen3next states a key length of 256 against an embedding of 2048 over
    // 16 heads, which would derive as 128 -- half the real cache.
    TempDir dir;
    const std::filesystem::path file = dir.path() / "stated.gguf";
    write_test_gguf(file, /*layers=*/48, /*embd=*/2048, /*heads=*/16,
                    /*kv_heads=*/{2}, /*key_len=*/256, /*vocab=*/151936);

    const ModelShape shape = read_model_shape(file);
    CHECK(shape.known);
    CHECK_EQ(shape.kv_per_token, std::uint64_t{48} * 2 * 512 * 2);
}

// ---------------------------------------------------------------------------
// Resource monitor
// ---------------------------------------------------------------------------

TEST(a_gpu_line_becomes_a_reading) {
    util::ResourceSample sample;
    CHECK(util::parse_gpu_line("NVIDIA GeForce RTX 4070, 2749, 12282, 55, 47", sample));
    CHECK_EQ(sample.name, "NVIDIA GeForce RTX 4070");
    CHECK_EQ(sample.used,  std::uint64_t{2749} * 1024 * 1024);
    CHECK_EQ(sample.total, std::uint64_t{12282} * 1024 * 1024);
    CHECK_EQ(sample.temperature_c, 55);
    CHECK_EQ(sample.busy_percent, 47);
    CHECK_EQ(sample.memory_percent(), 22);
}

TEST(a_card_that_reports_no_sensor_is_not_a_parse_failure) {
    // Plenty of cards report no temperature, and some report no utilization.
    // That is a fact about the card, and the memory figures are still wanted.
    util::ResourceSample sample;
    CHECK(util::parse_gpu_line("Quadro K600, 100, 1024, [N/A], [N/A]", sample));
    CHECK_EQ(sample.temperature_c, -1);
    CHECK_EQ(sample.busy_percent, -1);
    CHECK_EQ(sample.memory_percent(), 10);

    // But a line with no memory in it is not a reading at all.
    CHECK(!util::parse_gpu_line("something went wrong", sample));
    CHECK(!util::parse_gpu_line("", sample));
}

TEST(memory_is_measured_as_available_not_free) {
    // The distinction that matters on a machine running Crucible: after reading a
    // 30 GB model, nearly all of "free" memory is page cache. Reporting 98%
    // used would be true of nothing anybody cares about.
    const std::string meminfo =
        "MemTotal:       49000000 kB\n"
        "MemFree:          800000 kB\n"
        "MemAvailable:   40000000 kB\n"
        "Buffers:          100000 kB\n";
    std::uint64_t used  = 0;
    std::uint64_t total = 0;
    CHECK(util::parse_meminfo(meminfo, used, total));
    CHECK_EQ(total, std::uint64_t{49000000} * 1024);
    CHECK_EQ(used,  std::uint64_t{9000000} * 1024);

    CHECK(!util::parse_meminfo("nothing useful here\n", used, total));
}

TEST(macos_page_counts_become_bytes_used) {
    // Trimmed from real vm_stat output. The trailing full stop is part of the
    // format, and the page size is 16 KiB on Apple silicon rather than 4.
    const std::string vm_stat =
        "Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
        "Pages free:                       100.\n"
        "Pages active:                     500.\n"
        "Pages inactive:                   200.\n"
        "Pages speculative:                 50.\n"
        "Pages wired down:                 150.\n"
        "Pages purgeable:                   25.\n";

    const std::uint64_t page  = 16384;
    const std::uint64_t total = std::uint64_t{1000} * page;
    std::uint64_t used = 0;
    CHECK(util::parse_vm_stat(vm_stat, page, total, used));

    // Free, speculative and purgeable are all available without evicting
    // anything anyone wants, which is 175 of the 1000 pages.
    CHECK_EQ(used, std::uint64_t{825} * page);

    // Nothing recognizable is not a reading, however long the text is.
    CHECK(!util::parse_vm_stat("no pages here\n", page, total, used));
    CHECK(!util::parse_vm_stat(vm_stat, page, /*total=*/0, used));
}

TEST(processor_time_is_split_into_busy_and_total) {
    // Fields: user nice system idle iowait irq softirq steal. Idle and iowait
    // are the two that are not work.
    std::uint64_t busy  = 0;
    std::uint64_t total = 0;
    CHECK(util::parse_stat("cpu  100 20 30 700 50 0 0 0\ncpu0 1 2 3 4\n", busy, total));
    CHECK_EQ(total, std::uint64_t{900});
    CHECK_EQ(busy,  std::uint64_t{150});

    // The per-core lines are not the aggregate, and must not be read as one.
    CHECK(!util::parse_stat("cpu0 1 2 3 4\n", busy, total));
}

TEST(a_processor_name_is_a_name_not_a_legal_notice) {
    CHECK_EQ(util::parse_cpu_name("model name\t: 12th Gen Intel(R) Core(TM) i5-12400\n"),
             "12th Gen Intel Core i5-12400");
    CHECK_EQ(util::parse_cpu_name("model name : AMD Ryzen 9 7950X 16-Core Processor\n"),
             "AMD Ryzen 9 7950X 16-Core");
    // The clock is not part of the name, and on a modern part it is not the
    // clock either.
    CHECK_EQ(util::parse_cpu_name("model name : Intel(R) Xeon(R) CPU E5-2670 @ 2.60GHz\n"),
             "Intel Xeon E5-2670");
    CHECK(util::parse_cpu_name("flags : fpu vme\n").empty());
}
