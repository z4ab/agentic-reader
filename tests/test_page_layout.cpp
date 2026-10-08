// M2.2: spec tests for PageLayout (app/src/pdf/PageLayout.h).
//
// Interface choices where the spec is silent:
//   - Global namespace, header app/src/pdf/PageLayout.h, no wx dependency:
//       struct PageSizePts { double w; double h; };          // points
//       struct PageRect { int x; int y; int w; int h; };     // virtual pixels
//       class PageLayout {
//       public:
//           static constexpr int kGap = 12;
//           void compute(const std::vector<PageSizePts>& sizes, double zoom,
//                        int viewportWidth, double dpiScale);
//           int pageCount() const;
//           PageRect pageRect(int page) const;
//           int totalWidth() const;
//           int totalHeight() const;
//           int pageAt(int y) const;
//           std::vector<int> visiblePages(const PageRect& viewRect) const;
//       };
//   - Units: virtual pixels are logical (DIP) pixels. At zoom 1, 1 pt = 1 px.
//     Page size = lround(pts * zoom). dpiScale does not change any rect; it only
//     affects the render scale used by PdfView (spec item 5).
//   - Page indices are 0-based, matching PdfDocument.
//   - Margins: kGap (12 px) above the first page, between pages, and below the
//     last page. totalHeight = sum(page h) + kGap * (n + 1).
//   - Width: totalWidth = max(viewportWidth, widest page w + 2 * kGap).
//     Each page x = (totalWidth - w) / 2 with integer division.
//   - pageAt(y) returns the page whose [y, y + h) contains y, or -1 for a gap,
//     a margin, or outside the layout.
//   - visiblePages returns ascending indices of pages whose rect overlaps
//     viewRect with positive area (rects are half-open, touching edges do not count).
//   - Empty input gives pageCount 0, totalHeight kGap, pageAt -1, no visible pages.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "pdf/PageLayout.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

const std::vector<PageSizePts> kLetter3 = {{612, 792}, {612, 792}, {612, 792}};

PageLayout make(const std::vector<PageSizePts>& sizes, double zoom = 1.0,
                int viewportWidth = 1000, double dpiScale = 1.0)
{
    PageLayout l;
    l.compute(sizes, zoom, viewportWidth, dpiScale);
    return l;
}

} // namespace

TEST_CASE("gap constant is 12px")
{
    CHECK(PageLayout::kGap == 12);
}

TEST_CASE("pages stack vertically with 12px gaps and margins")
{
    auto l = make(kLetter3);
    REQUIRE(l.pageCount() == 3);
    for (int i = 0; i < 3; ++i) {
        auto r = l.pageRect(i);
        CHECK(r.w == 612);
        CHECK(r.h == 792);
        CHECK(r.y == 12 + i * (792 + 12));
    }
    CHECK(l.totalHeight() == 3 * 792 + 4 * 12);
}

TEST_CASE("zoom scales page sizes and keeps gaps fixed")
{
    auto l = make(kLetter3, 2.0, 1000);
    auto r1 = l.pageRect(1);
    CHECK(l.pageRect(0).h == 1584);
    CHECK(r1.w == 1224);
    CHECK(r1.y == 12 + 1584 + 12);
    CHECK(l.totalHeight() == 3 * 1584 + 4 * 12);
}

TEST_CASE("mixed page heights accumulate")
{
    auto l = make({{100, 50}, {200, 300}, {100, 70}});
    CHECK(l.pageRect(0).y == 12);
    CHECK(l.pageRect(1).y == 12 + 50 + 12);
    CHECK(l.pageRect(2).y == 12 + 50 + 12 + 300 + 12);
    CHECK(l.totalHeight() == 50 + 300 + 70 + 4 * 12);
}

TEST_CASE("pages are centered horizontally in a wide viewport")
{
    auto l = make(kLetter3, 1.0, 1000);
    CHECK(l.totalWidth() == 1000);
    for (int i = 0; i < 3; ++i)
        CHECK(l.pageRect(i).x == (1000 - 612) / 2);
}

TEST_CASE("narrow pages center against the viewport, not the widest page")
{
    auto l = make({{400, 100}, {200, 100}}, 1.0, 800);
    CHECK(l.pageRect(0).x == 200);
    CHECK(l.pageRect(1).x == 300);
}

TEST_CASE("viewport narrower than the page widens the layout")
{
    auto l = make(kLetter3, 1.0, 300);
    CHECK(l.totalWidth() == 612 + 2 * 12);
    CHECK(l.pageRect(0).x == 12);
}

TEST_CASE("DPI scale does not change layout rects")
{
    auto a = make(kLetter3, 1.5, 1000, 1.0);
    auto b = make(kLetter3, 1.5, 1000, 2.0);
    CHECK(a.totalWidth() == b.totalWidth());
    CHECK(a.totalHeight() == b.totalHeight());
    for (int i = 0; i < 3; ++i) {
        CHECK(a.pageRect(i).x == b.pageRect(i).x);
        CHECK(a.pageRect(i).y == b.pageRect(i).y);
        CHECK(a.pageRect(i).w == b.pageRect(i).w);
        CHECK(a.pageRect(i).h == b.pageRect(i).h);
    }
}

TEST_CASE("pageAt maps y to pages and gaps")
{
    auto l = make(kLetter3);
    const int p1 = 12 + 792 + 12;
    CHECK(l.pageAt(-5) == -1);
    CHECK(l.pageAt(0) == -1);
    CHECK(l.pageAt(11) == -1);
    CHECK(l.pageAt(12) == 0);
    CHECK(l.pageAt(12 + 791) == 0);
    CHECK(l.pageAt(12 + 792) == -1);
    CHECK(l.pageAt(p1 - 1) == -1);
    CHECK(l.pageAt(p1) == 1);
    CHECK(l.pageAt(l.pageRect(2).y + 400) == 2);
    CHECK(l.pageAt(l.totalHeight() - 1) == -1);
    CHECK(l.pageAt(l.totalHeight() + 100) == -1);
}

TEST_CASE("visiblePages returns overlapping pages in order")
{
    auto l = make(kLetter3);
    const int w = l.totalWidth();

    CHECK(l.visiblePages({0, 0, w, 500}) == std::vector<int>{0});
    CHECK(l.visiblePages({0, 700, w, 300}) == std::vector<int>{0, 1});
    CHECK(l.visiblePages({0, 0, w, l.totalHeight()}) == std::vector<int>{0, 1, 2});
    CHECK(l.visiblePages({0, l.pageRect(2).y + 10, w, 100}) == std::vector<int>{2});
}

TEST_CASE("visiblePages excludes gaps and edge-touching pages")
{
    auto l = make(kLetter3);
    const int w = l.totalWidth();
    const int gapTop = 12 + 792;
    // Exactly the gap between page 0 and page 1.
    CHECK(l.visiblePages({0, gapTop, w, 12}).empty());
    // Ends exactly where page 0 starts.
    CHECK(l.visiblePages({0, 0, w, 12}).empty());
    // Below the layout.
    CHECK(l.visiblePages({0, l.totalHeight() + 10, w, 100}).empty());
}

TEST_CASE("empty document")
{
    auto l = make({});
    CHECK(l.pageCount() == 0);
    CHECK(l.totalHeight() == 12);
    CHECK(l.pageAt(0) == -1);
    CHECK(l.visiblePages({0, 0, 1000, 1000}).empty());
}
