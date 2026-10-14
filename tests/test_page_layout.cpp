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
//
// M10.1: SinglePage mode, and the ViewNav fit helpers on a 16:9 page. Interface choices:
//   - PageLayout.h gains, in the global namespace:
//       enum class LayoutMode { Continuous, SinglePage };
//       void compute(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth,
//                    double dpiScale, LayoutMode mode = LayoutMode::Continuous,
//                    int shownPage = 0, int viewportHeight = 0);
//     The existing four-argument calls still compile and keep their output, so no
//     existing test was changed.
//   - Continuous ignores shownPage and viewportHeight.
//   - SinglePage: pageCount() is the number of pages passed in. Every page except
//     shownPage has pageRect {0, 0, 0, 0}. The shown page's origin is
//     x = (totalWidth - w) / 2 and y = (totalHeight - h) / 2 (integer division).
//   - totalWidth = max(viewportWidth, shown w + 2 * kGap).
//     totalHeight = max(viewportHeight, shown h + 2 * kGap). viewportHeight 0 means
//     the page plus margins.
//   - pageAt, visiblePages and viewToPage consider only the shown page.
//   - pageToView on a page that is not shown returns {0, 0, 0, 0}, as for an invalid page.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "pdf/PageLayout.h"
#include "pdf/ViewNav.h"

#include <doctest/doctest.h>

#include <cmath>
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

namespace {

const PageSizePts kLetterPage{612, 792};
const PageSizePts kSlidePage{960, 540};

PageLayout makeSingle(const std::vector<PageSizePts>& sizes, int shown, double zoom, int vw,
                      int vh)
{
    PageLayout l;
    l.compute(sizes, zoom, vw, 1.0, LayoutMode::SinglePage, shown, vh);
    return l;
}

bool sameRect(PageRect a, PageRect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

} // namespace

TEST_CASE("SinglePage gives a rect only to the shown page")
{
    auto l = makeSingle(kLetter3, 1, 1.0, 1000, 800);
    REQUIRE(l.pageCount() == 3);
    CHECK(sameRect(l.pageRect(0), {0, 0, 0, 0}));
    CHECK(sameRect(l.pageRect(2), {0, 0, 0, 0}));
    CHECK(l.pageRect(1).w == 612);
    CHECK(l.pageRect(1).h == 792);
}

TEST_CASE("SinglePage centres the shown page on both axes")
{
    auto l = makeSingle(kLetter3, 0, 1.0, 1000, 1000);
    CHECK(l.totalWidth() == 1000);
    CHECK(l.totalHeight() == 1000);
    CHECK(sameRect(l.pageRect(0), {194, 104, 612, 792}));

    const std::vector<PageSizePts> mixed = {kLetterPage, kLetterPage, kSlidePage};
    auto m = makeSingle(mixed, 2, 1.0, 1200, 800);
    CHECK(m.totalWidth() == 1200);
    CHECK(m.totalHeight() == 800);
    CHECK(sameRect(m.pageRect(2), {120, 130, 960, 540}));
}

TEST_CASE("SinglePage total size is page plus margins when the viewport is smaller")
{
    auto l = makeSingle(kLetter3, 0, 1.0, 300, 400);
    CHECK(l.totalWidth() == 612 + 2 * 12);
    CHECK(l.totalHeight() == 792 + 2 * 12);
    CHECK(sameRect(l.pageRect(0), {12, 12, 612, 792}));
}

TEST_CASE("SinglePage total size takes the viewport where it is larger than the page")
{
    auto l = makeSingle(kLetter3, 2, 1.0, 1000, 400);
    CHECK(l.totalWidth() == 1000);
    CHECK(l.totalHeight() == 792 + 2 * 12);
    CHECK(sameRect(l.pageRect(2), {194, 12, 612, 792}));
}

TEST_CASE("SinglePage zoom scales the shown page and centres it")
{
    auto l = makeSingle(kLetter3, 1, 0.5, 1000, 1000);
    CHECK(sameRect(l.pageRect(1), {347, 302, 306, 396}));
    CHECK(sameRect(l.pageRect(0), {0, 0, 0, 0}));
}

TEST_CASE("SinglePage total width uses the shown page, not the widest page")
{
    const std::vector<PageSizePts> sizes = {{2000, 100}, kLetterPage};
    auto l = makeSingle(sizes, 1, 1.0, 1000, 1000);
    CHECK(l.totalWidth() == 1000);
    CHECK(l.totalHeight() == 1000);
    CHECK(sameRect(l.pageRect(1), {194, 104, 612, 792}));
}

TEST_CASE("SinglePage pageAt returns the shown page only")
{
    // Shown page 1 spans y 104..895.
    auto l = makeSingle(kLetter3, 1, 1.0, 1000, 1000);
    CHECK(l.pageAt(0) == -1);
    CHECK(l.pageAt(50) == -1);
    CHECK(l.pageAt(103) == -1);
    CHECK(l.pageAt(104) == 1);
    CHECK(l.pageAt(700) == 1);
    CHECK(l.pageAt(895) == 1);
    CHECK(l.pageAt(896) == -1);
    CHECK(l.pageAt(l.totalHeight() - 1) == -1);
}

TEST_CASE("SinglePage pageAt is -1 in the margins of an overflowing page")
{
    // Shown page 0 spans y 12..803 in a 400px viewport; the total height is 816.
    auto l = makeSingle(kLetter3, 0, 1.0, 1000, 400);
    CHECK(l.pageAt(11) == -1);
    CHECK(l.pageAt(12) == 0);
    CHECK(l.pageAt(803) == 0);
    CHECK(l.pageAt(804) == -1);
    CHECK(l.pageAt(815) == -1);
}

TEST_CASE("SinglePage visiblePages returns only the shown page")
{
    // Shown page 1 covers x 194..805 and y 104..895.
    auto l = makeSingle(kLetter3, 1, 1.0, 1000, 1000);
    CHECK(l.visiblePages({0, 0, 1000, 1000}) == std::vector<int>{1});
    CHECK(l.visiblePages({300, 103, 100, 2}) == std::vector<int>{1});
    // Edge-touching views do not count.
    CHECK(l.visiblePages({0, 0, 1000, 104}).empty());
    CHECK(l.visiblePages({0, 896, 1000, 100}).empty());
    CHECK(l.visiblePages({0, 500, 194, 100}).empty());
}

TEST_CASE("SinglePage pageToView maps the shown page and gives no rect to other pages")
{
    // Shown page 0 is at (194, 104).
    auto l = makeSingle(kLetter3, 0, 1.0, 1000, 1000);
    CHECK(sameRect(l.pageToView(0, {0, 0, 612, 792}), {194, 104, 612, 792}));
    CHECK(sameRect(l.pageToView(0, {100, 200, 50, 60}), {294, 304, 50, 60}));
    CHECK(sameRect(l.pageToView(1, {0, 0, 10, 10}), {0, 0, 0, 0}));
}

TEST_CASE("SinglePage viewToPage returns the shown page inside its rect and -1 outside")
{
    auto l = makeSingle(kLetter3, 0, 1.0, 1000, 1000);
    PagePoint p = l.viewToPage({194, 104});
    CHECK(p.page == 0);
    CHECK(p.pt.x == doctest::Approx(0.0));
    CHECK(p.pt.y == doctest::Approx(0.0));

    p = l.viewToPage({494, 504});
    CHECK(p.page == 0);
    CHECK(p.pt.x == doctest::Approx(300.0));
    CHECK(p.pt.y == doctest::Approx(400.0));

    CHECK(l.viewToPage({805, 895}).page == 0);
    CHECK(l.viewToPage({193, 104}).page == -1);
    CHECK(l.viewToPage({194, 103}).page == -1);
    CHECK(l.viewToPage({806, 500}).page == -1);
    CHECK(l.viewToPage({500, 896}).page == -1);
}

TEST_CASE("SinglePage pageToView and viewToPage round trip")
{
    // Zoom 1.5: shown page 1 is 918x1188, at (41, 12) in a 1000x1212 layout.
    auto l = makeSingle(kLetter3, 1, 1.5, 1000, 1000);
    REQUIRE(sameRect(l.pageRect(1), {41, 12, 918, 1188}));

    const double tol = 1.0 / 1.5;
    const double pts[][2] = {{0, 0}, {100, 200}, {33.3, 77.7}, {611, 791}};
    for (const auto& pt : pts) {
        const PageRect v = l.pageToView(1, {pt[0], pt[1], 0, 0});
        const PagePoint back = l.viewToPage({v.x, v.y});
        CHECK(back.page == 1);
        CHECK(std::fabs(back.pt.x - pt[0]) <= tol);
        CHECK(std::fabs(back.pt.y - pt[1]) <= tol);
    }
}

TEST_CASE("Continuous ignores shownPage and viewportHeight")
{
    PageLayout base;
    base.compute(kLetter3, 1.3, 1000, 1.0);
    PageLayout other;
    other.compute(kLetter3, 1.3, 1000, 1.0, LayoutMode::Continuous, 2, 5000);
    REQUIRE(base.pageCount() == other.pageCount());
    CHECK(base.totalWidth() == other.totalWidth());
    CHECK(base.totalHeight() == other.totalHeight());
    for (int i = 0; i < base.pageCount(); ++i)
        CHECK(sameRect(base.pageRect(i), other.pageRect(i)));
    CHECK(base.pageRect(0).w > 0);
}

TEST_CASE("SinglePage with no pages has no pages to hit")
{
    auto l = makeSingle({}, 0, 1.0, 1000, 1000);
    CHECK(l.pageCount() == 0);
    CHECK(l.pageAt(0) == -1);
    CHECK(l.visiblePages({0, 0, 1000, 1000}).empty());
}

TEST_CASE("fit helpers fit a 16:9 page to width and height")
{
    // Viewport 1000 x 600 leaves 976 x 576 after the side and top/bottom margins.
    CHECK(fitWidthZoom(kSlidePage, 1000) == doctest::Approx(976.0 / 960.0));
    CHECK(fitPageZoom(kSlidePage, 1000, 600) == doctest::Approx(976.0 / 960.0));
    // Height limits at 400px: (400 - 24) / 540.
    CHECK(fitPageZoom(kSlidePage, 1000, 400) == doctest::Approx(376.0 / 540.0));
}
