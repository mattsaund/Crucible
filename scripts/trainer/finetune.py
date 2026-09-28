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

Why an adapter and not a full fine-tune: rewriting every weight of a 7B model
wants a data-center GPU and produces a 14 GB file. A LoRA adapter trains a few
million parameters beside a frozen base, fits on a consumer card, and is
merged back into a normal model at the end -- so what comes out is an ordinary
model that any llama.cpp or MLX program can load, not something that needs
this program to use.
"""

from __future__ import annotations

import argparse
import json
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
    """One progress record. Flushed, because the reader is a pipe."""
    record = {"event": event}
    record.update(fields)
    sys.stdout.write(json.dumps(record) + "\n")
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


def load_dataset_from(recipe: dict, tokenizer, context: int):
    """Every data asset in the recipe, tokenized into one dataset.

    Three shapes are accepted because three shapes are what people have: a
    Huggingface dataset, a folder of text, and JSONL of either {"text": ...}
    or {"messages": [...]}. Chat records go through the tokenizer's own chat
    template, so a model taught on them answers in the format it was trained
    to speak.
    """
    from datasets import Dataset, concatenate_datasets, load_dataset

    def to_text(record: dict) -> str | None:
        if isinstance(record.get("messages"), list):
            try:
                return tokenizer.apply_chat_template(
                    record["messages"], tokenize=False
                )
            except Exception:
                # A base model with no chat template. Flatten it rather than
                # dropping the record: the content is still worth training on.
                return "\n".join(
                    f"{m.get('role', '')}: {m.get('content', '')}"
                    for m in record["messages"]
                )
        for key in ("text", "content", "output", "completion"):
            if isinstance(record.get(key), str) and record[key].strip():
                return record[key]
        return None

    parts = []
    for asset in recipe.get("data") or []:
        reference = asset.get("path") or asset.get("id") or ""
        if not reference:
            continue
        source = asset.get("source", "hub")
        phase("data", f"reading {reference}")

        if source == "hub":
            loaded = load_dataset(reference, split="train")
            texts = [t for t in (to_text(r) for r in loaded) if t]
            parts.append(Dataset.from_dict({"text": texts}))
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

        texts: list[str] = []
        for one in files:
            if one.suffix in (".jsonl", ".json"):
                for line in one.read_text(encoding="utf-8", errors="replace").splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        text = to_text(json.loads(line))
                    except json.JSONDecodeError:
                        text = None
                    if text:
                        texts.append(text)
            else:
                body = one.read_text(encoding="utf-8", errors="replace").strip()
                if body:
                    texts.append(body)
        if not texts:
            fail(f"no usable text in {reference}")
        parts.append(Dataset.from_dict({"text": texts}))

    if not parts:
        fail("this recipe has no training data")

    data = parts[0] if len(parts) == 1 else concatenate_datasets(parts)
    emit("data_ready", records=len(data))

    def tokenize(batch):
        out = tokenizer(
            batch["text"],
            truncation=True,
            max_length=context,
            padding="max_length",
        )
        # Causal language modeling: the labels are the inputs, and the loss
        # over a padded position is thrown away.
        out["labels"] = [
            [(t if m == 1 else -100) for t, m in zip(ids, mask)]
            for ids, mask in zip(out["input_ids"], out["attention_mask"])
        ]
        return out

    return data.map(tokenize, batched=True, remove_columns=["text"])


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

    data = load_dataset_from(recipe, tokenizer, context)

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


def run_logged(command: list[str]) -> None:
    """Run a child and pass its output through as log lines."""
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    assert process.stdout is not None
    for line in process.stdout:
        sys.stdout.write(line if line.endswith("\n") else line + "\n")
        sys.stdout.flush()
    if process.wait() != 0:
        fail(f"{Path(command[0]).name} exited {process.returncode}")


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
    # are -- need llama-quantize, a separate program from llama.cpp that
    # Crucible does not build. So the ones that can be done in one step are
    # done in one step, and the rest come out at F16 with a line saying why.
    direct = {"F16": "f16", "Q8_0": "q8_0"}
    quantizer = args.quantize_bin if args.quantize_bin else shutil.which("llama-quantize")
    if quantizer and not Path(quantizer).exists() and not shutil.which(quantizer):
        quantizer = None

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
    if not quantizer:
        emit(
            "note",
            text=f"no llama-quantize here, so this is F16 rather than {quantization}. "
                 "It loads the same; it is larger.",
        )
        return unquantized

    phase("quantizing", quantization)
    quantized = out / f"{name}.{quantization}.gguf"
    run_logged([quantizer, str(unquantized), str(quantized), quantization])
    unquantized.unlink(missing_ok=True)
    return quantized


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Fine-tune a Crucible expert")
    parser.add_argument("--recipe", required=True, help="recipe.json to run")
    parser.add_argument("--out", required=True, help="directory for the result")
    parser.add_argument("--convert-script", default="", help="convert_hf_to_gguf.py")
    parser.add_argument("--quantize-bin", default="", help="llama-quantize")
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
