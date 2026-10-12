// M3.1: spec tests for the text layer and coordinate mapping
// (app/src/pdf/TextLayer.h, PdfDocument::textBoxes, PageLayout::pageToView/viewToPage,
// and the text boxes RenderWorker posts with each image).
//
// Test data (tests/data/, reached through the compile definition TEST_DATA_DIR):
//   sample.pdf   3 pages, 612 x 792 pt, pdfTeX. Page index 0 holds 30 words in reading
//                order: "Page One" (title), a 3-line paragraph "The quick brown fox jumps
//                over the lazy dog. This sample document exists to / test PDF loading,
//                rendering, and text extraction in Agentic Reader. Marker / word: zebra."
//                and the page number "1". "zebra." is box 28 with bbox
//                x 144.348, y 184.633, w 30.567, h 10.616 (pdftotext -bbox gives the same).
//                Line-final words ("One", "to", "Marker", "zebra.") have has_space_after false.
//   rotated.pdf  sample.pdf with /Rotate 90 on page index 0 (pageSizePts 792 x 612).
//   utf8.pdf     1 page, 300 x 200 pt, one line "Café naïve Größe €5" in Helvetica with
//                WinAnsiEncoding. Written by tests/data/make_utf8_pdf.py.
//
// Interface choices where the spec is silent:
//   - Page points use poppler's text_list() convention: origin at the top-left of the
//     page as displayed (after /Rotate), y grows downward, 1 unit = 1 pt.
//   - New wx-free types, visible after including pdf/PageLayout.h:
//         struct PointPts { double x; double y; };                 // page points
//         struct RectPts { double x; double y; double w; double h; }; // page points
//         struct PagePoint { int page; PointPts pt; };             // page -1: no page
//     ViewPoint { int x; int y; } moves from ViewNav.h to PageLayout.h (ViewNav.h already
//     includes PageLayout.h, so its users still see it).
//   - PageLayout gains
//         PageRect pageToView(int page, const RectPts& rectPts) const;
//         PagePoint viewToPage(ViewPoint point) const;
//     View coordinates are the layout's virtual DIP pixels, the same space as pageRect()
//     (scroll offset not applied). view = pageRect(page).origin + pts * zoom, each edge
//     rounded to the nearest pixel. HiDPI: view space is DIP, as PdfView's paint and mouse
//     coordinates are, so the dpiScale passed to compute() does not change either mapping
//     (it only sets render resolution). pageToView on an invalid page returns {0, 0, 0, 0}.
//     viewToPage returns page -1 when the point is in no page rect (half-open), so gaps,
//     side margins and points outside the layout give -1; pt is unspecified then.
//   - struct TextBox { std::string text; RectPts bbox; bool hasSpaceAfter; int index; };
//     default-constructible, visible after including pdf/PdfDocument.h. text is UTF-8,
//     bbox in page points, index is the box's position in reading order (== its index in
//     the vector).
//   - std::vector<TextBox> PdfDocument::textBoxes(int page) const; empty for an invalid page.
//   - TextLayer (pdf/TextLayer.h) is default-constructed and is a plain per-page cache. It
//     never loads text itself:
//         void setBoxes(int page, std::vector<TextBox> boxes);
//         bool hasPage(int page) const;
//         const std::vector<TextBox>* boxes(int page) const;   // nullptr when not loaded
//         void clear();
//         int boxAt(int page, PointPts pointPts) const;        // box index, or -1
//         std::string textBetween(int page, int fromIdx, int toIdx) const;
//     boxAt returns the index of the box whose bbox contains the point, -1 for no box or a
//     page not loaded. textBetween includes both ends; fromIdx > toIdx is swapped (a
//     backward selection drag). A page not loaded gives "".
//   - Joining rule: between box i and box i + 1 insert "\n" when box i+1's baseline
//     (bbox.y + bbox.h) is lower than box i's by more than half of box i's bbox.h (the
//     line height); otherwise " " when box i has hasSpaceAfter, otherwise nothing. The
//     newline replaces the space. Nothing is appended after the last box of the range.
//     The synthetic tests use equal heights, so taking the next box's height also passes.
//   - Lazy loading (spec 3): RenderRequest gains `bool withText = false;` and RenderResult
//     gains `std::optional<std::vector<TextBox>> textBoxes;`. The worker calls
//     textBoxes(page) on its own PdfDocument only when the request has withText, and puts
//     them in the same RenderResult as the image. Otherwise textBoxes is std::nullopt.
//     PdfView asks for text when its TextLayer lacks the page; that GUI wiring is not
//     tested here. The tests do check that the result (with its boxes) is queued from the
//     worker thread, not the test thread.
//   - The test sink overrides wxEvtHandler::QueueEvent (as in test_open_files.cpp), so the
//     tests need no wxApp and no event loop.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PageLayout.h"
#include "pdf/PdfDocument.h"
#include "pdf/RenderWorker.h"
#include "pdf/TextLayer.h"

