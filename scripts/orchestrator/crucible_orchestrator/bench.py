# SPDX-License-Identifier: MIT
"""Benchmarks: models through the suite, many items at a time.

The suite is crucible-bench-1: nine benchmarks, each item asked as a chat
with greedy decoding and its answer checked here. Every Crucible asks the
same items the same way, so a result from one machine can stand beside a
result from another; that is what the public board is for
(github.com/mattsaund/crucible-bench).

What the core does for this, and only the core can: load the model under
test (bench.load), answer (bench.ask queues items, bench.answers hands back
the answers as they finish, as many decoded at once as the cards hold), fetch
dataset pages from Hugging Face (bench.rows, cached), fingerprint the model
file (bench.hash), say what the machine is (bench.machine), show progress
(bench.publish) and keep the result (bench.save). Everything else, from which
items to ask to whether an answer was right, is here and in bench_score.
"""

import collections
import concurrent.futures
import hashlib
import sys
import threading
import time

from . import bench_score as score
from .rpc import Disconnected

SUITE = "crucible-bench-1"
SCHEMA = 1

# Every benchmark of the suite: where its rows are and how many there are.
# The totals are part of the suite: a dataset that grew would change which
# items a size picks, and so what its scores mean.
BENCHMARKS = [
    {"id": "mmlu_pro", "name": "MMLU-Pro", "skill": "knowledge",
     "dataset": "TIGER-Lab/MMLU-Pro", "config": "default", "split": "test", "total": 12032},
    {"id": "arc", "name": "ARC-Challenge", "skill": "reasoning",
     "dataset": "allenai/ai2_arc", "config": "ARC-Challenge", "split": "test", "total": 1172},
    {"id": "gsm8k", "name": "GSM8K", "skill": "math",
     "dataset": "openai/gsm8k", "config": "main", "split": "test", "total": 1319},
    {"id": "math500", "name": "MATH-500", "skill": "math",
     "dataset": "HuggingFaceH4/MATH-500", "config": "default", "split": "test", "total": 500},
    {"id": "humaneval", "name": "HumanEval", "skill": "code",
     "dataset": "openai/openai_humaneval", "config": "openai_humaneval", "split": "test", "total": 164},
    {"id": "mbpp", "name": "MBPP", "skill": "code",
     "dataset": "google-research-datasets/mbpp", "config": "sanitized", "split": "test", "total": 257},
    {"id": "ifeval", "name": "IFEval", "skill": "instructions",
     "dataset": "google/IFEval", "config": "default", "split": "train", "total": 541},
    {"id": "hellaswag", "name": "HellaSwag", "skill": "commonsense",
     "dataset": "Rowan/hellaswag", "config": "default", "split": "validation", "total": 10042},
    {"id": "truthfulqa", "name": "TruthfulQA", "skill": "truthfulness",
     "dataset": "truthfulqa/truthful_qa", "config": "multiple_choice", "split": "validation", "total": 817},
]
BY_ID = {b["id"]: b for b in BENCHMARKS}

SKILLS = ["knowledge", "reasoning", "math", "code", "instructions", "commonsense", "truthfulness"]

SIZES = {"quick": 50, "standard": 200, "full": None}

# One budget for every reply, so the settings a result reports are the
# settings every item had. Room for a thinking model to think: at 4096,
# gpt-oss-20b spent the whole budget thinking on 31 of 50 IFEval items and
# answered nothing, and its score measured the budget. One that does not
# think stops long before it. A model too large to load with this much
# context is run with the smaller pair, and its result says so.
MAX_TOKENS = 16384
CONTEXT = 20480
SMALL_MAX_TOKENS = 4096
SMALL_CONTEXT = 8192
CODE_SECONDS = 15.0

# Items queued beyond the ones being answered, for each the core answers at
# once: a slot that comes free takes the next at once, rather than waiting
# on a test being run here, and the core takes them in the order queued.
AHEAD = 2

# The speed a result reports is one item's at a time, as it always was, and
# what a person asking the model in the chat sees: items many at a time each
# go slower and all together go much faster. So the first items are asked
# alone and timed, until this many tokens or items.
SOLO_TOKENS = 256
SOLO_ITEMS = 8

