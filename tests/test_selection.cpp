// M3.2: spec tests for text selection (app/src/pdf/Selection.h) and the PdfView
// selection interface (selection(), EVT_PDF_SELECTION_CHANGED, the context menu hook).
// The mouse, highlight, clipboard and context menu are checked in the UI by
// tests/acceptance/m3.2.sh.
//
// Test data (tests/data/, reached through the compile definition TEST_DATA_DIR):
//   sample.pdf   3 pages. Page 0: "Page One" (boxes 0-1), a 3-line paragraph (boxes 2-28,
//                lines 2-15, 16-26 and 27-28, ending "word: zebra."), page number "1" (29).
//                Page 1: "Page Two" (0-1), "Attention is all you need. ..." (2-18), "2" (19).
//                Page 2: "Page Three" (0-1), "The third and final page closes the sample
//                document." (2-10), "3" (11). See tests/test_text_layer.cpp.
//   twocol.pdf   1 page, written by tests/data/make_twocol_pdf.py: heading "Two Column
//                Sample" (boxes 0-2), a left column of four lines (3-28, last word
//                "omega."), a right column of four lines (29-53, first word "Bravo", last
//                word "zulu."). Poppler's reading order is heading, left column, right
//                column. "omega." has no space after it and sits low on the page, "Bravo"
//                sits high: the column jump.
//
// Interface choices where the spec is silent (all in pdf/Selection.h, wx-free):
//   - struct TextPos { int page = 0; int boxIndex = 0; };
//     An aggregate. 0-based page and the box's index in that page's TextLayer vector.
//     Defaulted == and <=> give document order (page first, then boxIndex).
//   - struct SelectionInfo { std::string text; int firstPage; int lastPage; };
//     firstPage and lastPage are 0-based (like PdfView::currentPage()).
//   - class Selection, an immutable range of boxes, both ends included:
//         Selection(TextPos a, TextPos b);   // either order: normalizes so start() <= end()
//         TextPos start() const;
//         TextPos end() const;
//         bool contains(TextPos pos) const;   // start() <= pos <= end() (for painting)
//         std::string text(const TextLayer& layer) const;
//         SelectionInfo info(const TextLayer& layer) const;  // {text(layer), start.page, end.page}
//         static Selection word(TextPos pos);                     // double-click: that one box
//         static Selection line(const TextLayer& layer, TextPos pos);  // triple-click
//     There is no empty Selection: PdfView keeps std::optional<Selection>, and a click on
//     empty space resets it.
//   - Selection::text joins the boxes of each page with the copy rule below, and the parts
//     of consecutive pages with a single "\n". Start page: start.boxIndex to its last box.
//     Pages between: every box. End page: box 0 to end.boxIndex. A page the TextLayer has
//     not loaded, or a page with no boxes, contributes nothing and adds no "\n" (so no
//     blank lines). Nothing is appended after the last box.
//   - Copy rule between box a and the next box b of the same page: "\n" when b's baseline
//     (y + h) is lower than a's by more than half a's height (the M3.1 rule), or when b
//     lies entirely above a (b.y + b.h <= a.y): the column jump from the bottom of one
//     column to the top of the next (carried M3.1 review note). Otherwise " " when a has
//     hasSpaceAfter, otherwise nothing. Whether TextLayer::textBetween gains the column
//     rule too is the implementer's choice; these tests only read Selection.
//   - Selection::line(layer, pos) extends from pos backwards and forwards over boxes of the
//     same page while the copy rule puts no "\n" between neighbours. So a line never
//     crosses a column jump, and triple-clicking the right column selects only its line,
//     not the left column's line at the same height. For a page not loaded, or an index
//     outside the page's boxes, it returns word(pos).
//   - PdfView (pdf/PdfView.h), checked at compile time only (a wxWindow needs a display):
//         std::optional<SelectionInfo> selection() const;  // nullopt without a selection
//         void setContextMenuHook(std::function<void(wxMenu&)> hook);
//     The context menu holds "&Copy" (wxID_COPY, may show "Ctrl+C"), enabled only when a
//     selection exists. Before showing the menu PdfView calls the hook (if set) with it, so
//     M5.3 can append "Ask about selection" and bind its handler. A right-click never
//     changes the selection.
//   - EVT_PDF_SELECTION_CHANGED is declared in pdf/PdfEvents.h as a wxCommandEvent, like
//     the other PdfView events. It is sent whenever the selection changes, including when
//     it is cleared (click on empty space, new document). Read the new value from
//     PdfView::selection().
//   - Ctrl+C or "Copy" with no selection leaves the clipboard unchanged.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PdfDocument.h"
#include "pdf/PdfEvents.h"
#include "pdf/PdfView.h"
#include "pdf/Selection.h"
#include "pdf/TextLayer.h"

