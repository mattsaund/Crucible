# SPDX-License-Identifier: MIT
"""An MLX delegator that answers from a script, for the core's tests.

It speaks the protocol of scripts/mlx/delegator.py, one JSON object a line
each way, with no MLX and no model, so the core's side of an MLX delegator
can be tested on any machine with a Python. The core runs it in place of the
real one when CRUCIBLE_MLX_DELEGATOR names it.

    scripted_delegator.py <model folder>

The folder is a fake MLX model, and its scripted.json says how this behaves.

    {"fail": "...", "takes_effort": false, "scores": {"Writing": -0.1},
     "otherwise": -5.0, "pause": 0.0, "die_on": "score", "written": "..."}

`fail` says the model could not be read and ends it. `scores` is what a
label scores, and `otherwise` what any other does. An empty label has no
score, as the real one's has none. `pause` is how long the first score
takes, for a test that gives up on one. `die_on` names a method it ends
without answering. `written` is what generate writes. Every request is
added to requests.jsonl in the folder, so a test can read back what the
core asked.
"""

import json
import os
import sys
import time


def main():
    folder = sys.argv[1]
    try:
        with open(os.path.join(folder, "scripted.json"), encoding="utf-8") as source:
            script = json.load(source)
    except (OSError, ValueError):
        script = {}
    if script.get("fail"):
        print(json.dumps({"error": script["fail"]}), flush=True)
        return 1
    print(json.dumps({"ready": True, "takes_effort": bool(script.get("takes_effort"))}), flush=True)

    paused = False
    for line in sys.stdin:
        request = json.loads(line)
        with open(os.path.join(folder, "requests.jsonl"), "a", encoding="utf-8") as log:
            log.write(json.dumps(request) + "\n")
        method = request.get("method")
        if method == script.get("die_on"):
            os._exit(3)
        reply = {"id": request.get("id")}
        if method == "format":
            reply["text"] = "".join("<%s>%s" % (m["role"], m["content"]) for m in request["messages"]) + "<assistant>"
        elif method == "score":
            if script.get("pause") and not paused:
                paused = True
                time.sleep(script["pause"])
            scores = script.get("scores") or {}
            reply["scores"] = [None if not label else scores.get(label, script.get("otherwise", -5.0))
                               for label in request["labels"]]
        elif method == "generate":
            reply["text"] = script.get("written", "A name")
        else:
            reply["error"] = "no method called %r" % method
        print(json.dumps(reply), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
