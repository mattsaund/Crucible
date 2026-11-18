# SPDX-License-Identifier: MIT
"""Crucible's orchestration: which expert answers, the loop a prompt is
worked in, and the benchmarks.

Run by Crucible as a child process, with its own Python, and spoken to over a
pipe -- see rpc.py. Nothing here imports anything outside the standard library,
so the Python Crucible downloads is all it needs.
"""

# Bumped whenever a message changes shape. The core refuses a package that
# speaks another version rather than finding out halfway through a prompt.
PROTOCOL = 2