#include <wx/event.h>
#include <wx/image.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

bool near(double a, double b, double tol)
{
    return std::abs(a - b) <= tol + 1e-9;
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

RectPts rectPts(double x, double y, double w, double h)
{
    RectPts r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

PointPts pointPts(double x, double y)
{
    PointPts p;
    p.x = x;
    p.y = y;
    return p;
}

PointPts center(const RectPts& r)
{
    return pointPts(r.x + r.w / 2, r.y + r.h / 2);
}

PageLayout makeLayout(const std::vector<PageSizePts>& sizes, double zoom, int viewportWidth,
                      double dpiScale)
{
    PageLayout l;
    l.compute(sizes, zoom, viewportWidth, dpiScale);
    return l;
}

// Text lines of the page: "Page One", three paragraph lines (wrapped here), the page number.
const char* const kPage0Words[] = {
    "Page", "One",
    "The", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog.", "This", "sample",
        "document", "exists", "to",
    "test", "PDF", "loading,", "rendering,", "and", "text", "extraction", "in", "Agentic",
        "Reader.", "Marker",
    "word:", "zebra.",
    "1",
};
constexpr int kZebra = 28;

const std::string kParagraph0 =
    "The quick brown fox jumps over the lazy dog. This sample document exists to\n"
    "test PDF loading, rendering, and text extraction in Agentic Reader. Marker\n"
    "word: zebra.";

// Collects RenderResults that a RenderWorker posts from its thread.
class CaptureSink : public wxEvtHandler
{
public:
    void QueueEvent(wxEvent* event) override
    {
        const std::unique_ptr<wxEvent> owned(event);
        auto* threadEvent = dynamic_cast<wxThreadEvent*>(event);
        if (!threadEvent || threadEvent->GetEventType() != EVT_PAGE_RENDERED)
            return;
        std::lock_guard lock(m_mutex);
        m_results.push_back(threadEvent->GetPayload<RenderResult>());
        m_threads.push_back(std::this_thread::get_id());
        m_cv.notify_all();
    }

    // Waits until `count` results arrived. Returns them, or fewer on timeout.
    std::vector<RenderResult> waitFor(std::size_t count)
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::seconds(20),
                      [&] { return m_results.size() >= count; });
        return m_results;
    }

    std::vector<std::thread::id> threads()
    {
        std::lock_guard lock(m_mutex);
        return m_threads;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<RenderResult> m_results;
    std::vector<std::thread::id> m_threads;
};

RenderRequest request(int page, double scale, unsigned generation, bool withText)
{
    RenderRequest r{page, scale, generation};
    r.withText = withText;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Spec 1 and 2: PdfDocument::textBoxes

TEST_CASE("text_layer textBoxes lists sample.pdf page 0 words in reading order")
{
    auto doc = openOk("sample.pdf");
    const std::vector<TextBox> boxes = doc->textBoxes(0);
    REQUIRE(boxes.size() == std::size(kPage0Words));
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        CAPTURE(i);
        CHECK(boxes[i].text == kPage0Words[i]);
        CHECK(boxes[i].index == static_cast<int>(i));
    }
}

TEST_CASE("text_layer textBoxes gives bboxes in page points from the top-left")
{
    auto doc = openOk("sample.pdf");
    const std::vector<TextBox> boxes = doc->textBoxes(0);
    REQUIRE(boxes.size() > static_cast<std::size_t>(kZebra));

    const RectPts z = boxes[kZebra].bbox;
    CHECK(near(z.x, 144.348, 0.5));
    CHECK(near(z.y, 184.633, 0.5));
    CHECK(near(z.w, 30.567, 0.5));
    CHECK(near(z.h, 10.616, 0.5));

    // The title sits near the top of the page, the page number near the bottom.
    const RectPts title = boxes[0].bbox;
    CHECK(near(title.x, 110.854, 0.5));
    CHECK(near(title.y, 125.806, 0.5));
    CHECK(boxes.back().bbox.y > 600);

    for (const TextBox& b : boxes) {
        CAPTURE(b.text);
        CHECK(b.bbox.w > 0);
        CHECK(b.bbox.h > 0);
        CHECK(b.bbox.x >= 0);
        CHECK(b.bbox.y >= 0);
        CHECK(b.bbox.x + b.bbox.w <= 612);
        CHECK(b.bbox.y + b.bbox.h <= 792);
    }
}

TEST_CASE("text_layer textBoxes reports has_space_after")
{
    auto doc = openOk("sample.pdf");
    const std::vector<TextBox> boxes = doc->textBoxes(0);
    REQUIRE(boxes.size() == std::size(kPage0Words));
    CHECK(boxes[0].hasSpaceAfter);        // "Page"
    CHECK_FALSE(boxes[1].hasSpaceAfter);  // "One", end of the title line
    CHECK(boxes[2].hasSpaceAfter);        // "The"
    CHECK(boxes[10].hasSpaceAfter);       // "dog."
    CHECK_FALSE(boxes[15].hasSpaceAfter); // "to", end of a line
    CHECK_FALSE(boxes[kZebra].hasSpaceAfter);
}

TEST_CASE("text_layer textBoxes reads each page separately")
{
    auto doc = openOk("sample.pdf");
    const std::vector<TextBox> p1 = doc->textBoxes(1);
    REQUIRE(p1.size() == 20);
    CHECK(p1[0].text == "Page");
    CHECK(p1[1].text == "Two");
    CHECK(p1[2].text == "Attention");
    CHECK(p1[18].text == "apart.");

    const std::vector<TextBox> p2 = doc->textBoxes(2);
    REQUIRE(p2.size() == 12);
    CHECK(p2[1].text == "Three");
    CHECK(p2[10].text == "document.");
}

TEST_CASE("text_layer textBoxes is empty for an invalid page")
{
    auto doc = openOk("sample.pdf");
    CHECK(doc->textBoxes(-1).empty());
    CHECK(doc->textBoxes(3).empty());
}

TEST_CASE("text_layer textBoxes on a rotated page uses the displayed orientation")
{
    auto doc = openOk("rotated.pdf");
    const wxRealPoint size = doc->pageSizePts(0);
    REQUIRE(near(size.x, 792, 0.5));
    REQUIRE(near(size.y, 612, 0.5));

    const std::vector<TextBox> boxes = doc->textBoxes(0);
    REQUIRE(boxes.size() == std::size(kPage0Words));
    CHECK(boxes[kZebra].text == "zebra.");
    for (const TextBox& b : boxes) {
        CAPTURE(b.text);
        CHECK(b.bbox.x >= 0);
        CHECK(b.bbox.y >= 0);
        CHECK(b.bbox.x + b.bbox.w <= size.x + 0.5);
        CHECK(b.bbox.y + b.bbox.h <= size.y + 0.5);
    }
    // Rotated 90 degrees clockwise: the text runs down the page, so words are taller than wide.
    CHECK(boxes[kZebra].bbox.h > boxes[kZebra].bbox.w);
}

TEST_CASE("text_layer textBoxes converts non-ASCII text to UTF-8")
{
    auto doc = openOk("utf8.pdf");
    const std::vector<TextBox> boxes = doc->textBoxes(0);
    REQUIRE(boxes.size() == 4);
    CHECK(boxes[0].text == "Caf\xC3\xA9");             // Café
    CHECK(boxes[1].text == "na\xC3\xAFve");            // naïve
    CHECK(boxes[2].text == "Gr\xC3\xB6\xC3\x9F" "e");  // Größe
    CHECK(boxes[3].text == "\xE2\x82\xAC" "5");        // €5
    CHECK(boxes[0].hasSpaceAfter);
    CHECK_FALSE(boxes[3].hasSpaceAfter);
}

// ---------------------------------------------------------------------------
// Spec 4: PageLayout::pageToView and viewToPage

TEST_CASE("text_layer pageToView maps points by zoom and page offset")
{
    // zoom 2, viewport 1000: totalWidth = 1224 + 24 = 1248.
    // Page 0 rect {12, 12, 1224, 1584}. Page 1 (306 x 396) rect {318, 1608, 612, 792}.
    const PageLayout l = makeLayout({{612, 792}, {306, 396}}, 2.0, 1000, 1.0);
    REQUIRE(l.pageRect(1).x == 318);
    REQUIRE(l.pageRect(1).y == 1608);

    const PageRect v = l.pageToView(1, rectPts(100, 50, 30, 10));
    CHECK(near(v.x, 518, 1));
    CHECK(near(v.y, 1708, 1));
    CHECK(near(v.w, 60, 1));
    CHECK(near(v.h, 20, 1));

    const PageRect v0 = l.pageToView(0, rectPts(10, 20, 5, 5));
    CHECK(near(v0.x, 32, 1));
    CHECK(near(v0.y, 52, 1));
    CHECK(near(v0.w, 10, 1));
    CHECK(near(v0.h, 10, 1));
}

TEST_CASE("text_layer pageToView of the whole page is the page rect")
{
    const std::vector<PageSizePts> sizes = {{612, 792}, {300, 400}, {792, 612}};
    for (double zoom : {0.25, 0.5, 1.0, 1.37, 2.0, 3.5}) {
        const PageLayout l = makeLayout(sizes, zoom, 900, 1.0);
        for (int page = 0; page < 3; ++page) {
            CAPTURE(zoom);
            CAPTURE(page);
            const auto& s = sizes[static_cast<std::size_t>(page)];
            const PageRect v = l.pageToView(page, rectPts(0, 0, s.w, s.h));
            const PageRect r = l.pageRect(page);
            CHECK(near(v.x, r.x, 1));
            CHECK(near(v.y, r.y, 1));
            CHECK(near(v.x + v.w, r.x + r.w, 1));
            CHECK(near(v.y + v.h, r.y + r.h, 1));
        }
    }
}

TEST_CASE("text_layer viewToPage finds the page and the point in points")
{
    const PageLayout l = makeLayout({{612, 792}, {306, 396}}, 2.0, 1000, 1.0);

    const PagePoint p = l.viewToPage(ViewPoint{518, 1708});
    CHECK(p.page == 1);
    CHECK(near(p.pt.x, 100, 0.5));
    CHECK(near(p.pt.y, 50, 0.5));

    const PagePoint q = l.viewToPage(ViewPoint{12, 12});
    CHECK(q.page == 0);
    CHECK(near(q.pt.x, 0, 0.5));
    CHECK(near(q.pt.y, 0, 0.5));
}

TEST_CASE("text_layer viewToPage returns no page outside the pages")
{
    const PageLayout l = makeLayout({{612, 792}, {306, 396}}, 2.0, 1000, 1.0);
    CHECK(l.viewToPage(ViewPoint{500, 5}).page == -1);     // top margin
    CHECK(l.viewToPage(ViewPoint{500, 1600}).page == -1);  // gap between pages
    CHECK(l.viewToPage(ViewPoint{300, 1708}).page == -1);  // left of the narrower page 1
    CHECK(l.viewToPage(ViewPoint{1000, 1708}).page == -1); // right of page 1
    CHECK(l.viewToPage(ViewPoint{500, l.totalHeight() - 1}).page == -1); // bottom margin
    CHECK(l.viewToPage(ViewPoint{500, l.totalHeight() + 50}).page == -1);
    CHECK(l.viewToPage(ViewPoint{-5, 100}).page == -1);

    const PageLayout empty = makeLayout({}, 1.0, 800, 1.0);
    CHECK(empty.viewToPage(ViewPoint{10, 10}).page == -1);
}

TEST_CASE("text_layer pageToView on an invalid page returns an empty rect")
{
    const PageLayout l = makeLayout({{612, 792}}, 1.0, 800, 1.0);
    for (int page : {-1, 1}) {
        const PageRect v = l.pageToView(page, rectPts(10, 10, 20, 20));
        CHECK(v.x == 0);
        CHECK(v.y == 0);
        CHECK(v.w == 0);
        CHECK(v.h == 0);
    }
}

TEST_CASE("text_layer viewToPage(pageToView(r)) returns r within 1px")
{
    // Mixed sizes so pages have different x offsets; viewport 300 is narrower than the
    // widest page at most zooms, 2000 is wider.
    const std::vector<PageSizePts> sizes = {{612, 792}, {420, 540}, {792, 612}, {612, 792}};
    // Interior rects (at least 8 pt inside every page), including the
    // real word boxes of "Page", "dog." and "zebra." from sample.pdf.
    const std::vector<RectPts> rects = {
        rectPts(110.854, 125.806, 40.632, 15.287),
        rectPts(315.369, 155.741, 21.462, 10.616),
        rectPts(144.348, 184.633, 30.567, 10.616),
        rectPts(8, 8, 1.3, 0.7),
        rectPts(250.25, 333.75, 33.3, 44.4),
        rectPts(17.1, 300.9, 270.2, 90.3),
    };

    for (double dpi : {1.0, 1.25, 1.5, 2.0}) {
        for (double zoom : {0.25, 0.5, 0.8, 1.0, 1.37, 2.0, 3.5, 8.0}) {
            for (int viewport : {300, 2000}) {
                const PageLayout l = makeLayout(sizes, zoom, viewport, dpi);
                for (int page = 0; page < static_cast<int>(sizes.size()); ++page) {
                    for (const RectPts& r : rects) {
                        CAPTURE(dpi);
                        CAPTURE(zoom);
                        CAPTURE(viewport);
                        CAPTURE(page);
                        CAPTURE(r.x);
                        CAPTURE(r.y);
                        const PageRect v = l.pageToView(page, r);

                        const PagePoint tl = l.viewToPage(ViewPoint{v.x, v.y});
                        REQUIRE(tl.page == page);
                        // 1px in view space is 1 / zoom points.
                        CHECK(std::abs(tl.pt.x - r.x) * zoom <= 1.0 + 1e-9);
                        CHECK(std::abs(tl.pt.y - r.y) * zoom <= 1.0 + 1e-9);

                        const PagePoint br = l.viewToPage(ViewPoint{v.x + v.w, v.y + v.h});
                        REQUIRE(br.page == page);
                        CHECK(std::abs(br.pt.x - (r.x + r.w)) * zoom <= 1.0 + 1e-9);
                        CHECK(std::abs(br.pt.y - (r.y + r.h)) * zoom <= 1.0 + 1e-9);
                    }
                }
            }
        }
    }
}

TEST_CASE("text_layer mapping is in DIPs and ignores the HiDPI scale")
{
    const std::vector<PageSizePts> sizes = {{612, 792}, {300, 400}};
    const RectPts r = rectPts(144.348, 184.633, 30.567, 10.616);
    for (double zoom : {0.5, 1.0, 1.5, 3.0}) {
        const PageLayout base = makeLayout(sizes, zoom, 1000, 1.0);
        for (double dpi : {1.25, 1.5, 2.0, 3.0}) {
            CAPTURE(zoom);
            CAPTURE(dpi);
            const PageLayout hi = makeLayout(sizes, zoom, 1000, dpi);
            for (int page = 0; page < 2; ++page) {
                const PageRect a = base.pageToView(page, r);
                const PageRect b = hi.pageToView(page, r);
                CHECK(a.x == b.x);
                CHECK(a.y == b.y);
                CHECK(a.w == b.w);
                CHECK(a.h == b.h);

                const PagePoint pa = base.viewToPage(ViewPoint{a.x + 3, a.y + 2});
                const PagePoint pb = hi.viewToPage(ViewPoint{a.x + 3, a.y + 2});
                CHECK(pa.page == pb.page);
                CHECK(near(pa.pt.x, pb.pt.x, 1e-6));
                CHECK(near(pa.pt.y, pb.pt.y, 1e-6));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Spec 3 and 5: TextLayer cache, boxAt, textBetween

TEST_CASE("text_layer TextLayer caches boxes per page")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    CHECK_FALSE(layer.hasPage(0));
    CHECK(layer.boxes(0) == nullptr);

    layer.setBoxes(0, doc->textBoxes(0));
    layer.setBoxes(2, doc->textBoxes(2));
    CHECK(layer.hasPage(0));
    CHECK_FALSE(layer.hasPage(1));
    CHECK(layer.hasPage(2));
    REQUIRE(layer.boxes(0) != nullptr);
    CHECK(layer.boxes(0)->size() == std::size(kPage0Words));
    REQUIRE(layer.boxes(2) != nullptr);
    CHECK(layer.boxes(2)->size() == 12);
    CHECK(layer.boxes(1) == nullptr);

    // A page with no text is still loaded.
    layer.setBoxes(1, {});
    CHECK(layer.hasPage(1));
    REQUIRE(layer.boxes(1) != nullptr);
    CHECK(layer.boxes(1)->empty());

    layer.clear();
    CHECK_FALSE(layer.hasPage(0));
    CHECK_FALSE(layer.hasPage(1));
    CHECK_FALSE(layer.hasPage(2));
    CHECK(layer.boxes(0) == nullptr);
}

TEST_CASE("text_layer boxAt finds a known word in sample.pdf")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));

    CHECK(layer.boxAt(0, pointPts(159.6, 190.0)) == kZebra); // middle of "zebra."
    CHECK(layer.boxAt(0, pointPts(145.0, 186.0)) == kZebra); // near its top-left
    CHECK(layer.boxAt(0, pointPts(131.0, 133.0)) == 0); // "Page"
    CHECK(layer.boxAt(0, pointPts(205.0, 161.0)) == 5); // "fox"
    CHECK(layer.boxAt(0, pointPts(50.0, 50.0)) == -1); // margin
    CHECK(layer.boxAt(0, pointPts(141.7, 190.0)) == -1); // between "word:" and "zebra."
    CHECK(layer.boxAt(0, pointPts(159.6, 230.0)) == -1); // below the paragraph
    CHECK(layer.boxAt(1, pointPts(159.6, 190.0)) == -1); // page not loaded
}

TEST_CASE("text_layer boxAt finds the word under a view point at any zoom")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(1, doc->textBoxes(1));
    const RectPts attention = (*layer.boxes(1))[2].bbox;

    std::vector<PageSizePts> sizes;
    for (int i = 0; i < doc->pageCount(); ++i) {
        const wxRealPoint s = doc->pageSizePts(i);
        sizes.push_back({s.x, s.y});
    }
    for (double dpi : {1.0, 2.0}) {
        for (double zoom : {0.5, 1.0, 1.75, 4.0}) {
            CAPTURE(dpi);
            CAPTURE(zoom);
            const PageLayout l = makeLayout(sizes, zoom, 1200, dpi);
            const PageRect v = l.pageToView(1, attention);
            const PagePoint p = l.viewToPage(ViewPoint{v.x + v.w / 2, v.y + v.h / 2});
            REQUIRE(p.page == 1);
            CHECK(layer.boxAt(p.page, p.pt) == 2);
        }
    }
}

