# SPDX-License-Identifier: MIT
"""Fine-tune one expert from a Crucible recipe.

Crucible writes a recipe -- a base model, the data to specialize it on, and
how hard to train -- and this turns it into a model file. It is run by the
Create tab as a child process in Crucible's own Python environment, and it is
also runnable by hand:

    crucible-trainer/venv/bin/python finetune.py --recipe recipe.json --out out/

Everything it says about its progress goes to stdout as one JSON object per
line. Anything that is not a JSON object with an "event" key is log text --
transformers, torch and the CUDA loader all write to the stream too, and the
reader keeps those for the log rather than trying to understand them.

The loss is taken over the answers in the data, not over the questions:
a model is taught to reply, and the prompt it is given is not its to
predict. About a tenth of the records, the same ones every run, are held out
of training and scored on that loss before the first step and after the
last, which says as a number whether the fine-tune got better than its base
at what it was trained for.

Why an adapter and not a full fine-tune: rewriting every weight of a 7B model
wants a data-center GPU and produces a 14 GB file. A LoRA adapter trains a few
million parameters beside a frozen base, fits on a consumer card, and is
merged back into a normal model at the end -- so what comes out is an ordinary
model that any llama.cpp or MLX program can load, not something that needs
this program to use.
"""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Talking to Crucible
# ---------------------------------------------------------------------------


def emit(event: str, **fields) -> None:
    """One progress record. Flushed, because the reader is a pipe.

    On a line of its own, whatever was written before it. A progress bar --
    the trainer's, a download's -- draws on standard error and does not end
    its line, the two streams reach Crucible as one, and an event glued to
    the end of a bar is a line that does not parse: every training step was
    lost that way, and the page's bar stood at nothing. The blank line this
    leaves where there was no bar is passed over.
    """
    record = {"event": event}
    record.update(fields)
    sys.stdout.write("\n" + json.dumps(record) + "\n")
    sys.stdout.flush()


def phase(name: str, detail: str = "") -> None:
    emit("phase", name=name, detail=detail)


def fail(message: str, hint: str = "") -> "NoReturn":  # type: ignore[name-defined]
    emit("failed", message=message, hint=hint)
    sys.exit(1)


def pick_gpu() -> str:
    """Which card to train on, as a CUDA_VISIBLE_DEVICES value.

    One card, chosen before torch is imported, and this is not a preference.
    The training runs on the first card torch can see, and any other it can
    see is one it may take memory on. (The HuggingFace Trainer this used to
    run under went further: it wrapped the model in DataParallel across
    every card it saw, which a 4-bit model cannot survive.)

    Training an adapter does not want more than one card anyway: the whole
    premise is that the frozen base fits on one. So the one with the most
    free memory is picked, and the rest are left for whatever else is using
    the machine -- including Crucible's own inference.

    A choice already made is kept, an empty one too: that is CUDA's way of
    saying no card at all, and it was taken for no choice, and a card picked.
    """
    if "CUDA_VISIBLE_DEVICES" in os.environ:
        return os.environ["CUDA_VISIBLE_DEVICES"]
    try:
        reply = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=10, check=True,
        ).stdout
    except Exception:
        return ""
    free = [int(line) for line in reply.split() if line.strip().isdigit()]
    if not free:
        return ""
    best = max(range(len(free)), key=lambda i: free[i])
    emit("note", text=f"training on GPU {best}, which has the most free memory")
    return str(best)


# ---------------------------------------------------------------------------
# The recipe
# ---------------------------------------------------------------------------


def base_reference(recipe: dict) -> str:
    """Where the weights to specialize come from.

    A GGUF is refused here rather than three minutes later inside
    transformers. GGUF is an inference format: the weights have been
    quantized and the tensors rearranged for a runtime that only does forward
    passes, and there is no way back to something trainable. Fine-tuning needs
    the original repository.
    """
    base = recipe.get("base") or {}
    ref = base.get("path") or base.get("id") or ""
    if not ref:
        fail("this recipe has no base model")
    if ref.lower().endswith(".gguf"):
        fail(
            "the base model is a GGUF, which cannot be fine-tuned",
            "GGUF is a quantized inference format. Pick the model's Huggingface "
            "repository as the base instead -- the GGUF you have was made from it.",
        )
    if base.get("source") == "local":
        directory = Path(ref)
        if not (directory / "config.json").exists():
            fail(
                f"{ref} is not a Huggingface model directory",
                "It needs the config.json and safetensors the model was published with.",
            )
    return ref


def chat_answers(tokenizer, messages: list) -> tuple[str, list[list[int]]]:
    """A conversation as the model is trained on it, and where its answers are.

    The answers are the assistant's turns, found by rendering the
    conversation up to each one: a reply, and whatever ends its turn, lie
    between the prefix that asks for it and the prefix that includes it. A
    template that renders an earlier turn differently once it is no longer
    the last -- some drop an earlier answer's reasoning -- gives prefixes
    that do not line up, and then no answers are told apart and the whole
    conversation is learned, as everything was before they were.

    Spans are [start, end) character offsets into the text.
    """
    text = tokenizer.apply_chat_template(messages, tokenize=False)
    spans: list[list[int]] = []
    for i, message in enumerate(messages):
        if not isinstance(message, dict) or message.get("role") != "assistant":
            continue
        try:
            asking = tokenizer.apply_chat_template(
                messages[:i], tokenize=False, add_generation_prompt=True
            )
            answered = tokenizer.apply_chat_template(messages[: i + 1], tokenize=False)
        except Exception:
            return text, []
        if not (answered.startswith(asking) and text.startswith(answered)):
            return text, []
        if len(answered) > len(asking):
            spans.append([len(asking), len(answered)])
    return text, spans


