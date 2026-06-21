#include "furniture.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* catalog: two kinds, names correct, cycling wraps both ways */
    {
        CHECK(furniture_catalog_count() == 2);
        CHECK(strcmp(furniture_catalog_name(0), "table") == 0);
        CHECK(strcmp(furniture_catalog_name(1), "bookshelf") == 0);
        CHECK(furniture_catalog_name(2) == (const char *)0);   /* out of range */
        CHECK(furniture_catalog_cycle(0,  1) == 1);
        CHECK(furniture_catalog_cycle(1,  1) == 0);            /* wrap forward */
        CHECK(furniture_catalog_cycle(0, -1) == 1);            /* wrap back */
    }
    /* kind predicates */
    {
        CHECK(furniture_is_table("table"));
        CHECK(!furniture_is_table("bookshelf"));
        CHECK(furniture_is_shelf("bookshelf"));
        CHECK(!furniture_is_shelf("table"));
        CHECK(!furniture_is_table((const char *)0));
        CHECK(!furniture_is_shelf("card"));
    }
    if (fails == 0) printf("furniture_test: OK\n");
    return fails ? 1 : 0;
}
