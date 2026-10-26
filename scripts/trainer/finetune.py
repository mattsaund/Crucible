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
import hashlib
import json
import math
import os
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
    Left to itself the HuggingFace Trainer sees several GPUs and wraps the
    model in torch.nn.DataParallel, which replicates it onto each of them --
    and a 4-bit quantized model cannot be replicated, so the first forward
    pass dies inside a worker thread with a traceback that mentions neither
    quantization nor DataParallel.

    Training an adapter does not want more than one card anyway: the whole
    premise is that the frozen base fits on one. So the one with the most
    free memory is picked, and the rest are left for whatever else is using
    the machine -- including Crucible's own inference.
    """
    if os.environ.get("CUDA_VISIBLE_DEVICES"):
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
    from datasets import load_dataset

    records: list[tuple[str, list[list[int]]]] = []
    for asset in recipe.get("data") or []:
        reference = asset.get("path") or asset.get("id") or ""
        if not reference:
            continue
        source = asset.get("source", "hub")
        phase("data", f"reading {reference}")

        if source == "hub":
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


def encode(tokenizer, texts: list, spans: list, context: int, pad: bool) -> dict:
    """Tokens and labels for some records: the same for training as for the
    score, which is what makes the score a measure of what was trained.

    Answers are found by the characters each token came from, which only a
    fast tokenizer reports. Without one every record is learned whole.
    """
    fast = bool(getattr(tokenizer, "is_fast", False))
    out = tokenizer(
        texts,
        truncation=True,
        max_length=context,
        padding="max_length" if pad else False,
        return_offsets_mapping=fast,
    )
    offsets = out.pop("offset_mapping") if fast else [[] for _ in texts]
    # Causal language modeling: the labels are the inputs, and the loss over
    # a padded position, or one that is not an answer, is thrown away.
    out["labels"] = [
        answer_labels(ids, offsets[i], mask, spans[i])
        for i, (ids, mask) in enumerate(zip(out["input_ids"], out["attention_mask"]))
    ]
    return out


def to_dataset(records: list, tokenizer, context: int):
    """The training records, tokenized into one dataset."""
    from datasets import Dataset

    data = Dataset.from_dict({
        "text": [text for text, _ in records],
        "spans": [spans for _, spans in records],
    })
    return data.map(
        lambda batch: encode(tokenizer, batch["text"], batch["spans"], context, pad=True),
        batched=True,
        remove_columns=["text", "spans"],
    )


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


def score(model, examples: dict, compute) -> dict | None:
    """The model's loss over the answers in `examples`, one record at a time.

    Taken the way the trainer takes it, under the same mixed precision, so
    that the number before training and the one after differ by the
    training and nothing else. Under QLoRA both are of the four-bit base,
    without the adapter and with it.
    """
    import contextlib

    import torch
    import torch.nn.functional as functional

    device = next(model.parameters()).device
    autocast = (
        torch.autocast(device_type="cuda", dtype=compute)
        if device.type == "cuda" and compute is not torch.float32
        else contextlib.nullcontext()
    )
    training = model.training
    model.eval()
    total, tokens = 0.0, 0
    try:
        with torch.no_grad(), autocast:
            for ids, mask, labels in zip(
                examples["input_ids"], examples["attention_mask"], examples["labels"]
            ):
                logits = model(
                    input_ids=torch.tensor([ids], device=device),
                    attention_mask=torch.tensor([mask], device=device),
                ).logits[0, :-1].float()
                # Each position predicts the next token, so the labels move
                # one along.
                target = torch.tensor(labels[1:], device=device)
                total += functional.cross_entropy(
                    logits, target, ignore_index=-100, reduction="sum"
                ).item()
                tokens += int((target != -100).sum().item())
    finally:
        model.train(training)
    return summary(total, tokens)


def report_score(of: str, model, examples: dict, records: int, compute) -> None:
    """Score `model` on the held-out records and say so, as `of` ("base" or
    "tuned"). A score that cannot be had is said too, and the run goes on:
    it is a measure of the training, not a part of it."""
    phase("scoring", "the base" if of == "base" else "the fine-tune")
    try:
        result = score(model, examples, compute)
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


def train_torch(recipe: dict, out: Path, args) -> Path:
    """LoRA or QLoRA with peft, and a merged model at the end."""
    import torch
    from peft import LoraConfig, get_peft_model
    from transformers import (
        AutoModelForCausalLM,
        AutoTokenizer,
        Trainer,
        TrainerCallback,
        TrainingArguments,
    )

    reference = base_reference(recipe)
    qlora = recipe.get("method", "qlora") == "qlora"
    context = int(recipe.get("context", 512))
    epochs = int(recipe.get("epochs", 2))
    learning_rate = float(recipe.get("learning_rate", 1e-5))

    if torch.cuda.is_available():
        compute = torch.bfloat16 if torch.cuda.is_bf16_supported() else torch.float16
        emit("device", name=torch.cuda.get_device_name(0), kind="cuda")
    else:
        # bfloat16 on a processor is slower than float32, not faster: there is
        # no hardware for it, so every operation is emulated.
        compute = torch.float32
        qlora = False  # bitsandbytes has no processor path worth the name
        emit("device", name="cpu", kind="cpu")

    phase("tokenizer", reference)
    tokenizer = AutoTokenizer.from_pretrained(reference, trust_remote_code=False)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token

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
        # on a model too big for one -- and which Trainer refuses outright for
        # a quantized model, because an optimizer step cannot straddle
        # devices. An adapter fits on one card by construction; that is the
        # whole point of training one.
        load_args["device_map"] = {"": torch.cuda.current_device()}

    model = AutoModelForCausalLM.from_pretrained(reference, **load_args)
    model.config.use_cache = False

    if qlora:
        from peft import prepare_model_for_kbit_training

        model = prepare_model_for_kbit_training(model)

    # Rank 16 over every linear layer. Rank is the one LoRA knob that trades
    # capacity for memory, and 16 is the value the literature settles on for
    # subject specialization -- 8 underfits a new domain, 64 costs four times
    # the adapter for a difference that does not show up on a small corpus.
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

    records, held, skipped = hold_out(read_records(recipe, tokenizer))
    emit("data_ready", records=len(records))
    data = to_dataset(records, tokenizer, context)

    # The base's score, before a step: the adapter starts at nothing, so the
    # model as it stands answers as the base does.
    if skipped:
        emit("heldout", records=0, skipped=skipped)
    else:
        examples = encode(
            tokenizer, [text for text, _ in held], [spans for _, spans in held], context, pad=False
        )
        report_score("base", model, examples, len(held), compute)

    class Report(TrainerCallback):
        """Crucible's progress bar, and the loss curve behind it."""

        def on_log(self, args, state, control, logs=None, **kwargs):
            logs = logs or {}
            if "loss" in logs:
                emit(
                    "step",
                    step=int(state.global_step),
                    total=int(state.max_steps),
                    loss=float(logs["loss"]),
                    epoch=float(logs.get("epoch", 0.0)),
                )

    workdir = out / "run"
    settings = TrainingArguments(
        output_dir=str(workdir),
        num_train_epochs=epochs,
        per_device_train_batch_size=1,
        gradient_accumulation_steps=8,
        learning_rate=learning_rate,
        logging_steps=1,
        save_strategy="no",
        report_to=[],
        bf16=compute is torch.bfloat16,
        fp16=compute is torch.float16,
        gradient_checkpointing=True,
        max_steps=int(args.max_steps) if args.max_steps else -1,
    )

    phase("training", f"{epochs} passes over {len(data)} records")
    trainer = Trainer(
        model=model,
        args=settings,
        train_dataset=data,
        callbacks=[Report()],
    )
    trainer.train()

    if not skipped:
        report_score("tuned", model, examples, len(held), compute)

    # --- merge, so what comes out is an ordinary model ---------------------
    #
    # The adapter alone is a few megabytes and useless without the base. A
    # merged model is one directory that any program can load, which is what
    # somebody asked for when they asked for an expert.
    phase("merging", "folding the adapter into the base")
    adapter = out / "adapter"
    model.save_pretrained(adapter)
    tokenizer.save_pretrained(adapter)

    del model
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
        "--learning-rate", str(float(recipe.get("learning_rate", 1e-5))),
    ]
    run_logged(command)

    phase("merging", "fusing the adapter")
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
    child = Path(command[0]).name
    with subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
    ) as process:
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(as_log(line.rstrip("\r\n"), child) + "\n")
            sys.stdout.flush()
        status = process.wait()
    if status != 0:
        fail(f"{child} exited {status}")


# ---------------------------------------------------------------------------
# Getting it into a file somebody can load
# ---------------------------------------------------------------------------


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
    # --quantize-cmd, or a llama-quantize. So the ones that can be done in one
    # step are done in one step, and the rest are converted to F16 and then
    # quantized, or left at F16 with a line saying why when nothing can.
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

    if quantization in direct:
        phase("converting", f"to GGUF at {quantization}")
        produced = out / f"{name}.{quantization}.gguf"
        run_logged([
            sys.executable, str(args.convert_script), str(merged),
            "--outfile", str(produced), "--outtype", direct[quantization],
        ])
        return produced

    phase("converting", "to GGUF")
    unquantized = out / f"{name}.F16.gguf"
    run_logged([
        sys.executable, str(args.convert_script), str(merged),
        "--outfile", str(unquantized), "--outtype", "f16",
    ])
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

    on_mac_gpu = sys.platform == "darwin" and os.uname().machine == "arm64"
    try:
        import mlx_lm  # noqa: F401
        have_mlx = True
    except ImportError:
        have_mlx = False

    merged = (train_mlx if (on_mac_gpu and have_mlx) else train_torch)(recipe, out, args)

    produced = to_gguf(merged, out, recipe, args)
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