TEST_CASE("text_layer textBetween returns the expected sentence")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));
    layer.setBoxes(1, doc->textBoxes(1));

    CHECK(layer.textBetween(0, 2, 10) == "The quick brown fox jumps over the lazy dog.");
    CHECK(layer.textBetween(1, 2, 6) == "Attention is all you need.");
    CHECK(layer.textBetween(0, 0, 1) == "Page One");
    CHECK(layer.textBetween(0, kZebra, kZebra) == "zebra.");
}

TEST_CASE("text_layer textBetween joins lines with newlines")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));

    CHECK(layer.textBetween(0, 2, kZebra) == kParagraph0);
    CHECK(layer.textBetween(0, 1, 2) == "One\nThe");
    CHECK(layer.textBetween(0, 15, 16) == "to\ntest");
    CHECK(layer.textBetween(0, 0, 29) == "Page One\n" + kParagraph0 + "\n1");
}

TEST_CASE("text_layer textBetween accepts a backward range")
{
    auto doc = openOk("sample.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));
    CHECK(layer.textBetween(0, 10, 2) == "The quick brown fox jumps over the lazy dog.");
}

TEST_CASE("text_layer textBetween is empty for a page not loaded")
{
    TextLayer layer;
    CHECK(layer.textBetween(0, 0, 3) == "");
}

TEST_CASE("text_layer textBetween keeps UTF-8 text")
{
    auto doc = openOk("utf8.pdf");
    TextLayer layer;
    layer.setBoxes(0, doc->textBoxes(0));
    CHECK(layer.textBetween(0, 0, 3) ==
          "Caf\xC3\xA9 na\xC3\xAFve Gr\xC3\xB6\xC3\x9F" "e \xE2\x82\xAC" "5");
}

TEST_CASE("text_layer joining rule on synthetic boxes")
{
    TextLayer layer;

    SUBCASE("no space flag and same baseline concatenates")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, false, 0),
                           makeBox("bar", 20, 100, 20, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foobar");
    }
    SUBCASE("space flag adds one space")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 25, 100, 20, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foo bar");
    }
    SUBCASE("baseline lower by less than half the line height stays on the line")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 25, 104, 20, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foo bar");
    }
    SUBCASE("baseline higher stays on the line")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, false, 0),
                           makeBox("2", 20, 95, 5, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foo2");
    }
    SUBCASE("baseline lower by more than half the line height starts a new line")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, false, 0),
                           makeBox("bar", 0, 106, 20, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foo\nbar");
    }
    SUBCASE("newline replaces the space")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 0, 114, 20, 10, false, 1)});
        CHECK(layer.textBetween(0, 0, 1) == "foo\nbar");
    }
    SUBCASE("nothing follows the last box of the range")
    {
        layer.setBoxes(0, {makeBox("foo", 0, 100, 20, 10, true, 0),
                           makeBox("bar", 25, 100, 20, 10, true, 1),
                           makeBox("baz", 0, 120, 20, 10, false, 2)});
        CHECK(layer.textBetween(0, 0, 0) == "foo");
        CHECK(layer.textBetween(0, 0, 1) == "foo bar");
        CHECK(layer.textBetween(0, 0, 2) == "foo bar\nbaz");
    }
}

