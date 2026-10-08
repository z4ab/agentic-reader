// M1.2: prove the poppler-cpp binding links and works.
//
// Test data: tests/data/sample.pdf
//   3 pages, US Letter (612 x 792 pt), embedded Type 1 fonts (CMR12, CMBX12).
//   Generated with pdflatex from this source (no \usepackage[T1]{fontenc}, so the
//   fonts are vector Type 1, not bitmap Type 3):
//     page 1: "Page One" / "The quick brown fox jumps over the lazy dog. ...
//              Marker word: zebra."
//     page 2: "Page Two" / "Attention is all you need. ..."
//     page 3: "Page Three" / "The third and final page closes the sample document."
//   Known word asserted on page index 0: "zebra" (appears only on page 1).
//
// Interface choices where the spec is silent:
//   - The test calls poppler-cpp directly. It includes the headers as
//     <poppler-document.h>, <poppler-page.h>, <poppler-page-renderer.h>,
//     <poppler-image.h>, which is the layout poppler-cpp.pc exports
//     (-I<prefix>/include/poppler/cpp).
//   - The test target links only reader_core, so reader_core must carry
//     poppler-cpp (headers and library) as a PUBLIC dependency.
//   - The PDF path reaches the test as the compile definition SAMPLE_PDF_PATH
//     (absolute path, set in tests/CMakeLists.txt).
//   - "Non-empty image" means: valid, 612 x 792 at 72 dpi, and at least one
//     pixel that is not the white paper color (text was drawn).
//   - "Boxes containing a known word" means: some text_box on page 0 whose
//     UTF-8 text contains "zebra". Punctuation may stay attached ("zebra.").
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef SAMPLE_PDF_PATH
#error "SAMPLE_PDF_PATH must be defined by tests/CMakeLists.txt"
#endif

namespace {

std::unique_ptr<poppler::document> loadSample()
{
    return std::unique_ptr<poppler::document>(
        poppler::document::load_from_file(SAMPLE_PDF_PATH));
}

std::unique_ptr<poppler::page> loadPage(const poppler::document& doc, int index)
{
    return std::unique_ptr<poppler::page>(doc.create_page(index));
}

std::string toUtf8(const poppler::ustring& text)
{
    const poppler::byte_array bytes = text.to_utf8();
    return std::string(bytes.begin(), bytes.end());
}

bool hasNonWhitePixel(const poppler::image& img)
{
    REQUIRE(img.format() == poppler::image::format_argb32);
    const char* data = img.const_data();
    for (int y = 0; y < img.height(); ++y) {
        const char* row = data + static_cast<std::ptrdiff_t>(y) * img.bytes_per_row();
        for (int x = 0; x < img.width(); ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, row + static_cast<std::ptrdiff_t>(x) * 4, 4);
            if (pixel != 0xFFFFFFFFu) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("poppler loads the sample PDF with 3 pages")
{
    const auto doc = loadSample();
    REQUIRE(doc != nullptr);
    CHECK_FALSE(doc->is_locked());
    CHECK(doc->pages() == 3);
}

TEST_CASE("poppler renders page 0 at 72 dpi to a non-empty image")
{
    const auto doc = loadSample();
    REQUIRE(doc != nullptr);
    const auto page = loadPage(*doc, 0);
    REQUIRE(page != nullptr);

    poppler::page_renderer renderer;
    const poppler::image img = renderer.render_page(page.get(), 72.0, 72.0);

    REQUIRE(img.is_valid());
    CHECK(img.width() == 612);
    CHECK(img.height() == 792);
    REQUIRE(img.const_data() != nullptr);
    CHECK(hasNonWhitePixel(img));
}

TEST_CASE("poppler text_list on page 0 contains the known word")
{
    const auto doc = loadSample();
    REQUIRE(doc != nullptr);
    const auto page = loadPage(*doc, 0);
    REQUIRE(page != nullptr);

    const std::vector<poppler::text_box> boxes = page->text_list();
    REQUIRE_FALSE(boxes.empty());

    bool found = false;
    for (const poppler::text_box& box : boxes) {
        if (toUtf8(box.text()).find("zebra") != std::string::npos) {
            found = true;
            CHECK(box.bbox().width() > 0.0);
            CHECK(box.bbox().height() > 0.0);
            break;
        }
    }
    CHECK(found);
}
