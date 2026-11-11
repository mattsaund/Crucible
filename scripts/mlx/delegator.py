# SPDX-License-Identifier: MIT
"""An MLX model as Crucible's delegator.

The delegator routes each message to an expert and decides whether it is
answered or built. It does that by scoring rather than writing. Every
expert's name is weighed as the text that follows the prompt, and the most
likely one wins (LoadedModel::score_labels in src/llm/loaded_model.cpp). A
chat API only generates, and gives no probabilities for text it is handed,
so an MLX model cannot route through mlx_lm.server the way it answers. This
loads the model with mlx-lm directly and answers the core's questions.

    python delegator.py <model folder>

It reads one JSON object a line on standard input and writes one a line on
standard output. Once the model is loaded it says

    {"ready": true, "takes_effort": false}

or {"error": "..."} and exits. After that, each request has an "id", and its
answer carries the same one.

    {"id": 1, "method": "format", "messages": [{"role": ..., "content": ...}]}
        -> {"id": 1, "text": "<the conversation in the model's chat template>"}
    {"id": 2, "method": "score", "prompt": "...", "labels": ["...", ...],
     "prefix": "...", "context": 4096}
        -> {"id": 2, "scores": [-0.4, -3.1, null, ...]}
    {"id": 3, "method": "generate", "prompt": "...", "max_tokens": 64,
     "temperature": 0.2}
        -> {"id": 3, "text": "..."}

A request it could not answer comes back as {"id": n, "error": "..."}. A
score is null for a label that could not be read, where the core's own
scorer gives kUnscored. Closing its input ends it.

It runs on Crucible's Python, the training environment's, which has mlx-lm
on a Mac with Apple Silicon. Nothing it loads is fetched. The folder is a
local one, and the core starts it with HF_HUB_OFFLINE set.
"""

from __future__ import annotations

import copy
import json
import sys
from collections import OrderedDict

# Where answers go. main() makes it the real standard output and points
# sys.stdout at standard error, so that whatever else prints, a library's
# warning or a progress line, goes to the core's log. A line of it written
# in the middle of an answer would be read as the answer.
PROTOCOL = sys.stdout

# How many instruction prefixes are kept read, as the core's scorer keeps
# them (kKeptPrompts). A route asks with three orders of the roster, and a
# build's tasks and the answer-or-build question have prompts of their own.
KEPT_PREFIXES = 6

# How many tokens are read at once. A long prompt read whole asks for its
# whole attention at once, which on a small Mac is memory it does not have.
READ_STEP = 512

# ChatML, which the core falls back to for a model with no template.
CHATML = ("{% for message in messages %}<|im_start|>{{ message['role'] }}\n"
          "{{ message['content'] }}<|im_end|>\n{% endfor %}"
          "{% if add_generation_prompt %}<|im_start|>assistant\n{% endif %}")


def say(message):
    PROTOCOL.write(json.dumps(message) + "\n")
    PROTOCOL.flush()


def reusable_prefix(cached, wanted):
    """How much of `wanted` the tokens of `cached` cover from its start.

    Never all of it, as in the core's detail::reusable_prefix. The last
    token is always read, since that is where the scores come from."""
    common = 0
    while common < len(cached) and common < len(wanted) and cached[common] == wanted[common]:
        common += 1
    if common == len(wanted) and common > 0:
        common -= 1
    return common


def takes_effort(template):
    """Whether the chat template reads a reasoning effort, as gpt-oss's
    harmony format does. The core then asks it for little thinking when it
    writes, as it asks a GGUF (LoadedModel::takes_effort)."""
    return bool(template) and ("reasoning_effort" in template or "<|channel|>" in template)


