// M2.1: spec tests for PdfDocument (app/src/pdf/PdfDocument.h).
//
// Test data (tests/data/, reached through the compile definition TEST_DATA_DIR):
//   sample.pdf     3 pages, 612 x 792 pt, black text on white (see test_poppler.cpp).
//                  Pixel (5,5) at scale 1 is in the top-left margin and is white.
//   locked.pdf     sample.pdf encrypted with AES-256, user password "secret",
//                  owner password "ownerpw":
//                    qpdf --encrypt secret ownerpw 256 -- sample.pdf locked.pdf
//   rotated.pdf    sample.pdf with /Rotate 90 on page index 0 only:
//                    qpdf --rotate=+90:1 sample.pdf rotated.pdf
//   color.pdf      1 page, 200 x 100 pt. Left half (x < 100 pt) filled pure red,
//                  right half filled pure blue. Hand-written content stream
//                  "1 0 0 rg 0 0 100 100 re f / 0 0 1 rg 100 0 100 100 re f",
//                  rewritten with qpdf --deterministic-id for a valid xref.
//   not-a-pdf.pdf  38 bytes of plain text with a .pdf extension.
//
// Interface choices where the spec is silent:
//   - Page indices are 0-based, matching poppler.
//   - open() returns nullptr on failure and sets `error` to exactly one of
//     "file not found", "not a PDF", "password required". The test does not
//     check `error` on success.
//   - A wrong password on a locked document also reports "password required".
//   - The password unlocks a document encrypted with that user password.
//   - wxSize2D does not exist in wxWidgets 3.3.3 (the vcpkg version). pageSizePts()
//     returns wxRealPoint instead, with x = width and y = height in points.
//   - path() returns something comparable with std::filesystem::path and equal
//     to the path passed to open().
//   - renderPage() returns a wxImage that has an alpha channel. Its size is
//     ceil(pageSizePts * scale) per axis. Poppler's default render size rounds
//     to nearest, so scale 1.1 on a 612 x 792 page (673.2 x 871.2) expects
//     674 x 872, not 673 x 871.
//   - The tests use no wxApp. wxImage works without one.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PdfDocument.h"

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

std::filesystem::path dataFile(const char* name)
{
    return std::filesystem::path(TEST_DATA_DIR) / name;
}

std::unique_ptr<PdfDocument> openOk(const char* name, const std::string& password = {})
{
    std::string error;
    auto doc = PdfDocument::open(dataFile(name), password, error);
    INFO("open error: " << error);
    REQUIRE(doc != nullptr);
    return doc;
}

std::string openError(const std::filesystem::path& path, const std::string& password = {})
{
    std::string error;
    const auto doc = PdfDocument::open(path, password, error);
    CHECK(doc == nullptr);
    return error;
}

int ceilSize(double pts, double scale)
{
    return static_cast<int>(std::ceil(pts * scale));
}

void checkRgba(const wxImage& img, int x, int y, int r, int g, int b)
{
    INFO("pixel (" << x << ", " << y << ")");
    CHECK(img.GetRed(x, y) == r);
    CHECK(img.GetGreen(x, y) == g);
    CHECK(img.GetBlue(x, y) == b);
    CHECK(img.GetAlpha(x, y) == 255);
}

