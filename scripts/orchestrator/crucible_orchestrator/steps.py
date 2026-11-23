# SPDX-License-Identifier: MIT
"""What a loop needs to know about the steps it takes.

One prompt is worked in a loop: the expert acts, sees what came of it, and acts
again until the request is done. What is here keeps that loop honest about
itself: whether a call is one it has already made with nothing changed since,
whether it changes anything at all, what a RUN that only printed lines of a
file should have been, and what to tell a model whose reply nearly called a
tool and did not.
"""

import os
import re
import shlex


def clip(text, limit):
    """At most `limit` characters, cut at a word rather than through one."""
    text = text.strip()
    if len(text) <= limit:
        return text
    cut = text[:limit]
    # Between two words is a line break as much as a space: a thought that
    # lists files or code a line at a time can go far without a space, and was
    # cut through a word. A long run with no break at all, a path or some
    # JSON, is cut after its last slash or comma rather than inside a name.
    space = max(cut.rfind(" "), cut.rfind("\n"), cut.rfind("\t"))
    mark = max((m.end() for m in re.finditer(r"[^\w\s]+", cut)), default=-1)
    if space > limit // 2:
        cut = cut[:space]
    elif mark > limit // 2:
        cut = cut[:mark]
    elif space > 0:
        cut = cut[:space]
    return cut.rstrip(" ,.;:\n\t") + "..."


def line(text, limit):
    """`text` as one line of at most `limit` characters, for a step's summary:
    the transcript draws an action on a line, and a line break in it is a space
    there."""
    return clip(" ".join((text or "").split()), limit)


def nudge_for(parsed):
    """What to say to a reply that called nothing, from what it nearly did.

    "Did not try" is not "tried and got the syntax wrong": a model writing
    `WRITE /path "fixed it"` is failing on one character, and is told which.
    """
    attempted = parsed.get("attempted", "none")
    if attempted == "call":
        return ("That tool call could not be read, so nothing was run. Make the call again, "
                "complete and on its own.")
    if parsed.get("inline") and attempted and attempted != "none":
        # "Here it is. WRITE: style.css" and a fence: right, but a call starts
        # a line, and nothing before it on the line is read.
        return ("That was nearly right, but it cannot be run: %s has to start a line of "
                "its own, with nothing before it on that line, and what goes with it on "
                "the lines after. Send it again like that." % attempted.upper())
    if attempted == "write":
        return ("That was nearly right, but it cannot be run. WRITE needs a colon "
                "after it, and the new contents of the file go in a fenced block "
                "on the following lines -- never on the same line, and never as a "
                "description of the change. Exactly this shape:\n\n"
                "WRITE: path/to/file\n```\n<the complete new contents>\n```\n\n"
                "Try that again.")
    if attempted and attempted != "none":
        verb = attempted.upper()
        return ("That was nearly right, but it cannot be run: %s needs a colon "
                "after it, like `%s: ...`. Write it again with the colon."
                % (verb, verb))
    return ("Take one action now, using exactly one of the commands you "
            "were given, on a line of its own, with its colon.")


# --- the same call again -------------------------------------------------------
#
# A prompt does not run a call it has already made: the answer would be the
# one it already has, so the model is told that and to use it. The loop shows
# the model only its last few exchanges, so a call is a repeat while the
# earlier one's result is still among them and nothing has changed since: the
# same verb given the same thing, or the same lines of the same file however
# they were read. Asked to make a header blue without being told which file,
# gpt-oss read the page with `sed -n` and grepped it for color names until its
# rounds ran out.
#
# The first repeat is not run, and is answered with REPEAT_NUDGE. The next one
# has the loop answer from what it has. A change to a file starts the count
# again.

REPEAT_NUDGE = ("You already did exactly that, and its result is above: nothing has changed "
                "since. Use it, and take a different step. Another repeat ends this {}.")

# Commands that only look. A RUN made of nothing else changes nothing, so what
# was seen before it still stands; after any other command, it is seen afresh.
LOOKING = frozenset(("cat", "nl", "head", "tail", "sed", "grep", "egrep", "fgrep", "rg", "ag",
                     "ls", "find", "wc", "tree", "file", "stat", "pwd", "which", "less", "more",
                     "du", "cut", "diff", "cd", "echo"))

# The verbs that only look, whatever they are given.
LOOKS = frozenset(("list", "read", "find", "search", "fetch"))

# The verbs whose answer is how things are at that moment, the screen or what a
# running program has printed, which asking again rightly changes.
MOMENTS = frozenset(("screenshot", "click", "move", "type", "key", "scroll", "start", "stop", "logs"))

# READ's own range, "big.py 120-180", and a sed script that prints lines by
# number: 120,180p, 120p or 120,$p.
READ_RANGE = re.compile(r"^(.*\S)\s+(\d{1,9})\s*-\s*(\d{1,9})$")
SED_LINES = re.compile(r"^(\d{1,9})(?:,(\d{1,9}|\$))?p$")

