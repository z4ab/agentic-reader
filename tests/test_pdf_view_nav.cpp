// M2.4: spec tests for the window-free zoom and navigation math behind PdfView.
//
// Interface choices where the spec is silent:
//   - Header app/src/pdf/ViewNav.h, global namespace, no wx dependency, builds into
//     reader_core. It uses PageSizePts, PageRect and PageLayout from pdf/PageLayout.h:
//       constexpr double kMinZoom = 0.25;   // 25%
//       constexpr double kMaxZoom = 8.0;    // 800%
//       constexpr double kZoomStep = 0.10;  // one Ctrl+wheel notch or Ctrl+`+`/`-` press
//       double clampZoom(double zoom);
//       double stepZoom(double zoom, int steps);
//       double fitWidthZoom(const PageSizePts& page, int viewportWidth);
//       double fitPageZoom(const PageSizePts& page, int viewportWidth, int viewportHeight);
//       int mostVisiblePage(const PageLayout& layout, const PageRect& viewRect);
//       struct ViewPoint { int x; int y; };
//       ViewPoint zoomScrollOrigin(const PageLayout& before, const PageLayout& after,
//                                  ViewPoint origin, ViewPoint anchor);
//   - Zoom is a scale factor (1.0 = 100%, 1 pt = 1 DIP), as in PageLayout::compute.
//   - clampZoom limits to [kMinZoom, kMaxZoom].
//   - stepZoom adds steps * kZoomStep (10 percentage points per step, no snapping),
//     then clamps. 100% -> 110% -> 120%. Negative steps zoom out.
//   - Viewport sizes are the PdfView client size in DIPs. Fit modes leave the
//     PageLayout::kGap margin on both sides:
//       fitWidthZoom = clampZoom((viewportWidth - 2 * kGap) / page.w)
//       fitPageZoom  = clampZoom(min((viewportWidth - 2 * kGap) / page.w,
//                                    (viewportHeight - 2 * kGap) / page.h))
//     PdfView chooses which page size to pass.
//   - mostVisiblePage returns the 0-based page whose rect has the largest overlap
//     area (width x height, not height alone and not the visible fraction) with
//     viewRect. Ties go to the lower index. -1 when no page overlaps (view in a gap,
//     outside the layout, or an empty layout). PdfView keeps its previous current
//     page on -1.
//   - zoomScrollOrigin: `origin` is the scroll origin (virtual DIP position of the
//     client's top-left) under `before`. `anchor` is a point in client coordinates,
//     the cursor for Ctrl+wheel. The result is the scroll origin under `after` that
//     puts the same document point (same page, same position in PDF points within
//     that page) back at `anchor`. The result is not clamped to the scroll range;
//     PdfView clamps it. Only anchors that lie on a page are specified. Page-relative
//     offsets scale by the page's size ratio between the two layouts; the fixed
//     12px gaps do not scale, so (origin + anchor) * ratio - anchor is wrong.
//     Tolerance is 2px for rounding.
//   - EVT_PDF_PAGE_CHANGED is declared in app/src/pdf/PdfEvents.h with
//     wxDECLARE_EVENT(EVT_PDF_PAGE_CHANGED, wxCommandEvent) and defined in
//     reader_core. The new 0-based page travels in the event's int (SetInt/GetInt).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "pdf/PageLayout.h"
#include "pdf/PdfEvents.h"
#include "pdf/ViewNav.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <vector>

