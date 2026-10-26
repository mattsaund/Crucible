# SPDX-License-Identifier: MIT
"""A conversation's name and an expert's examples, out of what a model said."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import naming  # noqa: E402


class Names(unittest.TestCase):
    def test_the_decoration_is_taken_off(self):
        self.assertEqual(naming.name_from('"Math homework."'), "Math homework")
        self.assertEqual(naming.name_from("Title: fixing a Python import\n\nBecause..."),
                         "Fixing a Python import")
        self.assertEqual(naming.name_from("\n**Trip to Lisbon**\n"), "Trip to Lisbon")

    def test_a_sentence_is_not_a_title_and_nothing_is_not_one_either(self):
        self.assertEqual(naming.name_from(
            "This conversation is about the user asking how to do their math"), "")
        self.assertEqual(naming.name_from(""), "")

    def test_with_no_model_the_opening_words_of_the_question(self):
        self.assertEqual(naming.fallback_name(
            "Question: can you help with my math homework? It is due\nAnswer: Sure"),
            "Can you help")
        self.assertEqual(naming.fallback_name("Question: test\nAnswer: ok"), "Test")
        # Cut at five, but not on a word that leads into the sixth.
        self.assertEqual(naming.fallback_name("Why does water boil at a lower temperature?"),
                         "Why does water boil")
        self.assertEqual(naming.fallback_name("Fix the bug in the parser"), "Fix the bug")
        # Whole, a prompt keeps its last word whatever it is.
        self.assertEqual(naming.fallback_name("What is this for"), "What is this for")

    def test_the_prompt_shows_what_a_title_looks_like(self):
        self.assertIn("Math homework", naming.naming_prompt("Question: x"))


class Examples(unittest.TestCase):
    def test_parsed_out_of_whatever_the_model_replied_with(self):
        # Every shape here is one a model actually produced.
        got = naming.parse_examples(
            "Here are two questions:\n"
            "1. What preload should an M8 bolt take in aluminum?\n"
            "2) \"How do I damp a resonant bracket at 40Hz?\"\n"
            "- a third one that should not be kept\n")
        self.assertEqual(got, ["What preload should an M8 bolt take in aluminum?",
                               "How do I damp a resonant bracket at 40Hz?"])

    def test_a_preamble_line_is_not_mistaken_for_a_question(self):
        got = naming.parse_examples(
            "Sure, here you go:\nok\nWhat is the yield strength of 6061-T6 aluminum?\n")
        self.assertEqual(got, ["What is the yield strength of 6061-T6 aluminum?"])


class Core:
    def __init__(self, text, available=True):
        self.text = text
        self.available = available
        self.asked = []

    def call(self, method, params=None):
        self.asked.append((method, params))
        if method == "delegator.ready":
            return {"available": self.available}
        if method == "delegator.done":
            return {}
        if method == "delegator.generate":
            return {"text": self.text}
        raise AssertionError(method)


class Through(unittest.TestCase):
    def test_a_name_is_asked_of_the_delegator(self):
        core = Core("Orbital speed")
        self.assertEqual(naming.name_session(core, {"excerpt": "Question: how fast\nAnswer: 7.7 km/s"}),
                         {"name": "Orbital speed"})
        self.assertEqual(core.asked[1][1]["max_tokens"], 24)
        # Not worth a load over the expert chosen in the box.
        self.assertEqual(core.asked[0], ("delegator.ready", {"optional": True}))

    def test_no_delegator_means_the_opening_words(self):
        core = Core("", available=False)
        self.assertEqual(naming.name_session(core, {"excerpt": "Question: how fast is the ISS"}),
                         {"name": "How fast is the ISS"})
        # Opened and closed, and nothing generated between.
        self.assertEqual([m for m, _ in core.asked], ["delegator.ready", "delegator.done"])

    def test_an_old_conversation_is_named_only_by_a_delegator_already_loaded(self):
        core = Core("Orbital speed")
        self.assertEqual(naming.name_session(core, {"excerpt": "Question: how fast", "spare": True}),
                         {"name": "Orbital speed"})
        self.assertEqual(core.asked[0], ("delegator.ready", {"optional": True, "load": False}))
        self.assertIs(core.asked[1][1]["load"], False)
        # With none loaded, no name: its opening words are its title already.
        idle = Core("", available=False)
        self.assertEqual(naming.name_session(idle, {"excerpt": "Question: how fast is the ISS",
                                                    "spare": True}),
                         {"name": ""})

    def test_examples_are_written_for_a_new_expert(self):
        core = Core("What is the torque on a bolt?\nHow do I size a beam?")
        out = naming.write_examples(core, {"name": "Engineering", "blurb": "bolts and beams"})
        self.assertEqual(out["examples"], ["What is the torque on a bolt?", "How do I size a beam?"])
        self.assertIn('An expert called "Engineering" handles: bolts and beams',
                      core.asked[1][1]["messages"][0]["content"])


if __name__ == "__main__":
    unittest.main()
