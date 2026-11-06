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
        out = finetune.encode(tokenizer, [text], [spans], context=400)
        self.assertNotIn("offset_mapping", out)
        ids, labels = out["input_ids"][0], out["labels"][0]
        # As long as the record, not the context: padding is the batch's.
        self.assertEqual(len(labels), len(text))
        self.assertEqual(labelled(text, labels, ids), "4<|im_end|>\n6<|im_end|>\n")

    def test_a_record_scores_the_same_in_a_batch_as_alone(self):
        # The score and the training both take records in batches padded to
        # their longest; at a real position, nothing changes but the padding.
        tokenizer = ChatML()
        short, short_spans = finetune.chat_answers(tokenizer, CHAT[:3])
        long, long_spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [short, long], [short_spans, long_spans], context=400)
        batch = finetune.padded(out, [1, 0], pad=0)
        width = len(batch["input_ids"][0])
        self.assertEqual(width % 8, 0)
        self.assertGreaterEqual(width, len(long))
        self.assertTrue(all(len(row) == width for key in batch for row in batch[key]))
        alone = out["labels"][0]
        self.assertEqual(batch["labels"][1][:len(alone)], alone)
        self.assertEqual(batch["input_ids"][1][:len(short)], out["input_ids"][0])

    def test_answers_cut_off_by_the_context_leave_the_whole_record(self):
        # A record with nothing to learn would make a step of nothing.
        tokenizer = ChatML()
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [text], [spans], context=20)
        self.assertEqual(out["labels"][0], out["input_ids"][0])

    def test_without_offsets_every_record_is_learned_whole(self):
        tokenizer = ChatML()
        tokenizer.is_fast = False
        text, spans = finetune.chat_answers(tokenizer, CHAT)
        out = finetune.encode(tokenizer, [text], [spans], context=400)
        self.assertEqual(labelled(text, out["labels"][0], out["input_ids"][0]), text)

    def test_padding_is_never_learned(self):
        labels = finetune.answer_labels([5, 6, 0, 0], [(0, 1), (1, 2), (0, 0), (0, 0)], [1, 1, 0, 0], [])
        self.assertEqual(labels, [5, 6, -100, -100])
        # A token reaching into an answer from before it is the answer's.
        labels = finetune.answer_labels([5, 6, 7], [(0, 2), (2, 4), (4, 6)], [1, 1, 1], [[3, 6]])
        self.assertEqual(labels, [-100, 6, 7])
        # Nor is a batch's: never attended to, never a label.
        examples = {"input_ids": [[5, 6, 7], [8]], "attention_mask": [[1, 1, 1], [1]],
                    "labels": [[5, 6, 7], [8]]}
        batch = finetune.padded(examples, [0, 1], pad=0)
        self.assertEqual(batch["attention_mask"][1], [1] + [0] * 7)
        self.assertEqual(batch["labels"][1], [8] + [-100] * 7)
        self.assertEqual(batch["input_ids"][1], [8] + [0] * 7)

    def test_the_first_token_is_never_scored(self):
        # Each position predicts the next, so there is nothing before it.
        self.assertEqual(finetune.answer_tokens([5, 6, 7]), 2)
        self.assertEqual(finetune.answer_tokens([-100, 6, -100, 8]), 2)
        self.assertEqual(finetune.answer_tokens([5]), 0)


