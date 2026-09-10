// SPDX-License-Identifier: MIT
//
// GPUs: the priority order, how a model is split across cards, and the memory
// policy that decides how much of one to use.
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// GPU priority order
//
// The order is edited in a panel and stored as device indices, so the config
// and the machine can disagree: a card unplugged since, or added since. What
// the panel shows has to be the machine's cards in the config's order, and
// nothing else.
// ---------------------------------------------------------------------------

namespace {

ComputeDevice gpu_at(int index, std::string name) {
    ComputeDevice device;
    device.index       = index;
    device.name        = std::move(name);
    device.description = device.name;
    device.is_gpu      = true;
    return device;
}

/// The device indices, as "2,0,1" -- a string so a mismatch prints both orders
/// rather than the address of a vector.
std::string indices_of(const std::vector<ComputeDevice>& devices) {
    std::string out;
    for (const ComputeDevice& device : devices) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(device.index);
    }
    return out;
}

}  // namespace

TEST(a_configured_gpu_order_is_laid_over_the_cards_present) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2, 0, 1})), std::string{"2,0,1"});
}

TEST(an_unmentioned_gpu_goes_to_the_end_rather_than_vanishing) {
    // A card added since the order was written. Dropping it would quietly stop
    // Crucible using hardware the machine has.
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2})), std::string{"2,0,1"});
}

TEST(an_order_naming_a_gpu_that_is_gone_leaves_no_hole) {
    // The config was written with three cards; two are plugged in today.
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {2, 1, 0})), std::string{"2,0"});
}

TEST(a_gpu_listed_twice_takes_only_its_first_place) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B")};
    const std::vector<ComputeDevice> ordered = apply_priority_order(gpus, {1, 1, 0});
    CHECK_EQ(ordered.size(), std::size_t{2});
    CHECK_EQ(indices_of(ordered), std::string{"1,0"});
}

TEST(no_configured_order_leaves_the_cards_in_ggml_order) {
    const std::vector<ComputeDevice> gpus{gpu_at(0, "A"), gpu_at(1, "B"), gpu_at(2, "C")};
    CHECK_EQ(indices_of(apply_priority_order(gpus, {})), std::string{"0,1,2"});
}

TEST(the_vulkan_backend_asks_for_the_spirv_headers) {
    // The package that was missing from the installer, and whose absence made
    // the Vulkan build fail at configure time with an error naming a CMake
    // package rather than anything installable.
    const std::string apt(backend_info(BackendKind::Vulkan).apt_packages);
    CHECK(apt.find("spirv-headers") != std::string::npos);
    CHECK(apt.find("glslc") != std::string::npos);
    CHECK(apt.find("libvulkan-dev") != std::string::npos);
}

// ---------------------------------------------------------------------------
// GPU splitting
// ---------------------------------------------------------------------------

namespace {

ComputeDevice fake_gpu(int index, std::string name, std::uint64_t bytes) {
    ComputeDevice device;
    device.index        = index;
    device.name         = name;
    device.description  = std::move(name);
    device.memory_total = bytes;
    device.memory_free  = bytes;
    device.is_gpu       = true;
    return device;
}

constexpr std::uint64_t kGb = 1024ULL * 1024ULL * 1024ULL;

/// Replay llama.cpp's own layer assignment for a `tensor_split`.
///
/// Copied from llama-model.cpp: the split is made cumulative, normalized, and
/// unit `i` goes to the first device whose share exceeds `i / total`. Anything
/// this file claims about layer counts is only true if it survives this.
std::vector<int> llama_cpp_assignment(const std::vector<float>& split, int units) {
    std::vector<float> cumulative(split.size(), 0.0F);
    float running = 0.0F;
    for (std::size_t i = 0; i < split.size(); ++i) {
        running      += split[i];
        cumulative[i] = running;
    }
    for (float& value : cumulative) {
        value /= running;
    }

    std::vector<int> counts(split.size(), 0);
    for (int unit = 0; unit < units; ++unit) {
        const auto share = static_cast<float>(unit) / static_cast<float>(units);
        const auto found = std::upper_bound(cumulative.begin(), cumulative.end(), share);
        const auto device = static_cast<std::size_t>(std::distance(cumulative.begin(), found));
        if (device < counts.size()) {
            ++counts[device];
        }
    }
    return counts;
}

float sum_of(const std::vector<float>& split) {
    float total = 0.0F;
    for (const float share : split) {
        total += share;
    }
    return total;
}

}  // namespace