#include <wx/event.h>
#include <wx/menu.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

std::filesystem::path dataFile(const char* name)
{
    return std::filesystem::path(TEST_DATA_DIR) / name;
}

std::unique_ptr<PdfDocument> openOk(const char* name)
{
    std::string error;
    auto doc = PdfDocument::open(dataFile(name), {}, error);
    REQUIRE_MESSAGE(doc != nullptr, "open failed: " << error);
    return doc;
}

// A TextLayer with every page of the file loaded.
TextLayer loadAll(const char* name)
{
    auto doc = openOk(name);
    TextLayer layer;
    for (int p = 0; p < doc->pageCount(); ++p)
        layer.setBoxes(p, doc->textBoxes(p));
    return layer;
}

TextBox makeBox(std::string text, double x, double y, double w, double h, bool spaceAfter,
                int index)
{
    TextBox b;
    b.text = std::move(text);
    b.bbox.x = x;
    b.bbox.y = y;
    b.bbox.w = w;
    b.bbox.h = h;
    b.hasSpaceAfter = spaceAfter;
    b.index = index;
    return b;
}

TextPos pos(int page, int boxIndex)
{
    return TextPos{page, boxIndex};
}

// sample.pdf page 0.
constexpr int kZebra = 28;
constexpr int kPageNumber0 = 29;
const std::string kParagraph0 =
    "The quick brown fox jumps over the lazy dog. This sample document exists to\n"
    "test PDF loading, rendering, and text extraction in Agentic Reader. Marker\n"
    "word: zebra.";
const std::string kPage1 =
    "Page Two\n"
    "Attention is all you need. The second page holds different text so tests can\n"
    "tell pages apart.\n"
    "2";
const std::string kPage2 =
    "Page Three\n"
    "The third and final page closes the sample document.\n"
    "3";

// twocol.pdf.
constexpr int kAlpha = 3;
constexpr int kOmega = 28;
constexpr int kBravo = 29;
constexpr int kZulu = 53;
const std::string kLeftColumn =
    "Alpha starts the left column and\n"
    "its words run down a narrow strip\n"
    "of the page until the left side\n"
    "comes to an end at omega.";
const std::string kRightColumn =
    "Bravo opens the right column and\n"
    "a reader meets it only after the\n"
    "left column is done, so it closes\n"
    "the page here at zulu.";

} // namespace

// ---------------------------------------------------------------------------
// Interface checks (spec 5, 6, 7)

static_assert(std::is_aggregate_v<TextPos>);
static_assert(std::is_same_v<decltype(SelectionInfo::text), std::string>);
static_assert(std::is_same_v<decltype(SelectionInfo::firstPage), int>);
static_assert(std::is_same_v<decltype(SelectionInfo::lastPage), int>);
static_assert(std::is_same_v<decltype(std::declval<const PdfView&>().selection()),
                             std::optional<SelectionInfo>>);
static_assert(std::is_invocable_v<decltype(&PdfView::setContextMenuHook), PdfView&,
                                  std::function<void(wxMenu&)>>);
static_assert(std::is_same_v<decltype(EVT_PDF_SELECTION_CHANGED),
                             const wxEventTypeTag<wxCommandEvent>>);

TEST_CASE("selection EVT_PDF_SELECTION_CHANGED is its own event type")
{
    const wxEventType changed = EVT_PDF_SELECTION_CHANGED;
    CHECK(changed != wxEVT_NULL);
    CHECK(changed != static_cast<wxEventType>(EVT_PDF_PAGE_CHANGED));
    CHECK(changed != static_cast<wxEventType>(EVT_PDF_ZOOM_CHANGED));
    CHECK(changed != static_cast<wxEventType>(EVT_PDF_FILE_DROPPED));
}