def flat_answers(messages: list) -> tuple[str, list[list[int]]]:
    """A conversation for a base model with no chat template: one line a
    message, "role: content", with each assistant's content as an answer."""
    lines: list[str] = []
    spans: list[list[int]] = []
    at = 0
    for message in messages:
        if not isinstance(message, dict):
            continue
        role = str(message.get("role", ""))
        content = str(message.get("content", ""))
        line = f"{role}: {content}"
        if role == "assistant" and content:
            spans.append([at + len(role) + 2, at + len(line)])
        lines.append(line)
        at += len(line) + 1
    return "\n".join(lines), spans


def to_record(record: dict, tokenizer) -> tuple[str, list[list[int]]] | None:
    """One record of data as (text, answer spans), or None when it has none.

    Chat records go through the tokenizer's own chat template, so a model
    taught on them answers in the format it was trained to speak. Anything
    else is one text, which is all answer.
    """
    if isinstance(record.get("messages"), list):
        try:
            return chat_answers(tokenizer, record["messages"])
        except Exception:
            # A base model with no chat template. Flatten it rather than
            # dropping the record: the content is still worth training on.
            return flat_answers(record["messages"])
    for key in ("text", "content", "output", "completion"):
        if isinstance(record.get(key), str) and record[key].strip():
            return record[key], []
    return None


def read_records(recipe: dict, tokenizer) -> list[tuple[str, list[list[int]]]]:
    """Every record in every data asset of the recipe, as (text, answer spans).

    Three shapes are accepted because three shapes are what people have: a
    Huggingface dataset, a folder of text, and JSONL of either {"text": ...}
    or {"messages": [...]}.
    """
    records: list[tuple[str, list[list[int]]]] = []
    for asset in recipe.get("data") or []:
        reference = asset.get("path") or asset.get("id") or ""
        if not reference:
            continue
        source = asset.get("source", "hub")
        phase("data", f"reading {reference}")

        if source == "hub":
            # Imported only for a dataset from the hub: it is a quarter of a
            # second of pyarrow and pandas that a file on disk does not need.
            from datasets import load_dataset

            loaded = load_dataset(reference, split="train")
            records.extend(r for r in (to_record(dict(one), tokenizer) for one in loaded) if r and r[0])
            continue

        path = Path(reference)
        files = []
        if path.is_dir():
            for pattern in ("*.txt", "*.md", "*.jsonl", "*.json"):
                files.extend(sorted(path.rglob(pattern)))
        elif path.exists():
            files = [path]
        else:
            fail(f"{reference} is not on this machine")

        found: list[tuple[str, list[list[int]]]] = []
        for one in files:
            if one.suffix in (".jsonl", ".json"):
                for line in one.read_text(encoding="utf-8", errors="replace").splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        parsed = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    record = to_record(parsed, tokenizer) if isinstance(parsed, dict) else None
                    if record and record[0]:
                        found.append(record)
            else:
                body = one.read_text(encoding="utf-8", errors="replace").strip()
                if body:
                    found.append((body, []))
        if not found:
            fail(f"no usable text in {reference}")
        records.extend(found)

    if not records:
        fail("this recipe has no training data")
    return records


# The rate a recipe that names none trains at: Crucible's own default (see
# recipe.hpp), for a recipe written by hand.
LEARNING_RATE = 1e-4

# How much of the data is held out of training to score the result on: one
# record in ten, and at least a few and at most a couple of hundred. Fewer
# than three is not a score, and past two hundred the number stops moving
# while every one more costs a forward pass, twice.
HOLD_OUT_SHARE = 10
HOLD_OUT_LEAST = 3
HOLD_OUT_MOST = 200