namespace {

constexpr PageSizePts kLetter{612, 792};
constexpr int kGap = PageLayout::kGap;
// The PDF pane's client size with the M2.4 toolbar at the default window size.
constexpr int kViewW = 755;
constexpr int kViewH = 806;

PageLayout make(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth = kViewW)
{
    PageLayout l;
    l.compute(sizes, zoom, viewportWidth, 1.0);
    return l;
}

std::vector<PageSizePts> letters(int n)
{
    return std::vector<PageSizePts>(static_cast<size_t>(n), kLetter);
}

// Origin that keeps `anchor` on the same document point, computed from the exact
// zoom values instead of the rounded rects.
ViewPoint expectedOrigin(const PageLayout& before, double z0, const PageLayout& after, double z1,
                         int page, ViewPoint origin, ViewPoint anchor)
{
    const PageRect r0 = before.pageRect(page);
    const PageRect r1 = after.pageRect(page);
    const double px = (origin.x + anchor.x - r0.x) / z0;
    const double py = (origin.y + anchor.y - r0.y) / z0;
    return {static_cast<int>(std::lround(r1.x + px * z1)) - anchor.x,
            static_cast<int>(std::lround(r1.y + py * z1)) - anchor.y};
}

void checkNear(ViewPoint got, ViewPoint want)
{
    CHECK(std::abs(got.x - want.x) <= 2);
    CHECK(std::abs(got.y - want.y) <= 2);
}

} // namespace

TEST_CASE("zoom limits are 25% to 800% with 10% steps")
{
    CHECK(kMinZoom == doctest::Approx(0.25));
    CHECK(kMaxZoom == doctest::Approx(8.0));
    CHECK(kZoomStep == doctest::Approx(0.10));
}

TEST_CASE("clampZoom keeps zoom within 25%..800%")
{
    CHECK(clampZoom(0.0) == doctest::Approx(0.25));
    CHECK(clampZoom(0.1) == doctest::Approx(0.25));
    CHECK(clampZoom(0.25) == doctest::Approx(0.25));
    CHECK(clampZoom(1.0) == doctest::Approx(1.0));
    CHECK(clampZoom(1.37) == doctest::Approx(1.37));
    CHECK(clampZoom(8.0) == doctest::Approx(8.0));
    CHECK(clampZoom(8.01) == doctest::Approx(8.0));
    CHECK(clampZoom(50.0) == doctest::Approx(8.0));
}

TEST_CASE("stepZoom moves 10 percentage points per step")
{
    CHECK(stepZoom(1.0, 1) == doctest::Approx(1.1));
    CHECK(stepZoom(1.0, 2) == doctest::Approx(1.2));
    CHECK(stepZoom(1.0, -1) == doctest::Approx(0.9));
    CHECK(stepZoom(1.0, -3) == doctest::Approx(0.7));
    CHECK(stepZoom(1.0, 0) == doctest::Approx(1.0));
    // Not snapped to a multiple of 10%: fit width at 119.4% steps to 129.4%.
    CHECK(stepZoom(1.194, 1) == doctest::Approx(1.294));
}

TEST_CASE("stepZoom clamps at both ends")
{
    CHECK(stepZoom(0.3, -1) == doctest::Approx(0.25));
    CHECK(stepZoom(0.25, -1) == doctest::Approx(0.25));
    CHECK(stepZoom(1.1, -12) == doctest::Approx(0.25));
    CHECK(stepZoom(7.95, 1) == doctest::Approx(8.0));
    CHECK(stepZoom(8.0, 1) == doctest::Approx(8.0));
    CHECK(stepZoom(1.0, 100) == doctest::Approx(8.0));
}

TEST_CASE("fitWidthZoom fills the viewport width minus the side gaps")
{
    CHECK(fitWidthZoom(kLetter, 612 + 2 * kGap) == doctest::Approx(1.0));
    CHECK(fitWidthZoom(kLetter, 1224 + 2 * kGap) == doctest::Approx(2.0));
    CHECK(fitWidthZoom(kLetter, kViewW) == doctest::Approx((kViewW - 2.0 * kGap) / 612.0));
    // Landscape page.
    CHECK(fitWidthZoom({792, 612}, 792 + 2 * kGap) == doctest::Approx(1.0));
}

TEST_CASE("fitWidthZoom is clamped")
{
    CHECK(fitWidthZoom(kLetter, 100) == doctest::Approx(0.25));
    CHECK(fitWidthZoom({50, 50}, 2000) == doctest::Approx(8.0));
}

TEST_CASE("fitPageZoom fits the whole page, limited by the tighter dimension")
{
    // Height limits: a tall window would allow 2x by width, but 1x by height.
    CHECK(fitPageZoom(kLetter, 1224 + 2 * kGap, 792 + 2 * kGap) == doctest::Approx(1.0));
    // Width limits.
    CHECK(fitPageZoom(kLetter, 612 + 2 * kGap, 5000) == doctest::Approx(1.0));
    // The M2.4 pane: height limits, about 98.7%.
    CHECK(fitPageZoom(kLetter, kViewW, kViewH) == doctest::Approx((kViewH - 2.0 * kGap) / 792.0));
    // Fit page is never larger than fit width.
    CHECK(fitPageZoom(kLetter, kViewW, kViewH) <= fitWidthZoom(kLetter, kViewW));
}

TEST_CASE("fitPageZoom is clamped")
{
    CHECK(fitPageZoom(kLetter, 2000, 150) == doctest::Approx(0.25));
    CHECK(fitPageZoom({20, 20}, 2000, 2000) == doctest::Approx(8.0));
}

TEST_CASE("mostVisiblePage at the top of the document is page 0")
{
    auto l = make(letters(3), 1.0);
    CHECK(mostVisiblePage(l, {0, 0, l.totalWidth(), kViewH}) == 0);
}

TEST_CASE("mostVisiblePage picks the page with more visible area, not the top page")
{
    // One viewport down at fit width: page 0's bottom ~150px, page 1's top ~640px.
    const double z = fitWidthZoom(kLetter, kViewW);
    auto l = make(letters(520), z);
    const PageRect view{0, kViewH, l.totalWidth(), kViewH};
    REQUIRE(l.visiblePages(view) == std::vector<int>{0, 1});
    CHECK(mostVisiblePage(l, view) == 1);
}

TEST_CASE("mostVisiblePage uses area, not the visible fraction")
{
    // Page 0 is fully visible but short; page 1 shows 200px of a tall page.
    auto l = make({{612, 100}, kLetter}, 1.0);
    const int top = l.pageRect(0).y;
    const PageRect view{0, top, l.totalWidth(), 100 + kGap + 200};
    REQUIRE(l.visiblePages(view) == std::vector<int>{0, 1});
    CHECK(mostVisiblePage(l, view) == 1);
}

TEST_CASE("mostVisiblePage uses area, not visible height")
{
    // Page 0 is wide (1000 x 100 visible), page 1 is narrow (200 x 300 visible).
    auto l = make({{1000, 400}, {200, 400}}, 1.0, 1048);
    const int y = l.pageRect(0).y + 300;
    const PageRect view{0, y, l.totalWidth(), 100 + kGap + 300};
    REQUIRE(l.visiblePages(view) == std::vector<int>{0, 1});
    CHECK(mostVisiblePage(l, view) == 0);
}

TEST_CASE("mostVisiblePage breaks ties toward the lower page")
{
    auto l = make(letters(3), 1.0);
    // 300px of page 0, the gap, 300px of page 1.
    const int y = l.pageRect(1).y - kGap - 300;
    CHECK(mostVisiblePage(l, {0, y, l.totalWidth(), 300 + kGap + 300}) == 0);
    // All three pages fully visible.
    CHECK(mostVisiblePage(l, {0, 0, l.totalWidth(), l.totalHeight()}) == 0);
}

TEST_CASE("mostVisiblePage deep in a long document")
{
    auto l = make(letters(520), 1.0);
    const PageRect r = l.pageRect(399);
    CHECK(mostVisiblePage(l, {0, r.y + 100, l.totalWidth(), kViewH}) == 399);
    // The last page at the end of the document.
    CHECK(mostVisiblePage(l, {0, l.totalHeight() - kViewH, l.totalWidth(), kViewH}) == 519);
}