// ---------------------------------------------------------------------------
// Spec 1: positions and normalization

TEST_CASE("selection TextPos orders by page, then box index")
{
    CHECK(pos(0, 5) == pos(0, 5));
    CHECK_FALSE(pos(0, 5) == pos(0, 6));
    CHECK(pos(0, 5) < pos(0, 6));
    CHECK(pos(0, 99) < pos(1, 0));
    CHECK(pos(2, 0) > pos(1, 40));
    CHECK(pos(1, 3) <= pos(1, 3));
}

TEST_CASE("selection keeps a forward range as given")
{
    const Selection s(pos(0, 2), pos(0, 10));
    CHECK(s.start() == pos(0, 2));
    CHECK(s.end() == pos(0, 10));
}

TEST_CASE("selection normalizes a backward range on one page")
{
    const Selection s(pos(0, 10), pos(0, 2));
    CHECK(s.start() == pos(0, 2));
    CHECK(s.end() == pos(0, 10));
}

TEST_CASE("selection normalizes a backward range across pages")
{
    // Dragging up from page 2 to page 0: the end box index is smaller than the start's,
    // but the page order decides.
    const Selection s(pos(2, 1), pos(0, 7));
    CHECK(s.start() == pos(0, 7));
    CHECK(s.end() == pos(2, 1));

    // Higher box index on the earlier page.
    const Selection t(pos(1, 3), pos(0, 28));
    CHECK(t.start() == pos(0, 28));
    CHECK(t.end() == pos(1, 3));
}

TEST_CASE("selection of a single box has equal ends")
{
    const Selection s(pos(1, 4), pos(1, 4));
    CHECK(s.start() == pos(1, 4));
    CHECK(s.end() == pos(1, 4));
}

TEST_CASE("selection contains the boxes between its ends in document order")
{
    const Selection s(pos(2, 1), pos(0, 28)); // backward, across three pages
    CHECK_FALSE(s.contains(pos(0, 0)));
    CHECK_FALSE(s.contains(pos(0, 27)));
    CHECK(s.contains(pos(0, 28)));
    CHECK(s.contains(pos(0, 29)));
    CHECK(s.contains(pos(1, 0)));   // every box of a middle page
    CHECK(s.contains(pos(1, 19)));
    CHECK(s.contains(pos(2, 0)));
    CHECK(s.contains(pos(2, 1)));
    CHECK_FALSE(s.contains(pos(2, 2)));
    CHECK_FALSE(s.contains(pos(3, 0)));

    const Selection one(pos(0, 5), pos(0, 5));
    CHECK(one.contains(pos(0, 5)));
    CHECK_FALSE(one.contains(pos(0, 4)));
    CHECK_FALSE(one.contains(pos(0, 6)));
    CHECK_FALSE(one.contains(pos(1, 5)));
}

// ---------------------------------------------------------------------------
// Text of a selection on one page

TEST_CASE("selection text of one page matches the words in order")
{
    const TextLayer layer = loadAll("sample.pdf");
    CHECK(Selection(pos(0, 2), pos(0, 10)).text(layer) ==
          "The quick brown fox jumps over the lazy dog.");
    CHECK(Selection(pos(0, 2), pos(0, kZebra)).text(layer) == kParagraph0);
    CHECK(Selection(pos(0, kZebra), pos(0, kZebra)).text(layer) == "zebra.");
    CHECK(Selection(pos(1, 2), pos(1, 6)).text(layer) == "Attention is all you need.");
}

TEST_CASE("selection text of a backward range equals the forward one")
{
    const TextLayer layer = loadAll("sample.pdf");
    CHECK(Selection(pos(0, kZebra), pos(0, 2)).text(layer) == kParagraph0);
    CHECK(Selection(pos(0, 10), pos(0, 3)).text(layer) ==
          "quick brown fox jumps over the lazy dog.");
}

TEST_CASE("selection text keeps UTF-8")
{
    const TextLayer layer = loadAll("utf8.pdf");
    CHECK(Selection(pos(0, 3), pos(0, 0)).text(layer) ==
          "Caf\xC3\xA9 na\xC3\xAFve Gr\xC3\xB6\xC3\x9F" "e \xE2\x82\xAC" "5");
}