# Answers checked at once: running the code a model wrote is a program of its
# own, up to CODE_SECONDS, and the next answers arrive meanwhile.
CHECKERS = 8

# Progress is published at most this often, in seconds, and when a benchmark
# or a model is done: a small model answers dozens of items a second.
PUBLISH_EVERY = 0.5

LETTERS = "ABCDEFGHIJKLMNOP"   # MMLU-Pro has ten options, TruthfulQA up to thirteen



def _answer_line(count):
    """How to give a letter. The format, never an example of it: shown
    "Answer: B", small models answer B to everything."""
    letters = list(LETTERS[:count])
    return ("Think it through if you need to. The last line of your reply must be in the form "
            "'Answer: $LETTER' (without quotes), where LETTER is one of %s or %s."
            % (", ".join(letters[:-1]), letters[-1]))


def item_count(bench, size):
    """How many items `size` asks of `bench`."""
    wanted = SIZES.get(size, SIZES["quick"])
    return bench["total"] if wanted is None else min(wanted, bench["total"])


def _key(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def pick(bench, size):
    """The row indices a size asks, the same on every machine: ordered by the
    SHA-256 of "crucible-bench-1/<id>/<index>", the first n, then in order."""
    order = sorted(range(bench["total"]), key=lambda i: _key("%s/%s/%d" % (SUITE, bench["id"], i)))
    return sorted(order[:item_count(bench, size)])


def skills_of(benchmarks):
    """Each skill's score: the mean of its benchmarks that ran. Worked out
    from the counts, unrounded, the way the board works them out."""
    out = {}
    for skill in SKILLS:
        scores = [b["correct"] / b["n"] for bid, b in benchmarks.items()
                  if BY_ID.get(bid, {}).get("skill") == skill and b.get("n")]
        if scores:
            out[skill] = sum(scores) / len(scores)
    return out


def overall_of(skills):
    return sum(skills.values()) / len(skills) if skills else None


# ---------------------------------------------------------------------------
# Asking


def _choices(options, letters=LETTERS):
    return "\n".join("%s. %s" % (letters[i], str(text).strip()) for i, text in enumerate(options))


def _choice_check(gold, count):
    letters = LETTERS[:count]

    def check(answer):
        picked = score.choice_letter(answer, letters)
        return picked == gold, picked
    return check


def ask(bench, row, index):
    """The messages for one item, and how its answer is checked: a function
    from the answer to (right, what it read)."""
    kind = bench["id"]
    if kind == "mmlu_pro":
        options = row.get("options") or []
        text = "%s\n\n%s\n\n%s" % (row["question"].strip(), _choices(options), _answer_line(len(options)))
        return [{"role": "user", "content": text}], _choice_check(str(row["answer"]).strip(), len(options))

    if kind == "arc":
        choices = row.get("choices") or {}
        labels = [str(l) for l in choices.get("label") or []]
        options = choices.get("text") or []
        key = str(row.get("answerKey", "")).strip()
        gold = LETTERS[labels.index(key)] if key in labels else ""
        text = "%s\n\n%s\n\n%s" % (row["question"].strip(), _choices(options), _answer_line(len(options)))
        return [{"role": "user", "content": text}], _choice_check(gold, len(options))

    if kind == "hellaswag":
        endings = row.get("endings") or []
        gold = LETTERS[int(row["label"])] if str(row.get("label", "")).strip().isdigit() else ""
        context = (row.get("ctx") or "").strip()
        label = (row.get("activity_label") or "").strip()
        text = ("Which ending continues this best?\n\n%s%s\n\n%s\n\n%s"
                % (label + ": " if label else "", context, _choices(endings), _answer_line(len(endings))))
        return [{"role": "user", "content": text}], _choice_check(gold, len(endings))

    if kind == "truthfulqa":
        targets = row.get("mc1_targets") or {}
        pairs = list(zip(targets.get("choices") or [], targets.get("labels") or []))
        # MC1 lists the true answer first; shown in an order that is fixed
        # for every machine and says nothing.
        pairs.sort(key=lambda p: _key("%s/truthfulqa/%d/%s" % (SUITE, index, p[0])))
        gold = next((LETTERS[i] for i, (_, label) in enumerate(pairs) if label == 1), "")
        text = "%s\n\n%s\n\n%s" % (row["question"].strip(), _choices([c for c, _ in pairs]),
                                   _answer_line(len(pairs)))
        return [{"role": "user", "content": text}], _choice_check(gold, len(pairs))

    if kind == "gsm8k":
        gold = score.gsm8k_gold(row["answer"])
        text = ("%s\n\nSolve it step by step. The last line of your reply must be the final answer as "
                "a number alone, in the form '#### N' (without quotes)." % row["question"].strip())

        def check(answer):
            found = score.final_number(answer)
            return bool(found is not None and score.same_number(found, gold)), found
        return [{"role": "user", "content": text}], check

    if kind == "math500":
        gold = row["answer"]
        text = "%s\n\nSolve it, and put the final answer in \\boxed{}." % row["problem"].strip()

        def check(answer):
            found = score.math_answer(answer)
            return bool(found is not None and score.math_equal(found, gold)), found
        return [{"role": "user", "content": text}], check

    if kind == "humaneval":
        text = ("Complete this Python function. Reply with the whole function, from its signature "
                "on, in one ```python code block.\n\n```python\n%s```" % row["prompt"])

        def check(answer):
            program = score.humaneval_program(row, score.code_from(answer, row.get("entry_point")))
            passed, detail = score.run_python(program, timeout=CODE_SECONDS, python=sys.executable)
            return passed, detail[-300:]
        return [{"role": "user", "content": text}], check

    if kind == "mbpp":
        tests = row.get("test_list") or []
        text = ("%s\n\nYour code should pass these tests:\n%s\n\nReply with the code in one "
                "```python code block." % (row["prompt"].strip(), "\n".join(tests)))

        def check(answer):
            program = score.mbpp_program(row, score.code_from(answer))
            passed, detail = score.run_python(program, timeout=CODE_SECONDS, python=sys.executable)
            return passed, detail[-300:]
        return [{"role": "user", "content": text}], check

    if kind == "ifeval":
        def check(answer):
            follows = score.ifeval_follows(answer, row.get("instruction_id_list") or [],
                                           row.get("kwargs") or [], row.get("prompt") or "")
            return bool(follows) and all(follows), follows
        return [{"role": "user", "content": row["prompt"]}], check

    raise ValueError("no such benchmark: %s" % kind)


# ---------------------------------------------------------------------------
# Running


# Waits between tries at a page the datasets server would not give. The core
# already tries four times over half a minute; a server that is busy for
# longer than that gets two more chances, a minute apart.
FETCH_WAITS = (20, 60)


def fetch(core, bench, indices):
    """The rows at `indices`, by index, fetched a page of a hundred at a time."""
    rows = {}
    for page in sorted({i // 100 * 100 for i in indices}):
        for wait in tuple(FETCH_WAITS) + (None,):
            got = core.call("bench.rows", {"dataset": bench["dataset"], "config": bench["config"],
                                           "split": bench["split"], "offset": page, "length": 100}) or {}
            if not got.get("error"):
                break
            if wait is None or not _pause(core, wait):
                raise RuntimeError(got["error"])
        for n, item in enumerate(got.get("rows") or []):
            rows[item.get("row_idx", page + n)] = item.get("row") or {}
    return rows


def _stopped(core):
    return bool((core.call("bench.stopped") or {}).get("stopped"))


def _pause(core, seconds):
    """Wait, unless the run is stopped meanwhile. Whether it waited it out."""
    end = time.time() + seconds
    while time.time() < end:
        if _stopped(core):
            return False
        time.sleep(min(1.0, max(0.0, end - time.time())))
    return True


def _clean(mapping):
    """The board refuses null: a key with nothing to say is left out."""
    return {k: v for k, v in mapping.items() if v is not None and v != ""}


def _checked(check, answer):
    try:
        return check(answer)
    except Exception as error:   # a checker meeting something new
        return False, "checker: %s" % error


class _Asking:
    """One model's items: queued with the core in order, a few ahead of what
    it answers at once, and each answer checked as it comes back."""

    def __init__(self, core, at_once, budget, effort, tick):
        self.core = core
        self.at_once = max(1, at_once)
        self.budget = budget
        self.effort = effort
        self.tick = tick          # called with each item: checked, or None when it was not answered
        self.stopped = False
        # Timed: what the result's speed is worked out from. Every item when
        # the core answers one at a time; when it answers many, the first
        # ones, asked alone. See SOLO_TOKENS.
        self.timing = {"prompt_tokens": 0, "prompt_ms": 0.0, "output_tokens": 0, "output_ms": 0.0}
        self.solo = self.at_once > 1
        self._timed = 0
        self._waiting = collections.deque()
        self._asked = {}       # key -> (bench, index, check)
        self._checking = []    # (future, bench, index, reply)
        self._failed = set()   # benchmarks an answer failed in: the rest of them are not asked

    def run(self, tasks):
        """Ask every (bench, index, row) of `tasks` and check what comes back.
        Whatever way it ends, every answer given by then is checked and kept:
        a Stop, or a core that failed partway."""
        self._waiting.extend(tasks)
        with concurrent.futures.ThreadPoolExecutor(max_workers=CHECKERS) as checkers:
            try:
                while (self._waiting or self._asked or self._checking) and not self.stopped:
                    self._queue()
                    if self._asked:
                        self._take(checkers)
                    elif self._checking:
                        concurrent.futures.wait([c[0] for c in self._checking], timeout=0.5,
                                                return_when=concurrent.futures.FIRST_COMPLETED)
                    self._kept(wait=False)
            finally:
                self._kept(wait=True)
        return self.stopped

    def _queue(self):
        """Items queued with the core up to what it answers at once and
        AHEAD more, one at a time while the speed is timed."""
        room = 1 if self.solo else self.at_once * (1 + AHEAD)
        batch = []
        while self._waiting and len(self._asked) < room:
            bench, index, row = self._waiting.popleft()
            if bench["id"] in self._failed:
                continue
            messages, check = ask(bench, row, index)
            key = "%s/%d" % (bench["id"], index)
            self._asked[key] = (bench, index, check)
            batch.append({"key": key, "messages": messages})
        if batch:
            self.core.call("bench.ask", {"items": batch, "max_tokens": self.budget, "effort": self.effort})

    def _take(self, checkers):
        """The answers finished since the last look, each sent to be checked."""
        got = self.core.call("bench.answers", {"wait": 0.5}) or {}
        if got.get("error"):
            raise RuntimeError(got["error"])
        self.stopped = self.stopped or bool(got.get("stopped"))
        for reply in got.get("answers") or []:
            task = self._asked.pop(reply.get("key"), None)
            if task is None:
                continue
            bench, index, check = task
            if reply.get("stopped"):
                self.stopped = True
            elif reply.get("error"):
                self._failed.add(bench["id"])
                self.tick(bench, index, None, reply)
            else:
                self._time(reply)
                future = checkers.submit(_checked, check, reply.get("answer") or "")
                self._checking.append((future, bench, index, reply))

    def _kept(self, wait):
        """The checks that are done, or with `wait` every one, kept."""
        for done in [c for c in self._checking if wait or c[0].done()]:
            self._checking.remove(done)
            future, bench, index, reply = done
            self.tick(bench, index, future.result(), reply)

    def _time(self, reply):
        if not self.solo and self.at_once > 1:
            return
        for key in ("prompt_tokens", "output_tokens"):
            self.timing[key] += int(reply.get(key) or 0)
        for key in ("prompt_ms", "output_ms"):
            self.timing[key] += float(reply.get(key) or 0.0)
        self._timed += 1
        if self.timing["output_tokens"] >= SOLO_TOKENS or self._timed >= SOLO_ITEMS:
            self.solo = False

    def speed(self):
        out = {}
        if self.timing["prompt_ms"] > 0:
            out["prompt_tps"] = round(self.timing["prompt_tokens"] / (self.timing["prompt_ms"] / 1000.0), 1)
        if self.timing["output_ms"] > 0:
            out["gen_tps"] = round(self.timing["output_tokens"] / (self.timing["output_ms"] / 1000.0), 1)
        return out


def run(core, params):
    """A run: every model asked, a result kept for each, progress published."""
    size = params.get("size") if params.get("size") in SIZES else "quick"
    wanted = params.get("benchmarks") or [b["id"] for b in BENCHMARKS]
    benches = [b for b in BENCHMARKS if b["id"] in wanted]
    effort = params.get("effort") if params.get("effort") in ("low", "medium", "high") else ""
    models = [m for m in (params.get("models") or []) if m.get("path")]
    run_id = params.get("id") or time.strftime("%Y%m%d-%H%M%S")
    machine = core.call("bench.machine") or {}

    state = {
        "running": True, "id": run_id, "size": size, "started": int(time.time()),
        "benchmarks": [b["id"] for b in benches],
        "models": [{"path": m["path"], "name": m.get("name") or m["path"], "state": "waiting",
                    "benchmarks": {}} for m in models],
    }

    last = [0.0]

    def publish(soon=False):
        """Say how the run is going: at once, or with `soon`, unless it was
        said within PUBLISH_EVERY."""
        if soon and time.time() - last[0] < PUBLISH_EVERY:
            return
        last[0] = time.time()
        core.call("bench.publish", {"run": state})

    publish()
    # Every page the run needs, before any model is loaded: a network that
    # fails says so at once, and nothing holds the cards while it waits.
    rows_of, errors = {}, {}
    for bench in benches:
        if _stopped(core):
            break
        state["fetching"] = bench["id"]
        publish()
        try:
            rows_of[bench["id"]] = fetch(core, bench, pick(bench, size))
        except Exception as error:   # no network, or a dataset moved
            errors[bench["id"]] = str(error)
    state.pop("fetching", None)
    if errors:
        state["errors"] = errors
    publish()

    for at, model in enumerate(models):
        entry = state["models"][at]
        if _stopped(core):
            state["stopped"] = True
            entry["state"] = "stopped"
            continue
        entry["state"] = "loading"
        publish()
        # n_ctx, not "context": that key says whose a call is (see rpc.py).
        context, budget = CONTEXT, MAX_TOKENS
        loaded = core.call("bench.load", {"path": model["path"], "name": entry["name"], "n_ctx": context}) or {}
        if not loaded.get("ok") and not _stopped(core):
            context, budget = SMALL_CONTEXT, SMALL_MAX_TOKENS
            loaded = core.call("bench.load", {"path": model["path"], "name": entry["name"],
                                              "n_ctx": context}) or {}
        if not loaded.get("ok"):
            entry["state"] = "failed"
            entry["error"] = loaded.get("error") or "could not be loaded"
            publish()
            continue
        entry["state"] = "running"
        entry["load_ms"] = loaded.get("load_ms", 0)
        publish()

        facts = core.call("bench.model", {"path": model["path"]}) or {}
        # Which file exactly: a minute for a large model the first time, kept
        # after that. Taken while the model answers, which is the cards'
        # work and not the disk's, rather than before the first question.
        fingerprint = {}

        def take_fingerprint(path=model["path"]):
            fingerprint.update(core.call("bench.hash", {"path": path}) or {})
        hashing = threading.Thread(target=take_fingerprint, daemon=True)
        hashing.start()
        began = time.time()
        # Its own clock: the time left goes at this model's pace, not at
        # the pace of a faster one before it.
        entry["started"] = int(began)
        items = {}
        tasks = []
        for bench in benches:
            indices = pick(bench, size)
            progress = {"done": 0, "n": len(indices), "correct": 0}
            entry["benchmarks"][bench["id"]] = progress
            rows = rows_of.get(bench["id"])
            if rows is None:
                progress["error"] = errors.get(bench["id"], "not fetched")
                continue
            missing = next((i for i in indices if rows.get(i) is None), None)
            if missing is not None:
                progress["error"] = "row %d was not in its page" % missing
                continue
            items[bench["id"]] = []
            tasks.extend((bench, index, rows[index]) for index in indices)

        def current():
            """The first benchmark still being answered: where the run is."""
            for bench in benches:
                p = entry["benchmarks"][bench["id"]]
                if not p.get("error") and p["done"] < p["n"]:
                    return bench["id"]
            return None

        def result_of(stopped, failure=None):
            """The result as it stands: the board's fields, with the model's
            own name and the GPUs' names beside them for this machine's
            screen. Those are free text, which the board does not take, so
            posting_of (methods_bench.cpp) leaves them out. The model's
            repository is looked for only when it is posted."""
            # In the order of the suite, as they were asked one at a time.
            kept = {bid: sorted(got, key=lambda item: item["i"]) for bid, got in items.items() if got}
            counts = {bid: {"n": p["n"], "correct": p["correct"]} for bid, p in entry["benchmarks"].items()
                      if p["done"] == p["n"] and not p.get("error")}
            skills = skills_of(counts)
            result = {
                "schema": SCHEMA, "suite": SUITE, "size": size,
                "crucible": machine.get("crucible", ""), "llama": machine.get("llama", ""),
                "date": time.strftime("%Y-%m-%d"),
                "model": _clean({"file": facts.get("file") or model["path"].replace("\\", "/").split("/")[-1],
                                 "sha256": fingerprint.get("sha256"),
                                 "bytes": facts.get("bytes"), "name": facts.get("name"), "arch": facts.get("arch"),
                                 "params": facts.get("params"), "quant": facts.get("quant")}),
                "settings": {"context": context, "max_tokens": budget, "temperature": 0,
                             "effort": effort or "default"},
                "hardware": _clean({"backend": machine.get("backend"), "gpu_count": machine.get("gpu_count"),
                                    "gpus": machine.get("gpus") or None,
                                    "memory_gb": round(machine.get("memory_gb") or 0, 1) or None}),
                "benchmarks": counts,
                "speed": asking.speed(),
                "seconds": int(time.time() - began),
                # Kept here and not posted: what each item was and what was said.
                "id": "%s-%d" % (run_id, at),
                "path": model["path"],
                "stopped": stopped,
                "skills": skills,
                "overall": overall_of(skills),
                "items": kept,
                "at_once": asking.at_once,
            }
            missed = {bid: p["error"] for bid, p in entry["benchmarks"].items() if p.get("error")}
            if missed:
                result["errors"] = missed
            if failure:
                result["error"] = failure
            cut = {bid: sum(1 for item in got if item.get("limit")) for bid, got in kept.items()}
            cut = {bid: n for bid, n in cut.items() if n}
            if cut:
                result["cut_off"] = cut
            return result

        def tick(bench, index, checked, reply):
            progress = entry["benchmarks"][bench["id"]]
            if checked is None:
                # Not answered: the benchmark cannot be whole, and says why.
                progress["error"] = reply.get("error") or "not answered"
                entry["current"] = current()
                publish()
                return
            right, seen = checked
            answer = reply.get("answer") or ""
            progress["done"] += 1
            progress["correct"] += 1 if right else 0
            # A reply stopped in a loop is one that would have run to the
            # budget: counted as cut off, and marked so it can be told apart.
            looped = bool(reply.get("looped"))
            item = {"i": index, "ok": bool(right), "read": seen if isinstance(seen, (str, list)) else str(seen),
                    "answer": answer[-800:], "ms": reply.get("ms", 0),
                    "tokens": reply.get("output_tokens", 0), "limit": bool(reply.get("hit_limit")) or looped}
            if looped:
                item["looped"] = True
            items[bench["id"]].append(item)
            entry["current"] = current()
            if progress["done"] < progress["n"]:
                publish(soon=True)
                return
            # A benchmark done is kept at once, as a run that has not ended:
            # should the program go before the last answer, it is not lost.
            core.call("bench.save", {"result": result_of(stopped=True)})
            publish()

        entry["current"] = current()
        publish()
        asking = _Asking(core, int(loaded.get("at_once") or 1), budget, effort, tick)
        failure = None
        try:
            stopped = asking.run(tasks)
        except Disconnected:
            raise
        except Exception as error:   # the core failed partway: what was answered is kept
            stopped, failure = False, str(error)
            entry["error"] = failure
        hashing.join()   # given up on at a Stop, so never long
        result = result_of(stopped, failure)
        skills = result["skills"]
        saved = core.call("bench.save", {"result": result}) or {}
        core.call("bench.release")
        entry["state"] = "stopped" if stopped else "failed" if failure else "done"
        entry["result"] = result["id"]
        entry["skills"] = skills
        entry["overall"] = result["overall"]
        entry["saved"] = saved.get("path", "")
        entry.pop("current", None)
        publish()
        if stopped:
            state["stopped"] = True
            for rest in state["models"][at + 1:]:
                rest["state"] = "stopped"
            break

    state["running"] = False
    state["ended"] = int(time.time())
    publish()
    return {"ok": True, "id": run_id}
