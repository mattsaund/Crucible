# SPDX-License-Identifier: MIT
"""Checking benchmark answers, against replies shaped like the ones models write:
option letters, GSM8K numbers, MATH's LaTeX, programs, and IFEval's instructions."""

import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import bench_score as score  # noqa: E402

# Root is held to no process limit, so a program run as root can fork.
ROOT = hasattr(os, "geteuid") and os.geteuid() == 0


class ChoiceLetter(unittest.TestCase):
    def test_explicit_statements(self):
        for reply, letter in [
                ("Mass is conserved here, so the answer is (C).", "C"),
                ("Answer: B", "B"),
                ("**Answer:** D", "D"),
                ("**Answer: D**", "D"),
                ("Final answer: C", "C"),
                ("So the final answer is $\\boxed{\\text{C}}$.", "C"),
                ("\\boxed{(B)}", "B"),
                ("The answer is clearly option B.", "B"),
                ("The answer is 'B'.", "B"),
                ("answer: (b)", "B"),
                ("The correct option is D.", "D"),
                ("Option B is correct.", "B"),
                ("B is the correct answer.", "B"),
                ("I would choose C.", "C")]:
            self.assertEqual(score.choice_letter(reply, "ABCD"), letter, reply)

    def test_the_last_statement_wins(self):
        self.assertEqual(score.choice_letter(
            "The answer is (A). Wait, that ignores friction. Actually, the answer is (D).", "ABCD"), "D")
        self.assertEqual(score.choice_letter(
            "The answer isn't A, it's B.\n\nAnswer: B", "ABCD"), "B")

    def test_articles_and_pronouns_are_not_letters(self):
        self.assertEqual(score.choice_letter(
            "Answer: A good way to see this is to compare the masses. The answer is (B).", "ABCD"), "B")
        self.assertEqual(score.choice_letter("The answer is a bit subtle. Let's see.\nAnswer: C", "ABCD"), "C")
        self.assertIsNone(score.choice_letter("The answer is I think (B)", "ABCDEFGHIJ"))
        self.assertIsNone(score.choice_letter("I think A is a strong candidate, but I'm not sure.", "ABCD"))
        self.assertIsNone(score.choice_letter("Answer: A 5% increase", "ABCD"))
        self.assertIsNone(score.choice_letter("Only I is correct among the statements.", "ABCDEFGHIJ"))

    def test_a_and_i_as_letters(self):
        self.assertEqual(score.choice_letter("Answer: A because the others break symmetry", "ABCD"), "A")
        self.assertEqual(score.choice_letter("The answer is A.", "ABCD"), "A")
        self.assertEqual(score.choice_letter("The answer is I.", "ABCDEFGHIJ"), "I")
        self.assertEqual(score.choice_letter("Answer: I", "ABCDEFGHIJ"), "I")

    def test_a_line_with_only_the_letter(self):
        for reply in ("Comparing the four options:\n\nC", "(C)", "C.", "C)", "**C**", "\n  C  \n"):
            self.assertEqual(score.choice_letter(reply, "ABCD"), "C", reply)

    def test_a_reply_that_starts_with_a_label(self):
        self.assertEqual(score.choice_letter("B) London", "ABCD"), "B")
        self.assertEqual(score.choice_letter("B. London\n\nIt has been the capital for centuries.", "ABCD"), "B")

    def test_the_options_read_back_are_not_a_choice(self):
        self.assertIsNone(score.choice_letter("A. Paris\nB. London\nC. Rome\nD. Berlin", "ABCD"))

    def test_only_the_options_there_are(self):
        self.assertIsNone(score.choice_letter("Answer: E", "ABCD"))
        self.assertEqual(score.choice_letter("The answer is (J)", "ABCDEFGHIJ"), "J")
        self.assertEqual(score.choice_letter("The answer is (C)", ["A", "B", "C", "D"]), "C")

    def test_digit_labels(self):
        # A few ARC items label their choices 1 to 4.
        self.assertEqual(score.choice_letter("The answer is 3.", "1234"), "3")
        self.assertIsNone(score.choice_letter("The answer is 3.", "ABCD"))

    def test_nothing_to_read(self):
        self.assertIsNone(score.choice_letter("", "ABCD"))
        self.assertIsNone(score.choice_letter(None, "ABCD"))
        self.assertIsNone(score.choice_letter("I am not sure about this one.", "ABCD"))


GSM8K_ANSWER = ("Janet sells 16 - 3 - 4 = <<16-3-4=9>>9 duck eggs a day.\nShe makes 9 * 2 = "
                "$<<9*2=18>>18 every day at the farmer\u2019s market.\n#### 18")


class Numbers(unittest.TestCase):
    def test_the_gold_of_a_gsm8k_answer(self):
        self.assertEqual(score.gsm8k_gold(GSM8K_ANSWER), "18")
        self.assertEqual(score.gsm8k_gold("So the total is 1,000.\n#### 1,000"), "1000")
        self.assertIsNone(score.gsm8k_gold("no marker here"))
        self.assertIsNone(score.gsm8k_gold(None))

    def test_hashes_first(self):
        self.assertEqual(score.final_number(GSM8K_ANSWER), "18")
        self.assertEqual(score.final_number("The answer is 5.\n#### 7"), "7")

    def test_a_markdown_heading_is_not_hashes(self):
        self.assertEqual(score.final_number("#### Step 3: add\n5 + 3 = 8\n\nThe answer is 8."), "8")

    def test_the_box_before_the_phrase(self):
        self.assertEqual(score.final_number("The answer is 12, so \\boxed{18}."), "18")
        self.assertEqual(score.final_number("Total: $\\boxed{\\$1,080}$"), "1080")
        self.assertEqual(score.final_number("\\boxed{3 \\times 6 = 18}"), "18")

    def test_the_phrase_before_the_last_number(self):
        self.assertEqual(score.final_number("The answer is 42. It took 3 steps to get there."), "42")
        self.assertEqual(score.final_number("**Answer:** $1,234.50 per month"), "1234.5")

    def test_the_last_number_otherwise(self):
        self.assertEqual(score.final_number("She buys 3 packs of 4 pens, so she has 12 pens."), "12")

    def test_how_numbers_are_written(self):
        self.assertEqual(score.final_number("The answer is $1,080.00."), "1080")
        self.assertEqual(score.final_number("It went up by 25%."), "25")
        self.assertEqual(score.final_number("The temperature is -3 degrees."), "-3")
        self.assertEqual(score.final_number("The answer is 0.50"), "0.5")
        self.assertEqual(score.final_number("The answer is \u22127."), "-7")
        self.assertEqual(score.final_number("From 3-5 people, so 5."), "5")
        self.assertEqual(score.final_number("Each costs .75"), "0.75")

    def test_no_number(self):
        self.assertIsNone(score.final_number(""))
        self.assertIsNone(score.final_number(None))
        self.assertIsNone(score.final_number("I cannot work this out."))

    def test_the_same_number(self):
        self.assertTrue(score.same_number("72", "72.0"))
        self.assertTrue(score.same_number("72.00", "72"))
        self.assertTrue(score.same_number("1,000", "1000"))
        self.assertTrue(score.same_number("$5", "5"))
        self.assertTrue(score.same_number("-3", "-3.0"))
        self.assertTrue(score.same_number("100000000", "100000000.01"))
        self.assertTrue(score.same_number(18, "18"))
        self.assertFalse(score.same_number("72", "73"))
        self.assertFalse(score.same_number("0.5", "0.6"))
        self.assertFalse(score.same_number("72", "seventy-two"))
        self.assertFalse(score.same_number(None, "72"))
        self.assertFalse(score.same_number("", ""))
        self.assertFalse(score.same_number("inf", "inf"))