class Delegator:
    """One model, and the prefixes of prompts it has read.

    The parts that touch MLX are kept to a few methods, so the bookkeeping
    around them can be tested with a model that is not one."""

    def __init__(self, model, tokenizer):
        self.model = model
        self.tokenizer = tokenizer
        self.kept = OrderedDict()
        # Tokens read through the model, for a test to see a prefix kept.
        self.read_count = 0

    # --- what the core asks ---------------------------------------------------

    def format(self, messages):
        """The conversation in the model's own chat template, with the turn
        the model answers opened at the end. ChatML for a model without one,
        as the core falls back to for a GGUF without one."""
        template = getattr(self.tokenizer, "chat_template", None)
        return self.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True,
                                                  **({} if template else {"chat_template": CHATML}))

    def score(self, prompt, labels, prefix="", context=0):
        """How likely each label is as the continuation of `prompt`, as a
        natural log-probability summed over the label's tokens. None for a
        label that turns into no tokens at all."""
        scores = [None] * len(labels)
        if not labels:
            return scores
        tokens = self.encode(prompt)
        if not tokens:
            return scores
        recurs = 0
        if context > 0 and len(tokens) >= context:
            # The head goes, and the prefix with it, as in the core.
            tokens = tokens[len(tokens) - (context - min(256, context // 4)):]
        elif prefix and prompt.startswith(prefix):
            # The prefix as it tokenizes inside the whole prompt, since a
            # token can straddle where it ends, and only what the two agree on
            # is the same from one call to the next.
            recurs = reusable_prefix(self.encode(prefix), tokens)

        cache = self.recall(tokens[:recurs])
        first = self.log_probabilities(self.read(cache, tokens[recurs:]))

        later = []
        for i, label in enumerate(labels):
            ids = self.tokenizer.encode(label, add_special_tokens=False)
            if ids:
                scores[i] = float(first[ids[0]])
                if len(ids) > 1:
                    later.append((i, ids))
        if later:
            self.label_rests(cache, later, scores)
        return scores

    def generate(self, prompt, max_tokens=64, temperature=0.2):
        from mlx_lm import generate
        from mlx_lm.sample_utils import make_sampler

        return generate(self.model, self.tokenizer, prompt=prompt, max_tokens=max_tokens,
                        sampler=make_sampler(temp=temperature), verbose=False)

    # --- the parts -----------------------------------------------------------

    def encode(self, text):
        """`text` as the model's tokens, the special ones in it read as
        special. The start-of-text token is added unless the text begins
        with it already, which a chat template's output often does, as
        mlx-lm's own generate decides."""
        bos = getattr(self.tokenizer, "bos_token", None)
        add = not (bos and text.startswith(bos))
        return list(self.tokenizer.encode(text, add_special_tokens=add))

    def recall(self, head):
        """A cache holding `head` read, to read the rest of a prompt into.

        A copy of one kept, because reading the question into the kept one
        would leave it holding that question for the next. The least
        recently used goes when more are kept than KEPT_PREFIXES."""
        if not head:
            return self.fresh_cache()
        key = tuple(head)
        if key in self.kept:
            self.kept.move_to_end(key)
        else:
            cache = self.fresh_cache()
            self.read(cache, head)
            self.kept[key] = cache
            while len(self.kept) > KEPT_PREFIXES:
                self.kept.popitem(last=False)
        return self.branch(self.kept[key])

    def label_rests(self, cache, later, scores):
        """Add the log-probability of each label's tokens after its first.

        All of them in one pass, each label on a row of its own branched from
        the prompt's cache, as the core reads a roster's labels at once. A
        pass costs about the same for one row as for ten, and a label at a
        time was most of a route. A cache mlx-lm cannot make a batch of is
        read a label at a time instead."""
        batch = self.batch_of(cache, len(later))
        if batch is None:
            for i, ids in later:
                scores[i] += self.label_rest(cache, ids)
            return
        width = max(len(ids) for _, ids in later) - 1
        # A shorter label's row is filled out with its own last token read.
        # The filling comes after every position a score is read at, so it
        # changes none of them.
        rows = [ids[:-1] + [ids[-2]] * (width - len(ids) + 1) for _, ids in later]
        logits = self.read_rows(batch, rows)
        for row, (i, ids) in enumerate(later):
            for position, token in enumerate(ids[1:]):
                scores[i] += float(logits[row, position, token])

    def label_rest(self, cache, ids):
        """The log-probability of each of a label's tokens after its first,
        read after the prompt and taken back off again.

        A cache that can be cut is read into and cut back, which costs
        nothing. One that cannot (a recurrent state, a sliding window) is
        read into a copy, so every label is scored against the prompt
        alone and not behind the labels before it."""
        if self.trimmable(cache):
            branch = cache
        else:
            branch = self.branch(cache)
        total = 0.0
        logits = self.read_all(branch, ids[:-1])
        for position, token in enumerate(ids[1:]):
            total += float(self.log_probabilities(logits[position])[token])
        if branch is cache:
            self.trim(cache, len(ids) - 1)
        return total

    # --- MLX ------------------------------------------------------------------

    def fresh_cache(self):
        from mlx_lm.models.cache import make_prompt_cache

        return make_prompt_cache(self.model)

    @staticmethod
    def branch(cache):
        return copy.deepcopy(cache)

    @staticmethod
    def batch_of(cache, count):
        """`count` copies of `cache` as one batch, or None for a cache that
        mlx-lm cannot batch."""
        try:
            return [layer.merge([layer] * count) for layer in cache]
        except (AttributeError, NotImplementedError, TypeError, ValueError):
            return None

    @staticmethod
    def trimmable(cache):
        from mlx_lm.models.cache import can_trim_prompt_cache

        return can_trim_prompt_cache(cache)

    @staticmethod
    def trim(cache, count):
        from mlx_lm.models.cache import trim_prompt_cache

        trim_prompt_cache(cache, count)

    def read(self, cache, tokens):
        """Read `tokens` into `cache` and return the logits after the last."""
        import mlx.core as mx

        logits = None
        for start in range(0, len(tokens), READ_STEP):
            piece = tokens[start:start + READ_STEP]
            logits = self.model(mx.array(piece)[None], cache=cache)[0, -1]
            mx.eval(logits, [c.state for c in cache])
            self.read_count += len(piece)
        return logits

    def read_all(self, cache, tokens):
        """Read `tokens` into `cache` and return the logits after each."""
        import mlx.core as mx

        logits = self.model(mx.array(tokens)[None], cache=cache)[0]
        mx.eval(logits)
        self.read_count += len(tokens)
        return logits

    def read_rows(self, cache, rows):
        """Read each of `rows` into its row of a batched cache, and return the
        log-probabilities after every token of every row."""
        import mlx.core as mx

        logits = self.model(mx.array(rows), cache=cache).astype(mx.float32)
        logits = logits - mx.logsumexp(logits, axis=-1, keepdims=True)
        mx.eval(logits)
        self.read_count += sum(len(row) for row in rows)
        return logits

    @staticmethod
    def log_probabilities(logits):
        """Natural log-probabilities over the vocabulary, worked out in 32
        bits as the core's are. A model's own 16 bits round the normalizer,
        and the differences between labels are small."""
        import mlx.core as mx

        logits = logits.astype(mx.float32)
        return logits - mx.logsumexp(logits)


def load(path):
    """The model and its tokenizer, on the Mac's GPU when MLX can reach it
    and on the processor when it cannot. A Mac in a virtual machine, as a
    CI runner is, has no Metal device."""
    import mlx.core as mx
    from mlx_lm import load as load_model

    if not mx.metal.is_available():
        mx.set_default_device(mx.cpu)
    model, tokenizer = load_model(path)
    return Delegator(model, tokenizer)


def answer(delegator, request):
    method = request.get("method")
    if method == "format":
        return {"text": delegator.format(request.get("messages") or [])}
    if method == "score":
        labels = [label if isinstance(label, str) else "" for label in request.get("labels") or []]
        return {"scores": delegator.score(request.get("prompt") or "", labels,
                                          request.get("prefix") or "", int(request.get("context") or 0))}
    if method == "generate":
        return {"text": delegator.generate(request.get("prompt") or "",
                                           int(request.get("max_tokens") or 64),
                                           float(request.get("temperature", 0.2)))}
    raise ValueError(f"no method called {method!r}")


def serve(delegator, lines):
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except ValueError:
            say({"id": None, "error": "a request that is not JSON"})
            continue
        reply = {"id": request.get("id")}
        try:
            reply.update(answer(delegator, request))
        except Exception as error:   # one bad request is not the end of the model
            reply["error"] = f"{type(error).__name__}: {error}"
        say(reply)


def main():
    global PROTOCOL
    PROTOCOL, sys.stdout = sys.stdout, sys.stderr
    if len(sys.argv) != 2:
        say({"error": "usage: delegator.py <model folder>"})
        return 2
    try:
        delegator = load(sys.argv[1])
    except Exception as error:   # said to the core, which shows it
        say({"error": f"{type(error).__name__}: {error}"})
        return 1
    template = getattr(delegator.tokenizer, "chat_template", None)
    say({"ready": True, "takes_effort": takes_effort(template if isinstance(template, str) else "")})
    serve(delegator, sys.stdin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
