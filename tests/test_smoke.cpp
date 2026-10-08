// Smoke test for the doctest + ctest harness (M1.1 spec step 6).
// Each test executable defines its own main through doctest.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("test harness runs")
{
    CHECK(1 + 1 == 2);
}
