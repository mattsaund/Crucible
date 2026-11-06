# SPDX-License-Identifier: MIT
"""The process Crucible starts: say hello, then answer until the pipe closes."""

import sys

from . import PROTOCOL, chat, intent, naming, routing
from .rpc import Channel


def handlers(channel):
    # Builds, cooks and benchmarks are read in the first time one is asked
    # for, not as this starts: they are most of the package, a chat turn needs
    # none of them, and the first prompt of a session waited on all of it.
    cores = []

    def core():
        from . import cook
        if not cores:
            cores.append(cook.Core(channel))
        return cores[0]

    def cook_run(params):
        from . import cook
        return cook.run(core(), params)

    def build_run(params):
        from . import build
        return build.run(core(), params)

    def bench_run(params):
        from . import bench
        return bench.run(channel, params)

    return {
        "ping": lambda params: {"pong": params.get("say", "")},
        "route": lambda params: routing.route(channel, params).to_json(),
        "route.explain": lambda params: routing.explain(channel, params),
        "decide": lambda params: intent.decide(channel, params).to_json(),
        "decide.odds": lambda params: intent.odds(channel, params),
        "chat.turn": lambda params: chat.turn(channel, params),
        "cook.run": cook_run,
        "build.run": build_run,
        "bench.run": bench_run,
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
    channel.notify("hello", {"protocol": PROTOCOL, "python": sys.version.split()[0]})
    channel.serve()


if __name__ == "__main__":
    main()