TEST(the_lowest_priority_card_never_holds_more_than_a_higher_one) {
    // The property a user checks by watching nvidia-smi, on three equal cards
    // with a model that needs about half of them.
    //
    // The card last in the order does inherit the rounding slack the earlier
    // ones leave -- at most one unit each, because they are filled to capacity
    // and a layer cannot be split -- so this is "never more", not "exactly the
    // least by target".
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "first",  10 * kGb),
        fake_gpu(1, "second", 10 * kGb),
        fake_gpu(2, "third",  10 * kGb),
    };

    // Twenty-one units of a gigabyte each: not a whole number of cards, so
    // there is slack to misplace.
    ModelFit fit;
    fit.units.assign(21, kGb);
    fit.resident = 21 * kGb;

    const std::vector<int> counts =
        plan_gpu_split(GpuSplitMode::Priority, gpus, {0, 1, 2}, 0, fit).units;
    CHECK_EQ(counts.size(), std::size_t{3});
    if (counts.size() != 3) {
        return;
    }

    // Priority means the cards named first are filled first, so the last one
    // must not end up with more than either of them.
    CHECK(counts[2] <= counts[0]);
    CHECK(counts[2] <= counts[1]);

    // And every unit is still placed, and no card is over what it can hold.
    CHECK_EQ(counts[0] + counts[1] + counts[2], 21);
    for (const int on_card : counts) {
        CHECK(static_cast<std::uint64_t>(on_card) * kGb
              <= usable_memory(gpus[0], kCardHeadroom));
    }
}

TEST(reversing_the_priority_order_reverses_the_split) {
    // The property a user actually checks: put a card last and it should hold
    // less than when you put it first. Before honor_targets these two calls
    // returned the same answer.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "first",  10 * kGb),
        fake_gpu(1, "second", 10 * kGb),
        fake_gpu(2, "third",  10 * kGb),
    };
    ModelFit fit;
    fit.units.assign(21, kGb);
    fit.resident = 21 * kGb;

    const std::vector<int> forwards =
        plan_gpu_split(GpuSplitMode::Priority, gpus, {0, 1, 2}, 0, fit).units;
    const std::vector<int> backwards =
        plan_gpu_split(GpuSplitMode::Priority, gpus, {2, 1, 0}, 0, fit).units;

    CHECK_EQ(forwards.size(), std::size_t{3});
    CHECK_EQ(backwards.size(), std::size_t{3});
    if (forwards.size() != 3 || backwards.size() != 3) {
        return;
    }
    CHECK(backwards[2] >= forwards[2]);
    CHECK(backwards[0] <= forwards[0]);
}

TEST(the_split_handed_to_llama_is_indexed_by_gpu_position) {
    // llama.cpp indexes tensor_split by position among the GPUs it selected,
    // not by ggml device index. Those agree only when the GPUs occupy the first
    // indices -- true with CUDA alone, and false the moment something else
    // registers first. Crucible plans in device-index space because that is
    // what /devices prints, so the conversion happens once, at the boundary.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(3, "first",  8 * kGb),
        fake_gpu(5, "second", 8 * kGb),
    };
    // A plan in device-index space: index 3 gets a quarter, index 5 the rest.
    std::vector<float> planned(6, 0.0F);
    planned[3] = 0.25F;
    planned[5] = 0.75F;

    const std::vector<float> handed = llama_tensor_split(gpus, planned);

    // Sized to what llama.cpp reads out of the pointer, not to what we planned.
    // It copies llama_max_devices() floats, and a shorter buffer is read past
    // its end.
    CHECK_EQ(handed.size(), static_cast<std::size_t>(llama_max_devices()));
    CHECK(std::abs(handed[0] - 0.25F) < 0.001F);
    CHECK(std::abs(handed[1] - 0.75F) < 0.001F);
    for (std::size_t i = 2; i < handed.size(); ++i) {
        CHECK(handed[i] == 0.0F);
    }

    // An empty plan stays empty: that is llama.cpp's own default, which is to
    // divide across every GPU itself.
    CHECK(llama_tensor_split(gpus, {}).empty());
}

TEST(the_main_gpu_handed_to_llama_is_a_position_too) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(3, "first",  8 * kGb),
        fake_gpu(5, "second", 8 * kGb),
    };
    CHECK_EQ(llama_main_gpu(gpus, 3), 0);
    CHECK_EQ(llama_main_gpu(gpus, 5), 1);
    // A card that is no longer there falls back to the first, which is what
    // llama.cpp would have used anyway. An out-of-range value makes it refuse
    // the load outright.
    CHECK_EQ(llama_main_gpu(gpus, 9), 0);
}

TEST(auto_split_leaves_the_decision_to_llama_cpp) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb), fake_gpu(1, "B", 8 * kGb)};
    CHECK(compute_tensor_split(GpuSplitMode::Auto, gpus, {}, 0).empty());
}

TEST(a_single_gpu_is_never_split) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb)};
    CHECK(compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0).empty());
    CHECK(compute_tensor_split(GpuSplitMode::Priority, gpus, {}, 0).empty());
}

TEST(even_split_is_proportional_to_memory_not_to_device_count) {
    // The whole point: a 16 GB card should take twice the work of an 8 GB one,
    // or the small card runs out first and a model that would have fit fails.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "big", 16 * kGb),
        fake_gpu(1, "small", 8 * kGb),
    };
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0);

    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    // Proportional to what each card can *offer*, which is its memory less the
    // headroom every card keeps back -- so a little over two thirds rather than
    // exactly two thirds, and the smaller card gives up proportionally more.
    const auto big   = static_cast<float>(usable_memory(gpus[0], kCardHeadroom));
    const auto small = static_cast<float>(usable_memory(gpus[1], kCardHeadroom));
    CHECK(std::abs(split[0] - big / (big + small)) < 0.001F);
    CHECK(std::abs(split[1] - small / (big + small)) < 0.001F);
    CHECK(split[0] > 2.0F / 3.0F);
}

TEST(even_split_falls_back_to_equal_shares_when_memory_is_unknown) {
    std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 0), fake_gpu(1, "B", 0)};
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 0);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[0] - 0.5F) < 0.001F);
    CHECK(std::abs(split[1] - 0.5F) < 0.001F);
}

TEST(priority_split_fills_the_first_card_before_touching_the_next) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "slow", 8 * kGb),
        fake_gpu(1, "fast", 8 * kGb),
    };
    // Device 1 is named first, and a model small enough to live there alone
    // must not be spread at all -- that is the whole point of "priority".
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 0}, 0, ModelFit{4 * kGb, 0, 0, {}});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
    CHECK(std::abs(split[0]) < 0.001F);
}

TEST(priority_split_spills_only_what_the_first_card_cannot_hold) {
    // 10 GB of usable space per card after headroom, and a 14 GB model: the
    // first card takes what it can and the second takes the remainder. The
    // old fixed falloff gave the first card 80% of everything regardless,
    // which is how a model that fits across the machine failed to load.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 10 * kGb),
        fake_gpu(1, "B", 10 * kGb),
    };
    const std::uint64_t usable = usable_memory(gpus[0], kCardHeadroom);
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{14 * kGb, 0, 0, {}});

    CHECK_EQ(split.size(), std::size_t{2});
    const auto expected_first = static_cast<float>(usable) / static_cast<float>(14 * kGb);
    CHECK(std::abs(split[0] - expected_first) < 0.01F);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    // And the first card is never asked for more than it holds.
    CHECK(split[0] * static_cast<float>(14 * kGb) <= static_cast<float>(usable) + 1.0F);
}

TEST(no_card_is_ever_asked_for_more_than_it_can_hold) {
    // The bug this exists for, with the real shape of it: three cards of
    // 12/16/12 GB, the middle one ranked first, and a 32 GB model. The fixed
    // falloff handed the 16 GB card 76% -- 24 GB of weights -- and left 13 GB
    // untouched on the other two.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "RTX 4070",    12 * kGb),
        fake_gpu(1, "RTX 5060 Ti", 16 * kGb),
        fake_gpu(2, "RTX 3060",    12 * kGb),
    };
    const std::uint64_t model = 32 * kGb;
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 2, 0}, 0, ModelFit{model, 0, 0, {}});

    CHECK_EQ(split.size(), std::size_t{3});
    for (const ComputeDevice& gpu : gpus) {
        const auto share = split[static_cast<std::size_t>(gpu.index)];
        const auto bytes = static_cast<std::uint64_t>(share * static_cast<float>(model));
        CHECK(bytes <= usable_memory(gpu, kCardHeadroom) + kGb / 64);
    }
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
    // Ranked first, so it carries the most.
    CHECK(split[1] > split[2]);
    CHECK(split[1] > split[0]);
}