class Boxed(unittest.TestCase):
    def test_braces_nest(self):
        self.assertEqual(score.last_boxed("so $\\boxed{\\frac{1}{2}}$."), "\\frac{1}{2}")
        self.assertEqual(score.last_boxed("\\boxed{\\left( 3, \\frac{\\pi}{2} \\right)}"),
                         "\\left( 3, \\frac{\\pi}{2} \\right)")
        self.assertEqual(score.last_boxed("\\boxed{\\{1, 2\\}}"), "\\{1, 2\\}")

    def test_the_last_box(self):
        self.assertEqual(score.last_boxed("First \\boxed{3}, then \\boxed{4}."), "4")

    def test_without_braces(self):
        self.assertEqual(score.last_boxed("The answer is $\\boxed 5$."), "5")
        self.assertEqual(score.last_boxed("\\boxed -3."), "-3")

    def test_fbox(self):
        self.assertEqual(score.last_boxed("\\fbox{7}"), "7")

    def test_a_box_that_never_closes(self):
        self.assertIsNone(score.last_boxed("The answer is \\boxed{\\frac{1}{2"))

    def test_no_box(self):
        self.assertIsNone(score.last_boxed("The answer is 5."))
        self.assertIsNone(score.last_boxed(""))
        self.assertIsNone(score.last_boxed(None))
        self.assertIsNone(score.last_boxed("\\boxedtext{5}"))

    def test_the_answer_of_a_solution(self):
        self.assertEqual(score.math_answer("Adding, $x = 4$, so the answer is $\\boxed{4}$."), "4")
        self.assertEqual(score.math_answer(
            "Final Answer: The final answer is $\\frac{1}{2}$. I hope it is correct."), "\\frac{1}{2}")
        self.assertEqual(score.math_answer("So the answer is 0.5. Done."), "0.5")
        self.assertEqual(score.math_answer("We get $x^2$ and then $x = 7$"), "x = 7")
        self.assertEqual(score.math_answer("which gives \\(3\\sqrt{2}\\)"), "3\\sqrt{2}")
        self.assertIsNone(score.math_answer("I could not solve it."))
        self.assertIsNone(score.math_answer(None))


class MathEqual(unittest.TestCase):
    def same(self, pred, gold):
        self.assertTrue(score.math_equal(pred, gold), "%r should equal %r" % (pred, gold))

    def different(self, pred, gold):
        self.assertFalse(score.math_equal(pred, gold), "%r should not equal %r" % (pred, gold))

    def test_math500_golds(self):
        for gold, answers in [
                ("\\frac{14}{3}", ["\\dfrac{14}{3}", "14/3", "4\\frac{2}{3}", "\\frac{14}{3}."]),
                ("\\left( 3, \\frac{\\pi}{2} \\right)", ["(3, \\frac{\\pi}{2})", "(3,\\pi/2)",
                                                        "\\left(3,\\frac\\pi2\\right)"]),
                ("p - q", ["p-q", "p - q"]),
                ("\\text{Evelyn}", ["Evelyn", "\\textbf{Evelyn}", "evelyn"]),
                ("11\\sqrt2", ["11\\sqrt{2}", "11 \\sqrt{2}"]),
                ("6 - 5i", ["6-5i"]),
                ("\\$18.90", ["18.90", "18.9", "\\$18.90", "18.90 dollars"]),
                ("10\\%", ["10", "10%"]),
                ("x^2+7x+10", ["x^2 + 7x + 10", "x^{2}+7x+10"]),
                ("[-2,7]", ["[-2, 7]", "x \\in [-2,7]", "\\left[-2,7\\right]"]),
                ("\\infty", ["\\infty", "\u221e"])]:
            for answer in answers:
                self.same(answer, gold)

    def test_differences_that_matter(self):
        self.different("\\frac{3}{14}", "\\frac{14}{3}")
        self.different("4.67", "\\frac{14}{3}")
        self.different("q - p", "p - q")
        self.different("6 + 5i", "6 - 5i")
        self.different("(-2,7)", "[-2,7]")
        self.different("x^2+7x+12", "x^2+7x+10")
        self.different("Evelyn", "\\text{Evelyn Smith}")
        self.different("-\\infty", "\\infty")
        self.different("11\\sqrt3", "11\\sqrt2")
        self.different("0.1", "10\\%")
        self.different("18.09", "\\$18.90")
        self.different("(-2,8)", "(8,-2)")

    def test_numbers_by_value(self):
        self.same("0.5", "\\frac{1}{2}")
        self.same("\\frac12", "\\frac{1}{2}")
        self.same("1/2", "\\frac 12")
        self.same("0.35625", ".35625")
        self.same("58500", "58,500")
        self.same("11111111100", "11,\\! 111,\\! 111,\\! 100")
        self.same("\\frac{9}{5}", "1\\frac{4}{5}")
        self.same("-\\frac{3}{8}", "-0.375")

    def test_degrees_and_units(self):
        self.same("30", "30^\\circ")
        self.same("30\u00b0", "30^\\circ")
        self.same("30^{\\circ}", "30")
        self.same("\\frac{270}{7}", "\\frac{270}7\\text{ degrees}")
        self.same("5.4", "5.4 \\text{ cents}")
        self.same("864", "864 \\mbox{ inches}^2")
        self.same("864 square inches", "864 \\mbox{ inches}^2")
        self.same("15\\text{ cm}^2", "15\\mbox{ cm}^2")

    def test_a_leading_variable(self):
        self.same("5", "x=5")
        self.same("x = 5", "5")
        self.same("y=2x+3", "y = 2x + 3")
        self.different("x = 6", "x=5")

    def test_lists_in_any_order(self):
        self.same("-2, 1", "1,-2")
        self.same("7,5,3", "3, 5, 7")
        self.same("x = 1, x = -2", "1,-2")
        self.different("1, 2", "1,-2")

    def test_choices_bases_and_matrices(self):
        self.same("C", "\\text{(C)}")
        self.same("(C)", "\\text{(C)}")
        self.different("D", "\\text{(C)}")
        self.same("4210_5", "4210_{5}")
        self.same("4210", "4210_{5}")
        self.different("4210_6", "4210_{5}")
        self.same("\\begin{bmatrix} -\\frac{1}{3} \\\\ \\frac{2}{3} \\\\ \\frac{5}{3} \\end{bmatrix}",
                  "\\begin{pmatrix} -1/3 \\\\ 2/3 \\\\ 5/3 \\end{pmatrix}")

    def test_sets_functions_and_scientific_notation(self):
        self.same("\\{3,5,7\\}", "3, 5, 7")
        self.same("\\{-2, 1\\pm\\sqrt{5}\\}", "\\{1\\pm\\sqrt{5},-2\\}")
        self.different("\\{1\\}\\cup\\{2\\}", "1,2")
        self.same("\\cot(x)", "\\cot x")
        self.different("\\log(x+1)", "\\log x + 1")
        self.same("6.72 \\times 10^{-5}", ".0000672")
        self.different("6.72", ".0000672")

    def test_a_boxed_answer_is_unwrapped(self):
        self.same("\\boxed{\\dfrac{14}{3}}", "\\frac{14}{3}")

    def test_nothing_is_not_an_answer(self):
        self.different(None, "5")
        self.different("", "5")
        self.different("5", None)
        self.different("x=", "5")