bool hasNonWhitePixel(const wxImage& img)
{
    for (int y = 0; y < img.GetHeight(); ++y) {
        for (int x = 0; x < img.GetWidth(); ++x) {
            if (img.GetRed(x, y) != 255 || img.GetGreen(x, y) != 255
                || img.GetBlue(x, y) != 255) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("pdf_document opens sample.pdf with 3 pages and stores its path")
{
    const auto doc = openOk("sample.pdf");
    CHECK(doc->pageCount() == 3);
    CHECK(std::filesystem::path(doc->path()) == dataFile("sample.pdf"));
}

TEST_CASE("pdf_document reports the page size of sample.pdf in points")
{
    const auto doc = openOk("sample.pdf");
    for (int page = 0; page < 3; ++page) {
        INFO("page " << page);
        const wxRealPoint size = doc->pageSizePts(page);
        CHECK(size.x == doctest::Approx(612.0));
        CHECK(size.y == doctest::Approx(792.0));
    }
}

TEST_CASE("pdf_document reports swapped sizes for a page rotated 90 degrees")
{
    const auto doc = openOk("rotated.pdf");
    const wxRealPoint rotated = doc->pageSizePts(0);
    CHECK(rotated.x == doctest::Approx(792.0));
    CHECK(rotated.y == doctest::Approx(612.0));

    const wxRealPoint upright = doc->pageSizePts(1);
    CHECK(upright.x == doctest::Approx(612.0));
    CHECK(upright.y == doctest::Approx(792.0));
}

TEST_CASE("pdf_document render size equals ceil(pts * scale)")
{
    const auto doc = openOk("sample.pdf");
    const wxRealPoint size = doc->pageSizePts(0);

    for (const double scale : {1.0, 0.5, 2.0, 1.1}) {
        INFO("scale " << scale);
        const wxImage img = doc->renderPage(0, scale);
        REQUIRE(img.IsOk());
        CHECK(img.GetWidth() == ceilSize(size.x, scale));
        CHECK(img.GetHeight() == ceilSize(size.y, scale));
    }
}

TEST_CASE("pdf_document render size of a rotated page follows the swapped size")
{
    const auto doc = openOk("rotated.pdf");
    for (const double scale : {1.0, 1.1}) {
        INFO("scale " << scale);
        const wxImage img = doc->renderPage(0, scale);
        REQUIRE(img.IsOk());
        CHECK(img.GetWidth() == ceilSize(792.0, scale));
        CHECK(img.GetHeight() == ceilSize(612.0, scale));
    }
}

TEST_CASE("pdf_document renders a known white pixel as opaque white and draws text")
{
    const auto doc = openOk("sample.pdf");
    const wxImage img = doc->renderPage(0, 1.0);
    REQUIRE(img.IsOk());
    REQUIRE(img.HasAlpha());
    checkRgba(img, 5, 5, 255, 255, 255);
    CHECK(hasNonWhitePixel(img));
}

TEST_CASE("pdf_document converts BGRA to RGB without swapping channels")
{
    const auto doc = openOk("color.pdf");
    for (const double scale : {1.0, 2.0}) {
        INFO("scale " << scale);
        const wxImage img = doc->renderPage(0, scale);
        REQUIRE(img.IsOk());
        REQUIRE(img.HasAlpha());
        CHECK(img.GetWidth() == ceilSize(200.0, scale));
        CHECK(img.GetHeight() == ceilSize(100.0, scale));
        const int mid = static_cast<int>(50 * scale);
        checkRgba(img, mid, mid, 255, 0, 0);
        checkRgba(img, static_cast<int>(150 * scale), mid, 0, 0, 255);
    }
}

TEST_CASE("pdf_document reports a missing file as file not found")
{
    CHECK(openError(dataFile("does-not-exist.pdf")) == "file not found");
}

TEST_CASE("pdf_document reports a non-PDF file as not a PDF")
{
    CHECK(openError(dataFile("not-a-pdf.pdf")) == "not a PDF");
}

TEST_CASE("pdf_document reports a locked document as password required")
{
    CHECK(openError(dataFile("locked.pdf")) == "password required");
    CHECK(openError(dataFile("locked.pdf"), "wrong") == "password required");
}

TEST_CASE("pdf_document opens a locked document with the right password")
{
    const auto doc = openOk("locked.pdf", "secret");
    CHECK(doc->pageCount() == 3);
    const wxImage img = doc->renderPage(0, 1.0);
    REQUIRE(img.IsOk());
    CHECK(img.GetWidth() == 612);
    CHECK(img.GetHeight() == 792);
}