TEST(priority_split_places_unranked_devices_after_ranked_ones) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 8 * kGb),
        fake_gpu(1, "B", 8 * kGb),
        fake_gpu(2, "C", 8 * kGb),
    };
    // Only device 2 is ranked. A GPU appearing later must never outrank one
    // that was deliberately placed, so a model that fits on one card goes
    // entirely to device 2.
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {2}, 0, ModelFit{4 * kGb, 0, 0, {}});
    CHECK_EQ(split.size(), std::size_t{3});
    CHECK(std::abs(split[2] - 1.0F) < 0.001F);
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[1]) < 0.001F);
}

TEST(priority_split_ignores_repeats_and_devices_that_are_not_there) {
    const std::vector<ComputeDevice> gpus{fake_gpu(0, "A", 8 * kGb), fake_gpu(1, "B", 8 * kGb)};
    // A device listed twice would otherwise take two shares of the split, and
    // device 7 is not in the machine at all.
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {1, 1, 7}, 0, ModelFit{4 * kGb, 0, 0, {}});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(a_bigger_model_reaches_further_down_the_priority_order) {
    // The reason the split is computed per-model rather than per-machine: the
    // same priority order means different things for a 1B delegator and a 30B
    // expert, and stamping one arrangement on both would either strand the
    // small model across three cards or refuse the large one.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
        fake_gpu(2, "C", 12 * kGb),
    };
    const std::vector<int> order{0, 1, 2};

    const std::vector<float> small =
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{2 * kGb, 0, 0, {}});
    const std::vector<float> large =
        compute_tensor_split(GpuSplitMode::Priority, gpus, order, 0, ModelFit{25 * kGb, 0, 0, {}});

    // The small one never leaves the first card.
    CHECK(std::abs(small[0] - 1.0F) < 0.001F);
    CHECK(std::abs(small[2]) < 0.001F);
    // The large one needs all three.
    CHECK(large[0] > 0.0F);
    CHECK(large[1] > 0.0F);
    CHECK(large[2] > 0.0F);
}

TEST(a_model_too_large_for_the_machine_is_divided_by_capacity) {
    // It will not load whatever is done here, and "Dedicated VRAM only" is
    // what turns that into a message. But piling the overflow onto one card
    // would make the failure worse than it has to be, so the split falls back
    // to what each card can hold.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "small", 4 * kGb),
        fake_gpu(1, "big",  12 * kGb),
    };
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{400 * kGb, 0, 0, {}});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(split[1] > split[0]);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(priority_falls_back_to_capacity_when_the_model_size_is_unknown) {
    // An unfilled seat, or a path that no longer resolves. Dividing by
    // capacity at least never hands a card more than its share.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "small", 4 * kGb),
        fake_gpu(1, "big",  12 * kGb),
    };
    const std::vector<float> split =
        compute_tensor_split(GpuSplitMode::Priority, gpus, {0, 1}, 0, ModelFit{});
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(split[1] > split[0]);
    CHECK(std::abs(sum_of(split) - 1.0F) < 0.001F);
}

TEST(single_mode_puts_everything_on_the_main_gpu) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 8 * kGb),
        fake_gpu(1, "B", 8 * kGb),
    };
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Single, gpus, {}, 1);
    CHECK_EQ(split.size(), std::size_t{2});
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[1] - 1.0F) < 0.001F);
}

TEST(the_split_vector_is_indexed_by_ggml_device_index_not_by_position) {
    // A machine whose GPUs are devices 1 and 3 (device 0 being the CPU) must
    // produce a four-long vector, or llama.cpp reads the weights off the end.
    const std::vector<ComputeDevice> gpus{fake_gpu(1, "A", 8 * kGb), fake_gpu(3, "B", 8 * kGb)};
    const std::vector<float> split = compute_tensor_split(GpuSplitMode::Even, gpus, {}, 1);
    CHECK_EQ(split.size(), std::size_t{4});
    CHECK(std::abs(split[0]) < 0.001F);
    CHECK(std::abs(split[2]) < 0.001F);
    CHECK(split[1] > 0.0F);
    CHECK(split[3] > 0.0F);
}