class Batches(unittest.TestCase):
    def test_every_record_is_trained_on_once_a_pass_in_steps_of_eight(self):
        lengths = [random.Random(n).randint(5, 500) for n in range(203)]
        for epoch in range(3):
            steps = finetune.plan_steps(lengths, epoch)
            seen = sorted(i for step in steps for i in step)
            self.assertEqual(seen, list(range(203)))
            self.assertTrue(all(1 <= len(step) <= finetune.PER_STEP for step in steps))
            # As many steps as a pass has always had.
            self.assertEqual(len(steps), math.ceil(203 / finetune.PER_STEP))

    def test_the_same_order_every_run_and_another_every_pass(self):
        lengths = list(range(1, 100))
        self.assertEqual(finetune.plan_steps(lengths, 0), finetune.plan_steps(lengths, 0))
        self.assertNotEqual(finetune.plan_steps(lengths, 0), finetune.plan_steps(lengths, 1))

    def test_a_step_is_records_of_about_one_length(self):
        # Sorted within runs of a few steps: a step pads to little.
        lengths = [random.Random(n).randint(5, 500) for n in range(640)]
        steps = finetune.plan_steps(lengths, 0)
        spread = [max(lengths[i] for i in step) - min(lengths[i] for i in step) for step in steps]
        self.assertLess(sum(spread) / len(spread), 100)

    def test_a_pass_holds_what_the_budget_allows_and_one_record_at_least(self):
        lengths = [100, 30, 500, 60, 61, 8]
        everything = list(range(len(lengths)))
        for budget in (1, 128, 300, 1000, 10000):
            out = finetune.passes(everything, lengths, budget)
            self.assertEqual(sorted(i for one in out for i in one), everything)
            for one in out:
                # Longest first, so the pass is as wide as its first record.
                self.assertEqual(one[0], max(one, key=lambda i: lengths[i]))
                if len(one) > 1:
                    self.assertLessEqual(len(one) * finetune.round_up(lengths[one[0]]), budget)
        self.assertEqual(len(finetune.passes(everything, lengths, 10000)), 1)
        self.assertEqual(len(finetune.passes(everything, lengths, 1)), len(lengths))

    def test_a_padded_length_is_a_multiple_of_eight(self):
        self.assertEqual([finetune.round_up(n) for n in (1, 8, 9, 511, 512)], [8, 8, 16, 512, 512])


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


class WarmConverter(unittest.TestCase):
    """The converter started during the merge, told its arguments after."""

    def script(self, directory, body):
        path = os.path.join(directory, "convert_hf_to_gguf.py")
        with open(path, "w", encoding="utf-8") as out:
            out.write(body)
        return path

    def run_converter(self, converter, arguments):
        out = io.StringIO()
        with redirect_stdout(out):
            converter.run(arguments)
        return out.getvalue().splitlines()

    def test_its_arguments_arrive_whether_it_was_warmed_or_not(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = self.script(directory, "import sys\nprint('given', sys.argv[1:])\n"
                                          "print('{\"event\": \"done\", \"path\": \"x\"}')\n")
            for warm in (True, False):
                converter = finetune.Converter(path)
                if warm:
                    converter.warm()
                lines = self.run_converter(converter, ["merged", "--outfile", "out.gguf"])
                self.assertEqual(lines[0], "given ['merged', '--outfile', 'out.gguf']")
                # What it prints is log, never one of the run's events.
                self.assertFalse(any(read_as_event(line) for line in lines))

    def test_one_that_fails_fails_the_run(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            converter = finetune.Converter(self.script(directory, "import sys\nsys.exit(4)\n"))
            converter.warm()
            out = io.StringIO()
            with redirect_stdout(out), self.assertRaises(SystemExit):
                converter.run([])
            failed = json.loads(out.getvalue().splitlines()[-1])
            self.assertEqual(failed["event"], "failed")
            self.assertIn("exited 4", failed["message"])

    def test_one_never_told_to_start_leaves_when_the_run_does(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = self.script(directory, "open(__file__ + '.ran', 'w').close()\n")
            converter = finetune.Converter(path)
            converter.warm()
            converter.process.stdin.close()
            self.assertEqual(converter.process.wait(timeout=60), 0)
            converter.process.stdout.close()
            self.assertFalse(os.path.exists(path + ".ran"))


class Card(unittest.TestCase):
    def test_a_card_already_chosen_is_kept_and_so_is_none(self):
        # An empty CUDA_VISIBLE_DEVICES is CUDA's way of saying no card; it
        # was read as no choice, and the card with the most memory taken.
        from unittest import mock

        for chosen in ("", "1", "-1"):
            with mock.patch.dict(os.environ, {"CUDA_VISIBLE_DEVICES": chosen}), \
                    mock.patch.object(finetune.subprocess, "run") as asked:
                self.assertEqual(finetune.pick_gpu(), chosen)
                asked.assert_not_called()


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
