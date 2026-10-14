// M10.1: spec tests for detectLayout (app/src/pdf/LayoutDetect.h).
//
// Interface choices where the spec is silent:
//   - PageLayout.h declares, in the global namespace:
//       enum class LayoutMode { Continuous, SinglePage };
//     and PageSizePts { double w; double h; } (points), as in the M2.2 spec.
//   - LayoutDetect.h includes "pdf/PageLayout.h" and declares:
//       LayoutMode detectLayout(const std::vector<PageSizePts>& sizes);
//   - Pure function, no wx dependency. Sizes are as PdfDocument::pageSizePts returns
//     them, after /Rotate. The caller applies the rotation, not detectLayout.
//   - Only the first 20 entries are checked, or all of them when there are fewer.
//   - A page is landscape when width / height >= 1.2 (exactly 1.2 counts).
//   - SinglePage when landscape pages * 2 > pages checked, so exactly half is
//     Continuous. No pages gives Continuous.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "pdf/LayoutDetect.h"
#include "pdf/PageLayout.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

const PageSizePts kLetterPage{612, 792};
const PageSizePts kA4Page{595.276, 841.89};
const PageSizePts kSlide16x9{960, 540};
const PageSizePts kSlide4x3{720, 540};

std::vector<PageSizePts> pages(int n, PageSizePts s)
{
    return std::vector<PageSizePts>(static_cast<size_t>(n), s);
}

std::vector<PageSizePts> concat(std::vector<PageSizePts> a, const std::vector<PageSizePts>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

} // namespace

TEST_CASE("Letter and A4 documents are Continuous")
{
    CHECK(detectLayout(pages(1, kLetterPage)) == LayoutMode::Continuous);
    CHECK(detectLayout(pages(10, kLetterPage)) == LayoutMode::Continuous);
    CHECK(detectLayout(pages(10, kA4Page)) == LayoutMode::Continuous);
}

TEST_CASE("16:9 deck is SinglePage")
{
    CHECK(detectLayout(pages(1, kSlide16x9)) == LayoutMode::SinglePage);
    CHECK(detectLayout(pages(30, kSlide16x9)) == LayoutMode::SinglePage);
}

TEST_CASE("4:3 deck is SinglePage")
{
    CHECK(detectLayout(pages(20, kSlide4x3)) == LayoutMode::SinglePage);
}

TEST_CASE("portrait document with one landscape page is Continuous")
{
    const auto doc = concat(pages(9, kLetterPage), pages(1, kSlide16x9));
    CHECK(detectLayout(doc) == LayoutMode::Continuous);
}

TEST_CASE("empty document is Continuous")
{
    CHECK(detectLayout({}) == LayoutMode::Continuous);
}

TEST_CASE("only the first 20 pages are checked")
{
    // First 20: 9 landscape. Pages 21 onward are all landscape and must not count.
    const auto few = concat(concat(pages(9, kSlide16x9), pages(11, kLetterPage)),
                            pages(100, kSlide16x9));
    CHECK(detectLayout(few) == LayoutMode::Continuous);

    // First 20: 11 landscape. Pages 21 onward are all portrait.
    const auto many = concat(concat(pages(11, kSlide16x9), pages(9, kLetterPage)),
                             pages(100, kLetterPage));
    CHECK(detectLayout(many) == LayoutMode::SinglePage);
}

TEST_CASE("ratio of exactly 1.2 counts as landscape")
{
    CHECK(detectLayout(pages(20, {120, 100})) == LayoutMode::SinglePage);
    CHECK(detectLayout(pages(20, {600, 500})) == LayoutMode::SinglePage);
    CHECK(detectLayout(pages(20, {119, 100})) == LayoutMode::Continuous);
}

TEST_CASE("exactly half landscape is not more than half")
{
    CHECK(detectLayout(concat(pages(1, kSlide16x9), pages(1, kLetterPage))) ==
          LayoutMode::Continuous);
    CHECK(detectLayout(concat(pages(2, kSlide16x9), pages(2, kLetterPage))) ==
          LayoutMode::Continuous);
    CHECK(detectLayout(concat(pages(10, kSlide16x9), pages(10, kLetterPage))) ==
          LayoutMode::Continuous);
    // One more than half.
    CHECK(detectLayout(concat(pages(11, kSlide16x9), pages(9, kLetterPage))) ==
          LayoutMode::SinglePage);
}