TEST(a_plan_lands_on_the_layer_counts_it_asked_for) {
    // The whole reason the split is built from unit counts rather than from a
    // proportion. llama.cpp places whole layers, so the only way to know a card
    // will hold what the arithmetic promised is to aim at a boundary -- and the
    // only way to know that worked is to replay llama.cpp's own rule.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 10 * kGb),
        fake_gpu(1, "B", 20 * kGb),
        fake_gpu(2, "C", 10 * kGb),
    };
    ModelFit fit;
    fit.units.assign(48, kGb / 2);          // 48 half-gigabyte layers
    fit.units.push_back(kGb / 4);           // plus a smaller output unit
    for (const std::uint64_t unit : fit.units) {
        fit.resident += unit;
    }

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {1, 0, 2}, 0, fit);
    CHECK(!plan.split.empty());
    CHECK_EQ(plan.units.size(), std::size_t{3});

    const std::vector<int> actual =
        llama_cpp_assignment(plan.split, static_cast<int>(fit.units.size()));
    CHECK_EQ(actual.size(), plan.units.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK_EQ(actual[i], plan.units[i]);
    }
}

TEST(a_plan_lands_where_it_aimed_across_a_thousand_machines) {
    // The strong form of the test above, because one worked example is not
    // enough to trust arithmetic that has to land exactly on a boundary
    // llama.cpp computes in single precision from numbers of its own. A
    // thousand shapes of machine and model, and every one of them has to place
    // the layers this file said it would.
    std::mt19937 random(20240607);
    std::uniform_int_distribution<int> card_count(2, 5);
    std::uniform_int_distribution<int> card_gb(2, 32);
    std::uniform_int_distribution<int> unit_count(4, 120);
    std::uniform_int_distribution<int> unit_mb(40, 1200);

    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<ComputeDevice> gpus;
        const int cards = card_count(random);
        for (int i = 0; i < cards; ++i) {
            gpus.push_back(fake_gpu(i, "card" + std::to_string(i),
                                    static_cast<std::uint64_t>(card_gb(random)) * kGb));
        }
        std::vector<int> order;
        for (int i = 0; i < cards; ++i) {
            order.push_back(i);
        }
        std::shuffle(order.begin(), order.end(), random);

        ModelFit fit;
        const int units = unit_count(random);
        for (int i = 0; i < units; ++i) {
            fit.units.push_back(static_cast<std::uint64_t>(unit_mb(random)) * 1024 * 1024);
            fit.resident += fit.units.back();
        }
        fit.per_card = static_cast<std::uint64_t>(unit_mb(random)) * 1024 * 1024 / 4;

        const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, order, 0, fit);
        if (plan.units.empty()) {
            continue;  // no memory to plan against; nothing is claimed
        }

        const std::vector<int> actual = llama_cpp_assignment(plan.split, units);
        for (std::size_t i = 0; i < plan.units.size(); ++i) {
            if (actual[i] != plan.units[i]) {
                CHECK_EQ(actual[i], plan.units[i]);
                return;  // one report is enough; a thousand would bury it
            }
        }
    }
}

TEST(a_card_the_priority_order_held_back_is_used_when_the_last_one_overflows) {
    // The greedy pass fills each card to its target and hands the leftovers to
    // whichever card is last in *index* order -- which is not the card the
    // priority order put last. So a card the order deliberately held short can
    // be sitting on spare capacity while another is handed more than it holds.
    //
    // Card 1 is ranked last, so priority gives it only what is left over; card 2
    // is last by index, so it receives the rounding slack. Without the
    // rebalance card 2 is overfilled and card 1 keeps room it was never asked
    // to give up.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "first",  4 * kGb),
        fake_gpu(1, "held",  16 * kGb),
        fake_gpu(2, "last",   4 * kGb),
    };
    ModelFit fit;
    fit.per_card = 0;
    for (int i = 0; i < 20; ++i) {
        fit.units.push_back(kGb);   // 20 GB in one-gigabyte layers
        fit.resident += kGb;
    }

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {0, 2, 1}, 0, fit);
    CHECK_EQ(plan.units.size(), std::size_t{3});

    for (const ComputeDevice& gpu : gpus) {
        const auto index = static_cast<std::size_t>(gpu.index);
        const auto held  = static_cast<std::uint64_t>(plan.units[index]) * kGb;
        CHECK(held <= usable_memory(gpu, kCardHeadroom));
    }
    int placed = 0;
    for (const int count : plan.units) {
        placed += count;
    }
    CHECK_EQ(placed, 20);
}

