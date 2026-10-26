# SPDX-License-Identifier: MIT
"""The benchmark runner: which items a size asks, how each is asked and
checked, the scores the board works out, and a run against a stand-in core."""

import hashlib
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import bench  # noqa: E402


def _documented_items(bench_id, rows, n):
    """The board README's own code for which items a size asks."""
    def key(text):
        return hashlib.sha256(text.encode("utf-8")).hexdigest()
    return sorted(sorted(range(rows), key=lambda i: key(f"crucible-bench-1/{bench_id}/{i}"))[:n])


class Items(unittest.TestCase):
    def test_counts_by_size(self):
        for b in bench.BENCHMARKS:
            self.assertEqual(bench.item_count(b, "quick"), 50)
            self.assertEqual(bench.item_count(b, "standard"), min(200, b["total"]))
            self.assertEqual(bench.item_count(b, "full"), b["total"])
        self.assertEqual(bench.item_count(bench.BY_ID["humaneval"], "standard"), 164)
        # Anything else is the quick size.
        self.assertEqual(bench.item_count(bench.BY_ID["arc"], "huge"), 50)

    def test_the_items_are_the_ones_the_board_documents(self):
        for bench_id in ("arc", "humaneval", "truthfulqa"):
            b = bench.BY_ID[bench_id]
            for size in ("quick", "standard"):
                self.assertEqual(bench.pick(b, size),
                                 _documented_items(bench_id, b["total"], bench.item_count(b, size)))
        self.assertEqual(bench.pick(bench.BY_ID["humaneval"], "full"), list(range(164)))

    def test_a_larger_size_keeps_the_smaller_ones_items(self):
        b = bench.BY_ID["gsm8k"]
        self.assertTrue(set(bench.pick(b, "quick")) <= set(bench.pick(b, "standard")))


class Scores(unittest.TestCase):
    def test_the_documented_example(self):
        skills = bench.skills_of({"mmlu_pro": {"n": 50, "correct": 31}, "gsm8k": {"n": 50, "correct": 44}})
        self.assertEqual(skills, {"knowledge": 0.62, "math": 0.88})
        self.assertAlmostEqual(bench.overall_of(skills), 0.75)

    def test_a_skill_is_the_mean_of_its_benchmarks(self):
        skills = bench.skills_of({"gsm8k": {"n": 50, "correct": 40}, "math500": {"n": 50, "correct": 20},
                                  "humaneval": {"n": 50, "correct": 25}})
        self.assertAlmostEqual(skills["math"], 0.6)
        self.assertAlmostEqual(skills["code"], 0.5)
        self.assertNotIn("knowledge", skills)
        self.assertAlmostEqual(bench.overall_of(skills), 0.55)

    def test_nothing_run_is_no_score(self):
        self.assertEqual(bench.skills_of({}), {})
        self.assertIsNone(bench.overall_of({}))


