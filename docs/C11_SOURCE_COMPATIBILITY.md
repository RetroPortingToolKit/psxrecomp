# C11 source compatibility

A C11 label must precede a statement. The empty statement after retry_candidates keeps the following local declaration valid without changing control flow.
main.cpp uses fntrace.h as the single owner of fntrace_is_game_started and its C linkage. Redundant block declarations can disagree under GCC 9.

Run `python runtime/tests/test_overlay_retry_c11.py --compiler gcc` and `python runtime/tests/test_fntrace_c_linkage.py`.
The first compiles the actual label/declaration excerpt in strict C11 mode. The second checks declaration ownership.
These focused checks do not replace a full GCC 9 compile/link of the current framework.