HUMANEVAL_0 = {
    "entry_point": "has_close_elements",
    "prompt": 'from typing import List\n\n\ndef has_close_elements(numbers: List[float], threshold: float)'
              ' -> bool:\n    """ Check if in given list of numbers, are any two numbers closer to each'
              ' other than\n    given threshold.\n    >>> has_close_elements([1.0, 2.0, 3.0], 0.5)\n    False\n'
              '    >>> has_close_elements([1.0, 2.8, 3.0, 4.0, 5.0, 2.0], 0.3)\n    True\n    """\n',
    "test": "\n\nMETADATA = {\n    'author': 'jt',\n    'dataset': 'test'\n}\n\n\ndef check(candidate):\n"
            "    assert candidate([1.0, 2.0, 3.9, 4.0, 5.0, 2.2], 0.3) == True\n"
            "    assert candidate([1.0, 2.0, 3.9, 4.0, 5.0, 2.2], 0.05) == False\n"
            "    assert candidate([1.0, 2.0, 5.9, 4.0, 5.0], 0.95) == True\n"
            "    assert candidate([1.0, 2.0, 5.9, 4.0, 5.0], 0.8) == False\n"
            "    assert candidate([1.0, 2.0, 3.0, 4.0, 5.0, 2.0], 0.1) == True\n"
            "    assert candidate([1.1, 2.2, 3.1, 4.1, 5.1], 1.0) == True\n"
            "    assert candidate([1.1, 2.2, 3.1, 4.1, 5.1], 0.5) == False\n\n",
}
HUMANEVAL_0_BODY = ("    for idx, elem in enumerate(numbers):\n        for idx2, elem2 in enumerate(numbers):\n"
                    "            if idx != idx2:\n                distance = abs(elem - elem2)\n"
                    "                if distance < threshold:\n                    return True\n\n"
                    "    return False\n")

HUMANEVAL_38 = {
    "entry_point": "decode_cyclic",
    "prompt": '\n\ndef encode_cyclic(s: str):\n    """\n    returns encoded string by cycling groups of three'
              ' characters.\n    """\n    # split string to groups. Each of length 3.\n    groups = [s[(3 * i):'
              'min((3 * i + 3), len(s))] for i in range((len(s) + 2) // 3)]\n    # cycle elements in each'
              ' group. Unless group has fewer elements than 3.\n    groups = [(group[1:] + group[0]) if len('
              'group) == 3 else group for group in groups]\n    return "".join(groups)\n\n\ndef decode_cyclic('
              's: str):\n    """\n    takes as input string encoded with encode_cyclic function. Returns '
              'decoded string.\n    """\n',
    "test": "\n\nMETADATA = {}\n\n\ndef check(candidate):\n    from random import randint, choice\n"
            "    import string\n\n    letters = string.ascii_lowercase\n    for _ in range(100):\n"
            "        str = ''.join(choice(letters) for i in range(randint(10, 20)))\n"
            "        encoded_str = encode_cyclic(str)\n        assert candidate(encoded_str) == str\n\n",
}

MBPP_11 = {
    "test_imports": [],
    "test_list": ['assert remove_Occ("hello","l") == "heo"', 'assert remove_Occ("abcda","a") == "bcd"',
                  'assert remove_Occ("PHP","P") == "H"'],
}
MBPP_11_CODE = ("def remove_Occ(s,ch): \n    for i in range(len(s)): \n        if (s[i] == ch): \n"
                "            s = s[0 : i] + s[i + 1:] \n            break\n    for i in range(len(s) - 1,-1,-1):  \n"
                "        if (s[i] == ch): \n            s = s[0 : i] + s[i + 1:] \n            break\n    return s ")
MBPP_82 = {
    "test_imports": ["import math"],
    "test_list": ["assert math.isclose(volume_sphere(10), 4188.790204786391, rel_tol=0.001)",
                  "assert math.isclose(volume_sphere(25), 65449.84694978735, rel_tol=0.001)",
                  "assert math.isclose(volume_sphere(20), 33510.32163829113, rel_tol=0.001)"],
}


class Code(unittest.TestCase):
    def test_the_block_that_defines_the_entry_point(self):
        reply = ("First a helper:\n\n```python\ndef helper(x):\n    return x\n```\n\nThen the function:\n\n"
                 "```python\ndef solve(x):\n    return helper(x) + 1\n```\n\nUsage:\n\n```python\nprint(solve(1))\n```")
        self.assertEqual(score.code_from(reply, "solve"), "def solve(x):\n    return helper(x) + 1")
        self.assertEqual(score.code_from(reply), "print(solve(1))")

    def test_plain_fences_and_other_languages(self):
        self.assertEqual(score.code_from("Here:\n```\nx = 1\n```\n"), "x = 1")
        self.assertEqual(score.code_from("```py\nx = 2\n```\n\n```bash\npython main.py\n```"), "x = 2")
        self.assertEqual(score.code_from("```Python3\nx = 3\n```"), "x = 3")

    def test_a_block_cut_off_or_indented(self):
        self.assertEqual(score.code_from("Sure:\n```python\ndef f():\n    return 1"), "def f():\n    return 1")
        self.assertEqual(score.code_from("1. Write it:\n   ```python\n   def f():\n       return 1\n   ```"),
                         "def f():\n    return 1")

    def test_no_fence_means_the_whole_text(self):
        self.assertEqual(score.code_from("def f():\n    return 1\n"), "def f():\n    return 1\n")
        self.assertEqual(score.code_from(None), "")

    def test_a_humaneval_body_goes_after_the_prompt(self):
        program = score.humaneval_program(HUMANEVAL_0, HUMANEVAL_0_BODY)
        self.assertTrue(program.startswith(HUMANEVAL_0["prompt"] + HUMANEVAL_0_BODY))
        self.assertTrue(program.endswith("check(has_close_elements)\n"))
        self.assertEqual(score.run_python(program)[0], True)
        self.assertEqual(score.run_python(score.humaneval_program(HUMANEVAL_0, "    return False\n"))[0], False)

    def test_a_whole_humaneval_function_keeps_the_imports(self):
        whole = ("def has_close_elements(numbers: List[float], threshold: float) -> bool:\n"
                 "    return any(abs(a - b) < threshold for i, a in enumerate(numbers)\n"
                 "               for j, b in enumerate(numbers) if i != j)\n")
        program = score.humaneval_program(HUMANEVAL_0, whole)
        self.assertLess(program.index("from typing import List"), program.index(whole))
        self.assertEqual(program.count("def has_close_elements"), 1)
        self.assertEqual(score.run_python(program)[0], True)

    def test_a_whole_humaneval_function_keeps_the_helpers_its_test_calls(self):
        whole = "def decode_cyclic(s: str):\n    return encode_cyclic(encode_cyclic(s))\n"
        passed, detail = score.run_python(score.humaneval_program(HUMANEVAL_38, whole))
        self.assertTrue(passed, detail)

    def test_mbpp_imports_code_and_asserts_each_on_a_line(self):
        program = score.mbpp_program(MBPP_11, MBPP_11_CODE)
        self.assertEqual(program.splitlines()[-3:], MBPP_11["test_list"])
        self.assertEqual(score.run_python(program)[0], True)
        code = "def volume_sphere(r):\n  return (4/3)*math.pi*r*r*r"
        program = score.mbpp_program(MBPP_82, code)
        self.assertTrue(program.startswith("import math\ndef volume_sphere"))
        self.assertEqual(score.run_python(program)[0], True)
        self.assertEqual(score.run_python(score.mbpp_program(MBPP_11, "def remove_Occ(s, ch):\n    return s"))[0],
                         False)


class RunPython(unittest.TestCase):
    def test_a_program_that_passes(self):
        self.assertEqual(score.run_python("assert 1 + 1 == 2\nprint('fine')"), (True, "fine\n"))

    def test_a_failed_assert(self):
        passed, detail = score.run_python("def f():\n    return 3\n\nassert f() == 4\n")
        self.assertFalse(passed)
        self.assertIn("AssertionError", detail)

    def test_a_syntax_error(self):
        passed, detail = score.run_python("def f(:\n    pass\n")
        self.assertFalse(passed)
        self.assertIn("SyntaxError", detail)

    def test_a_loop_that_never_ends(self):
        started = time.monotonic()
        self.assertEqual(score.run_python("while True:\n    pass\n", timeout=2), (False, "timed out"))
        self.assertLess(time.monotonic() - started, 8)

    def test_a_fresh_folder_that_is_removed_after(self):
        passed, detail = score.run_python("import os\nprint(os.getcwd())\nprint(os.listdir('.'))")
        self.assertTrue(passed)
        folder, listing = detail.splitlines()
        self.assertNotEqual(os.path.realpath(folder), os.path.realpath(os.getcwd()))
        self.assertEqual(listing, "['program.py']")
        self.assertFalse(os.path.exists(folder))

    def test_isolated_with_nothing_on_standard_input(self):
        passed, detail = score.run_python("import sys\nassert sys.flags.isolated\ninput()")
        self.assertFalse(passed)
        self.assertIn("EOFError", detail)

    def test_the_end_of_the_output(self):
        passed, detail = score.run_python("print('x' * 5000 + 'END')")
        self.assertTrue(passed)
        self.assertLessEqual(len(detail), 2000)
        self.assertTrue(detail.endswith("END\n"))

    def test_an_interpreter_that_is_not_there(self):
        passed, detail = score.run_python("pass", python=os.path.join(tempfile.gettempdir(), "no-such-python"))
        self.assertFalse(passed)
        self.assertTrue(detail)

    @unittest.skipIf(ROOT, "root is held to no process limit")
    def test_a_program_starts_no_other(self):
        # A grandchild in a session of its own would outlive the group a timeout
        # kills. It never starts, so the program fails at once instead of
        # sleeping out its time, and nothing is left to write the marker.
        with tempfile.TemporaryDirectory() as folder:
            marker = os.path.join(folder, "still-running")
            grandchild = "import time; time.sleep(1.5); open(%r, 'w').close()" % marker
            program = ("import subprocess, sys, time\n"
                       "subprocess.Popen([sys.executable, '-c', %r], start_new_session=True)\n"
                       "time.sleep(60)\n" % grandchild)
            started = time.monotonic()
            passed, detail = score.run_python(program, timeout=10)
            self.assertFalse(passed)
            self.assertNotEqual(detail, "timed out")
            self.assertLess(time.monotonic() - started, 8)
            time.sleep(2)
            self.assertFalse(os.path.exists(marker))

    @unittest.skipIf(os.name == "nt" or ROOT, "a POSIX fork, under a process limit")
    def test_a_fork_fails(self):
        # Twenty at most, each gone at once: should the limit ever stop holding,
        # this fails rather than filling the machine.
        program = ("import os\n"
                   "for _ in range(20):\n"
                   "    pid = os.fork()\n"
                   "    if pid == 0:\n"
                   "        os._exit(0)\n"
                   "    os.waitpid(pid, 0)\n"
                   "print('forked')\n")
        passed, detail = score.run_python(program)
        self.assertFalse(passed)
        self.assertIn("BlockingIOError", detail)

    def test_the_job_limits_are_laid_out_as_windows_lays_them_out(self):
        import ctypes
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.skipTest("the 64-bit layout")
        limits = score._job_limits_type()
        basic = type(limits().BasicLimitInformation)
        # Offsets from winnt.h, for x64 and arm64 alike.
        self.assertEqual(ctypes.sizeof(basic), 64)
        self.assertEqual(basic.LimitFlags.offset, 16)
        self.assertEqual(basic.ActiveProcessLimit.offset, 40)
        self.assertEqual(limits.ProcessMemoryLimit.offset, 112)
        self.assertEqual(ctypes.sizeof(limits), 144)

    @unittest.skipUnless(sys.platform.startswith("linux") or os.name == "nt",
                         "an address space limit holds on Linux, and a job's on Windows")
    def test_memory_is_limited(self):
        passed, detail = score.run_python("b = bytearray(5 << 30)\nprint(len(b))")
        self.assertFalse(passed)
        self.assertIn("MemoryError", detail)

    @unittest.skipIf(os.name == "nt", "no resource limits on Windows")
    def test_file_size_is_limited(self):
        passed, detail = score.run_python(
            "with open('big', 'wb') as f:\n    for _ in range(65):\n        f.write(b'x' * (1 << 20))\n")
        self.assertFalse(passed)