class Asking(unittest.TestCase):
    def check(self, bench_id, row, right, wrong, index=0):
        messages, check = bench.ask(bench.BY_ID[bench_id], row, index)
        self.assertEqual(messages[0]["role"], "user")
        self.assertTrue(check(right)[0], right)
        self.assertFalse(check(wrong)[0], wrong)
        return messages[0]["content"]

    def test_mmlu_pro(self):
        text = self.check("mmlu_pro", {"question": "Which is a noble gas?", "options": ["N", "O", "Ar", "H"],
                                       "answer": "C"}, "Argon is inert.\nAnswer: C", "Answer: A")
        self.assertIn("C. Ar", text)
        # The form of the answer, and no example of one to copy.
        self.assertIn("'Answer: $LETTER'", text)
        self.assertIn("one of A, B, C or D.", text)
        self.assertNotIn("Answer: B", text)

    def test_arc_answer_keys_that_are_numbers(self):
        text = self.check("arc", {"question": "Q?", "choices": {"label": ["1", "2", "3", "4"],
                                                                 "text": ["w", "x", "y", "z"]},
                                  "answerKey": "3"}, "Answer: C", "Answer: A")
        self.assertIn("C. y", text)

    def test_hellaswag(self):
        text = self.check("hellaswag", {"activity_label": "Baking", "ctx": "She opens the oven and",
                                        "endings": ["sings.", "takes out the bread.", "flies.", "sleeps."],
                                        "label": "1"}, "Answer: B", "Answer: D")
        self.assertIn("Baking: She opens the oven and", text)

    def test_truthfulqa_with_thirteen_choices(self):
        choices = ["true one"] + ["false %d" % i for i in range(12)]
        row = {"question": "Q?", "mc1_targets": {"choices": choices, "labels": [1] + [0] * 12}}
        messages, check = bench.ask(bench.BY_ID["truthfulqa"], row, 7)
        text = messages[0]["content"]
        line = next(l for l in text.splitlines() if l.endswith(". true one"))
        letter = line.split(".")[0]
        self.assertTrue(check("Answer: %s" % letter)[0])
        # Not always shown first, as MC1 lists it.
        shown = [l.split(". ", 1)[1] for l in text.splitlines() if len(l) > 3 and l[1:3] == ". "]
        self.assertEqual(sorted(shown), sorted(choices))
        orders = set()
        for index in range(20):
            first = next(l for l in bench.ask(bench.BY_ID["truthfulqa"], row, index)[0][0]["content"].splitlines()
                         if l.startswith("A. "))
            orders.add(first)
        self.assertGreater(len(orders), 1)

    def test_gsm8k(self):
        self.check("gsm8k", {"question": "How much?", "answer": "3 * 360 = 1080\n#### 1,080"},
                   "Three batches.\n#### 1080", "#### 1081")

    def test_math500(self):
        self.check("math500", {"problem": "Half of one?", "answer": "\\frac{1}{2}"},
                   "So it is $\\boxed{\\frac{1}{2}}$.", "$\\boxed{2}$")

    def test_humaneval_runs_the_tests(self):
        row = {"prompt": "def add(a, b):\n    \"\"\"Add two numbers.\"\"\"\n", "entry_point": "add",
               "test": "def check(candidate):\n    assert candidate(1, 2) == 3\n"}
        self.check("humaneval", row, "```python\ndef add(a, b):\n    return a + b\n```",
                   "```python\ndef add(a, b):\n    return a - b\n```")

    def test_mbpp_runs_the_tests(self):
        row = {"prompt": "Write a function to add two numbers.", "test_list": ["assert add(1, 2) == 3"],
               "test_imports": []}
        text = self.check("mbpp", row, "```python\ndef add(a, b):\n    return a + b\n```",
                          "```python\ndef add(a, b):\n    return a * b\n```")
        self.assertIn("assert add(1, 2) == 3", text)

    def test_ifeval(self):
        row = {"prompt": "Describe the sea without commas.", "instruction_id_list": ["punctuation:no_comma"],
               "kwargs": [{}]}
        self.check("ifeval", row, "The sea is wide and blue.", "The sea is wide, and blue.")

    def test_an_unknown_benchmark(self):
        with self.assertRaises(ValueError):
            bench.ask({"id": "nope"}, {}, 0)


class FakeCore:
    """The core as bench.run sees it: a model that answers A, and dataset rows
    where the even items' answer is A and the odd ones' is B."""

    def __init__(self, stop_after=None, load_error=None, rows_error=None, rows_fail=0, fits=None, cut=False):
        self.published = []
        self.saved = []
        self.chats = 0
        self.released = 0
        self.stop_after = stop_after
        self.load_error = load_error
        self.rows_error = rows_error
        self.rows_fail = rows_fail   # how many asks for rows fail before they work
        self.rows_asked = 0
        self.fits = fits             # the largest context the model loads with, or any
        self.cut = cut               # every reply runs into the budget
        self.budgets = set()

    def call(self, method, params=None):
        params = params or {}
        # The core reads "context" as whose a call is, always text.
        assert isinstance(params.get("context", ""), str), (method, params)
        if method == "bench.machine":
            return {"crucible": "0.9.5", "llama": "b11846", "backend": "Vulkan",
                    "gpus": ["Test GPU"], "gpu_count": 1, "memory_gb": 12.0}
        if method == "bench.publish":
            import copy
            self.published.append(copy.deepcopy(params["run"]))
            return {}
        if method == "bench.stopped":
            return {"stopped": self.stop_after is not None and self.chats >= self.stop_after}
        if method == "bench.load":
            if self.load_error:
                return {"ok": False, "error": self.load_error}
            if self.fits is not None and params["n_ctx"] > self.fits:
                return {"ok": False, "error": "not enough memory"}
            return {"ok": True, "load_ms": 5}
        if method == "bench.model":
            return {"file": "tiny-Q4_K_M.gguf", "bytes": 1000, "name": "Tiny", "arch": "llama",
                    "params": 1000000, "quant": "Q4_K_M"}
        if method == "bench.hash":
            return {"sha256": "ab" * 32}
        if method == "bench.rows":
            self.rows_asked += 1
            if self.rows_error:
                return {"error": self.rows_error}
            if self.rows_asked <= self.rows_fail:
                return {"error": "HTTP 503: busy"}
            offset = params["offset"]
            return {"rows": [{"row_idx": i, "row": {
                "question": "Q%d?" % i,
                "choices": {"label": ["A", "B", "C", "D"], "text": ["w", "x", "y", "z"]},
                "answerKey": "A" if i % 2 == 0 else "B"}} for i in range(offset, offset + params["length"])]}
        if method == "bench.chat":
            self.chats += 1
            self.budgets.add(params["max_tokens"])
            return {"answer": "Answer: A", "ms": 10, "prompt_tokens": 40, "prompt_ms": 20.0,
                    "output_tokens": 10, "output_ms": 100.0, "hit_limit": self.cut}
        if method == "bench.save":
            self.saved.append(params["result"])
            return {"path": "/results/%s.json" % params["result"]["id"]}
        if method == "bench.release":
            self.released += 1
            return {}
        raise AssertionError("unexpected call: %s" % method)


