"""
syntax_error_test.py — Verify syntactic parse error handling (US3 / T047).

This file contains deliberate invalid syntax. It should fail at compile/parse time
inside mp_embed_exec_str. Expected: firmware shows an error message and returns to
menu without crash or hard reset.
"""
def foo(:
