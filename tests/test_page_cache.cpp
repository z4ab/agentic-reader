// M2.3: spec tests for PageCache (app/src/pdf/PageCache.h).
//
// Interface choices where the spec is silent:
//   - Global namespace, header app/src/pdf/PageCache.h:
//       class PageCache {
//       public:
//           static constexpr std::size_t kDefaultBudgetBytes = 256 * 1024 * 1024;
//           explicit PageCache(std::size_t budgetBytes = kDefaultBudgetBytes);
//           void put(int page, double scale, const wxBitmap& bitmap, std::size_t bytes);
//           const wxBitmap* get(int page, double scale);
//           bool contains(int page, double scale) const;
//           void clear();
//           std::size_t count() const;   // number of entries
//           std::size_t bytes() const;   // sum of entry sizes
//           std::size_t budget() const;
//       };
//   - The caller passes each entry's size in bytes to put(). PdfView passes the
//     bitmap's pixel size (width * height * 4). The cache never measures the
//     bitmap, so tests use fake sizes with invalid (default) wxBitmaps.
//   - The key is (page, scale) with exact double equality. The same page at two
//     scales is two entries.
//   - get() returns nullptr on a miss. On a hit it returns a pointer to the
//     stored bitmap, valid until the next put() or clear(), and marks the entry
//     most recently used. contains() does not change recency.
//   - put() on an existing key replaces its bitmap and size and marks it most
//     recently used.
//   - After put(), least recently used entries are evicted while bytes() > budget().
//     An entry is never evicted by its own put(): an entry larger than the budget
//     stays as the only entry. bytes() == budget() evicts nothing.
//   - clear() removes every entry and keeps the budget.
//   - The tests use no wxApp. Default-constructed wxBitmap needs none.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PageCache.h"

#include <wx/bitmap.h>

#include <cstddef>

namespace {

const wxBitmap kBmp;

} // namespace

TEST_CASE("default budget is 256 MB")
{
    CHECK(PageCache::kDefaultBudgetBytes == std::size_t{256} * 1024 * 1024);
    PageCache cache;
    CHECK(cache.budget() == std::size_t{256} * 1024 * 1024);
}

TEST_CASE("custom budget is kept")
{
    PageCache cache(1000);
    CHECK(cache.budget() == 1000);
}

TEST_CASE("empty cache misses")
{
    PageCache cache(100);
    CHECK(cache.count() == 0);
    CHECK(cache.bytes() == 0);
    CHECK(cache.get(0, 1.0) == nullptr);
    CHECK_FALSE(cache.contains(0, 1.0));
}

TEST_CASE("put then get hits and sums bytes")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 10);
    cache.put(1, 1.0, kBmp, 20);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 30);
    CHECK(cache.get(0, 1.0) != nullptr);
    CHECK(cache.get(1, 1.0) != nullptr);
    CHECK(cache.get(2, 1.0) == nullptr);
    CHECK(cache.contains(0, 1.0));
    CHECK(cache.contains(1, 1.0));
}

TEST_CASE("key includes scale")
{
    PageCache cache(100);
    cache.put(3, 1.0, kBmp, 10);
    CHECK(cache.contains(3, 1.0));
    CHECK_FALSE(cache.contains(3, 2.0));
    CHECK(cache.get(3, 2.0) == nullptr);

    cache.put(3, 2.0, kBmp, 40);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 50);
    CHECK(cache.contains(3, 1.0));
    CHECK(cache.contains(3, 2.0));
}

TEST_CASE("over budget evicts the least recently used entry")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    cache.put(2, 1.0, kBmp, 40);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 80);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK(cache.contains(1, 1.0));
    CHECK(cache.contains(2, 1.0));
}

TEST_CASE("exactly at budget evicts nothing")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 50);
    cache.put(1, 1.0, kBmp, 50);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 100);

    cache.put(2, 1.0, kBmp, 1);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 51);
    CHECK_FALSE(cache.contains(0, 1.0));
}

TEST_CASE("get marks an entry most recently used")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    REQUIRE(cache.get(0, 1.0) != nullptr);
    cache.put(2, 1.0, kBmp, 40);
    CHECK(cache.contains(0, 1.0));
    CHECK_FALSE(cache.contains(1, 1.0));
    CHECK(cache.contains(2, 1.0));
}

