/* fuzzy_test.c — exercises the command-palette fuzzy matcher: subsequence
   correctness, case-insensitivity, ranking (boundary/contiguous beat scattered),
   empty-query-matches-all, no-match, and position reporting. Built by
   `build.sh fuzzytest` with ASan/UBSan. */
#include "fuzzy.h"

#include <stdio.h>

static int failures = 0;

static void check(int cond, const char *msg) {
    if (cond) {
        printf("ok: %s\n", msg);
    } else {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

int main(void) {
    int score_a, score_b, pos[8];

    check(fuzzy_match("bl", "Toggle bloom", NULL, NULL, 0) == SOL_TRUE,
          "\"bl\" matches \"Toggle bloom\"");

    check(fuzzy_match("BLOOM", "Toggle bloom", NULL, NULL, 0) == SOL_TRUE,
          "\"BLOOM\" matches case-insensitively");

    check(fuzzy_match("mb", "Toggle bloom", NULL, NULL, 0) == SOL_FALSE,
          "\"mb\" does not match (wrong order)");

    check(fuzzy_match("xyz", "Toggle bloom", NULL, NULL, 0) == SOL_FALSE,
          "\"xyz\" does not match");

    check(fuzzy_match("", "anything", &score_a, NULL, 0) == SOL_TRUE && score_a == 0,
          "empty query matches with score 0");

    /* word-boundary hit outranks a mid-word hit */
    fuzzy_match("m", "Toggle mist", &score_a, NULL, 0);  /* 'm' after a space */
    fuzzy_match("m", "Storm",       &score_b, NULL, 0);  /* 'm' mid-word */
    check(score_a > score_b, "word-boundary \"m\" outranks mid-word \"m\"");

    /* contiguous run outranks a scattered match */
    fuzzy_match("bc", "aabcd", &score_a, NULL, 0);  /* b,c adjacent */
    fuzzy_match("bc", "abxcd", &score_b, NULL, 0);  /* b,c with a gap */
    check(score_a > score_b, "contiguous \"bc\" outranks scattered \"bc\"");

    {
        sol_bool m = fuzzy_match("bl", "Toggle bloom", NULL, pos, 8);
        check(m == SOL_TRUE && pos[0] == 7 && pos[1] == 8,
              "positions of \"bl\" in \"Toggle bloom\" are 7,8");
    }

    /* first-char hit outranks a mid-word hit */
    fuzzy_match("t", "toggle", &score_a, NULL, 0);   /* 't' at position 0 */
    fuzzy_match("t", "set",    &score_b, NULL, 0);   /* 't' at position 2 */
    check(score_a > score_b, "first-char hit outranks mid-word hit");

    check(fuzzy_match(NULL, "x", NULL, NULL, 0) == SOL_FALSE, "NULL query => no match");
    check(fuzzy_match("x", NULL, NULL, NULL, 0) == SOL_FALSE, "NULL cand => no match");

    if (failures == 0) {
        printf("fuzzy_test: OK\n");
        return 0;
    }
    printf("fuzzy_test: %d FAILURE(S)\n", failures);
    return 1;
}
