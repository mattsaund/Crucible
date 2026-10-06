# SPDX-License-Identifier: MIT
"""Routing, without a model: keywords, the policy, and the delegator's arithmetic
against a core that answers with scores chosen here."""

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import routing  # noqa: E402

ROSTER = [
    {"id": "mathematics", "name": "Mathematics", "tag": "MATH",
     "blurb": "Proofs, algebra, calculus", "keywords": ["derivative", "integral", "polynomial", "theorem",
                                                        "eigenvalue"],
     "examples": ["Prove that the square root of 2 is irrational."]},
    {"id": "programming", "name": "Programming", "tag": "PROG",
     "blurb": "Code in many languages", "keywords": ["function", "segfault", "compile", "code"],
     "examples": ["Write a Rust function that reverses a string."]},
    {"id": "physics", "name": "Physics", "tag": "PHYS",
     "blurb": "Mechanics and relativity", "keywords": ["lagrangian", "torque", "orbit"], "examples": []},
    {"id": "chemistry", "name": "Chemistry", "tag": "CHEM",
     "blurb": "Reactions and bonding", "keywords": ["ion", "reaction", "enthalpy"], "examples": []},
    {"id": "biology", "name": "Biology", "tag": "BIO",
     "blurb": "Cells and genes", "keywords": ["gene", "cell", "enzyme"], "examples": []},
    {"id": "language", "name": "Language", "tag": "LANG",
     "blurb": "Grammar and style", "keywords": ["proofread"], "examples": []},
]


def roster():
    return routing.Roster(ROSTER)


def seats(*filled):
    return {e["id"]: {"model": e["id"] in filled} for e in ROSTER}


def route_of(prompt):
    return routing.keyword_route(prompt, roster()).expert


class Keywords(unittest.TestCase):
    def test_the_obvious_subject(self):
        self.assertEqual(route_of("compute the derivative of this polynomial"), "mathematics")
        self.assertEqual(route_of("my code hits a segfault when I compile"), "programming")
        self.assertEqual(route_of("explain the lagrangian of this system"), "physics")
        self.assertEqual(route_of("balance this reaction and find the enthalpy"), "chemistry")
        self.assertEqual(route_of("how does an enzyme change a gene"), "biology")

    def test_nobody_when_nothing_matches(self):
        decision = routing.keyword_route("hello there", roster())
        self.assertEqual(decision.expert, "")
        self.assertEqual(decision.source, routing.FALLBACK)
        self.assertEqual(decision.confidence, 0.0)

    def test_whole_words_only(self):
        # "ion" sits in "question" and "opinion", "cell" in "excellent".
        self.assertEqual(route_of("an excellent question about your opinion"), "")
        # "gene" hides inside "generate" and "general".
        self.assertEqual(route_of("generate a general overview"), "")
        self.assertEqual(route_of("what is an ion"), "chemistry")
        self.assertEqual(route_of("describe a gene"), "biology")
        self.assertEqual(route_of("write a function to parse"), "programming")

    def test_case_does_not_matter(self):
        self.assertEqual(route_of("A SEGFAULT"), "programming")

    def test_confidence_reflects_ambiguity(self):
        clear = routing.keyword_route("derivative integral polynomial theorem eigenvalue", roster())
        mixed = routing.keyword_route("derivative of the enzyme torque code", roster())
        self.assertGreater(clear.confidence, mixed.confidence)
        self.assertLessEqual(clear.confidence, 0.95)
        self.assertEqual(clear.detail, "5 keyword matches")

    def test_a_seat_with_no_keywords_never_wins(self):
        quiet = routing.Roster(ROSTER + [{"id": "quiet", "name": "Quiet", "keywords": []}])
        self.assertEqual(routing.keyword_route("mmm", quiet).expert, "")