def follows(response, instruction, prompt="", **kwargs):
    return score.ifeval_follows(response, [instruction], [kwargs], prompt)[0]


class Keywords(unittest.TestCase):
    def test_existence(self):
        kind = "keywords:existence"
        self.assertTrue(follows("Hannah the intern will grow fast.", kind, keywords=["intern", "grow"]))
        self.assertTrue(follows("INTERNS? No: one INTERN, who will Grow.", kind, keywords=["intern", "grow"]))
        self.assertFalse(follows("Hannah's internship will help her grow.", kind, keywords=["intern", "grow"]))
        self.assertFalse(follows("Hannah the intern.", kind, keywords=["intern", "grow"]))

    def test_frequency(self):
        kind = "keywords:frequency"
        self.assertTrue(follows("They went there and then left.", kind, keyword="the", relation="less than",
                                frequency=2))
        self.assertFalse(follows("The cat sat on the mat.", kind, keyword="the", relation="less than", frequency=2))
        self.assertTrue(follows("War, war and more war.", kind, keyword="war", relation="at least", frequency=3))
        self.assertFalse(follows("Software toward an award.", kind, keyword="war", relation="at least",
                                 frequency=1))

    def test_forbidden_words(self):
        kind = "keywords:forbidden_words"
        self.assertTrue(follows("Jazz is calm and slow.", kind, forbidden_words=["rock", "loud"]))
        self.assertFalse(follows("Rock music is fun.", kind, forbidden_words=["rock", "loud"]))

    def test_letter_frequency(self):
        kind = "keywords:letter_frequency"
        self.assertTrue(follows("Quick, quiet quails.", kind, letter="q", let_relation="at least", let_frequency=3))
        self.assertFalse(follows("A quick fox.", kind, letter="q", let_relation="at least", let_frequency=2))
        self.assertTrue(follows("Wow! Nice.", kind, letter="!", let_relation="less than", let_frequency=2))
        self.assertFalse(follows("Wow! Nice!", kind, letter="!", let_relation="less than", let_frequency=2))


class Lengths(unittest.TestCase):
    def test_number_of_sentences(self):
        kind = "length_constraints:number_sentences"
        self.assertTrue(follows("It rained. We stayed in!", kind, relation="less than", num_sentences=3))
        self.assertFalse(follows("It rained. We stayed in! Why?", kind, relation="less than", num_sentences=3))
        # Abbreviations, initials, decimals and list numbers end nothing.
        text = ("Dr. Smith met J. K. Rowling at 5 p.m. in the U.S. on a Monday. Pi is about 3.14, e.g. in "
                "circles.\n1. First item\n2. Second item")
        self.assertTrue(follows(text, kind, relation="at least", num_sentences=3))
        self.assertFalse(follows(text, kind, relation="at least", num_sentences=4))

    def test_sentences_in_other_scripts(self):
        kind = "length_constraints:number_sentences"
        text = "\u092f\u0939 \u090f\u0915 \u0939\u0948\u0964 \u0935\u0939 \u0926\u0942\u0938\u0930\u093e \u0939\u0948\u0964"
        self.assertTrue(follows(text, kind, relation="at least", num_sentences=2))
        self.assertTrue(follows("\u4eca\u5929\u5f88\u597d\u3002\u6211\u4eec\u53bb\u516c\u56ed\u3002", kind,
                                relation="at least", num_sentences=2))

    def test_number_of_paragraphs(self):
        kind = "length_constraints:number_paragraphs"
        self.assertTrue(follows("First part.\n***\nSecond part.\n\n***\n\nThird part.", kind, num_paragraphs=3))
        self.assertTrue(follows("***\nFirst.\n***\nSecond.\n***", kind, num_paragraphs=2))
        self.assertFalse(follows("First part.\n***\nSecond part.", kind, num_paragraphs=3))
        self.assertFalse(follows("First.\n***\n***\nSecond.\n***\nThird.", kind, num_paragraphs=3))
        self.assertFalse(follows("***Bold*** start.\n\nStill one.", kind, num_paragraphs=2))

    def test_number_of_words(self):
        kind = "length_constraints:number_words"
        self.assertTrue(follows("Don't stop - keep going, well-known friend.", kind, relation="less than",
                                num_words=7))
        self.assertFalse(follows("Don't stop - keep going, well-known friend.", kind, relation="less than",
                                 num_words=6))
        self.assertTrue(follows("one two three four five", kind, relation="at least", num_words=5))
        self.assertFalse(follows("one two three four", kind, relation="at least", num_words=5))

    def test_first_word_of_a_paragraph(self):
        kind = "length_constraints:nth_paragraph_first_word"
        text = "Tottenham were better.\n\nStockton were not.\n\nBonding matters most."
        self.assertTrue(follows(text, kind, num_paragraphs=3, nth_paragraph=3, first_word="bonding"))
        self.assertTrue(follows('"' + text.replace("Bonding", "**Bonding**") + '"', kind, num_paragraphs=3,
                                nth_paragraph=3, first_word="bonding"))
        self.assertFalse(follows(text, kind, num_paragraphs=3, nth_paragraph=2, first_word="bonding"))
        self.assertFalse(follows(text, kind, num_paragraphs=4, nth_paragraph=3, first_word="bonding"))
        self.assertFalse(follows(text.replace("\n\n", "\n"), kind, num_paragraphs=3, nth_paragraph=3,
                                 first_word="bonding"))


class Content(unittest.TestCase):
    def test_placeholders(self):
        kind = "detectable_content:number_placeholders"
        self.assertTrue(follows("Dear [name], we live at [address].", kind, num_placeholders=2))
        self.assertFalse(follows("Dear [name], we live at [ ] and [].", kind, num_placeholders=2))

    def test_postscript(self):
        kind = "detectable_content:postscript"
        self.assertTrue(follows("Thanks!\n\nP.S. Bring snacks.", kind, postscript_marker="P.S."))
        self.assertTrue(follows("thanks\n\np.p.s. bring snacks", kind, postscript_marker="P.P.S"))
        self.assertTrue(follows("Thanks!\n\n**P.P.S** Bring snacks.", kind, postscript_marker="P.P.S"))
        self.assertFalse(follows("Thanks!\n\nPS: Bring snacks.", kind, postscript_marker="P.S."))
        self.assertFalse(follows("Thanks!\n\nP.S. Bring snacks.", kind, postscript_marker="P.P.S"))


