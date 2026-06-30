/* diskpath_test.c — headless unit tests for the pure path module. C89. */
#include "diskpath.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void t_parent(void) {
    char b[256];
    diskpath_parent("/Users/fran/Documents", b, (int)sizeof b); assert(strcmp(b, "/Users/fran") == 0);
    diskpath_parent("/a/b", b, (int)sizeof b);                  assert(strcmp(b, "/a") == 0);
    diskpath_parent("/a",   b, (int)sizeof b);                  assert(strcmp(b, "/") == 0);
    diskpath_parent("/a/b/",b, (int)sizeof b);                  assert(strcmp(b, "/a") == 0);
    diskpath_parent("/",    b, (int)sizeof b);                  assert(strcmp(b, "/") == 0);
    diskpath_parent("",     b, (int)sizeof b);                  assert(b[0] == '\0');
    diskpath_parent("/aaaa/b", b, 4);  assert(strcmp(b, "/aa") == 0);  /* clamp: parent "/aaaa" truncates to "/aa" */
}
static void t_edge(void) {
    char b[256];
    /* NULL args absorbed */
    assert(!diskpath_is_root((const char *)0));
    diskpath_join((const char *)0, (const char *)0, b, (int)sizeof b); assert(strcmp(b, "/") == 0);
    /* degenerate cap: cap==1 yields just the NUL */
    diskpath_parent("/a/b", b, 1); assert(b[0] == '\0');
    diskpath_join("/a", "b", b, 1); assert(b[0] == '\0');
}
static void t_join(void) {
    char b[256];
    diskpath_join("/Users/fran", "Documents", b, (int)sizeof b); assert(strcmp(b, "/Users/fran/Documents") == 0);
    diskpath_join("/",  "Users", b, (int)sizeof b);              assert(strcmp(b, "/Users") == 0);
    diskpath_join("/a/","b",     b, (int)sizeof b);              assert(strcmp(b, "/a/b") == 0);
    diskpath_join("/aaaa", "bbbb", b, 6);                        assert(strlen(b) <= 5); /* clamp */
}
static void t_basename(void) {
    assert(strcmp(diskpath_basename("/a/b"), "b") == 0);
    assert(strcmp(diskpath_basename("/a/b/c.txt"), "c.txt") == 0);
    assert(strcmp(diskpath_basename("foo"), "foo") == 0);
    assert(strcmp(diskpath_basename("/"), "") == 0);
}
static void t_isroot(void) {
    assert(diskpath_is_root("/"));
    assert(!diskpath_is_root("/a"));
    assert(!diskpath_is_root(""));
}
int main(void) {
    t_parent(); t_join(); t_basename(); t_isroot(); t_edge();
    printf("diskpath: all tests passed\n");
    return 0;
}