class Prompt(unittest.TestCase):
    def test_labels_are_names_in_roster_order(self):
        self.assertEqual(roster().labels()[:3], ["Mathematics", "Programming", "Physics"])

    def test_system_prompt_lists_tag_name_and_remit(self):
        prompt = roster().system_prompt()
        self.assertTrue(prompt.startswith("You label a question with the one subject it belongs to."))
        # It asks for what is scored: the name.
        self.assertIn("Reply with only the subject's name.", prompt)
        self.assertIn("MATH  Mathematics: Proofs, algebra, calculus\n", prompt)
        self.assertNotIn("anything else", prompt.lower())

    def test_three_orders_each_seat_first_in_one_of_them(self):
        orders = roster().orders()
        self.assertEqual(len(orders), 3)
        self.assertEqual(sorted(orders[1]), list(range(len(ROSTER))))
        self.assertEqual({order[0] for order in orders}, {0, len(ROSTER) - 1, len(ROSTER) // 2})
        # The prompt and the examples follow the order they are given.
        reversed_prompt = roster().system_prompt(orders[1])
        self.assertLess(reversed_prompt.index("Language"), reversed_prompt.index("Mathematics"))
        self.assertEqual(roster().examples(orders[1])[0][1], "Programming")

    def test_examples_answer_with_the_label_alone(self):
        self.assertIn(("Prove that the square root of 2 is irrational.", "Mathematics"),
                      roster().examples())

    def test_harmony_is_asked_where_its_answer_goes(self):
        self.assertEqual(routing.answer_prefix("...<|start|>assistant"), "<|channel|>final<|message|>")
        self.assertEqual(routing.answer_prefix("<|im_start|>assistant\n"), "")


class Policy(unittest.TestCase):
    def apply(self, expert, confidence, source, filled, **settings):
        decision = routing.Decision(expert, confidence, source)
        return routing.apply_policy(decision, roster(), seats(*filled), settings)

    def test_a_confident_route_to_a_filled_seat_stands(self):
        out = self.apply("physics", 0.95, routing.MODEL, ["physics", "language"])
        self.assertEqual((out.expert, out.source), ("physics", routing.MODEL))

    def test_unsure_goes_to_the_default(self):
        out = self.apply("physics", 0.40, routing.MODEL, ["physics", "language"],
                         min_confidence=0.6, default_expert="language")
        self.assertEqual((out.expert, out.source), ("language", routing.FALLBACK))
        self.assertIn("undecided (Physics at 40%)", out.detail)

    def test_unsure_with_no_default_keeps_the_guess_and_says_so(self):
        out = self.apply("physics", 0.40, routing.MODEL, ["physics"], min_confidence=0.6)
        self.assertEqual(out.expert, "physics")
        self.assertTrue(out.detail.startswith("undecided (Physics at 40%)"))

    def test_a_pin_ignores_the_floor(self):
        out = self.apply("physics", 1.0, routing.PINNED, ["physics", "language"],
                         min_confidence=0.99, default_expert="language")
        self.assertEqual((out.expert, out.source), ("physics", routing.PINNED))

    def test_a_zero_floor_disables_the_check(self):
        out = self.apply("physics", 0.01, routing.MODEL, ["physics", "language"],
                         min_confidence=0.0, default_expert="language")
        self.assertEqual(out.expert, "physics")

    def test_an_empty_seat_sends_work_to_the_default(self):
        out = self.apply("chemistry", 0.95, routing.MODEL, ["physics", "language"],
                         default_expert="language")
        self.assertEqual((out.expert, out.source), ("language", routing.FALLBACK))
        self.assertIn("Chemistry has no model", out.detail)

    def test_with_no_default_any_filled_seat_is_used(self):
        out = self.apply("chemistry", 0.95, routing.MODEL, ["physics"])
        self.assertEqual(out.expert, "physics")
        self.assertIn("Chemistry has no model; used Physics", out.detail)

    def test_nobody_named_is_said_as_such(self):
        out = self.apply("", 0.0, routing.FALLBACK, ["physics"])
        self.assertEqual(out.detail, "no expert was named; used Physics")

    def test_with_nothing_configured_the_route_reports_it(self):
        out = self.apply("chemistry", 0.95, routing.MODEL, [])
        self.assertIn("no experts have a model", out.detail)

    def test_a_default_with_no_model_is_not_used(self):
        out = self.apply("chemistry", 0.95, routing.MODEL, ["physics"], default_expert="language")
        self.assertEqual(out.expert, "physics")


class FakeCore:
    """Answers the delegator's calls with scores chosen per question -- and,
    when asked for, per which seat the system prompt lists first."""

    def __init__(self, scores_for, available=True):
        self.scores_for = scores_for
        self.available = available
        self.calls = []

    def call(self, method, params=None):
        self.calls.append(method)
        if method == "delegator.ready":
            return {"available": self.available, "path": "/models/delegator.gguf"}
        if method == "delegator.format":
            # The last user message is the question; the format is the model's.
            listed = params["messages"][0]["content"].split("\n\n", 1)[1].split("\n")[0]
            first = listed.split("  ", 1)[1].split(":")[0]
            return {"text": "FIRST:" + first + "\nQ:" + params["messages"][-1]["content"]
                            + "<|start|>assistant"}
        if method == "delegator.score":
            head, rest = params["prompt"].split("\nQ:", 1)
            question = rest.split("<|start|>")[0]
            assert params["prompt"].endswith("<|channel|>final<|message|>")
            scores_for = self.scores_for
            if scores_for.__code__.co_argcount == 3:
                return {"scores": scores_for(question, params["labels"], head[len("FIRST:"):]),
                        "canceled": False}
            return {"scores": scores_for(question, params["labels"]), "canceled": False}
        raise AssertionError("unexpected call " + method)


def request(prompt, filled=("mathematics", "programming", "physics"), **extra):
    out = {"prompt": prompt, "roster": ROSTER, "seats": seats(*filled), "routing": {}}
    out.update(extra)
    return out


class Delegator(unittest.TestCase):
    def setUp(self):
        routing.ModelRouter._bias.clear()

    def test_the_best_label_wins_and_its_share_is_the_confidence(self):
        def scores(question, labels):
            if question in routing.CONTENT_FREE:
                return [0.0] * len(labels)
            return [5.0 if label == "Physics" else 0.0 for label in labels]
        out = routing.route(FakeCore(scores), request("why is the sky blue"))
        self.assertEqual((out.expert, out.source), ("physics", routing.MODEL))
        expected = 1.0 / (1.0 + 5 * math.exp(-5.0))
        self.assertAlmostEqual(out.confidence, expected, places=6)

    def test_the_measured_lean_is_taken_out(self):
        # A delegator that says Mathematics to nothing at all is leaning; a
        # quarter of that lean comes off every question.
        def scores(question, labels):
            if question in routing.CONTENT_FREE:
                return [8.0 if label == "Mathematics" else 0.0 for label in labels]
            return [2.0 if label in ("Mathematics", "Physics") else 0.0 for label in labels]
        out = routing.route(FakeCore(scores), request("a question"))
        self.assertEqual(out.expert, "physics")

    def test_the_lean_is_measured_once_per_delegator(self):
        core = FakeCore(lambda q, labels: [1.0] + [0.0] * (len(labels) - 1))
        routing.route(core, request("one"))
        routing.route(core, request("two"))
        # Three content-free questions in each of three orders, once; then
        # three orders a question.
        self.assertEqual(core.calls.count("delegator.score"), 9 + 3 * 2)

    def test_a_lean_towards_the_first_seat_listed_is_averaged_away(self):
        # A delegator that likes Physics for this question, and likes whatever
        # it was shown first a little more than that.
        def scores(question, labels, first):
            leaning = [2.0 if label == first else 0.0 for label in labels]
            if question in routing.CONTENT_FREE:
                return leaning
            return [s + (1.0 if label == "Physics" else 0.0) for s, label in zip(leaning, labels)]
        shown_once = routing.ModelRouter(FakeCore(scores), roster(), "/m", method="single")
        self.assertEqual(shown_once.route("why is the sky blue").expert, "mathematics")
        routing.ModelRouter._bias.clear()
        out = routing.route(FakeCore(scores), request("why is the sky blue"))
        self.assertEqual(out.expert, "physics")

    def test_unscorable_falls_back_to_keywords(self):
        def scores(question, labels):
            return [0.0] * len(labels) if question in routing.CONTENT_FREE else [None] * len(labels)
        out = routing.route(FakeCore(scores), request("my code will not compile"))
        self.assertEqual((out.expert, out.source), ("programming", routing.KEYWORD))
        self.assertEqual(out.detail, "delegator could not be scored, used keywords")

    def test_no_delegator_routes_on_keywords(self):
        out = routing.route(FakeCore(None, available=False), request("explain the lagrangian"))
        self.assertEqual((out.expert, out.source), ("physics", routing.KEYWORD))

    def test_a_pin_needs_no_delegator(self):
        core = FakeCore(None)
        out = routing.route(core, request("anything", pinned="physics"))
        self.assertEqual((out.expert, out.source, out.confidence), ("physics", routing.PINNED, 1.0))
        self.assertEqual(core.calls, [])


if __name__ == "__main__":
    unittest.main()