TEST_CASE("mostVisiblePage returns -1 when no page is visible")
{
    auto l = make(letters(3), 1.0);
    const int gapTop = l.pageRect(0).y + l.pageRect(0).h;
    CHECK(mostVisiblePage(l, {0, gapTop, l.totalWidth(), kGap}) == -1);
    CHECK(mostVisiblePage(l, {0, l.totalHeight() + 50, l.totalWidth(), 100}) == -1);
    CHECK(mostVisiblePage(make({}, 1.0), {0, 0, kViewW, kViewH}) == -1);
}

TEST_CASE("zoomScrollOrigin is the identity when the layout does not change")
{
    auto l = make(letters(520), 1.0);
    const ViewPoint origin{0, l.pageRect(399).y + 50};
    checkNear(zoomScrollOrigin(l, l, origin, {377, 296}), origin);
}

TEST_CASE("zoomScrollOrigin keeps the point under the cursor on page 400")
{
    // Fixed 12px gaps add 400 * 12px above page 400 that do not scale with zoom.
    auto before = make(letters(520), 1.0);
    auto after = make(letters(520), 1.3);
    const ViewPoint origin{0, before.pageRect(399).y + 50};
    const ViewPoint anchor{377, 296};
    const ViewPoint got = zoomScrollOrigin(before, after, origin, anchor);
    checkNear(got, expectedOrigin(before, 1.0, after, 1.3, 399, origin, anchor));
    // The same page is under the cursor afterwards.
    CHECK(after.pageAt(got.y + anchor.y) == 399);
}

TEST_CASE("zoomScrollOrigin zooming out re-centers the narrower page")
{
    auto before = make(letters(3), 1.0);
    auto after = make(letters(3), 0.5);
    const ViewPoint origin{0, before.pageRect(1).y + 100};
    const ViewPoint anchor{377, 400};
    const ViewPoint got = zoomScrollOrigin(before, after, origin, anchor);
    checkNear(got, expectedOrigin(before, 1.0, after, 0.5, 1, origin, anchor));
    // Page x moves from 71 to 224; x 377 at 100% maps to x 377 at 50%.
    CHECK(std::abs(got.x) <= 2);
}

TEST_CASE("zoomScrollOrigin scrolls horizontally when the page outgrows the viewport")
{
    auto before = make(letters(3), 1.0);
    auto after = make(letters(3), 2.0);
    const ViewPoint origin{0, before.pageRect(1).y + 200};
    const ViewPoint anchor{500, 300};
    const ViewPoint got = zoomScrollOrigin(before, after, origin, anchor);
    checkNear(got, expectedOrigin(before, 1.0, after, 2.0, 1, origin, anchor));
    CHECK(got.x > 0);
}

TEST_CASE("zoomScrollOrigin uses the rect of the page under the cursor")
{
    // A narrow middle page: its x offset differs from the wide pages around it.
    const std::vector<PageSizePts> sizes = {kLetter, {300, 400}, kLetter};
    auto before = make(sizes, 1.5);
    auto after = make(sizes, 1.1);
    const PageRect r = before.pageRect(1);
    const ViewPoint origin{0, r.y - 200};
    const ViewPoint anchor{r.x + 100, 200 + 150};
    REQUIRE(before.pageAt(origin.y + anchor.y) == 1);
    const ViewPoint got = zoomScrollOrigin(before, after, origin, anchor);
    checkNear(got, expectedOrigin(before, 1.5, after, 1.1, 1, origin, anchor));
}

TEST_CASE("zoomScrollOrigin round trip returns to the starting origin")
{
    auto a = make(letters(520), 1.0);
    auto b = make(letters(520), 1.3);
    const ViewPoint origin{0, a.pageRect(249).y + 30};
    const ViewPoint anchor{371, 296};
    const ViewPoint there = zoomScrollOrigin(a, b, origin, anchor);
    checkNear(zoomScrollOrigin(b, a, there, anchor), origin);
}

TEST_CASE("EVT_PDF_PAGE_CHANGED carries the 0-based page in the int")
{
    wxCommandEvent event(EVT_PDF_PAGE_CHANGED);
    event.SetInt(41);
    CHECK(event.GetEventType() == EVT_PDF_PAGE_CHANGED);
    CHECK(event.GetInt() == 41);
}

