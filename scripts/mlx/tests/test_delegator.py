# SPDX-License-Identifier: MIT
"""The MLX delegator against a model small enough to make here.

A two-layer model with random weights and a tokenizer of one token a byte,
written to a folder the way an MLX model is published, so nothing is
fetched. What it says is nonsense; what is tested is that its scores are
the log-probabilities the model gives each label after the prompt, the same
whether the instructions were kept or read again, and that the core's
questions get the answers delegator.py promises.

Skipped where MLX cannot run, which is everywhere but a Mac with Apple
Silicon, and there too without mlx-lm. On the processor when MLX has no
GPU to reach, as in a virtual machine.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import delegator  # noqa: E402

try:
    import mlx.core as mx
    import mlx_lm  # noqa: F401
    from tokenizers import Tokenizer, models, pre_tokenizers, decoders, processors
    from transformers import PreTrainedTokenizerFast

    if not mx.metal.is_available():
        mx.set_default_device(mx.cpu)
    mx.eval(mx.array([1.0]) + 1)
    MLX = ""
except Exception as error:  # noqa: BLE001 - any reason MLX cannot run here is a reason to skip
    MLX = f"MLX cannot run here: {type(error).__name__}: {error}"

TEMPLATE = ("{{ bos_token }}{% for message in messages %}<|im_start|>{{ message['role'] }}\n"
            "{{ message['content'] }}<|im_end|>\n{% endfor %}"
            "{% if add_generation_prompt %}<|im_start|>assistant\n{% endif %}")


def make_model(folder, kind="llama"):
    """A tiny model and its tokenizer, written to `folder`: a llama, a llama
    with a sliding window ("sliding"), or an LFM2 ("lfm2"), whose short
    convolutions keep a state rather than a cache of tokens. Neither of the
    last two caches can be cut back, so a label is scored from a copy."""
    from mlx.utils import tree_flatten
    from mlx_lm.models import lfm2, llama

    bytes_to_unicode = pre_tokenizers.ByteLevel.alphabet()
    vocab = {character: index for index, character in enumerate(sorted(bytes_to_unicode))}
    specials = ["<s>", "<|im_start|>", "<|im_end|>"]
    for token in specials:
        vocab[token] = len(vocab)
    tokenizer = Tokenizer(models.BPE(vocab=vocab, merges=[]))
    tokenizer.pre_tokenizer = pre_tokenizers.ByteLevel(add_prefix_space=False)
    tokenizer.decoder = decoders.ByteLevel()
    tokenizer.add_special_tokens(specials)
    tokenizer.post_processor = processors.TemplateProcessing(single="<s> $A", special_tokens=[("<s>", vocab["<s>"])])
    fast = PreTrainedTokenizerFast(tokenizer_object=tokenizer, bos_token="<s>", eos_token="<|im_end|>")
    fast.chat_template = TEMPLATE
    fast.save_pretrained(folder)

    if kind == "lfm2":
        config = {"model_type": "lfm2", "vocab_size": len(vocab), "hidden_size": 32, "num_hidden_layers": 2,
                  "num_attention_heads": 4, "num_key_value_heads": 2, "max_position_embeddings": 4096,
                  "norm_eps": 1e-5, "conv_bias": False, "conv_L_cache": 3, "block_dim": 32,
                  "block_multiple_of": 16, "block_ffn_dim_multiplier": 1.0, "block_auto_adjust_ff_dim": False,
                  "block_ff_dim": 64, "full_attn_idxs": [1]}
        module = lfm2
    else:
        config = {"model_type": "llama", "hidden_size": 32, "num_hidden_layers": 2, "intermediate_size": 64,
                  "num_attention_heads": 4, "num_key_value_heads": 2, "rms_norm_eps": 1e-5,
                  "vocab_size": len(vocab), "rope_theta": 10000.0, "tie_word_embeddings": True,
                  "max_position_embeddings": 4096}
        if kind == "sliding":
            config["sliding_window"] = 4
            config["layer_types"] = ["sliding_attention", "full_attention"]
        module = llama
    mx.random.seed(7)
    model = module.Model(module.ModelArgs.from_dict(config))
    mx.eval(model.parameters())
    mx.save_safetensors(os.path.join(folder, "model.safetensors"), dict(tree_flatten(model.parameters())))
    with open(os.path.join(folder, "config.json"), "w", encoding="utf-8") as out:
        json.dump(config, out)


def expected(model, prompt_tokens, label_tokens):
    """A label's log-probability read the long way, with prompt and label in
    one pass and no cache, each label token's share added up."""
    sequence = prompt_tokens + label_tokens
    logits = model(mx.array(sequence)[None])[0].astype(mx.float32)
    total = 0.0
    for offset, token in enumerate(label_tokens):
        row = logits[len(prompt_tokens) - 1 + offset]
        total += float((row - mx.logsumexp(row))[token])
    return total


