/* browser_test.c — pure-logic test for browser.c. GL-free; build.sh browsertest. */
#include "browser.h"
#include <string.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){ printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,m); fails++; } } while(0)

static void test_focus_clamps(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    CHECK(b.focus==0, "starts on Types");
    browser_key(&b, BROWSER_KEY_LEFT, counts);   CHECK(b.focus==0, "left clamps at 0");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==1, "right -> Entities");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==2, "right -> Commands");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==2, "right clamps at last col");
}

static void test_select_moves(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==1, "down");
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==2, "down");
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==2, "down clamps at count-1");
    browser_key(&b, BROWSER_KEY_UP, counts);
    browser_key(&b, BROWSER_KEY_UP, counts);
    browser_key(&b, BROWSER_KEY_UP, counts);     CHECK(b.sel[1]==0, "up clamps at 0");
}

static void test_empty_column(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=0; counts[2]=0;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==0, "down on empty col stays 0");
}

static void test_enter_descends_then_activates(void) {
    Browser b; int counts[3]; BrowserAction a;
    counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_NONE && b.focus==1, "enter Types->Entities");
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_NONE && b.focus==2, "enter Entities->Commands");
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_ACTIVATE, "enter on Commands activates");
}

static void test_cancel(void) {
    Browser b; int counts[3]; counts[0]=1; counts[1]=1; counts[2]=1;
    browser_reset(&b);
    CHECK(browser_key(&b, BROWSER_KEY_CANCEL, counts)==BROWSER_CLOSE, "esc closes in nav");
}

static void test_filter_mode(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);    /* focus Entities */
    browser_char(&b, 'a');
    CHECK(b.flen[1]==0, "letters don't filter in nav mode");
    browser_key(&b, BROWSER_KEY_FILTER, counts);   /* '/' enters filter mode */
    CHECK(b.filtering, "slash enters filter mode");
    browser_char(&b, 'a'); browser_char(&b, 'b');
    CHECK(b.flen[1]==2 && strcmp(b.filter[1],"ab")==0, "chars append while filtering");
    browser_key(&b, BROWSER_KEY_BACKSPACE, counts);
    CHECK(b.flen[1]==1 && strcmp(b.filter[1],"a")==0, "backspace trims");
    CHECK(browser_key(&b, BROWSER_KEY_ENTER, counts)==BROWSER_NONE && !b.filtering
          && strcmp(b.filter[1],"a")==0, "enter commits filter + leaves nav");
    browser_key(&b, BROWSER_KEY_FILTER, counts);
    browser_char(&b, 'x');
    CHECK(browser_key(&b, BROWSER_KEY_CANCEL, counts)==BROWSER_NONE && !b.filtering
          && b.flen[1]==0, "esc cancels filter (does NOT close) + clears");
    browser_key(&b, BROWSER_KEY_FILTER, counts);   /* re-enter filter on Entities */
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==1, "down moves sel while filtering");
    browser_key(&b, BROWSER_KEY_UP, counts);     CHECK(b.sel[1]==0, "up moves sel while filtering");
    browser_key(&b, BROWSER_KEY_CANCEL, counts); /* leave filter */
}

static void test_clamp_after_shrink(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=5; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);
    b.sel[1] = 4;
    counts[1] = 2;
    browser_clamp(&b, counts);
    CHECK(b.sel[1]==1, "selection clamps after shrink");
}

static void test_rank(void) {
    const char *names[3]; int out[3], n;
    names[0]="sunset"; names[1]="map-clip"; names[2]="paper";
    n = browser_rank("", names, 3, out, 3);          CHECK(n==3, "empty filter = all");
    n = browser_rank("map", names, 3, out, 3);       CHECK(n==1 && out[0]==1, "filter narrows + indexes");
    n = browser_rank("zzz", names, 3, out, 3);       CHECK(n==0, "no match");
}

static void test_filter_enter_last_col(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=2;
    browser_reset(&b);
    b.focus = 2;                                   /* Commands column */
    browser_key(&b, BROWSER_KEY_FILTER, counts);   /* filter mode on the last col */
    CHECK(browser_key(&b, BROWSER_KEY_ENTER, counts)==BROWSER_NONE && !b.filtering,
          "enter-in-filter on last col commits (no activate)");
}

int main(void) {
    test_focus_clamps(); test_select_moves(); test_empty_column();
    test_enter_descends_then_activates(); test_cancel();
    test_filter_mode(); test_clamp_after_shrink(); test_rank();
    test_filter_enter_last_col();
    if (fails==0) printf("browser_test: all passed\n");
    return fails ? 1 : 0;
}
