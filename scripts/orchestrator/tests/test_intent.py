# SPDX-License-Identifier: MIT
"""What a message becomes, without a model: the keywords, the prompt the
delegator is shown, and its arithmetic against a core that scores the two
labels with numbers chosen here."""

import json
import math
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import intent  # noqa: E402
from crucible_orchestrator.rpc import Disconnected  # noqa: E402

ROUTES = os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools", "routes")


def kind_of(prompt, files=False):
    return intent.keyword_decide(prompt, files).kind


class Keywords(unittest.TestCase):
    def test_a_question_is_answered_even_when_it_is_about_building(self):
        self.assertEqual(kind_of("How do I build a todo app in React?"), intent.ANSWER)
        self.assertEqual(kind_of("What does npm run build do?"), intent.ANSWER)
        self.assertEqual(kind_of("is a game engine worth it for a small app?"), intent.ANSWER)

    def test_software_asked_for_is_built_and_a_request_put_as_a_question_too(self):
        self.assertEqual(kind_of("Make me a todo app."), intent.BUILD)
        self.assertEqual(kind_of("Can you build me a recipe organizer?"), intent.BUILD)
        self.assertEqual(kind_of("I need a tool that converts CSV files to JSON."), intent.BUILD)
        # "command-line" is not a line of code.
        self.assertEqual(kind_of("Build a command-line tool that renames photos."), intent.BUILD)

    def test_software_described_as_build_took_it_is_built(self):
        # The README's own example, typed the way the Build tab was.
        self.assertEqual(kind_of("A budgeting app that charts my gross and net revenue"), intent.BUILD)
        self.assertEqual(kind_of("an app for tracking my habits"), intent.BUILD)
        # One that is there already, or a question about one, is not.
        self.assertEqual(kind_of("The app crashes when I click save."), intent.ANSWER)
        self.assertEqual(kind_of("A website is just a folder of pages?"), intent.ANSWER)
        self.assertEqual(kind_of("A function that reverses a string"), intent.ANSWER)

    def test_code_written_in_the_reply_is_an_answer(self):
        self.assertEqual(kind_of("Write a function that reverses a string."), intent.ANSWER)
        self.assertEqual(kind_of("Write a bash one-liner that counts lines."), intent.ANSWER)
        self.assertEqual(kind_of("Write me a haiku about compilers."), intent.ANSWER)

    def test_a_change_is_built_only_in_a_folder_with_something_in_it_and_never_a_small_one(self):
        self.assertEqual(kind_of("Add dark mode to this page.", files=True), intent.BUILD)
        self.assertEqual(kind_of("Now add drag and drop to reorder the items.", files=True), intent.BUILD)
        self.assertEqual(kind_of("Add dark mode to this page."), intent.ANSWER)
        self.assertEqual(kind_of("Add a docstring to the period function.", files=True), intent.ANSWER)
        self.assertEqual(kind_of("Fix the typo in the README.", files=True), intent.ANSWER)

    def test_what_was_attached_is_part_of_what_is_decided(self):
        self.assertEqual(intent.message_of("make this", ["sketch.png", "notes.md"]),
                         "make this\n\nAttached: sketch.png notes.md")
        self.assertEqual(intent.message_of("make this"), "make this")


def render(messages):
    """The chat format, as plainly as one can be written."""
    return "".join("<%s>%s\n" % (m["role"], m["content"]) for m in messages) + "<assistant>"


class ScoringCore:
    """The core, scoring the two labels with `scores(message, build_first)`,
    where `message` is the last user message rendered and `build_first` says
    which order the labels were shown in."""

    def __init__(self, scores, available=True, path="lfm.gguf", fail=None, canceled=False):
        self.scores = scores
        self.available = available
        self.path = path
        self.fail = fail
        self.canceled = canceled
        self.calls = []
        self.ready = []
        self.prompts = []

    def call(self, method, params=None):
        params = params or {}
        self.calls.append(method)
        if method == "delegator.ready":
            self.ready.append(params)
            return {"available": self.available, "path": self.path}
        if method == "delegator.done":
            return {}
        if method == "delegator.format":
            if self.fail:
                raise self.fail
            return {"text": render(params["messages"])}
        if method == "delegator.score":
            prompt = params["prompt"]
            self.prompts.append(prompt)
            self.assertions(params)
            message = re.findall(r"<user>(.*?)\n", prompt, re.S)[-1]
            build_first = "Reply with only one word: Build or Answer." in prompt
            return {"scores": self.scores(message, build_first),
                    "canceled": self.canceled and params["cancelable"]}
        raise AssertionError("unexpected call " + method)

    def assertions(self, params):
        assert params["labels"] == ["Answer", "Build"], params["labels"]
        assert params["prompt"].startswith(params["prefix"])