TEST(the_priority_card_takes_the_most_layers) {
    // Equal cards, so the only thing that can decide which carries more is the
    // order the user put them in.
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
        fake_gpu(2, "C", 12 * kGb),
    };
    ModelFit fit;
    fit.units.assign(30, kGb);
    fit.resident = 30 * kGb;

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {2, 0, 1}, 0, fit);
    CHECK_EQ(plan.units.size(), std::size_t{3});
    CHECK(plan.units[2] >= plan.units[0]);
    CHECK(plan.units[2] >= plan.units[1]);
}

TEST(a_model_that_fits_on_the_first_card_never_leaves_it) {
    const std::vector<ComputeDevice> gpus{
        fake_gpu(0, "A", 12 * kGb),
        fake_gpu(1, "B", 12 * kGb),
    };
    ModelFit fit;
    fit.units.assign(8, kGb / 4);
    fit.resident = 2 * kGb;

    const GpuPlan plan = plan_gpu_split(GpuSplitMode::Priority, gpus, {1, 0}, 0, fit);
    CHECK_EQ(plan.units[0], 0);
    CHECK_EQ(plan.units[1], 8);
    // And llama.cpp agrees.
    const std::vector<int> actual = llama_cpp_assignment(plan.split, 8);
    CHECK_EQ(actual[0], 0);
    CHECK_EQ(actual[1], 8);
}

TEST(a_setting_the_hardware_cannot_honor_says_so) {
    // The failure this exists for is silent: a split mode saved on a machine
    // with one card reads as configured, saves, and does nothing.
    ComputeDevice cpu;
    cpu.index   = 3;
    cpu.name    = "CPU";
    cpu.backend = "CPU";
    cpu.is_gpu  = false;

    // Nothing but a processor.
    {
        const GpuSettingSupport support = gpu_setting_support({cpu});
        CHECK(!support.split.empty());
        CHECK(!support.gpu_only.empty());
        CHECK(!support.vram_only.empty());
        // The reason has to name the way out, or a dimmed row is just a dimmed
        // row.
        CHECK(support.split.find("Runtimes") != std::string::npos);
    }

    // One card. Dividing a model between cards is meaningless; keeping the work
    // off the processor and refusing an oversized model are not.
    {
        ComputeDevice one = fake_gpu(0, "RTX 4070", 12 * kGb);
        one.backend       = "CUDA";
        const GpuSettingSupport support = gpu_setting_support({one, cpu});
        CHECK(!support.split.empty());
        CHECK(support.split.find("CUDA") != std::string::npos);
        CHECK(support.gpu_only.empty());
        CHECK(support.vram_only.empty());
    }

    // Two cards on a backend that spreads a model: everything works.
    {
        ComputeDevice a = fake_gpu(0, "RTX 4070", 12 * kGb);
        ComputeDevice b = fake_gpu(1, "RTX 3060", 12 * kGb);
        a.backend = b.backend = "CUDA";
        const GpuSettingSupport support = gpu_setting_support({a, b, cpu});
        CHECK(support.split.empty());
        CHECK(support.gpu_only.empty());
        CHECK(support.vram_only.empty());
    }
}

TEST(a_backend_that_runs_one_device_cannot_be_asked_to_split) {
    // Metal is the case in hand: unified memory and one GPU, so there is
    // neither anything to divide nor anywhere to divide it to. Two devices
    // reported by such a backend would still not make a split possible.
    ComputeDevice a = fake_gpu(0, "Apple M3 Max", 48 * kGb);
    ComputeDevice b = fake_gpu(1, "Apple M3 Max", 48 * kGb);
    a.backend = b.backend = "MTL";

    const GpuSettingSupport support = gpu_setting_support({a, b});
    CHECK(!support.split.empty());
    CHECK(support.split.find("Metal") != std::string::npos);  // its name, not ggml's id
    // The memory settings are still real there: Metal reports a working set,
    // and going past it makes the machine swap.
    CHECK(support.gpu_only.empty());
    CHECK(support.vram_only.empty());
}