TEST_CASE("selection info carries the text and the 0-based page range")
{
    const TextLayer layer = loadAll("sample.pdf");
    const SelectionInfo one = Selection(pos(0, kZebra), pos(0, 2)).info(layer);
    CHECK(one.text == kParagraph0);
    CHECK(one.firstPage == 0);
    CHECK(one.lastPage == 0);

    const SelectionInfo two = Selection(pos(2, 1), pos(1, 2)).info(layer);
    CHECK(two.firstPage == 1);
    CHECK(two.lastPage == 2);
    CHECK(two.text == Selection(pos(1, 2), pos(2, 1)).text(layer));
}

// ---------------------------------------------------------------------------
// Cross-page text extraction

TEST_CASE("selection text across two pages joins the pages with a newline")
{
    const TextLayer layer = loadAll("sample.pdf");
    CHECK(Selection(pos(0, kZebra), pos(1, 2)).text(layer) ==
          "zebra.\n1\nPage Two\nAttention");
    // From the page number of page 0 to the heading of page 1.
    CHECK(Selection(pos(0, kPageNumber0), pos(1, 1)).text(layer) == "1\nPage Two");
}

TEST_CASE("selection text across pages ignores the drag direction")
{
    const TextLayer layer = loadAll("sample.pdf");
    CHECK(Selection(pos(1, 2), pos(0, kZebra)).text(layer) ==
          "zebra.\n1\nPage Two\nAttention");
}

TEST_CASE("selection text across three pages includes the whole middle page")
{
    const TextLayer layer = loadAll("sample.pdf");
    CHECK(Selection(pos(0, 2), pos(2, 10)).text(layer) ==
          kParagraph0 + "\n1\n" + kPage1 + "\nPage Three\n" +
              "The third and final page closes the sample document.");
    CHECK(Selection(pos(2, 11), pos(0, 0)).text(layer) ==
          "Page One\n" + kParagraph0 + "\n1\n" + kPage1 + "\n" + kPage2);
}

TEST_CASE("selection text skips pages not loaded without blank lines")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));
    layer.setBoxes(2, doc->textBoxes(2));

    // Page 1 is missing from the layer.
    CHECK(Selection(pos(0, kZebra), pos(2, 1)).text(layer) == "zebra.\n1\nPage Three");

    // Missing first or last page.
    CHECK(Selection(pos(1, 5), pos(2, 1)).text(layer) == "Page Three");
    CHECK(Selection(pos(0, kZebra), pos(1, 5)).text(layer) == "zebra.\n1");

    // Nothing loaded.
    const TextLayer empty;
    CHECK(Selection(pos(0, 0), pos(2, 3)).text(empty) == "");
}

TEST_CASE("selection text skips pages without words")
{
    TextLayer layer;
    layer.setBoxes(0, {makeBox("end", 0, 100, 20, 10, false, 0)});
    layer.setBoxes(1, {});
    layer.setBoxes(2, {makeBox("start", 0, 100, 30, 10, false, 0)});
    CHECK(Selection(pos(0, 0), pos(2, 0)).text(layer) == "end\nstart");
}

TEST_CASE("selection text across pages starts mid-line and ends mid-line")
{
    // The start page contributes from the start box to its end, the end page from its first
    // box to the end box.
    TextLayer layer;
    layer.setBoxes(0, {makeBox("a", 0, 100, 10, 10, true, 0),
                       makeBox("b", 15, 100, 10, 10, true, 1),
                       makeBox("c", 30, 100, 10, 10, false, 2)});
    layer.setBoxes(1, {makeBox("d", 0, 100, 10, 10, true, 0),
                       makeBox("e", 15, 100, 10, 10, true, 1),
                       makeBox("f", 30, 100, 10, 10, false, 2)});
    CHECK(Selection(pos(0, 1), pos(1, 1)).text(layer) == "b c\nd e");
    CHECK(Selection(pos(1, 0), pos(0, 2)).text(layer) == "c\nd");
}

// ---------------------------------------------------------------------------
// Copy rule: line breaks and the column jump (carried from the M3.1 review)