class Formats(unittest.TestCase):
    def test_bullet_points(self):
        kind = "detectable_format:number_bullet_lists"
        text = "Names:\n* Liam\n* Noah\n- Oliver\n\n***\n**Bold** line\n* * *"
        self.assertTrue(follows(text, kind, num_bullets=3))
        self.assertFalse(follows(text, kind, num_bullets=2))

    def test_constrained_response(self):
        kind = "detectable_format:constrained_response"
        self.assertTrue(follows("Poodles vary in size. My answer is maybe.", kind))
        self.assertFalse(follows("My answer is yes. Or rather, My answer is no.", kind))
        self.assertFalse(follows("Yes.", kind))

    def test_highlighted_sections(self):
        kind = "detectable_format:number_highlighted_sections"
        self.assertTrue(follows("*Buena Onda* serves **empanadas** daily.", kind, num_highlights=2))
        self.assertFalse(follows("* a bullet with *one* highlight\n2 * 3 * 4", kind, num_highlights=2))

    def test_sections(self):
        kind = "detectable_format:multiple_sections"
        text = "SECTION 1\nI miss class.\n\n**SECTION 2:** Still missing it."
        self.assertTrue(follows(text, kind, section_spliter="SECTION", num_sections=2))
        self.assertFalse(follows(text, kind, section_spliter="SECTION", num_sections=3))
        self.assertFalse(follows("Section 1\nA\nSection 2\nB", kind, section_spliter="SECTION", num_sections=2))

    def test_json(self):
        kind = "detectable_format:json_format"
        self.assertTrue(follows('```json\n{"pros": ["pay"], "cons": []}\n```', kind))
        self.assertTrue(follows('{"pros": ["pay"]}', kind))
        self.assertFalse(follows('Here it is: {"pros": ["pay"]}', kind))
        self.assertFalse(follows('```json\n{"pros": ["pay"]}\n```\nHope that helps!', kind))

    def test_title(self):
        kind = "detectable_format:title"
        self.assertTrue(follows("<<I Quit>>\n\nDear boss,", kind))
        self.assertFalse(follows("<< >>\n\nDear boss,", kind))
        self.assertFalse(follows("I Quit\n\nDear boss,", kind))


class Combinations(unittest.TestCase):
    def test_two_responses(self):
        kind = "combination:two_responses"
        self.assertTrue(follows("God is called Allah.\n******\nGod is called Yahweh.", kind))
        self.assertTrue(follows("First joke.\n*******\nSecond joke.", kind))
        self.assertFalse(follows("Same answer.\n******\nSame answer.", kind))
        self.assertFalse(follows("One.\n******\nTwo.\n******\nThree.", kind))
        self.assertFalse(follows("Only one answer.", kind))

    def test_repeat_the_prompt(self):
        kind = "combination:repeat_prompt"
        request = "Write a funny haiku about moms, containing keywords \"mom\" and \"mother\" in your response."
        self.assertTrue(follows(request + "\n\nMom, oh mother dear", kind, prompt_to_repeat=request))
        self.assertTrue(follows(request.replace(" ", "  ") + "\nMom", kind, prompt_to_repeat=request))
        self.assertFalse(follows("Sure! " + request + "\nMom", kind, prompt_to_repeat=request))
        self.assertTrue(follows(request + "\nMom", kind, prompt=request))


class StartEnd(unittest.TestCase):
    def test_end_phrase(self):
        kind = "startend:end_checker"
        phrase = "Is there anything else I can help with?"
        self.assertTrue(follows("Here you go.\n" + phrase + "\n", kind, end_phrase=phrase))
        self.assertTrue(follows("HERE YOU GO. " + phrase.upper(), kind, end_phrase=phrase))
        self.assertFalse(follows(phrase + " Have a nice day.", kind, end_phrase=phrase))

    def test_quotation(self):
        kind = "startend:quotation"
        self.assertTrue(follows('"Adam and Eve lived in a garden."\n', kind))
        self.assertTrue(follows("\u201cAdam and Eve lived in a garden.\u201d", kind))
        self.assertFalse(follows('Adam and Eve lived in a "garden".', kind))
        self.assertFalse(follows('"', kind))


class Case(unittest.TestCase):
    def test_capital_words(self):
        kind = "change_case:capital_word_frequency"
        self.assertTrue(follows("This is VERY IMPORTANT for the T-SHIRT team.", kind, capital_relation="at least",
                                capital_frequency=3))
        self.assertTrue(follows("I think I can, and I will.", kind, capital_relation="less than",
                                capital_frequency=1))
        self.assertFalse(follows("NASA and ESA agree.", kind, capital_relation="less than", capital_frequency=2))

    def test_all_capitals_in_english(self):
        kind = "change_case:english_capital"
        self.assertTrue(follows("THE ASTEROID HIT AND THE DINOSAURS WERE GONE. WHAT WOULD HAPPEN NEXT?", kind))
        self.assertFalse(follows("The asteroid hit.", kind))
        self.assertFalse(follows("DER ASTEROID TRAF DIE ERDE UND DIE DINOSAURIER SIND NICHT MEHR DA, "
                                 "WEIL ES SEHR HEISS WAR UND ES KEIN ESSEN GAB.", kind))

    def test_all_lowercase_in_english(self):
        kind = "change_case:english_lowercase"
        self.assertTrue(follows("the forest and the river are where the land meets love.", kind))
        self.assertFalse(follows("The forest is green.", kind))
        self.assertFalse(follows("el perro corre por el parque con sus amigos y está muy feliz porque hace sol "
                                 "todo el día", kind))

    def test_no_commas(self):
        kind = "punctuation:no_comma"
        self.assertTrue(follows("No commas here. None at all.", kind))
        self.assertFalse(follows("One, two.", kind))
        self.assertFalse(follows("\u0647\u0630\u0627\u060c \u0630\u0644\u0643", kind))