def hold_out(records: list) -> tuple[list, list, str]:
    """The records to train on, the ones to score on, and why there are none
    of the second when there are not.

    The same records every run: picked by a hash of each one's text, not by
    where it is or at random, so training the recipe again scores the same
    answers and the two runs can be compared. A record whose text is also
    among the held out -- the same example twice -- is not trained on
    either, or the score would be of something the model had seen.
    """
    count = min(len(records) // HOLD_OUT_SHARE, HOLD_OUT_MOST)
    if count < HOLD_OUT_LEAST:
        return list(records), [], "too little data"
    order = sorted(
        range(len(records)),
        key=lambda i: hashlib.sha256(records[i][0].encode("utf-8")).hexdigest(),
    )
    chosen = set(order[:count])
    held = [records[i] for i in sorted(chosen)]
    seen = {text for text, _ in held}
    train = [r for i, r in enumerate(records) if i not in chosen and r[0] not in seen]
    if not train:
        return list(records), [], "too little data"
    return train, held, ""


def answer_labels(ids: list, offsets: list, mask: list, spans: list) -> list[int]:
    """What the loss is taken over: the tokens of the answers, and nothing
    that is padding.

    A token is an answer's when its characters overlap one. With no spans the
    whole text is the answer: plain text, or a conversation whose answers
    could not be told apart. So is a record whose answers the context cut off
    altogether, rather than a record with nothing in it to learn, which
    would make a step of nothing.
    """
    if spans and offsets:
        labels = [
            token if m == 1 and any(first < high and last > low for low, high in spans) else -100
            for token, m, (first, last) in zip(ids, mask, offsets)
        ]
        if any(label != -100 for label in labels):
            return labels
    return [token if m == 1 else -100 for token, m in zip(ids, mask)]


def encode(tokenizer, texts: list, spans: list, context: int) -> dict:
    """Tokens and labels for some records: the same for training as for the
    score, which is what makes the score a measure of what was trained.

    Each record as long as it is. Padding is added a batch at a time, to the
    longest record in the batch (see padded): padded to the whole context,
    a record of three hundred tokens was five hundred and twelve, and nearly
    half of every step went on positions that were thrown away.

    Answers are found by the characters each token came from, which only a
    fast tokenizer reports. Without one every record is learned whole.
    """
    fast = bool(getattr(tokenizer, "is_fast", False))
    out = tokenizer(
        texts,
        truncation=True,
        max_length=context,
        return_offsets_mapping=fast,
    )
    offsets = out.pop("offset_mapping") if fast else [[] for _ in texts]
    # Causal language modeling: the labels are the inputs, and the loss over
    # a position that is not an answer is thrown away.
    out["labels"] = [
        answer_labels(ids, offsets[i], mask, spans[i])
        for i, (ids, mask) in enumerate(zip(out["input_ids"], out["attention_mask"]))
    ]
    return out


# ---------------------------------------------------------------------------
# Batches
# ---------------------------------------------------------------------------

# Records per optimizer step. The trainer took them as eight passes of one
# record each, and now takes them in as few passes as the card holds; the
# number stays, so a recipe trains as many steps, at the same learning rate,
# as it always has.
PER_STEP = 8


def round_up(length: int) -> int:
    """A batch's padded length: a multiple of eight, the shape the card's
    matrix units work in."""
    return (length + 7) // 8 * 8


def plan_steps(lengths: list[int], epoch: int, seed: int = 42) -> list[list[int]]:
    """Which records each optimizer step trains on, for one pass over the data.

    Shuffled, then sorted by length within runs of a few steps, so that the
    records of one step are about one length and a batch of them pads to
    little; then the steps are shuffled again, so the long ones are not all
    at the start of a run. Another order each pass, the same ones each run.
    """
    shuffle = random.Random(seed * 1000 + epoch)
    order = list(range(len(lengths)))
    shuffle.shuffle(order)
    run = PER_STEP * 8
    steps: list[list[int]] = []
    for start in range(0, len(order), run):
        ordered = sorted(order[start:start + run], key=lambda i: -lengths[i])
        steps.extend(ordered[i:i + PER_STEP] for i in range(0, len(ordered), PER_STEP))
    shuffle.shuffle(steps)
    return steps


def passes(indices: list[int], lengths: list[int], budget: int) -> list[list[int]]:
    """`indices` in as few forward passes as `budget` tokens a pass allows.

    A pass is padded to its longest record, so the longest go first and each
    pass is as long as the record it starts with; one record is a pass
    whatever its length, since there is no smaller one.
    """
    out: list[list[int]] = []
    for i in sorted(indices, key=lambda i: -lengths[i]):
        if out and (len(out[-1]) + 1) * round_up(lengths[out[-1][0]]) <= budget:
            out[-1].append(i)
        else:
            out.append([i])
    return out


def padded(examples: dict, indices: list[int], pad: int) -> dict:
    """One pass's records, padded on the right to the longest of them.

    Padding is never attended to and never learned: its mask is 0 and its
    label -100. On the right, a causal model's answer at a real position is
    the same with it as without it.
    """
    width = round_up(max(len(examples["input_ids"][i]) for i in indices))
    out: dict = {"input_ids": [], "attention_mask": [], "labels": []}
    for i in indices:
        missing = width - len(examples["input_ids"][i])
        out["input_ids"].append(list(examples["input_ids"][i]) + [pad] * missing)
        out["attention_mask"].append(list(examples["attention_mask"][i]) + [0] * missing)
        out["labels"].append(list(examples["labels"][i]) + [-100] * missing)
    return out


def answer_tokens(labels: list) -> int:
    """How many of a record's tokens are scored: each position predicts the
    next token, so the first is never one."""
    return sum(1 for label in labels[1:] if label != -100)


def summary(total: float, tokens: int) -> dict | None:
    """Loss and perplexity from a loss summed over `tokens` answer tokens, or
    None when there is nothing to say: no tokens, or a sum that is not a
    number. A JSON NaN is not one the reader will parse."""
    if tokens <= 0:
        return None
    loss = total / tokens
    if not math.isfinite(loss):
        return None
    return {"loss": round(loss, 4), "perplexity": round(math.exp(min(loss, 700.0)), 4)}


def autocast_for(device, compute):
    """The mixed precision a pass runs under: the compute type on a card, and
    none on the processor, where it is float32 throughout."""
    import contextlib

    import torch

    if device.type == "cuda" and compute is not torch.float32:
        return torch.autocast(device_type="cuda", dtype=compute)
    return contextlib.nullcontext()


def to_device(batch: dict, device) -> tuple:
    import torch

    return tuple(torch.tensor(batch[key], device=device)
                 for key in ("input_ids", "attention_mask", "labels"))


def answer_loss(logits, labels):
    """The loss summed over a batch's answer tokens.

    Each position predicts the next token, so the labels move one along. Only
    the positions whose next token is an answer's go into the loss at all,
    and in float32: the logits are the largest tensor in a pass, a hundred
    and fifty thousand numbers a token for some vocabularies, and a copy of
    every one of them in float32 is what ran a card out of memory first.
    """
    import torch.nn.functional as functional

    target = labels[:, 1:]
    keep = target != -100
    return functional.cross_entropy(
        logits[:, :-1][keep].float(), target[keep], reduction="sum"
    )


def score(model, examples: dict, compute, budget: int, pad: int) -> dict | None:
    """The model's loss over the answers in `examples`.

    Taken the way the trainer takes it, under the same mixed precision and
    over the same tokens, so that the number before training and the one
    after differ by the training and nothing else. Under QLoRA both are of
    the four-bit base, without the adapter and with it. In batches of a
    similar length, as many to a pass as the training takes.
    """
    import torch

    device = next(model.parameters()).device
    lengths = [len(ids) for ids in examples["input_ids"]]
    training = model.training
    model.eval()
    total = torch.zeros((), dtype=torch.float64, device=device)
    tokens = sum(answer_tokens(labels) for labels in examples["labels"])
    try:
        with torch.no_grad():
            for indices in passes(list(range(len(lengths))), lengths, budget):
                ids, mask, labels = to_device(padded(examples, indices, pad), device)
                with autocast_for(device, compute):
                    logits = model(input_ids=ids, attention_mask=mask).logits
                total += answer_loss(logits, labels).double()
                del logits
    finally:
        model.train(training)
    return summary(total.item(), tokens)


def report_score(of: str, model, examples: dict, records: int, compute, budget: int,
                 pad: int) -> None:
    """Score `model` on the held-out records and say so, as `of` ("base" or
    "tuned"). A score that cannot be had is said too, and the run goes on:
    it is a measure of the training, not a part of it."""
    phase("scoring", "the base" if of == "base" else "the fine-tune")
    try:
        result = score(model, examples, compute, budget, pad)
    except Exception as error:  # noqa: BLE001 -- a run is worth more than its score
        emit("note", text=f"the held-out records could not be scored: {type(error).__name__}")
        result = None
    if result is None:
        emit("heldout", records=records, skipped="no score")
        return
    emit("heldout", of=of, records=records, **result)


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------


def has_kernels(torch) -> bool:
    """Whether this torch can run on the card it is about to train on.

    A wheel carries kernels for a fixed list of architectures, and a card
    outside it is not slow but unusable, though the import works and CUDA
    says it is there. Same major family and not newer than the card: what is
    built for sm_80 runs on every 8.x after it (see pyenv.cpp, status).
    """
    major, minor = torch.cuda.get_device_capability(0)
    built = []
    for arch in torch.cuda.get_arch_list():
        digits = "".join(c for c in arch[3:] if c.isdigit())
        if arch.startswith("sm_") and digits:
            built.append(int(digits))
    mine = major * 10 + minor
    return not built or any(c // 10 == major and c <= mine for c in built)


class Tuning:
    """The adapter's optimizer steps over the data, in passes as large as the
    card holds.

    In place of transformers' Trainer, which was given one record a pass and
    eight passes a step, every record padded to the whole context, and every
    layer's forward pass done twice to save memory whether it was short or
    not. The arithmetic is the Trainer's: AdamW without weight decay, the
    gradient clipped to a norm of one, the learning rate falling in a
    straight line to nothing at the last step, and a step's loss the mean
    over every answer token in it, however its records were split into
    passes.
    """

    # Of the memory free on the card, the share the passes may plan on. The
    # rest is for the optimizer's state, which appears at the first step, the
    # allocator's slack, and whatever else draws on the card: a display, a
    # model Crucible is answering with. Passes larger than this buy little.
    SHARE = 0.7

    def __init__(self, model, data: dict, lengths: list[int], pad: int, device, compute):
        self.model = model
        self.data = data
        self.lengths = lengths
        self.pad = pad
        self.device = device
        self.compute = compute
        self.answers = [answer_tokens(labels) for labels in data["labels"]]
        self.widest = round_up(max(lengths))
        # Tokens a pass may hold. A whole step at the longest, until the card
        # is measured: more than that would never be used.
        self.budget = PER_STEP * self.widest
        self.checkpointing = False
        # What to say when even that is too much: set by the run, which knows
        # whether QLoRA is still to try.
        self.smaller = "A shorter context or a smaller base takes less."

    def backward(self, indices: list[int], scale: float, scaler=None):
        """One forward and backward pass over the records at `indices`, the
        loss summed over their answer tokens and multiplied by `scale`."""
        ids, mask, labels = to_device(padded(self.data, indices, self.pad), self.device)
        with autocast_for(self.device, self.compute):
            logits = self.model(input_ids=ids, attention_mask=mask).logits
        loss = answer_loss(logits, labels) * scale
        del logits
        (scaler.scale(loss) if scaler is not None else loss).backward()
        return loss.detach()

    def measure(self) -> int:
        """How many tokens a pass can hold: the longest record once and then
        twice over, the difference being what a token costs and the rest what
        a pass costs at all, against the memory the card has free. Zero when
        not even one fits."""
        import torch

        longest = max(range(len(self.lengths)), key=self.lengths.__getitem__)
        torch.cuda.empty_cache()
        free, _ = torch.cuda.mem_get_info(self.device)
        before = torch.cuda.memory_allocated(self.device)
        peaks: list[int] = []
        for count in (1, 2):
            torch.cuda.reset_peak_memory_stats(self.device)
            try:
                self.backward([longest] * count, 0.0)
                peaks.append(torch.cuda.max_memory_allocated(self.device) - before)
            except torch.cuda.OutOfMemoryError:
                break
            finally:
                self.model.zero_grad(set_to_none=True)
        torch.cuda.empty_cache()
        if not peaks:
            return 0
        if len(peaks) == 2 and peaks[1] > peaks[0]:
            per_token = (peaks[1] - peaks[0]) / self.widest
        else:
            per_token = peaks[0] / self.widest
        fixed = max(peaks[0] - per_token * self.widest, 0.0)
        return max(int((free * self.SHARE - fixed) / per_token), 0)

    def checkpoint(self) -> None:
        """Recompute each layer's forward pass in the backward rather than
        keep its activations: a third more work, for most of the memory."""
        self.model.gradient_checkpointing_enable(gradient_checkpointing_kwargs={"use_reentrant": False})
        self.checkpointing = True

    def fit_memory(self) -> None:
        """Size the passes to the card. Checkpointing only when not even the
        longest record fits without it, and a run that cannot fit one either
        way says so before it starts rather than at its first step."""
        if self.device.type != "cuda":
            return
        measured = self.measure()
        if measured < self.widest:
            self.checkpoint()
            measured = self.measure()
        if measured < self.widest:
            fail("the base does not fit on this card at this context", self.smaller)
        self.budget = min(measured, self.budget)
        print(f"passes of up to {self.budget} tokens, "
              f"checkpointing {'on' if self.checkpointing else 'off'}", flush=True)

    def shrink(self) -> None:
        """Smaller passes, after the card ran out of memory anyway: something
        else took some of it, or the allocator's pieces stopped fitting. Then
        checkpointing, and then there is nothing left to give."""
        if self.budget > self.widest:
            self.budget = max(self.budget // 2, self.widest)
        elif not self.checkpointing:
            self.checkpoint()
        else:
            fail("the card ran out of memory", self.smaller)
        print(f"out of memory: passes of up to {self.budget} tokens, "
              f"checkpointing {'on' if self.checkpointing else 'off'}", flush=True)

    def step(self, indices: list[int], optimizer, scaler, trainable) -> float:
        """One optimizer step over the records at `indices`, and its loss.
        A step the card runs out of memory in is taken again from the start
        in smaller passes: nothing has been changed until it ends."""
        import torch

        answers = max(sum(self.answers[i] for i in indices), 1)
        while True:
            try:
                total = torch.zeros((), device=self.device)
                for batch in passes(indices, self.lengths, self.budget):
                    total += self.backward(batch, 1.0 / answers, scaler)
                break
            except torch.cuda.OutOfMemoryError:
                optimizer.zero_grad(set_to_none=True)
                torch.cuda.empty_cache()
                self.shrink()
        scaler.unscale_(optimizer)
        torch.nn.utils.clip_grad_norm_(trainable, 1.0)
        scaler.step(optimizer)
        scaler.update()
        optimizer.zero_grad(set_to_none=True)
        return float(total.item())

    def train(self, epochs: int, learning_rate: float, max_steps: int) -> None:
        import torch

        trainable = [p for p in self.model.parameters() if p.requires_grad]
        optimizer = torch.optim.AdamW(trainable, lr=learning_rate, weight_decay=0.0,
                                      fused=self.device.type == "cuda")
        per_epoch = math.ceil(len(self.lengths) / PER_STEP)
        total = max_steps or epochs * per_epoch
        schedule = torch.optim.lr_scheduler.LambdaLR(
            optimizer, lambda done: max(0.0, (total - done) / max(total, 1)))
        # Only half precision has its loss scaled: bfloat16 has float32's range.
        scaler = torch.amp.GradScaler(self.device.type, enabled=self.compute is torch.float16)
        step, epoch = 0, 0
        while step < total:
            for indices in plan_steps(self.lengths, epoch):
                loss = self.step(indices, optimizer, scaler, trainable)
                schedule.step()
                step += 1
                emit("step", step=step, total=total, loss=loss, epoch=round(step / per_epoch, 4),
                     tokens=sum(self.lengths[i] for i in indices))
                if step >= total:
                    break
            epoch += 1


def train_torch(recipe: dict, out: Path, args) -> Path:
    """LoRA or QLoRA with peft, and a merged model at the end."""
    qlora = recipe.get("method", "qlora") == "qlora"
    if not qlora:
        # peft and transformers import bitsandbytes whenever it is installed,
        # a second of every start, and only QLoRA uses it. Marked as absent
        # before either looks, which is how both then see it.
        sys.modules.setdefault("bitsandbytes", None)

    # Crucible does not ask before a run whether these import: asking was a
    # Python of its own importing all of them, seconds on every run, and the
    # answer is had here for nothing.
    try:
        import torch
        from peft import LoraConfig, get_peft_model
        from transformers import AutoModelForCausalLM, AutoTokenizer
    except ImportError as error:
        fail(f"the training environment is missing {error.name or 'a package'}",
             "Repair it in Settings.")

    reference = base_reference(recipe)
    context = int(recipe.get("context", 512))
    epochs = int(recipe.get("epochs", 2))
    learning_rate = float(recipe.get("learning_rate", LEARNING_RATE))

    if torch.cuda.is_available():
        if not has_kernels(torch):
            fail(f"this PyTorch has no kernels for {torch.cuda.get_device_name(0)}",
                 "Repair the training environment in Settings to get a build that has.")
        compute = torch.bfloat16 if torch.cuda.is_bf16_supported() else torch.float16
        device = torch.device("cuda", torch.cuda.current_device())
        emit("device", name=torch.cuda.get_device_name(0), kind="cuda")
        # What is left in float32 on the tensor cores, as the Trainer had it.
        torch.set_float32_matmul_precision("high")
    else:
        # bfloat16 on a processor is slower than float32, not faster: there is
        # no hardware for it, so every operation is emulated.
        compute = torch.float32
        device = torch.device("cpu")
        qlora = False  # bitsandbytes has no processor path worth the name
        emit("device", name="cpu", kind="cpu")

    phase("tokenizer", reference)
    tokenizer = AutoTokenizer.from_pretrained(reference, trust_remote_code=False)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token

    # The data before the base: a recipe whose data cannot be read is told so
    # in a second, not after a model has been loaded for it.
    records, held, skipped = hold_out(read_records(recipe, tokenizer))
    emit("data_ready", records=len(records))
    data = encode(tokenizer, [text for text, _ in records], [spans for _, spans in records], context)
    examples = None if skipped else encode(
        tokenizer, [text for text, _ in held], [spans for _, spans in held], context)

    phase("base", f"loading {reference}")
    load_args = {"dtype": compute}
    if qlora:
        from transformers import BitsAndBytesConfig

        load_args["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_use_double_quant=True,
            bnb_4bit_compute_dtype=compute,
        )
    if torch.cuda.is_available():
        # One card, named explicitly. `device_map="auto"` spreads the layers
        # over every GPU in the machine, which is what you want for inference
        # on a model too big for one -- and not for training a quantized one,
        # because an optimizer step cannot straddle devices. An adapter fits
        # on one card by construction; that is the whole point of training
        # one.
        load_args["device_map"] = {"": torch.cuda.current_device()}

    model = AutoModelForCausalLM.from_pretrained(reference, **load_args)
    model.config.use_cache = False

    if qlora and compute is torch.float16:
        from peft import prepare_model_for_kbit_training

        # In half precision the norms and the embedding go to float32, as
        # QLoRA was written: half precision's range is too small for them. Not
        # in bfloat16, which has float32's range: there the copies made every
        # token's activations twice the size, and checkpointing came on for a
        # 3B base that fits without it. Checkpointing is decided below either
        # way, by what the card holds, so it is not switched on here.
        model = prepare_model_for_kbit_training(model, use_gradient_checkpointing=False)

    # Rank 16 over every linear layer. Rank is the one LoRA knob that trades
    # capacity for memory, and 16 is the value the literature settles on for
    # subject specialization -- 8 underfits a new domain, 64 costs four times
    # the adapter for a difference that does not show up on a small corpus.
    # Seeded, as the Trainer seeded it: the same recipe starts from the same
    # adapter.
    torch.manual_seed(42)
    model = get_peft_model(
        model,
        LoraConfig(
            r=16,
            lora_alpha=32,
            lora_dropout=0.05,
            bias="none",
            task_type="CAUSAL_LM",
            target_modules="all-linear",
        ),
    )
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    total = sum(p.numel() for p in model.parameters())
    emit("adapter", trainable=trainable, total=total)

    pad = tokenizer.pad_token_id
    lengths = [len(ids) for ids in data["input_ids"]]
    tuning = Tuning(model, data, lengths, pad, device, compute)
    if not qlora:
        tuning.smaller = "QLoRA, a shorter context or a smaller base takes less."
    model.train()
    tuning.fit_memory()

    # The base's score, before a step: the adapter starts at nothing, so the
    # model as it stands answers as the base does.
    if skipped:
        emit("heldout", records=0, skipped=skipped)
    else:
        report_score("base", model, examples, len(held), compute, tuning.budget, pad)

    phase("training", f"{epochs} passes over {len(records)} records")
    tuning.train(epochs, learning_rate, int(args.max_steps or 0))

    if not skipped:
        report_score("tuned", model, examples, len(held), compute, tuning.budget, pad)

    # --- merge, so what comes out is an ordinary model ---------------------
    #
    # The adapter alone is a few megabytes and useless without the base. A
    # merged model is one directory that any program can load, which is what
    # somebody asked for when they asked for an expert.
    phase("merging", "folding the adapter into the base")
    converter(args).warm()
    adapter = out / "adapter"
    model.save_pretrained(adapter)
    tokenizer.save_pretrained(adapter)

    # The training's hold on the card goes with it, the loop's included, and
    # whatever cycle among the model's modules would keep it until the next
    # collection: what is left of the run is the processor's.
    del model, tuning
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    from peft import PeftModel

    # Reloaded unquantized: merging into 4-bit weights is not defined, and a
    # QLoRA adapter is trained to sit over the full-precision base anyway.
    base = AutoModelForCausalLM.from_pretrained(
        reference, dtype=torch.float16, device_map="cpu"
    )
    merged_model = PeftModel.from_pretrained(base, adapter).merge_and_unload()
    merged = out / "merged"
    merged_model.save_pretrained(merged, safe_serialization=True)
    tokenizer.save_pretrained(merged)
    return merged


def train_mlx(recipe: dict, out: Path, args) -> Path:
    """The Apple Silicon path, through mlx_lm rather than torch."""
    reference = base_reference(recipe)
    phase("training", "mlx_lm lora")
    adapter = out / "adapter"
    command = [
        sys.executable, "-m", "mlx_lm", "lora",
        "--model", reference,
        "--train",
        "--data", str(out / "data"),
        "--adapter-path", str(adapter),
        "--iters", str(args.max_steps or 600),
        "--batch-size", "1",
        "--max-seq-length", str(int(recipe.get("context", 512))),
        "--learning-rate", str(float(recipe.get("learning_rate", LEARNING_RATE))),
    ]
    run_logged(command)

    phase("merging", "fusing the adapter")
    converter(args).warm()
    merged = out / "merged"
    run_logged([
        sys.executable, "-m", "mlx_lm", "fuse",
        "--model", reference,
        "--adapter-path", str(adapter),
        "--save-path", str(merged),
    ])
    return merged


def as_log(line: str, child: str) -> str:
    """A child's line as it is passed on: as it was, unless Crucible could
    read it as an event, which only this script speaks.

    Anything that starts like a JSON object goes behind the child's name,
    where it cannot parse. Crucible's own quantizer once printed its progress
    as events, and its "done" finished the fine-tune at the file in the run
    folder that was about to be moved into the models folder.
    """
    if line.lstrip(" \t\r\n﻿").startswith("{"):
        return f"{child}: {line}"
    return line


def run_logged(command: list[str]) -> None:
    """Run a child and pass its output through as log lines."""
    pass_through(subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
    ), Path(command[0]).name)


def pass_through(process, child: str) -> None:
    """A child's output passed on as log lines until it ends, and its failure
    the run's."""
    with process:
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(as_log(line.rstrip("\r\n"), child) + "\n")
            sys.stdout.flush()
        status = process.wait()
    if status != 0:
        fail(f"{child} exited {status}")


class Converter:
    """llama.cpp's converter, started before the model it converts is there.

    Its Python, with torch and the converter's own packages, takes three
    seconds and more to start, and the merge before it about as long. So it
    is started when the merge is, imports what it will need, and waits on
    its standard input for its arguments; a run that ends before then ends
    its input, and it leaves. Started cold if nothing warmed it.
    """

    WAITING = (
        "import json, runpy, sys\n"
        "from pathlib import Path\n"
        "script = sys.argv[1]\n"
        "sys.path.insert(0, str(Path(script).parent))\n"
        "try:\n"
        "    import torch, transformers, conversion\n"
        "except Exception:\n"
        "    pass  # the script says what is wrong when it runs\n"
        "line = sys.stdin.readline()\n"
        "if not line.strip():\n"
        "    sys.exit(0)\n"
        "sys.argv = [script] + json.loads(line)\n"
        "runpy.run_path(script, run_name='__main__')\n"
    )

    def __init__(self, script: str):
        self.script = script
        self.process = None

    def warm(self) -> None:
        if self.process is None and self.script and Path(self.script).exists():
            self.process = subprocess.Popen(
                [sys.executable, "-c", self.WAITING, self.script],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace",
            )

    def run(self, arguments: list[str]) -> None:
        """Convert with `arguments`, the converter's own, as run_logged runs
        a child."""
        self.warm()
        process, self.process = self.process, None
        if process is None:
            fail(f"the converter is not at {self.script}")
        try:
            process.stdin.write(json.dumps(arguments) + "\n")
            process.stdin.close()
        except OSError:
            pass  # it has already ended, and says why below
        pass_through(process, Path(sys.executable).name)


# ---------------------------------------------------------------------------
# Getting it into a file somebody can load
# ---------------------------------------------------------------------------


def converter(args) -> Converter:
    """The run's converter, the one warmed during the merge when it was."""
    if getattr(args, "converter", None) is None:
        args.converter = Converter(args.convert_script)
    return args.converter


def to_gguf(merged: Path, out: Path, recipe: dict, args) -> Path | None:
    """Convert and quantize, if the tools to do it are here.

    Both come from llama.cpp: the converter is a script in its source tree and
    the quantizer is a binary built beside the runtimes. Crucible passes the
    paths when it has them. Without them this stops at the merged model and
    says so -- a directory of safetensors is a real result, just not the
    single file that was asked for.
    """
    if not args.convert_script or not Path(args.convert_script).exists():
        emit(
            "note",
            text="no llama.cpp converter here, so the result is a Huggingface "
                 "model directory rather than a GGUF",
        )
        return None

    name = recipe.get("id") or "expert"
    quantization = recipe.get("quantization", "Q4_K_M")

    # The converter can write two of the formats on its own. The k-quants --
    # Q4_K_M and friends, which is where the interesting size/quality trades
    # are -- need a quantizer: Crucible's own, which it passes as
    # --quantize-cmd, or a llama-quantize. So F16 is written in one step, and
    # the rest are converted to F16 and then quantized, or left at F16 with a
    # line saying why when nothing can. Q8_0 too, though the converter writes
    # it: in Python, at half the speed of F16 and the quantizer together.
    direct = {"F16": "f16", "Q8_0": "q8_0"}
    quantizer = args.quantize_bin if args.quantize_bin else shutil.which("llama-quantize")
    if quantizer and not Path(quantizer).exists() and not shutil.which(quantizer):
        quantizer = None
    # Crucible quantizes with its own llama.cpp when it is asked to, which it
    # is unless a llama-quantize was named: `crucible --quantize IN OUT TYPE`
    # takes llama-quantize's arguments.
    command = [quantizer] if quantizer else None
    if args.quantize_cmd:
        try:
            command = [str(part) for part in json.loads(args.quantize_cmd)] or command
        except ValueError:
            pass

    if quantization == "F16" or (quantization in direct and not command):
        phase("converting", f"to GGUF at {quantization}")
        produced = out / f"{name}.{quantization}.gguf"
        converter(args).run([str(merged), "--outfile", str(produced),
                             "--outtype", direct[quantization]])
        return produced

    phase("converting", "to GGUF")
    unquantized = out / f"{name}.F16.gguf"
    converter(args).run([str(merged), "--outfile", str(unquantized), "--outtype", "f16"])
    if not command:
        emit(
            "note",
            text=f"no quantizer here, so this is F16 rather than {quantization}. "
                 "It loads the same; it is larger.",
        )
        return unquantized

    phase("quantizing", quantization)
    quantized = out / f"{name}.{quantization}.gguf"
    run_logged(command + [str(unquantized), str(quantized), quantization])
    unquantized.unlink(missing_ok=True)
    return quantized


# ---------------------------------------------------------------------------


def deliver(produced: Path, export_dir: Path) -> Path:
    """Move the finished file into the models directory.

    That is where every expert's model is picked from, so a fine-tune that
    lands anywhere else is one that has to be gone looking for. The name drops
    the dots the converter uses -- "kitchen-physicist-Q4_K_M.gguf" -- so it
    reads like the other files there. A file of that name is replaced: training
    the same recipe again is asking for the new one.
    """
    phase("exporting", f"to {export_dir}")
    export_dir.mkdir(parents=True, exist_ok=True)
    name = produced.name
    if name.endswith(".gguf"):
        stem = name[: -len(".gguf")]
        head, _, quant = stem.rpartition(".")
        name = f"{head}-{quant}.gguf" if head else name
    target = export_dir / name
    # shutil.move copies when the two are on different drives, which is the
    # common case: the models directory is very often a big disk of its own.
    shutil.move(str(produced), str(target))
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description="Fine-tune a Crucible expert")
    parser.add_argument("--recipe", required=True, help="recipe.json to run")
    parser.add_argument("--out", required=True, help="directory for the result")
    parser.add_argument("--convert-script", default="", help="convert_hf_to_gguf.py")
    parser.add_argument("--quantize-bin", default="", help="llama-quantize")
    parser.add_argument("--quantize-cmd", default="",
                        help="the quantizer as a JSON list, given IN OUT TYPE after it")
    parser.add_argument("--export-dir", default="",
                        help="where the finished GGUF goes: Crucible's models directory")
    parser.add_argument("--max-steps", type=int, default=0,
                        help="stop after this many optimizer steps (a smoke test)")
    args = parser.parse_args()

    recipe = json.loads(Path(args.recipe).read_text(encoding="utf-8"))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    started = time.time()
    emit("started", name=recipe.get("name", ""), method=recipe.get("method", "qlora"),
         pid=os.getpid())

    # Before torch is imported anywhere: the device list is read once, at
    # import, and setting this afterwards does nothing.
    chosen = pick_gpu()
    if chosen:
        os.environ["CUDA_VISIBLE_DEVICES"] = chosen
    # Memory the allocator can grow in place rather than in fixed pieces: the
    # passes are of every length, and pieces cut for one stop fitting the
    # next, which is a card that runs out with a gigabyte free.
    if sys.platform.startswith("linux"):
        os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")

    on_mac_gpu = sys.platform == "darwin" and os.uname().machine == "arm64"
    try:
        import mlx_lm  # noqa: F401
        have_mlx = True
    except ImportError:
        have_mlx = False

    merged = (train_mlx if (on_mac_gpu and have_mlx) else train_torch)(recipe, out, args)

    produced = to_gguf(merged, out, recipe, args)
    if produced is not None:
        # The merged model was the converter's input, and a second copy of
        # the base in a folder nobody opens: gigabytes, kept for every recipe
        # ever trained. The GGUF is the result, and the adapter beside it is
        # what the training made.
        shutil.rmtree(merged, ignore_errors=True)
    if produced is not None and args.export_dir:
        produced = deliver(produced, Path(args.export_dir))
    emit(
        "done",
        path=str(produced or merged),
        gguf=produced is not None,
        seconds=round(time.time() - started, 1),
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        emit("canceled")
        sys.exit(130)
    except SystemExit:
        raise
    except Exception as error:  # noqa: BLE001 -- the reader needs the reason
        import traceback

        emit("failed", message=f"{type(error).__name__}: {error}",
             hint=traceback.format_exc(limit=3))
        sys.exit(1)