def fresh():
    """Each test measures the lean again: it is kept by the file it was
    measured on, and these all call theirs the same."""
    intent.Decider._bias.clear()
    intent.Decider._prefix.clear()


class ThePrompt(unittest.TestCase):
    def setUp(self):
        fresh()

    def test_each_order_names_the_two_its_way_round_and_ends_on_the_other_kind(self):
        decider = intent.Decider(ScoringCore(lambda m, b: [0.0, 0.0]), "lfm.gguf")
        answer_first, examples = decider.shown((intent.ANSWER, intent.BUILD), files=False)
        self.assertIn("Reply with only one word: Answer or Build.", answer_first)
        self.assertLess(answer_first.index("Answer: a question"), answer_first.index("Build: making"))
        self.assertIn("No project with files is open.", answer_first)
        # Taking turns, every one of them shown, answered with exactly a label.
        self.assertEqual([label for _, label in examples[:4]], ["Answer", "Build", "Answer", "Build"])
        self.assertEqual(len(examples), sum(len(v) for v in intent.EXAMPLES.values()))
        self.assertEqual(examples[-1][1], "Build")
        build_first, other = decider.shown((intent.BUILD, intent.ANSWER), files=True)
        self.assertIn("Reply with only one word: Build or Answer.", build_first)
        self.assertIn("The person has a project open with files in it.", build_first)
        self.assertEqual(other[0][1], "Build")
        self.assertEqual(other[-1][1], "Answer")

    def test_blind_says_nothing_of_the_folder(self):
        decider = intent.Decider(ScoringCore(lambda m, b: [0.0, 0.0]), "lfm.gguf", method="blind")
        system, _ = decider.shown((intent.ANSWER, intent.BUILD), files=True)
        self.assertNotIn("project open", system)
        self.assertNotIn("No project", system)


class TheArithmetic(unittest.TestCase):
    def setUp(self):
        fresh()

    def test_the_lean_is_taken_out_and_both_orders_are_averaged(self):
        # On a message with no content the delegator says Answer by 3; on the
        # message, by 1 in one order and 3 in the other. Less the lean, a
        # build by 2 and by 0: by 1 on average.
        def scores(message, build_first):
            if message in intent.CONTENT_FREE:
                return [0.0, -3.0]
            return [0.0, -3.0] if build_first else [0.0, -1.0]
        core = ScoringCore(scores)
        decider = intent.Decider(core, "lfm.gguf")
        odds = decider.build_odds("Make me a todo app.", files=False)
        self.assertAlmostEqual(odds, 1.0 / (1.0 + math.exp(-1.0)))
        decision = decider.decide("Make me a todo app.")
        self.assertEqual(decision.kind, intent.BUILD)
        self.assertEqual(decision.source, intent.MODEL)
        self.assertAlmostEqual(decision.confidence, odds)
        # The lean is measured once per file and prompt, not every message.
        scored = len(core.prompts)
        decider.decide("Make me a snake game.")
        self.assertEqual(len(core.prompts), scored + 2)

    def test_without_the_lean_taken_out_a_leaning_delegator_never_builds(self):
        core = ScoringCore(lambda m, b: [0.0, -3.0] if m in intent.CONTENT_FREE else [0.0, -1.0])
        self.assertEqual(intent.Decider(core, "lfm.gguf", calibration=0.0).decide("Make me a todo app.").kind,
                         intent.ANSWER)

    def test_a_build_the_words_do_not_ask_for_is_answered(self):
        # Sure of a build, for a poem: the expensive mistake, caught.
        core = ScoringCore(lambda m, b: [0.0, 0.0] if m in intent.CONTENT_FREE else [-5.0, 0.0])
        decision = intent.Decider(core, "lfm.gguf").decide("Write me a haiku about compilers.")
        self.assertEqual(decision.kind, intent.ANSWER)
        self.assertIn("not by its words", decision.detail)
        alone = intent.Decider(core, "lfm.gguf", method="alone").decide("Write me a haiku about compilers.")
        self.assertEqual(alone.kind, intent.BUILD)

    def test_short_of_even_odds_it_is_an_answer(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0] if m in intent.CONTENT_FREE else [0.0, -0.1])
        decision = intent.Decider(core, "lfm.gguf").decide("Make me a todo app.")
        self.assertEqual(decision.kind, intent.ANSWER)
        self.assertGreater(decision.confidence, 0.5)

    def test_a_stop_answers_and_a_score_that_could_not_be_had_falls_back_to_keywords(self):
        stopped = ScoringCore(lambda m, b: [0.0, 0.0], canceled=True)
        decision = intent.Decider(stopped, "lfm.gguf").decide("Make me a todo app.")
        self.assertEqual((decision.kind, decision.detail), (intent.ANSWER, "canceled"))
        fresh()
        unscored = ScoringCore(lambda m, b: [None, None])
        decision = intent.Decider(unscored, "lfm.gguf").decide("Make me a todo app.")
        self.assertEqual((decision.kind, decision.source), (intent.BUILD, intent.KEYWORD))
        self.assertIn("could not be scored", decision.detail)


class TheWholeDecision(unittest.TestCase):
    def setUp(self):
        fresh()

    def test_the_delegator_is_asked_for_only_if_it_can_be_spared_and_always_let_go(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0] if m in intent.CONTENT_FREE else [-4.0, 0.0])
        decision = intent.decide(core, {"prompt": "Make me a todo app.", "attachments": [], "files": False})
        self.assertEqual(decision.kind, intent.BUILD)
        self.assertEqual(core.ready, [{"optional": True}])
        self.assertEqual(core.calls[-1], "delegator.done")

    def test_no_delegator_is_keywords(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0], available=False)
        decision = intent.decide(core, {"prompt": "Add dark mode to this page.", "files": True})
        self.assertEqual((decision.kind, decision.source), (intent.BUILD, intent.KEYWORD))
        self.assertNotIn("delegator.score", core.calls)
        self.assertEqual(core.calls[-1], "delegator.done")

    def test_keywords_asked_for_by_name_need_no_delegator_at_all(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0])
        self.assertEqual(intent.decide(core, {"prompt": "Make me a todo app.", "method": "keywords"}).kind,
                         intent.BUILD)
        self.assertEqual(core.calls, [])

    def test_a_delegator_that_fails_is_keywords_and_a_core_that_went_away_is_not_caught(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0], fail=RuntimeError("no context"))
        decision = intent.decide(core, {"prompt": "What is a mutex?"})
        self.assertEqual((decision.kind, decision.source), (intent.ANSWER, intent.KEYWORD))
        self.assertIn("no context", decision.detail)
        self.assertEqual(core.calls[-1], "delegator.done")
        gone = ScoringCore(lambda m, b: [0.0, 0.0], fail=Disconnected("gone"))
        with self.assertRaises(Disconnected):
            intent.decide(gone, {"prompt": "What is a mutex?"})

    def test_the_odds_say_each_orders_scores(self):
        core = ScoringCore(lambda m, b: [0.0, 0.0] if m in intent.CONTENT_FREE else [-1.0, 0.0])
        out = intent.odds(core, {"prompt": "Make me a todo app."})
        self.assertAlmostEqual(out["build"], 1.0 / (1.0 + math.exp(-1.0)))
        self.assertEqual([o["first"] for o in out["orders"]], [intent.ANSWER, intent.BUILD])
        self.assertEqual(core.calls[-1], "delegator.done")


class TheMeasuredCases(unittest.TestCase):
    """The worked messages go into the delegator's prompt and the case files
    measure it. A worked message that is also a case measures how well the
    prompt was copied into the answer sheet; see test_routing.cpp's
    no_worked_example_is_a_benchmark_prompt, which this follows."""

    def cases(self):
        out = []
        for name in ("answer-or-build.json", "answer-or-build-held-out.json"):
            with open(os.path.join(ROUTES, name), encoding="utf-8") as f:
                for case in json.load(f)["cases"]:
                    self.assertIn(case[0], (intent.ANSWER, intent.BUILD))
                    out.append(case[1])
        return out

    def test_about_twenty_of_each_kind_and_files_said_as_a_flag(self):
        with open(os.path.join(ROUTES, "answer-or-build.json"), encoding="utf-8") as f:
            cases = json.load(f)["cases"]
        kinds = [case[0] for case in cases]
        self.assertGreaterEqual(kinds.count(intent.ANSWER), 20)
        self.assertGreaterEqual(kinds.count(intent.BUILD), 20)
        for case in cases:
            if len(case) > 2:
                self.assertEqual(set(case[2]), {"files"})

    def test_no_worked_message_is_a_measured_case(self):
        words = lambda text: set(re.findall(r"[a-z0-9]+", text.lower()))  # noqa: E731
        cases = self.cases()
        seen = {}
        for case in cases:
            for word in words(case):
                seen[word] = seen.get(word, 0) + 1
        # Rare: in at most two cases. Two worked messages that share two
        # rare words with a case are the same message.
        rare = {word for word, n in seen.items() if n <= 2}
        for kind, examples in intent.EXAMPLES.items():
            for example in examples:
                for case in cases:
                    self.assertNotEqual(example.lower(), case.lower())
                    shared = words(example) & words(case) & rare
                    self.assertLess(len(shared), 2, "%r is %r again (%s)" % (example, case, shared))


if __name__ == "__main__":
    unittest.main()
