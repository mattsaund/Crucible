# SPDX-License-Identifier: MIT
"""The process Crucible starts: say hello, then answer until the pipe closes."""

import platform
import sys

from . import PROTOCOL, build, cook, naming, routing
from .rpc import Channel


def handlers(channel):
    core = cook.Core(channel)
    return {
        "ping": lambda params: {"pong": params.get("say", "")},
        "route": lambda params: routing.route(channel, params).to_json(),
        "route.explain": lambda params: routing.explain(channel, params),
        "cook.run": lambda params: cook.run(core, params),
        "build.run": lambda params: build.run(core, params),
        "name.session": lambda params: naming.name_session(channel, params),
        "examples.write": lambda params: naming.write_examples(channel, params),
    }


def main():
    # The protocol owns standard output. Anything else that prints -- a
    # library's warning, a stray debugging line -- goes to standard error,
    # which the core keeps as a log, rather than into the middle of a message.
    out = sys.stdout.buffer
    sys.stdout = sys.stderr
    channel = Channel(sys.stdin.buffer, out)
    channel.handlers = handlers(channel)
    channel.notify("hello", {"protocol": PROTOCOL, "python": platform.python_version()})
    channel.serve()


if __name__ == "__main__":
    main()
