# SPDX-License-Identifier: MIT
"""The fine-tuner's parts that need no torch: which records are held out,
where the answers are, what the loss is taken over, the score's arithmetic,
and what a child's output becomes on the way to Crucible."""

import io
import json
import math
import os
import random
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import finetune  # noqa: E402


def read_as_event(line):
    """Crucible's rule for a line (src/lab/trainer.cpp, read_event): an
    object with an "event" is progress, and anything else is log."""
    try:
        parsed = json.loads(line)
    except ValueError:
        return False
    return isinstance(parsed, dict) and "event" in parsed


class ChatML:
    """A tokenizer with the chat template most models use, one token to a
    character, which is enough to see which characters a label covers."""

    is_fast = True

    def apply_chat_template(self, messages, tokenize=False, add_generation_prompt=False):
        text = "".join(f"<|im_start|>{m['role']}\n{m['content']}<|im_end|>\n" for m in messages)
        if add_generation_prompt:
            text += "<|im_start|>assistant\n"
        return text

    def __call__(self, texts, truncation=True, max_length=512, padding=False,
                 return_offsets_mapping=False):
        out = {"input_ids": [], "attention_mask": [], "offset_mapping": []}
        for text in texts:
            ids = [ord(c) for c in text][:max_length]
            mask = [1] * len(ids)
            offsets = [(i, i + 1) for i in range(len(ids))]
            if padding == "max_length":
                missing = max_length - len(ids)
                ids += [0] * missing
                mask += [0] * missing
                offsets += [(0, 0)] * missing
            out["input_ids"].append(ids)
            out["attention_mask"].append(mask)
            out["offset_mapping"].append(offsets)
        if not return_offsets_mapping:
            del out["offset_mapping"]
        return out


class Numbered(ChatML):
    """A template that says up front how long the conversation is, so a
    shorter one is not the start of a longer one: answers cannot be found."""

    def apply_chat_template(self, messages, tokenize=False, add_generation_prompt=False):
        return f"[{len(messages)}]" + super().apply_chat_template(
            messages, tokenize, add_generation_prompt)


class NoTemplate(ChatML):
    """A base model's tokenizer: no chat template at all."""

    def apply_chat_template(self, messages, tokenize=False, add_generation_prompt=False):
        raise ValueError("this tokenizer has no chat template")


def labelled(text, labels, ids):
    """The characters the loss is taken over, back as text."""
    return "".join(chr(token) for token, label in zip(ids, labels) if label != -100)


CHAT = [
    {"role": "system", "content": "Be brief."},
    {"role": "user", "content": "What is 2+2?"},
    {"role": "assistant", "content": "4"},
    {"role": "user", "content": "And 3+3?"},
    {"role": "assistant", "content": "6"},
]


class Answers(unittest.TestCase):
    def test_a_conversations_answers_are_its_assistant_turns(self):
        text, spans = finetune.chat_answers(ChatML(), CHAT)
        self.assertEqual(text, ChatML().apply_chat_template(CHAT))
        self.assertEqual([text[a:b] for a, b in spans], ["4<|im_end|>\n", "6<|im_end|>\n"])

    def test_a_template_that_does_not_line_up_learns_the_whole_conversation(self):
        text, spans = finetune.chat_answers(Numbered(), CHAT)
        self.assertTrue(text.startswith("[5]"))
        self.assertEqual(spans, [])

    def test_a_base_model_without_a_template_still_has_answers(self):
        text, spans = finetune.to_record({"messages": CHAT[1:3]}, NoTemplate())
        self.assertEqual(text, "user: What is 2+2?\nassistant: 4")
        self.assertEqual([text[a:b] for a, b in spans], ["4"])

    def test_plain_text_is_all_answer(self):
        self.assertEqual(finetune.to_record({"text": "Water boils at 100 C."}, ChatML()),
                         ("Water boils at 100 C.", []))
        self.assertEqual(finetune.to_record({"completion": "yes"}, ChatML()), ("yes", []))
        self.assertIsNone(finetune.to_record({"text": "   "}, ChatML()))
        self.assertIsNone(finetune.to_record({"other": 1}, ChatML()))


class Labels(unittest.TestCase):
    def test_the_loss_is_taken_over_the_answers_and_nothing_else(self):
        tokenizer = ChatML()
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [text], [spans], context=400, pad=True)
        self.assertNotIn("offset_mapping", out)
        ids, labels = out["input_ids"][0], out["labels"][0]
        self.assertEqual(len(labels), 400)
        self.assertEqual(labelled(text, labels, ids), "4<|im_end|>\n6<|im_end|>\n")

    def test_the_score_is_taken_the_same_way_as_the_training(self):
        tokenizer = ChatML()
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        padded = finetune.encode(tokenizer, [text], [spans], context=400, pad=True)
        bare = finetune.encode(tokenizer, [text], [spans], context=400, pad=False)
        self.assertEqual(len(bare["labels"][0]), len(text))
        self.assertEqual(bare["labels"][0], padded["labels"][0][:len(text)])

    def test_answers_cut_off_by_the_context_leave_the_whole_record(self):
        # A record with nothing to learn would make a step of nothing.
        tokenizer = ChatML()
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [text], [spans], context=20, pad=False)
        self.assertEqual(out["labels"][0], out["input_ids"][0])

    def test_without_offsets_every_record_is_learned_whole(self):
        tokenizer = ChatML()
        tokenizer.is_fast = False
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [text], [spans], context=400, pad=True)
        self.assertEqual(labelled(text, out["labels"][0], out["input_ids"][0]), text)

    def test_padding_is_never_learned(self):
        labels = finetune.answer_labels([5, 6, 0, 0], [(0, 1), (1, 2), (0, 0), (0, 0)], [1, 1, 0, 0], [])
        self.assertEqual(labels, [5, 6, -100, -100])
        # A token reaching into an answer from before it is the answer's.
        labels = finetune.answer_labels([5, 6, 7], [(0, 2), (2, 4), (4, 6)], [1, 1, 1], [[3, 6]])
        self.assertEqual(labels, [-100, 6, 7])