@unittest.skipIf(MLX, MLX)
class TinyModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        make_model(cls.folder.name)
        cls.delegator = delegator.load(cls.folder.name)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def conversation(self, question):
        return self.delegator.format([{"role": "system", "content": "Route each question to a seat."},
                                      {"role": "user", "content": question}])

    def test_the_conversation_is_in_the_models_own_template(self):
        text = self.conversation("why is the sky blue")
        self.assertTrue(text.startswith("<s><|im_start|>system\nRoute each question"))
        self.assertTrue(text.endswith("<|im_start|>assistant\n"))

    def test_a_template_that_starts_with_the_start_token_is_not_given_a_second(self):
        prompt = self.conversation("hi")
        tokens = self.delegator.encode(prompt)
        start = self.delegator.tokenizer.convert_tokens_to_ids("<s>")
        self.assertEqual(tokens[0], start)
        self.assertNotEqual(tokens[1], start)
        self.assertEqual(self.delegator.encode("hi")[0], start)

    def test_each_score_is_the_labels_log_probability_after_the_prompt(self):
        prompt = self.conversation("what is two and two")
        labels = ["Math", "Writing", "C"]
        scores = self.delegator.score(prompt, labels)
        tokens = self.delegator.encode(prompt)
        for label, score in zip(labels, scores):
            ids = self.delegator.tokenizer.encode(label, add_special_tokens=False)
            self.assertAlmostEqual(score, expected(self.delegator.model, tokens, ids), places=3)
        self.assertTrue(all(score < 0 for score in scores))

    def test_a_label_that_is_no_tokens_has_no_score(self):
        scores = self.delegator.score(self.conversation("hi"), ["Math", ""])
        self.assertIsNotNone(scores[0])
        self.assertIsNone(scores[1])
        self.assertEqual(self.delegator.score(self.conversation("hi"), []), [])

    def test_kept_instructions_score_the_same_and_are_not_read_again(self):
        prefix = self.conversation("\x00").split("\x00")[0]
        first = self.conversation("what is two and two")
        second = self.conversation("write me a poem about rain")
        labels = ["Math", "Writing"]
        cold = [self.delegator.score(first, labels), self.delegator.score(second, labels)]

        self.delegator.kept.clear()
        self.delegator.score(first, labels, prefix)
        before = self.delegator.read_count
        warm = self.delegator.score(second, labels, prefix)
        read = self.delegator.read_count - before
        for a, b in zip(cold[1], warm):
            self.assertAlmostEqual(a, b, places=3)
        # The question and the labels' later tokens, not the instructions.
        self.assertLess(read, len(self.delegator.encode(second)) - len(self.delegator.encode(prefix)) + 20)

    def test_a_prompt_longer_than_the_context_keeps_its_end(self):
        prompt = self.conversation("x " * 300)
        tokens = self.delegator.encode(prompt)
        context = 256
        kept = tokens[len(tokens) - (context - min(256, context // 4)):]
        score = self.delegator.score(prompt, ["Math"], context=context)[0]
        ids = self.delegator.tokenizer.encode("Math", add_special_tokens=False)
        self.assertAlmostEqual(score, expected(self.delegator.model, kept, ids), places=3)

    def test_labels_read_together_score_as_they_do_one_at_a_time(self):
        prompt = self.conversation("what is two and two")
        labels = ["Math", "Writing", "Code", "C"]
        together = self.delegator.score(prompt, labels)
        alone = delegator.Delegator(self.delegator.model, self.delegator.tokenizer)
        alone.batch_of = lambda cache, count: None
        cut = alone.score(prompt, labels)
        copied = delegator.Delegator(self.delegator.model, self.delegator.tokenizer)
        copied.batch_of = lambda cache, count: None
        copied.trimmable = lambda cache: False
        for a, b, c in zip(together, cut, copied.score(prompt, labels)):
            self.assertAlmostEqual(a, b, places=3)
            self.assertAlmostEqual(a, c, places=3)

    def test_it_writes(self):
        text = self.delegator.generate(self.conversation("name this chat"), max_tokens=4, temperature=0.0)
        self.assertIsInstance(text, str)


@unittest.skipIf(MLX, MLX)
class Caches(unittest.TestCase):
    """A cache that is a window, or a state, cannot be cut back to before a
    label, so every label is scored from a copy of the prompt's cache, alone
    or with the others in a batch."""

    def scores_against_the_prompt_alone(self, kind):
        with tempfile.TemporaryDirectory() as folder:
            make_model(folder, kind)
            model = delegator.load(folder)
            prompt = model.format([{"role": "user", "content": "what is two and two"}])
            tokens = model.encode(prompt)
            read = model.fresh_cache()
            model.read(read, tokens)
            self.assertFalse(model.trimmable(read))
            labels = ["Math", "Writing", "Code"]
            alone = delegator.Delegator(model.model, model.tokenizer)
            alone.batch_of = lambda cache, count: None
            for label, score, one in zip(labels, model.score(prompt, labels), alone.score(prompt, labels)):
                ids = model.tokenizer.encode(label, add_special_tokens=False)
                self.assertAlmostEqual(score, expected(model.model, tokens, ids), places=3)
                self.assertAlmostEqual(one, score, places=3)

    def test_a_window_model_scores_each_label_against_the_prompt_alone(self):
        self.scores_against_the_prompt_alone("sliding")

    def test_a_model_with_a_recurrent_state_scores_each_label_against_the_prompt_alone(self):
        self.scores_against_the_prompt_alone("lfm2")


@unittest.skipIf(MLX, MLX)
class Protocol(unittest.TestCase):
    """delegator.py as the core runs it, a JSON object a line each way."""

    def test_it_answers_the_cores_questions_and_ends_when_its_input_does(self):
        with tempfile.TemporaryDirectory() as folder:
            make_model(folder)
            script = os.path.join(os.path.dirname(__file__), "..", "delegator.py")
            requests = [
                {"id": 1, "method": "format", "messages": [{"role": "user", "content": "hi"}]},
                {"id": 2, "method": "score", "prompt": "<s><|im_start|>user\nhi<|im_end|>\n", "labels": ["a", "bc", ""]},
                {"id": 3, "method": "generate", "prompt": "<s>hi", "max_tokens": 2, "temperature": 0},
                {"id": 4, "method": "nothing"},
            ]
            done = subprocess.run([sys.executable, script, folder], input="".join(json.dumps(r) + "\n" for r in requests),
                                  capture_output=True, text=True, timeout=300, check=False)
            self.assertEqual(done.returncode, 0, done.stderr)
            replies = [json.loads(line) for line in done.stdout.splitlines()]
            self.assertEqual(replies[0], {"ready": True, "takes_effort": False})
            self.assertEqual(replies[1]["id"], 1)
            self.assertTrue(replies[1]["text"].endswith("<|im_start|>assistant\n"))
            self.assertEqual(replies[2]["id"], 2)
            self.assertEqual(len(replies[2]["scores"]), 3)
            self.assertIsNone(replies[2]["scores"][2])
            self.assertIn("text", replies[3])
            self.assertIn("error", replies[4])

    def test_a_folder_that_is_not_a_model_is_said_and_ends_it(self):
        with tempfile.TemporaryDirectory() as folder:
            script = os.path.join(os.path.dirname(__file__), "..", "delegator.py")
            done = subprocess.run([sys.executable, script, folder], input="", capture_output=True, text=True,
                                  timeout=120, check=False)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("error", json.loads(done.stdout.splitlines()[0]))


class WithoutMLX(unittest.TestCase):
    """The parts that need no model, which run everywhere."""

    def test_the_last_token_is_always_read(self):
        self.assertEqual(delegator.reusable_prefix([1, 2, 3], [1, 2, 3]), 2)
        self.assertEqual(delegator.reusable_prefix([1, 2], [1, 2, 3]), 2)
        self.assertEqual(delegator.reusable_prefix([1, 9], [1, 2, 3]), 1)
        self.assertEqual(delegator.reusable_prefix([], [1, 2, 3]), 0)

    def test_a_harmony_template_takes_an_effort(self):
        self.assertTrue(delegator.takes_effort("{{ reasoning_effort }}"))
        self.assertTrue(delegator.takes_effort("<|start|>assistant<|channel|>final"))
        self.assertFalse(delegator.takes_effort("<|im_start|>"))
        self.assertFalse(delegator.takes_effort(None))

    def test_a_request_it_cannot_answer_says_why_and_the_next_is_answered(self):
        class Plain:
            def format(self, messages):
                return "|".join(m["content"] for m in messages)

            def score(self, prompt, labels, prefix="", context=0):
                raise RuntimeError("no model")

        said = []
        real = delegator.say
        delegator.say = said.append
        try:
            delegator.serve(Plain(), ['{"id": 1, "method": "score", "labels": ["a"]}', "not json",
                                      '{"id": 2, "method": "format", "messages": [{"content": "a"}, {"content": "b"}]}'])
        finally:
            delegator.say = real
        self.assertEqual(said[0]["id"], 1)
        self.assertIn("no model", said[0]["error"])
        self.assertIn("error", said[1])
        self.assertEqual(said[2], {"id": 2, "text": "a|b"})


if __name__ == "__main__":
    unittest.main()