class Running(unittest.TestCase):
    def run_arc(self, core):
        return bench.run(core, {"models": [{"path": "/models/tiny-Q4_K_M.gguf", "name": "tiny"}],
                                "benchmarks": ["arc"], "size": "quick", "id": "run1"})

    def test_a_whole_run(self):
        core = FakeCore()
        self.assertEqual(self.run_arc(core), {"ok": True, "id": "run1"})
        self.assertEqual(core.chats, 50)
        self.assertEqual(len(core.saved), 1)
        result = core.saved[0]
        even = sum(1 for i in bench.pick(bench.BY_ID["arc"], "quick") if i % 2 == 0)
        self.assertEqual(result["benchmarks"], {"arc": {"n": 50, "correct": even}})
        self.assertAlmostEqual(result["skills"]["reasoning"], even / 50)
        self.assertAlmostEqual(result["overall"], even / 50)
        self.assertEqual(result["model"]["sha256"], "ab" * 32)
        self.assertEqual(result["model"]["quant"], "Q4_K_M")
        self.assertEqual(result["id"], "run1-0")
        self.assertEqual(result["speed"], {"prompt_tps": 2000.0, "gen_tps": 100.0})
        self.assertEqual(len(result["items"]["arc"]), 50)
        self.assertFalse(result["stopped"])
        self.assertEqual(core.released, 1)
        last = core.published[-1]
        self.assertFalse(last["running"])
        self.assertIn("ended", last)
        self.assertEqual(last["models"][0]["state"], "done")
        self.assertIn("started", last["models"][0])   # its own clock, for the time left
        self.assertNotIn("stopped", last)

    def test_what_is_posted_has_no_empty_values(self):
        core = FakeCore()
        self.run_arc(core)
        result = core.saved[0]

        def walk(value):
            if isinstance(value, dict):
                for v in value.values():
                    walk(v)
            self.assertIsNotNone(value)
        for key in ("model", "settings", "hardware"):
            walk(result[key])
        # The repository is looked for when a result is posted, not here.
        self.assertNotIn("repo", result["model"])

    def test_the_machine_is_kept_by_kind_and_count_and_its_names_beside(self):
        core = FakeCore()
        self.run_arc(core)
        result = core.saved[0]
        # The board takes the backend, the count and the memory. The names, of
        # the cards and of the model, are for this machine's screen.
        self.assertEqual(result["hardware"], {"backend": "Vulkan", "gpu_count": 1, "gpus": ["Test GPU"],
                                              "memory_gb": 12.0})
        self.assertEqual(result["model"]["name"], "Tiny")

    def test_a_machine_without_a_gpu_counts_none(self):
        core = FakeCore()
        machine = core.call

        def no_gpu(method, params=None):
            if method == "bench.machine":
                return {"crucible": "0.9.5", "llama": "b11846", "backend": "CPU", "gpus": [], "gpu_count": 0,
                        "memory_gb": 0.0}
            return machine(method, params)
        core.call = no_gpu
        self.run_arc(core)
        self.assertEqual(core.saved[0]["hardware"], {"backend": "CPU", "gpu_count": 0})

    def test_a_stopped_run_counts_no_partial_benchmark(self):
        core = FakeCore(stop_after=10)
        self.run_arc(core)
        result = core.saved[0]
        self.assertEqual(result["benchmarks"], {})
        self.assertTrue(result["stopped"])
        self.assertEqual(len(result["items"]["arc"]), 10)
        last = core.published[-1]
        self.assertTrue(last["stopped"])
        self.assertEqual(last["models"][0]["state"], "stopped")

    def test_a_model_that_does_not_load(self):
        core = FakeCore(load_error="not enough memory")
        self.run_arc(core)
        self.assertEqual(core.saved, [])
        model = core.published[-1]["models"][0]
        self.assertEqual(model["state"], "failed")
        self.assertEqual(model["error"], "not enough memory")

    def test_rows_that_cannot_be_fetched(self):
        core = FakeCore(rows_error="no network")
        with mock.patch.object(bench, "FETCH_WAITS", (0, 0)):
            self.run_arc(core)
        self.assertEqual(core.chats, 0)
        self.assertEqual(core.saved[0]["benchmarks"], {})
        self.assertEqual(core.saved[0]["errors"], {"arc": "no network"})
        self.assertEqual(core.published[-1]["models"][0]["benchmarks"]["arc"]["error"], "no network")
        # Tried three times, then given up on.
        self.assertEqual(core.rows_asked, 3)

    def test_a_busy_server_is_asked_again(self):
        core = FakeCore(rows_fail=2)
        with mock.patch.object(bench, "FETCH_WAITS", (0, 0)):
            self.run_arc(core)
        self.assertEqual(core.saved[0]["benchmarks"]["arc"]["n"], 50)
        self.assertNotIn("errors", core.saved[0])

    def test_every_page_is_fetched_before_a_model_loads(self):
        core = FakeCore()
        self.run_arc(core)
        fetching = [i for i, run in enumerate(core.published) if run.get("fetching")]
        loading = [i for i, run in enumerate(core.published) if run["models"][0]["state"] == "loading"]
        self.assertTrue(fetching and loading)
        self.assertLess(max(fetching), min(loading))


    def test_every_reply_has_room_to_think(self):
        core = FakeCore()
        self.run_arc(core)
        self.assertEqual(core.budgets, {bench.MAX_TOKENS})
        self.assertEqual(core.saved[0]["settings"]["max_tokens"], bench.MAX_TOKENS)
        self.assertEqual(core.saved[0]["settings"]["context"], bench.CONTEXT)
        self.assertNotIn("cut_off", core.saved[0])

    def test_a_model_too_large_for_the_context_runs_with_the_smaller_one_and_says_so(self):
        core = FakeCore(fits=bench.SMALL_CONTEXT)
        self.run_arc(core)
        settings = core.saved[0]["settings"]
        self.assertEqual(settings["context"], bench.SMALL_CONTEXT)
        self.assertEqual(settings["max_tokens"], bench.SMALL_MAX_TOKENS)
        self.assertEqual(core.budgets, {bench.SMALL_MAX_TOKENS})

    def test_replies_cut_off_by_the_budget_are_counted(self):
        core = FakeCore(cut=True)
        self.run_arc(core)
        self.assertEqual(core.saved[0]["cut_off"], {"arc": 50})


if __name__ == "__main__":
    unittest.main()