SAMPLES = {
    "kn": "\u0cb9\u0ccd\u0caf\u0cbe\u0c82\u0cac\u0cb0\u0ccd\u0c97\u0cb0\u0ccd \u0c92\u0c82\u0ca6\u0cc1 "
          "\u0cb0\u0cc0\u0ca4\u0cbf\u0caf \u0cb8\u0ccd\u0caf\u0cbe\u0c82\u0ca1\u0ccd\u0cb5\u0cbf\u0c9a\u0ccd "
          "\u0c86\u0c97\u0cbf\u0ca6\u0cc6.",
    "pa": "\u0a38\u0a3e\u0a2b\u0a1f\u0a35\u0a47\u0a05\u0a30 \u0a07\u0a70\u0a1c\u0a40\u0a28\u0a40\u0a05\u0a30 "
          "\u0a26\u0a47 \u0a24\u0a15\u0a28\u0a40\u0a15\u0a40 \u0a39\u0a41\u0a28\u0a30 \u0a39\u0a28\u0964",
    "mr": "\u0915\u093e\u092e\u093e\u0935\u0930 \u091c\u093e\u0923\u094d\u092f\u093e\u0938\u093e\u0920\u0940 "
          "\u092e\u0940 \u0930\u094b\u091c \u0927\u093e\u0935\u0924 \u0905\u0938\u0924\u094b. \u0939\u0947 "
          "\u0916\u0942\u092a \u0915\u0920\u0940\u0923 \u0906\u0939\u0947 \u0906\u0923\u093f \u092e\u0932\u093e "
          "\u0909\u0936\u0940\u0930 \u0939\u094b\u0924\u094b.",
    "hi": "\u0938\u094d\u091f\u093e\u0930\u094d\u091f\u0905\u092a \u0915\u0902\u092a\u0928\u093f\u092f\u094b\u0902 "
          "\u0915\u0947 \u0932\u093f\u090f \u0915\u0941\u091b \u0905\u091a\u094d\u091b\u0947 \u0935\u093f\u091a"
          "\u093e\u0930 \u0939\u0948\u0902\u0964 \u092f\u0939 \u090f\u0915 \u0915\u0935\u093f\u0924\u093e \u0939"
          "\u0948\u0964",
    "ne": "\u0935\u093f\u0930\u093e\u091f\u0928\u0917\u0930\u0915\u094b \u092f\u093e\u0924\u094d\u0930\u093e "
          "\u0926\u0938 \u0926\u093f\u0928\u0915\u094b \u0939\u0941\u0928\u094d\u091b\u0964 \u0939\u093e\u092e"
          "\u0940 \u092c\u091c\u093e\u0930 \u0918\u0941\u092e\u094d\u0928 \u091c\u093e\u0928\u094d\u091b\u094c"
          "\u0902 \u0930 \u0916\u093e\u0928\u093e \u092a\u0928\u093f \u0916\u093e\u0928\u094d\u091b\u094c\u0902"
          "\u0964",
    "fa": "\u06cc\u0648\u062f\u0627 \u0628\u0647 \u06cc\u06a9 \u062c\u0647\u0627\u0646 \u062f\u06cc\u06af\u0631 "
          "\u0645\u0646\u062a\u0642\u0644 \u0634\u062f \u0648 \u0686\u06cc\u0632\u0647\u0627\u06cc \u0639\u062c"
          "\u06cc\u0628\u06cc \u062f\u06cc\u062f.",
    "ur": "\u06cc\u06c1 \u06a9\u062a\u0627\u0628 \u06c1\u0648\u0645 \u0627\u0633\u06a9\u0648\u0644\u0646\u06af "
          "\u06a9\u06d2 \u0628\u0627\u0631\u06d2 \u0645\u06cc\u06ba \u06c1\u06d2\u06d4 \u0645\u06cc\u06ba \u0646"
          "\u06d2 \u0627\u0633\u06d2 \u067e\u0691\u06be\u0627\u06d4",
    "ar": "\u062a\u062a\u0637\u0644\u0639 \u0627\u0644\u0634\u0631\u0643\u0629 \u0625\u0644\u0649 \u062a\u0648"
          "\u0633\u064a\u0639 \u0639\u0645\u0644\u064a\u0627\u062a\u0647\u0627 \u0641\u064a \u0623\u0633\u0648"
          "\u0627\u0642 \u062c\u062f\u064a\u062f\u0629.",
    "vi": "Nhi\u1ec7m v\u1ee5 c\u1ee7a m\u1eb9 l\u00e0 nu\u00f4i d\u1ea1y con c\u00e1i, nh\u01b0ng b\u1ed1 "
          "c\u0169ng c\u00f3 th\u1ec3 gi\u00fap \u0111\u1ee1 r\u1ea5t nhi\u1ec1u.",
    "ko": "\uc81c\ubc29\uacfc \ub451\uc758 \ucc28\uc774\uc810\uc740 \ubb34\uc5c7\uc785\ub2c8\uae4c? "
          "\uc81c\ubc29\uc740 \uac15\uc744 \ub530\ub77c \uc313\uc740 \ubcbd\uc785\ub2c8\ub2e4.",
    "sw": "Hii ni hadithi ya kuchekesha kuhusu uchumba. Mtu mmoja alikwenda kwa mpenzi wake na kusema kwamba "
          "yeye ni tajiri sana.",
    "ru": "\u0420\u0435\u0448\u0451\u0442\u043a\u0430 \u044d\u0442\u043e \u0441\u0442\u0440\u0443\u043a\u0442"
          "\u0443\u0440\u0430, \u043a\u043e\u0442\u043e\u0440\u0430\u044f \u043f\u043e\u0432\u0442\u043e\u0440"
          "\u044f\u0435\u0442\u0441\u044f \u0432\u043e \u0432\u0441\u0435\u0445 \u043d\u0430\u043f\u0440\u0430"
          "\u0432\u043b\u0435\u043d\u0438\u044f\u0445. \u041e\u043d\u0430 \u043e\u0447\u0435\u043d\u044c "
          "\u0432\u0430\u0436\u043d\u0430 \u0432 \u043c\u0430\u0442\u0435\u043c\u0430\u0442\u0438\u043a\u0435.",
    "bg": "\u0411\u0440\u043e\u043d\u043a\u043e\u0441 \u0441\u0430 \u0442\u043e\u043b\u043a\u043e\u0432\u0430 "
          "\u0437\u043b\u0435 \u0442\u0430\u0437\u0438 \u0433\u043e\u0434\u0438\u043d\u0430, \u0447\u0435 "
          "\u0442\u0440\u043e\u0444\u0435\u0438\u0442\u0435 \u0438\u043c \u0441\u0435 \u043f\u043e\u043a\u0440"
          "\u0438\u0432\u0430\u0442 \u0441 \u043f\u0440\u0430\u0445. \u0422\u043e\u0432\u0430 \u0435 \u043c"
          "\u043d\u043e\u0433\u043e \u0442\u044a\u0436\u043d\u043e.",
    "pt": "Ad\u00e3o e Eva viviam no jardim do \u00c9den, onde tinham tudo o que precisavam. Eles n\u00e3o "
          "podiam comer o fruto da \u00e1rvore.",
    "gu": "\u0a97\u0ac1\u0a9c\u0ab0\u0abe\u0aa4\u0ac0\u0aa8\u0ac7 \u0aaa\u0ab9\u0ac7\u0ab2\u0ac0 \u0ab5\u0abe\u0ab0 "
          "\u0aae\u0ab3\u0aa4\u0ac0 \u0ab5\u0a96\u0aa4\u0ac7 \u0aa4\u0aae\u0ac7 \u0aaa\u0ac2\u0a9b\u0ac0 \u0ab6"
          "\u0a95\u0acb \u0a9b\u0acb.",
    "te": "\u0c2e\u0c47\u0c2e\u0c41 \u0c38\u0c43\u0c37\u0c4d\u0c1f\u0c3f\u0c15\u0c30\u0c4d\u0c24\u0c32 \u0c2c"
          "\u0c43\u0c02\u0c26\u0c02.",
    "it": "Uno yacht bianco naviga lento sul mare blu, e il vento canta una canzone nella notte.",
    "ta": "\u0b87\u0ba8\u0bcd\u0ba4 \u0b87\u0bb0\u0ba3\u0bcd\u0b9f\u0bc1 \u0ba4\u0bae\u0bbf\u0bb4\u0bcd "
          "\u0ba4\u0bbf\u0bb0\u0bc8\u0baa\u0bcd\u0baa\u0b9f\u0b99\u0bcd\u0b95\u0bb3\u0bc8 \u0baa\u0bbe\u0bb0"
          "\u0bc1\u0b99\u0bcd\u0b95\u0bb3\u0bcd.",
    "de": "Sie machen einen Fehler, wenn Sie unsere K\u00e4seprodukte nicht kaufen, und ich werde Ihnen zeigen, "
          "warum.",
    "fi": "Kun haistat kukkaa, nen\u00e4si tunnistaa sen tuoksun. Se on hyvin mukava tunne ja kaikki "
          "pit\u00e4v\u00e4t siit\u00e4.",
    "th": "\u0e2a\u0e40\u0e1b\u0e23\u0e14\u0e0a\u0e35\u0e15\u0e04\u0e37\u0e2d\u0e2d\u0e30\u0e44\u0e23 "
          "\u0e21\u0e31\u0e19\u0e21\u0e35\u0e41\u0e16\u0e27\u0e41\u0e25\u0e30\u0e04\u0e2d\u0e25\u0e31\u0e21"
          "\u0e19\u0e4c",
    "bn": "\u09e8\u09e6\u09e8\u09e8 \u09b8\u09be\u09b2\u09c7\u09b0 \u0997\u09c1\u0997\u09b2 \u0998\u09cb\u09b7"
          "\u09a3\u09be\u09af\u09bc \u0985\u09a8\u09c7\u0995 \u09a8\u09a4\u09c1\u09a8 \u09aa\u09a3\u09cd\u09af "
          "\u099b\u09bf\u09b2\u0964",
    "ja": "\u6771\u4eac\u306f\u65e5\u672c\u306e\u9996\u90fd\u3067\u3059\u3002\u3068\u3066\u3082\u5927\u304d"
          "\u306a\u90fd\u5e02\u3067\u3001\u305f\u304f\u3055\u3093\u306e\u4eba\u304c\u4f4f\u3093\u3067\u3044"
          "\u307e\u3059\u3002",
    "zh": "\u5317\u4eac\u662f\u4e2d\u56fd\u7684\u9996\u90fd\u3002\u8fd9\u662f\u4e00\u4e2a\u975e\u5e38\u5927"
          "\u7684\u57ce\u5e02\uff0c\u6709\u5f88\u591a\u4eba\u4f4f\u5728\u8fd9\u91cc\u3002",
    "he": "\u05d9\u05e8\u05d5\u05e9\u05dc\u05d9\u05dd \u05d4\u05d9\u05d0 \u05e2\u05d9\u05e8 \u05e2\u05ea\u05d9"
          "\u05e7\u05d4 \u05de\u05d0\u05d5\u05d3.",
    "fr": "Le chat est sur la table et il regarde les oiseaux dans le jardin avec beaucoup d'attention.",
    "es": "El perro corre por el parque con sus amigos y est\u00e1 muy feliz porque hace sol.",
    "en": "The quick brown fox jumps over the lazy dog, and it is happy with what it has done.",
}