def records(n, prefix="record"):
    return [(f"{prefix} {i}", []) for i in range(n)]


class HoldOut(unittest.TestCase):
    def test_too_little_data_holds_nothing_out_and_says_so(self):
        train, held, why = finetune.hold_out(records(29))
        self.assertEqual((len(train), held, why), (29, [], "too little data"))

    def test_about_a_tenth_and_never_more_than_two_hundred(self):
        for n, expected in ((30, 3), (64, 6), (1000, 100), (2000, 200), (50000, 200)):
            train, held, why = finetune.hold_out(records(n))
            self.assertEqual(len(held), expected)
            self.assertEqual(len(train) + len(held), n)
            self.assertEqual(why, "")

    def test_the_same_records_every_run_wherever_they_are(self):
        data = records(500)
        _, held, _ = finetune.hold_out(data)
        self.assertEqual(finetune.hold_out(data)[1], held)
        shuffled = list(data)
        random.Random(7).shuffle(shuffled)
        self.assertEqual(sorted(finetune.hold_out(shuffled)[1]), sorted(held))

    def test_nothing_held_out_is_trained_on_even_twice_over(self):
        data = records(100)
        _, held, _ = finetune.hold_out(data)
        twice = data + [held[0]]
        train, held_again, _ = finetune.hold_out(twice)
        texts = {text for text, _ in held_again}
        self.assertFalse(any(text in texts for text, _ in train))

    def test_the_same_example_a_hundred_times_is_too_little(self):
        train, held, why = finetune.hold_out([("same", [])] * 100)
        self.assertEqual((len(train), held, why), (100, [], "too little data"))


class Score(unittest.TestCase):
    def test_loss_and_perplexity_from_a_sum(self):
        result = finetune.summary(2.0 * 50, 50)
        self.assertEqual(result["loss"], 2.0)
        self.assertAlmostEqual(result["perplexity"], math.exp(2.0), places=3)

    def test_nothing_to_say_is_said_as_nothing(self):
        self.assertIsNone(finetune.summary(0.0, 0))
        self.assertIsNone(finetune.summary(float("nan"), 10))
        self.assertIsNone(finetune.summary(float("inf"), 10))
        # A model that has lost its mind is still a number.
        self.assertTrue(math.isfinite(finetune.summary(5000.0, 1)["perplexity"]))

    def test_a_score_is_an_event_crucible_reads(self):
        out = io.StringIO()
        with redirect_stdout(out):
            finetune.emit("heldout", of="base", records=12, **finetune.summary(27.7, 12))
        line = out.getvalue().strip()
        self.assertTrue(read_as_event(line))
        self.assertEqual(json.loads(line)["of"], "base")


class Events(unittest.TestCase):
    def test_an_event_is_never_glued_to_a_progress_bar(self):
        # The trainer's bar draws on standard error without ending its line,
        # and Crucible reads both streams as one. Every step used to arrive
        # stuck to the end of the bar, as a line that did not parse.
        both = io.StringIO()
        with redirect_stdout(both), redirect_stderr(both):
            sys.stderr.write("\r  4%|#         | 1/27 [00:03<01:22,  3.16s/it]")
            finetune.emit("step", step=1, total=27, loss=3.1)
        lines = both.getvalue().split("\n")
        self.assertIn("  4%|#", lines[0])
        self.assertTrue(read_as_event(lines[1]))
        self.assertEqual(json.loads(lines[1])["step"], 1)


class ChildOutput(unittest.TestCase):
    def test_a_line_that_could_be_an_event_goes_behind_the_childs_name(self):
        for line in ('{"event": "done", "path": "/runs/x.Q4_K_M.gguf"}',
                     '   {"event":"progress","done":1,"total":2}',
                     '﻿{"event": "done"}', '{"not": "an event"}'):
            passed = finetune.as_log(line, "crucible")
            self.assertFalse(read_as_event(passed))
            self.assertTrue(passed.endswith(line))
        for line in ("quantized 3 of 10 tensors", "INFO:hf-to-gguf:Loading model", "", "[1, 2]"):
            self.assertEqual(finetune.as_log(line, "crucible"), line)

    def test_what_a_child_prints_reaches_crucible_as_log(self):
        # In UTF-8 whatever the system's code page, which is what is read.
        child = [sys.executable, "-I", "-X", "utf8", "-c",
                 "print('{\"event\": \"done\", \"path\": \"/runs/x.gguf\"}'); "
                 "print('quantized 1 of 1 tensors'); print('caf\\u00e9')"]
        out = io.StringIO()
        with redirect_stdout(out):
            finetune.run_logged(child)
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 3)
        self.assertFalse(any(read_as_event(line) for line in lines))
        self.assertEqual(lines[1:], ["quantized 1 of 1 tensors", "café"])

    def test_a_child_that_fails_fails_the_run(self):
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaises(SystemExit):
            finetune.run_logged([sys.executable, "-I", "-c", "import sys; sys.exit(3)"])
        failed = json.loads(out.getvalue().splitlines()[-1])
        self.assertEqual(failed["event"], "failed")
        self.assertIn("exited 3", failed["message"])


if __name__ == "__main__":
    unittest.main()