TEST_CASE("selection text breaks lines on synthetic boxes")
{
    TextLayer layer;

    SUBCASE("same line keeps the space flag")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 25, 100, 20, 10, false, 1),
                           makeBox("baz", 45, 100, 20, 10, false, 2)});
        CHECK(Selection(pos(0, 0), pos(0, 2)).text(layer) == "foo barbaz");
    }
    SUBCASE("next line lower starts a new line")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 0, 114, 20, 10, false, 1)});
        CHECK(Selection(pos(0, 0), pos(0, 1)).text(layer) == "foo\nbar");
    }
    SUBCASE("a superscript slightly higher stays on the line")
    {
        layer.setBoxes(0, {makeBox("x", 0, 100, 10, 10, false, 0),
                           makeBox("2", 10, 95, 5, 10, true, 1),
                           makeBox("y", 20, 100, 10, 10, false, 2)});
        CHECK(Selection(pos(0, 0), pos(0, 2)).text(layer) == "x2 y");
    }
    SUBCASE("column jump to a box entirely above starts a new line")
    {
        // Last word of the left column at the bottom, first word of the right column at
        // the top, no space flag between them.
        layer.setBoxes(0, {makeBox("left", 72, 700, 20, 10, false, 0),
                           makeBox("right", 324, 100, 25, 10, true, 1),
                           makeBox("column", 352, 100, 30, 10, false, 2)});
        CHECK(Selection(pos(0, 0), pos(0, 2)).text(layer) == "left\nright column");
    }
    SUBCASE("column jump replaces the space flag")
    {
        layer.setBoxes(0, {makeBox("left", 72, 700, 20, 10, true, 0),
                           makeBox("right", 324, 100, 25, 10, false, 1)});
        CHECK(Selection(pos(0, 0), pos(0, 1)).text(layer) == "left\nright");
    }
    SUBCASE("a box that just touches the top of the previous one is above it")
    {
        layer.setBoxes(0, {makeBox("low", 72, 110, 20, 10, false, 0),
                           makeBox("high", 324, 100, 20, 10, false, 1)});
        CHECK(Selection(pos(0, 0), pos(0, 1)).text(layer) == "low\nhigh");
    }
}

TEST_CASE("selection text of a two-column page breaks at the column jump")
{
    const TextLayer layer = loadAll("twocol.pdf");
    const std::vector<TextBox>& boxes = *layer.boxes(0);
    REQUIRE(boxes.size() == 54);
    // Reading order as poppler reports it: heading, left column, right column.
    REQUIRE(boxes[kAlpha].text == "Alpha");
    REQUIRE(boxes[kOmega].text == "omega.");
    REQUIRE(boxes[kBravo].text == "Bravo");
    REQUIRE(boxes[kZulu].text == "zulu.");
    REQUIRE_FALSE(boxes[kOmega].hasSpaceAfter);

    CHECK(Selection(pos(0, kAlpha), pos(0, kOmega)).text(layer) == kLeftColumn);
    CHECK(Selection(pos(0, kBravo), pos(0, kZulu)).text(layer) == kRightColumn);
    CHECK(Selection(pos(0, kAlpha), pos(0, kZulu)).text(layer) ==
          kLeftColumn + "\n" + kRightColumn);
    CHECK(Selection(pos(0, kZulu), pos(0, 0)).text(layer) ==
          "Two Column Sample\n" + kLeftColumn + "\n" + kRightColumn);
    // Just across the jump: never "omega.Bravo".
    CHECK(Selection(pos(0, kOmega), pos(0, kBravo)).text(layer) == "omega.\nBravo");
}

// ---------------------------------------------------------------------------
// Spec 2: double-click word and triple-click line

TEST_CASE("selection word selects exactly the box")
{
    const TextLayer layer = loadAll("sample.pdf");
    const Selection s = Selection::word(pos(0, 4));
    CHECK(s.start() == pos(0, 4));
    CHECK(s.end() == pos(0, 4));
    CHECK(s.text(layer) == "brown");
    CHECK(Selection::word(pos(1, 2)).text(layer) == "Attention");
}