// M10.3: spec tests for the Single Page wheel flip helper, WheelFlip in ViewNav.h.
//
// Interface choices where the spec is silent:
//   - WheelFlip is a default-constructible struct in ViewNav.h with no wx dependency.
//     It holds the accumulated wheel rotation and the time of the last flip:
//       struct WheelFlip {
//           struct Step {
//               int flip = 0;      // -1 previous page, +1 next page, 0 no flip
//               bool pan = false;  // true: the wheel pans the overflowing page instead
//           };
//           Step onWheel(int rotation, int wheelDelta, long long nowMs,
//                        bool pageFitsVertically, bool atTopEdge, bool atBottomEdge,
//                        int page, int pageCount);
//       };
//   - rotation is GetWheelRotation() of one event: positive is up, -120 is one notch down.
//     wheelDelta is GetWheelDelta(), 120 here. nowMs is a monotonic clock in ms. A fresh
//     WheelFlip is never locked out, whatever nowMs is (the first test starts at 0).
//   - Accumulation: rotation adds to an accumulator. A flip happens when the accumulator
//     reaches one wheelDelta in either direction, then the accumulator resets to 0.
//     Tests use exact multiples (-40 three times is -120).
//   - Lockout: an event is ignored (flip 0, pan false, nothing accumulates) when
//     nowMs - lastFlipMs < 150. The lockout runs from the last flip, not from the last
//     ignored event. The event at exactly 150 ms is not ignored.
//   - Direction: rotation < 0 (wheel down) is forward (+1). rotation > 0 is back (-1).
//   - Fitting page (pageFitsVertically true): the wheel never pans. Each full notch flips.
//     The edge flags are passed as true in these tests.
//   - Overflowing page: a notch that still has content to scroll toward returns pan = true,
//     flip 0, and resets the accumulator. That is wheel down while !atBottomEdge, or wheel
//     up while !atTopEdge (wheel up at the bottom edge pans back up). A notch toward the
//     edge it is at (wheel down at atBottomEdge, wheel up at atTopEdge) accumulates like a
//     fitting page and flips one page on the first full notch. Accumulation across pan
//     events is not tested.
//   - No flip when the target page is out of range: before page 0 or past pageCount - 1.
//     Such a notch returns flip 0 and pan false.

namespace {

constexpr int kNotch = 120;
constexpr int kDeckPages = 8;

// One wheel event on a fitting page. A fitting page is at both edges, so the flags are true.
int fitFlip(WheelFlip& wf, int rotation, long long nowMs, int page, int pageCount = kDeckPages)
{
    return wf.onWheel(rotation, kNotch, nowMs, true, true, true, page, pageCount).flip;
}

} // namespace

TEST_CASE("WheelFlip: one notch flips one page, wheel down is next and wheel up is previous")
{
    WheelFlip down;
    const auto forward = down.onWheel(-kNotch, kNotch, 0, true, true, true, 0, kDeckPages);
    CHECK(forward.flip == 1);
    CHECK_FALSE(forward.pan);

    WheelFlip up;
    const auto back = up.onWheel(kNotch, kNotch, 0, true, true, true, 3, kDeckPages);
    CHECK(back.flip == -1);
    CHECK_FALSE(back.pan);
}

TEST_CASE("WheelFlip: a burst of notches inside 150 ms after a flip flips once")
{
    WheelFlip wf;
    // The first notch flips. The four after it, all inside the lockout, do not.
    int flips = 0;
    for (long long t : {0LL, 10LL, 30LL, 50LL, 149LL})
        flips += fitFlip(wf, -kNotch, t, 0);
    CHECK(flips == 1);
}

TEST_CASE("WheelFlip: partial rotations accumulate to one flip")
{
    WheelFlip wf;
    CHECK(fitFlip(wf, -40, 0, 0) == 0);
    CHECK(fitFlip(wf, -40, 10, 0) == 0);
    CHECK(fitFlip(wf, -40, 20, 0) == 1);
}