TEST_CASE("contains does not change recency")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    REQUIRE(cache.contains(0, 1.0));
    cache.put(2, 1.0, kBmp, 40);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK(cache.contains(1, 1.0));
    CHECK(cache.contains(2, 1.0));
}

TEST_CASE("a miss does not change recency")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    CHECK(cache.get(0, 2.0) == nullptr);
    cache.put(2, 1.0, kBmp, 40);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK(cache.contains(1, 1.0));
}

TEST_CASE("one put can evict several entries, oldest first")
{
    PageCache cache(100);
    for (int page = 0; page < 4; ++page)
        cache.put(page, 1.0, kBmp, 25);
    REQUIRE(cache.bytes() == 100);

    cache.put(10, 1.0, kBmp, 60);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 85);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK_FALSE(cache.contains(1, 1.0));
    CHECK_FALSE(cache.contains(2, 1.0));
    CHECK(cache.contains(3, 1.0));
    CHECK(cache.contains(10, 1.0));
}

TEST_CASE("an entry larger than the budget stays as the only entry")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 30);
    cache.put(1, 1.0, kBmp, 30);
    cache.put(2, 8.0, kBmp, 500);
    CHECK(cache.count() == 1);
    CHECK(cache.bytes() == 500);
    CHECK(cache.get(2, 8.0) != nullptr);

    cache.put(3, 1.0, kBmp, 10);
    CHECK(cache.count() == 1);
    CHECK(cache.bytes() == 10);
    CHECK_FALSE(cache.contains(2, 8.0));
    CHECK(cache.contains(3, 1.0));
}

TEST_CASE("put on an existing key replaces size and does not duplicate")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    cache.put(0, 1.0, kBmp, 10);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 50);
}

TEST_CASE("put on an existing key marks it most recently used")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 1.0, kBmp, 40);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(2, 1.0, kBmp, 40);
    CHECK(cache.contains(0, 1.0));
    CHECK_FALSE(cache.contains(1, 1.0));
    CHECK(cache.contains(2, 1.0));
}

TEST_CASE("growing an existing entry evicts others but not itself")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 30);
    cache.put(1, 1.0, kBmp, 30);
    cache.put(2, 1.0, kBmp, 30);
    cache.put(1, 1.0, kBmp, 70);
    CHECK(cache.count() == 2);
    CHECK(cache.bytes() == 100);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK(cache.contains(1, 1.0));
    CHECK(cache.contains(2, 1.0));
}

TEST_CASE("clear empties the cache and keeps the budget")
{
    PageCache cache(100);
    cache.put(0, 1.0, kBmp, 40);
    cache.put(1, 2.0, kBmp, 40);
    cache.clear();
    CHECK(cache.count() == 0);
    CHECK(cache.bytes() == 0);
    CHECK(cache.budget() == 100);
    CHECK_FALSE(cache.contains(0, 1.0));
    CHECK(cache.get(1, 2.0) == nullptr);

    cache.put(5, 1.0, kBmp, 100);
    CHECK(cache.count() == 1);
    CHECK(cache.bytes() == 100);
}

TEST_CASE("scrolling 520 letter pages keeps the most recent pages within budget")
{
    // A 612 x 792 pt page at scale 1 is 612 * 792 * 4 bytes, about 1.85 MB,
    // so the default 256 MB budget holds 138 such pages.
    constexpr std::size_t kPageBytes = std::size_t{612} * 792 * 4;
    constexpr int kPages = 520;
    const std::size_t fit = PageCache::kDefaultBudgetBytes / kPageBytes;
    REQUIRE(fit == 138);

    PageCache cache;
    for (int page = 0; page < kPages; ++page)
        cache.put(page, 1.0, kBmp, kPageBytes);

    CHECK(cache.count() == fit);
    CHECK(cache.bytes() == fit * kPageBytes);
    CHECK(cache.bytes() <= cache.budget());
    for (int page = 0; page < kPages; ++page) {
        const bool kept = page >= kPages - static_cast<int>(fit);
        CHECK(cache.contains(page, 1.0) == kept);
    }
}