TEST_CASE("selection line selects the boxes of the line in sample.pdf")
{
    const TextLayer layer = loadAll("sample.pdf");

    SUBCASE("first line of the paragraph")
    {
        for (int i : {2, 7, 15}) {
            CAPTURE(i);
            const Selection s = Selection::line(layer, pos(0, i));
            CHECK(s.start() == pos(0, 2));
            CHECK(s.end() == pos(0, 15));
        }
    }
    SUBCASE("middle line of the paragraph")
    {
        const Selection s = Selection::line(layer, pos(0, 19)); // "rendering,"
        CHECK(s.start() == pos(0, 16));
        CHECK(s.end() == pos(0, 26));
        CHECK(s.text(layer) ==
              "test PDF loading, rendering, and text extraction in Agentic Reader. Marker");
    }
    SUBCASE("last line of the paragraph stops before the page number")
    {
        const Selection s = Selection::line(layer, pos(0, kZebra));
        CHECK(s.start() == pos(0, 27));
        CHECK(s.end() == pos(0, kZebra));
        CHECK(s.text(layer) == "word: zebra.");
    }
    SUBCASE("title and page number")
    {
        CHECK(Selection::line(layer, pos(0, 1)).text(layer) == "Page One");
        const Selection n = Selection::line(layer, pos(0, kPageNumber0));
        CHECK(n.start() == pos(0, kPageNumber0));
        CHECK(n.end() == pos(0, kPageNumber0));
    }
    SUBCASE("a line never crosses pages")
    {
        CHECK(Selection::line(layer, pos(1, 0)).text(layer) == "Page Two");
        CHECK(Selection::line(layer, pos(1, 18)).text(layer) == "tell pages apart.");
    }
}

TEST_CASE("selection line on a two-column page stays in its column")
{
    const TextLayer layer = loadAll("twocol.pdf");
    REQUIRE(layer.boxes(0)->size() == 54);

    // Heading: stops before "Alpha" (lower line).
    CHECK(Selection::line(layer, pos(0, 1)).text(layer) == "Two Column Sample");

    // Last line of the left column: stops at "omega.", although "Bravo" (higher on the
    // page) follows in reading order.
    const Selection lastLeft = Selection::line(layer, pos(0, 26)); // "end"
    CHECK(lastLeft.start() == pos(0, 23));
    CHECK(lastLeft.end() == pos(0, kOmega));
    CHECK(lastLeft.text(layer) == "comes to an end at omega.");

    // First line of the right column: starts at "Bravo", not in the left column.
    const Selection firstRight = Selection::line(layer, pos(0, 31)); // "the"
    CHECK(firstRight.start() == pos(0, kBravo));
    CHECK(firstRight.end() == pos(0, 34));
    CHECK(firstRight.text(layer) == "Bravo opens the right column and");

    // A right-column line beside a left-column line at the same height.
    CHECK(Selection::line(layer, pos(0, 36)).text(layer) == "a reader meets it only after the");
    CHECK(Selection::line(layer, pos(0, 10)).text(layer) == "its words run down a narrow strip");
    CHECK(Selection::line(layer, pos(0, kZulu)).text(layer) == "the page here at zulu.");
}

TEST_CASE("selection line on synthetic boxes")
{
    TextLayer layer;
    layer.setBoxes(0, {makeBox("x", 0, 100, 10, 10, false, 0),
                       makeBox("2", 10, 95, 5, 10, true, 1),   // superscript
                       makeBox("y", 20, 100, 10, 10, false, 2),
                       makeBox("next", 0, 114, 20, 10, false, 3),
                       makeBox("top", 300, 20, 20, 10, true, 4),  // column jump
                       makeBox("row", 325, 20, 20, 10, false, 5)});

    const Selection a = Selection::line(layer, pos(0, 1));
    CHECK(a.start() == pos(0, 0));
    CHECK(a.end() == pos(0, 2));

    const Selection b = Selection::line(layer, pos(0, 3));
    CHECK(b.start() == pos(0, 3));
    CHECK(b.end() == pos(0, 3));

    const Selection c = Selection::line(layer, pos(0, 5));
    CHECK(c.start() == pos(0, 4));
    CHECK(c.end() == pos(0, 5));
    CHECK(c.text(layer) == "top row");
}

TEST_CASE("selection line on a page not loaded is the single box")
{
    const TextLayer layer;
    const Selection s = Selection::line(layer, pos(3, 7));
    CHECK(s.start() == pos(3, 7));
    CHECK(s.end() == pos(3, 7));
}