TEST(a_backend_that_reports_no_memory_cannot_refuse_a_model) {
    // vram_only compares a model against free device memory. A backend that
    // reports none makes it a setting that saves and then does nothing, which
    // is the state this whole mechanism exists to make visible.
    ComputeDevice a = fake_gpu(0, "some GPU", 0);
    ComputeDevice b = fake_gpu(1, "some GPU", 0);
    a.backend = b.backend = "Vulkan";
    a.memory_free = b.memory_free = 0;

    const GpuSettingSupport support = gpu_setting_support({a, b});
    CHECK(!support.vram_only.empty());
    CHECK(support.vram_only.find("Vulkan") != std::string::npos);
    // Two cards on a splitting backend, so this one is still fine.
    CHECK(support.split.empty());
    CHECK(support.gpu_only.empty());
}

TEST(split_modes_round_trip_through_their_ids) {
    for (const GpuSplitMode mode : {GpuSplitMode::Auto, GpuSplitMode::Even,
                                    GpuSplitMode::Priority, GpuSplitMode::Single}) {
        CHECK_EQ(static_cast<int>(gpu_split_mode_from_id(gpu_split_mode_id(mode))),
                 static_cast<int>(mode));
    }
    // Anything unrecognized must be the safe option, not a crash.
    CHECK_EQ(static_cast<int>(gpu_split_mode_from_id("nonsense")),
             static_cast<int>(GpuSplitMode::Auto));
}

TEST(the_gpu_policy_leaves_a_hand_written_split_alone_in_auto_mode) {
    Config config;
    config.gpu.mode = "auto";
    config.defaults.tensor_split = {0.7F, 0.3F};

    CHECK(apply_gpu_policy(config).empty());
    CHECK_EQ(config.defaults.tensor_split.size(), std::size_t{2});
    CHECK(std::abs(config.defaults.tensor_split[0] - 0.7F) < 0.001F);
}

// ---------------------------------------------------------------------------
// GPU memory policy
//
// "GPU-only compute" and "Dedicated VRAM only" are settings about the machine;
// apply_gpu_policy is what turns them into the per-model flags llama.cpp is
// actually handed.
// ---------------------------------------------------------------------------

TEST(vram_only_is_stamped_onto_every_model) {
    Config config;
    config.gpu.vram_only = true;
    apply_gpu_policy(config);

    // The delegator as well as the experts: it is resident for the whole
    // session, so it is the last model that should be allowed to sit in RAM.
    CHECK(config.router.vram_only);
    CHECK(config.router.direct_io);
    CHECK(config.router.no_host);
    CHECK(config.defaults.vram_only);
    for (const auto& [id, expert] : config.experts) {
        CHECK(expert.vram_only);
        CHECK(expert.direct_io);
    }
}

TEST(the_memory_policy_is_applied_even_in_auto_split_mode) {
    // Where the computing happens is a different question from how it is
    // divided between cards, and "auto" is an answer to the second one only.
    Config config;
    config.gpu.mode      = "auto";
    config.gpu.vram_only = true;
    apply_gpu_policy(config);
    CHECK(config.defaults.vram_only);
}

TEST(the_memory_policy_is_off_unless_it_is_asked_for) {
    Config config;
    config.gpu.vram_only = false;
    config.gpu.gpu_only  = false;
    apply_gpu_policy(config);
    CHECK(!config.defaults.vram_only);
    CHECK(!config.defaults.direct_io);
    CHECK(!config.defaults.no_host);
}

TEST(gpu_only_forces_every_layer_onto_the_card_or_does_nothing_at_all) {
    // On a machine with no GPU the processor is the only thing there is to
    // compute on, so overwriting the configured layer count would be a
    // destructive gesture with nothing to show for it.
    Config config;
    config.gpu.gpu_only          = true;
    config.defaults.n_gpu_layers = 12;
    apply_gpu_policy(config);

    if (gpu_devices().empty()) {
        CHECK(config.defaults.n_gpu_layers == 12);
        CHECK(!config.defaults.no_host);
    } else {
        // -1 is llama.cpp's "every layer plus the output".
        CHECK(config.defaults.n_gpu_layers == -1);
        CHECK(config.defaults.no_host);
    }
}

TEST(the_memory_settings_survive_a_round_trip_through_the_config_file) {
    TempDir dir;
    const std::filesystem::path file = dir.path() / "config.json";

    Config written;
    written.gpu.gpu_only  = false;
    written.gpu.vram_only = true;
    CHECK(save_config(written, file));

    std::vector<std::string> warnings;
    const Config read = load_config(file, warnings);
    CHECK(!read.gpu.gpu_only);
    CHECK(read.gpu.vram_only);
}