# How a shell says it has no such program: dash, bash, zsh and cmd.
NOT_FOUND = re.compile(r"(?:^|\n)[^\n:]*:\s*(?:(?:line )?\d+:\s*)?([\w.+-]+): (?:command )?not found"
                       r"|command not found: ([\w.+-]+)"
                       r"|'([^'\n]+)' is not recognized as an internal or external command")


def shell_words(command):
    """`command` in words, with each pipe, redirection and `&&` a word of its
    own; None when a shell could not read it either."""
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=True)
        lexer.whitespace_split = True
        return list(lexer)
    except ValueError:
        return None


def is_joint(word):
    """Whether a word is the shell's punctuation rather than a program's."""
    return bool(word) and set(word) <= set("();<>|&")


def project_path(path, root=""):
    """A file named the way the project's listing names it, relative to the root."""
    path = path.strip()
    base = root.rstrip("/")
    if base and path.startswith(base + "/"):
        path = path[len(base) + 1:]
    while path.startswith("./"):
        path = path[2:]
    return path


def printed_lines(words):
    """What one command of a pipeline prints of a file, as (file, first, last):
    a last of 0 is to the end, a first below 0 is that many lines from the end,
    and the file is None when the lines come down the pipe. None when the
    command does anything else."""
    program = os.path.basename(words[0]) if words else ""
    args = words[1:]
    if program in ("cat", "nl"):
        files = [a for a in args if not a.startswith("-")]
        return (files[0], 0, 0) if len(files) == 1 else None
    if program == "sed":
        if "-n" not in args or any(a.startswith("-i") or a.startswith("--in-place") for a in args):
            return None
        rest = [a for a in args if a not in ("-n", "-e")]
        lines = SED_LINES.match(rest[0]) if 1 <= len(rest) <= 2 else None
        if not lines:
            return None
        first = int(lines.group(1))
        last = first if lines.group(2) is None else 0 if lines.group(2) == "$" else int(lines.group(2))
        return (rest[1] if len(rest) == 2 else None), first, last
    if program in ("head", "tail"):
        count, files, wanted = "10", [], False
        for a in args:
            if wanted:
                count, wanted = a, False
            elif a in ("-n", "--lines"):
                wanted = True
            elif a.startswith("--lines="):
                count = a[len("--lines="):]
            elif re.fullmatch(r"-n\+?\d+|-\d+", a):
                count = a[2:] if a.startswith("-n") else a[1:]
            elif a.startswith("-"):
                return None   # bytes, following a growing file: not lines of it
            else:
                files.append(a)
        if len(files) > 1 or not re.fullmatch(r"\+?\d{1,9}", count):
            return None
        file = files[0] if files else None
        if program == "head":
            return (file, 1, int(count)) if not count.startswith("+") else None
        return (file, int(count[1:]), 0) if count.startswith("+") else (file, -int(count), 0)
    return None


def read_of(call, root=""):
    """The lines of a file a call does nothing but read, as (file, first, last):
    for READ, and for a RUN that prints them with sed -n, cat, nl, head or tail,
    alone or in a pipe. None for any other call."""
    kind = call.get("kind", "")
    argument = (call.get("argument") or "").strip()
    if kind == "read":
        ranged = READ_RANGE.match(argument)
        if ranged:
            return project_path(ranged.group(1), root), int(ranged.group(2)), int(ranged.group(3))
        return project_path(argument, root), 0, 0
    words = shell_words(argument) if kind == "run" else None
    if not words:
        return None
    stages = [[]]
    for word in words:
        if word == "|":
            stages.append([])
        elif is_joint(word):
            return None
        else:
            stages[-1].append(word)
    read = printed_lines(stages[0])
    if read is None or read[0] is None:
        return None
    for stage in stages[1:]:
        narrowed = printed_lines(stage)
        if narrowed is None or narrowed[0] is not None or read[1:] != (0, 0):
            return None
        read = (read[0],) + narrowed[1:]
    return (project_path(read[0], root),) + read[1:]


# What node says when the script it was given is written for a web page. A
# model takes it for a bug in what it just wrote, and LFM2.5 rewrote a working
# delete button five times trying to make it go away.
PAGE_ONLY = re.compile(r"ReferenceError: (?:document|window|localStorage|sessionStorage|navigator|alert|location) "
                       r"is not defined")
PAGE_ONLY_HINT = ("\n(That file runs in a web page, which node does not have, so this error says "
                  "nothing about your change. Read the file to check it.)")


def needs_a_page(call, result):
    """Whether a RUN failed only because node was given a script for a web page."""
    return (call.get("kind") == "run" and not result.get("ok")
            and bool(re.search(r"\bnode(?:js)?\b", call.get("argument") or ""))
            and bool(PAGE_ONLY.search(result.get("output") or "")))


# gpt-oss's own edit tool, which it runs as a command when it forgets this
# one has none. Told so in the result, with what to use instead: without it,
# it re-read the file it could not change until the repeat guard ended the task.
APPLY_PATCH = re.compile(r"\s*(?:cd\s+\S+\s*&&\s*)?apply_patch\b")
APPLY_PATCH_HINT = ("\n(There is no apply_patch here. Change part of a file with EDIT: <path> and "
                    "SEARCH/REPLACE blocks, or write it whole with WRITE: <path>.)")