// ---------------------------------------------------------------------------
// Spec 3: boxes are loaded on the render worker and posted with the image

TEST_CASE("text_layer RenderWorker posts text boxes with the image when asked")
{
    auto doc = openOk("sample.pdf");
    const std::vector<TextBox> expected = doc->textBoxes(0);

    CaptureSink sink;
    std::vector<RenderResult> results;
    {
        RenderWorker worker(&sink, dataFile("sample.pdf"));
        worker.setRequests({request(0, 0.5, 1, true), request(1, 0.5, 1, false)});
        results = sink.waitFor(2);
    }
    REQUIRE(results.size() == 2);

    const RenderResult& withText = results[0];
    CHECK(withText.page == 0);
    CHECK(withText.image.IsOk());
    REQUIRE(withText.textBoxes.has_value());
    REQUIRE(withText.textBoxes->size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK((*withText.textBoxes)[i].text == expected[i].text);
        CHECK((*withText.textBoxes)[i].index == expected[i].index);
        CHECK(near((*withText.textBoxes)[i].bbox.x, expected[i].bbox.x, 1e-6));
        CHECK(near((*withText.textBoxes)[i].bbox.y, expected[i].bbox.y, 1e-6));
    }

    // Lazy: no text unless the request asks for it.
    const RenderResult& imageOnly = results[1];
    CHECK(imageOnly.page == 1);
    CHECK(imageOnly.image.IsOk());
    CHECK_FALSE(imageOnly.textBoxes.has_value());

    // Both results, boxes included, were queued from the worker thread.
    for (const std::thread::id& id : sink.threads())
        CHECK(id != std::this_thread::get_id());
}

TEST_CASE("text_layer boxes from the render worker fill the TextLayer")
{
    CaptureSink sink;
    std::vector<RenderResult> results;
    {
        RenderWorker worker(&sink, dataFile("sample.pdf"));
        worker.setRequests({request(0, 1.0, 7, true)});
        results = sink.waitFor(1);
    }
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].textBoxes.has_value());

    TextLayer layer;
    layer.setBoxes(results[0].page, std::move(*results[0].textBoxes));
    REQUIRE(layer.hasPage(0));
    CHECK(layer.boxAt(0, pointPts(159.6, 190.0)) == kZebra);
    CHECK(layer.textBetween(0, 2, 10) == "The quick brown fox jumps over the lazy dog.");
}