class Languages(unittest.TestCase):
    def test_each_language_and_none_of_the_others(self):
        language = "language:response_language"
        for code, text in SAMPLES.items():
            for other in SAMPLES:
                self.assertEqual(follows(text, language, language=other), other == code,
                                 "%s text judged as %s" % (code, other))

    def test_the_mistakes_models_make(self):
        # Asked for Marathi, wrote Hindi; asked for Bulgarian, wrote Russian;
        # asked for Persian, wrote Arabic; asked for anything, wrote English.
        self.assertFalse(score._in_language(SAMPLES["hi"], "mr"))
        self.assertFalse(score._in_language(SAMPLES["ru"], "bg"))
        self.assertFalse(score._in_language(SAMPLES["es"], "pt"))
        self.assertFalse(score._in_language(SAMPLES["ar"], "fa"))
        self.assertFalse(score._in_language(SAMPLES["zh"], "ja"))
        self.assertFalse(score._in_language(SAMPLES["en"], "sw"))

    def test_a_repeated_english_request_then_the_answer(self):
        text = "Write an essay about the economy. Make it in Hindi.\n\n" + " ".join([SAMPLES["hi"]] * 3)
        self.assertTrue(score._in_language(text, "hi"))

    def test_region_codes_and_unknown_codes(self):
        self.assertTrue(score._in_language(SAMPLES["zh"], "zh-CN"))
        self.assertTrue(score._in_language(SAMPLES["pt"], "pt_BR"))
        self.assertFalse(score._in_language(SAMPLES["en"], "xx"))
        self.assertFalse(score._in_language("", "en"))
        self.assertFalse(score._in_language("12345", "en"))


class Instructions(unittest.TestCase):
    def test_several_at_once_with_the_datasets_none_values(self):
        none = {"keywords": None, "relation": None, "num_words": None, "end_phrase": None}
        got = score.ifeval_follows(
            "Hello there. Is there anything else I can help with?",
            ["punctuation:no_comma", "length_constraints:number_words", "startend:end_checker"],
            [dict(none), dict(none, relation="less than", num_words=20),
             dict(none, end_phrase="Is there anything else I can help with?")],
            "Say hello.")
        self.assertEqual(got, [True, True, True])

    def test_unknown_or_malformed_instructions_are_not_followed(self):
        self.assertEqual(score.ifeval_follows("Hi.", ["detectable_format:haiku"], [{}], ""), [False])
        self.assertEqual(score.ifeval_follows("Hi.", ["keywords:frequency"], [{"keyword": "hi"}], ""), [False])
        self.assertEqual(score.ifeval_follows("Hi.", ["length_constraints:number_words"],
                                              [{"relation": "about", "num_words": 3}], ""), [False])
        self.assertEqual(score.ifeval_follows("Hi.", [], [], ""), [])

    def test_an_empty_reply_follows_nothing(self):
        # What a reply cut off before its answer leaves, and true of "no
        # commas" and "lowercase" only because nothing was said.
        for empty in (None, "", "  \n"):
            self.assertEqual(score.ifeval_follows(empty, ["punctuation:no_comma", "change_case:english_lowercase"],
                                                  None, None), [False, False])


class IfevalScore(unittest.TestCase):
    def test_prompt_and_instruction_level(self):
        prompt_level, instruction_level = score.ifeval_score([[True, True], [True, False], [False]])
        self.assertAlmostEqual(prompt_level, 1 / 3)
        self.assertAlmostEqual(instruction_level, 3 / 5)
        self.assertIsInstance(prompt_level, float)

    def test_nothing_scored(self):
        self.assertEqual(score.ifeval_score([]), (0.0, 0.0))
        self.assertEqual(score.ifeval_score([[]]), (0.0, 0.0))


if __name__ == "__main__":
    unittest.main()