def read_hint(call, result, root=""):
    """For a RUN that printed lines of a file in the project and did nothing
    else, a line to go after its result naming the READ that does the same and
    numbers them; for a RUN of apply_patch, which is not here, what to use
    instead; for node run on a web page's script, what that error means; ""
    otherwise."""
    if needs_a_page(call, result):
        return PAGE_ONLY_HINT
    if call.get("kind") == "run" and APPLY_PATCH.match(call.get("argument") or ""):
        return APPLY_PATCH_HINT
    read = read_of(call, root) if call.get("kind") == "run" and result.get("ok") else None
    if read is None or os.path.isabs(read[0]) or read[0].startswith(".."):
        return ""
    path, first, last = read
    if first > 0 and last >= first:
        return "\n(READ: %s %d-%d reads the same lines, numbered.)" % (path, first, last)
    return "\n(READ: %s reads it numbered, and READ: %s 1-80 just those lines.)" % (path, path)


def not_installed(call, result):
    """The program a RUN reached for that this machine does not have, or ""."""
    if call.get("kind") != "run" or result.get("ok"):
        return ""
    found = NOT_FOUND.search(result.get("output") or "")
    return next((name for name in found.groups() if name), "") if found else ""


def only_looks(call):
    """Whether a call changes nothing, so that what was seen before it stands."""
    kind = call.get("kind", "")
    if kind in LOOKS:
        return True
    words = shell_words(call.get("argument") or "") if kind == "run" else None
    if not words:
        return False
    command = []
    for word in words + [";"]:
        if not is_joint(word):
            command.append(word)
            continue
        if word not in ("|", "||", "&&", ";"):
            return False   # a redirection, a subshell, a program left running
        if command:
            program = os.path.basename(command[0])
            if (program not in LOOKING
                    or (program == "sed" and any(a.startswith("-i") for a in command[1:]))
                    or (program == "find" and any(a in ("-delete", "-exec", "-execdir", "-ok", "-okdir")
                                                  for a in command[1:]))):
                return False
        command = []
    return True


def signature(call, root=""):
    """What makes two calls the same, or None for one that can rightly be made again."""
    kind = call.get("kind", "")
    if kind in MOMENTS:
        return None
    read = read_of(call, root)
    if read is not None:
        return ("read",) + read
    return (kind, (call.get("argument") or "").strip(), (call.get("content") or "").strip())


def spoken(call):
    """A call as the account of what was done names it: the verb, and what it
    was given."""
    return call.get("kind", "").upper() + ": " + line(call.get("argument") or "", 80)


class Repeats:
    """The calls one piece of work has made since anything last changed, each
    with where its result went among the exchanges the model is shown."""

    def __init__(self, root=""):
        self.root = root
        self.made = {}
        self.strikes = 0

    def clear(self):
        """A new piece of work, with a window of its own: nothing in it is seen yet."""
        self.made = {}
        self.strikes = 0

    def again(self, call, shown_from):
        """Whether `call` was made already, with its result in what the model was
        just shown (its exchanges from `shown_from` on) and nothing changed
        since. Each one is a strike."""
        key = signature(call, self.root)
        at = self.made.get(key) if key is not None else None
        if at is None or at < shown_from:
            return False
        self.strikes += 1
        return True

    def ran(self, call, result, at):
        """`call` was run, and its result is exchange `at`."""
        if result.get("changed"):
            self.made = {}
            self.strikes = 0
        elif not only_looks(call) and not not_installed(call, result):
            self.made = {}
        key = signature(call, self.root)
        if key is not None:
            self.made[key] = at


# --- going in circles ------------------------------------------------------------
#
# A loop going in circles is not making progress, and a model that would notice
# that itself would not be going in circles, so the loop notices. Over a window
# rather than a counter: a 1.2B expert once alternated a LIST and a RUN that
# could not work, neither repeated consecutively, forever. If the last few
# actions came from a handful of distinct ones and none changed a file, it is
# cycling. "Changed nothing" keeps edit-test-edit-test, which is also two
# actions repeating, out of it: that is the sound of work being done.

CYCLE_WINDOW = 8     # actions looked at
CYCLE_DISTINCT = 3   # at most this many distinct ones


class Cycles:
    """The last few actions, and whether they are going round."""

    def __init__(self, window=CYCLE_WINDOW, distinct=CYCLE_DISTINCT):
        self.window = window
        self.distinct = distinct
        self.seen = []

    def clear(self):
        self.seen = []

    def add(self, call, changed):
        """`call` was run, and `changed` says whether it changed a file. True
        once a full window is the same few actions changing nothing."""
        key = spoken(call) + "|" + (call.get("content") or "")
        self.seen = (self.seen + [(key, bool(changed))])[-self.window:]
        if len(self.seen) < self.window:
            return False
        return (not any(moved for _, moved in self.seen)
                and len({key for key, _ in self.seen}) <= self.distinct)