TEST_CASE("WheelFlip: a notch 150 ms after a flip flips again, 149 ms after does not")
{
    WheelFlip wf;
    CHECK(fitFlip(wf, -kNotch, 0, 0) == 1);
    // Still locked out at 149 ms, so this notch is ignored.
    CHECK(fitFlip(wf, -kNotch, 149, 1) == 0);
    // Locked out from the flip at 0, not from the ignored notch at 149: 150 ms is free.
    CHECK(fitFlip(wf, -kNotch, 150, 1) == 1);
}

TEST_CASE("WheelFlip: notches ignored in the lockout do not accumulate")
{
    WheelFlip wf;
    CHECK(fitFlip(wf, -kNotch, 0, 0) == 1);
    CHECK(fitFlip(wf, -60, 50, 1) == 0);
    CHECK(fitFlip(wf, -60, 100, 1) == 0);
    // Free again. The two ignored half notches added nothing, so this half notch does not flip.
    CHECK(fitFlip(wf, -60, 200, 1) == 0);
    CHECK(fitFlip(wf, -60, 210, 1) == 1);
}

TEST_CASE("WheelFlip: an overflowing page pans toward the bottom, then one notch at the bottom flips")
{
    WheelFlip wf;
    // Page 0 overflows and starts at its top edge. Wheel down pans until the bottom edge.
    for (long long t : {0LL, 200LL, 400LL}) {
        const auto pan = wf.onWheel(-kNotch, kNotch, t, false, true, false, 0, kDeckPages);
        CHECK(pan.pan);
        CHECK(pan.flip == 0);
    }
    // At the bottom edge the first full notch flips forward.
    const auto step = wf.onWheel(-kNotch, kNotch, 600, false, false, true, 0, kDeckPages);
    CHECK(step.flip == 1);
    CHECK_FALSE(step.pan);
}

TEST_CASE("WheelFlip: an overflowing page pans toward the top, then one notch at the top flips back")
{
    WheelFlip wf;
    // Page 3 overflows and is in the middle. Wheel up pans, and so does wheel up at the bottom edge.
    const auto mid = wf.onWheel(kNotch, kNotch, 0, false, false, false, 3, kDeckPages);
    CHECK(mid.pan);
    CHECK(mid.flip == 0);
    const auto bottom = wf.onWheel(kNotch, kNotch, 200, false, false, true, 3, kDeckPages);
    CHECK(bottom.pan);
    CHECK(bottom.flip == 0);
    // At the top edge the first full notch flips back.
    const auto step = wf.onWheel(kNotch, kNotch, 400, false, true, false, 3, kDeckPages);
    CHECK(step.flip == -1);
    CHECK_FALSE(step.pan);
}

TEST_CASE("WheelFlip: no flip before the first page")
{
    WheelFlip fits;
    CHECK(fitFlip(fits, kNotch, 0, 0) == 0);

    WheelFlip overflow;
    const auto step = overflow.onWheel(kNotch, kNotch, 0, false, true, false, 0, kDeckPages);
    CHECK(step.flip == 0);
}

TEST_CASE("WheelFlip: no flip past the last page")
{
    WheelFlip fits;
    CHECK(fitFlip(fits, -kNotch, 0, kDeckPages - 1) == 0);

    WheelFlip overflow;
    const auto step = overflow.onWheel(-kNotch, kNotch, 0, false, false, true, kDeckPages - 1, kDeckPages);
    CHECK(step.flip == 0);
}

TEST_CASE("WheelFlip: a one-page or empty document never flips")
{
    WheelFlip one;
    CHECK(fitFlip(one, -kNotch, 0, 0, 1) == 0);
    WheelFlip oneUp;
    CHECK(fitFlip(oneUp, kNotch, 0, 0, 1) == 0);
    WheelFlip empty;
    CHECK(fitFlip(empty, -kNotch, 0, 0, 0) == 0);
}
