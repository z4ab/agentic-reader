#pragma once

#include "pdf/PageLayout.h"

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace poppler {
class document;
}

// One word of a page's text layer, as poppler's text_list() reports it.
struct TextBox
{
    // UTF-8.
    std::string text;
    // Page points, origin at the top-left of the page as displayed.
    RectPts bbox{0, 0, 0, 0};
    bool hasSpaceAfter = false;
    // Position in reading order, equal to the box's index in its page's vector.
    int index = 0;
};

// Owns one poppler document and answers what the PDF view needs.
// Page indices are 0-based.
//
// Not thread-safe. Use each instance from one thread only. Threads that render
// in parallel open their own instance.
class PdfDocument
{
public:
    // Returns nullptr on failure and sets `error` to "file not found",
    // "not a PDF" or "password required". `password` is tried as both the
    // user and the owner password. A wrong password reports "password required".
    static std::unique_ptr<PdfDocument> open(const std::filesystem::path& path,
                                             const std::string& password,
                                             std::string& error);

    ~PdfDocument();

    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    int pageCount() const;

    // Crop box size in points (the area poppler renders), x = width and y = height. Pages rotated by 90
    // or 270 degrees report swapped sizes. Returns (0, 0) for an invalid page.
    wxRealPoint pageSizePts(int page) const;

    // Renders at 72 * scale dpi. The image has an alpha channel and measures
    // ceil(pageSizePts(page) * scale) per axis. Returns an invalid image for an
    // invalid page or a failed render.
    wxImage renderPage(int page, double scale) const;

    // The page's words in reading order. Empty for an invalid page or a page
    // without text. Slow on dense pages, so call it off the UI thread.
    std::vector<TextBox> textBoxes(int page) const;

    // Every occurrence of `query` (UTF-8) on the page in poppler's order, with
    // poppler's page::search semantics: substrings, across the spaces between
    // words of a line, case-insensitive unless `matchCase`. Rects use the
    // convention of TextBox::bbox. Empty for an invalid page or an empty query.
    // Stops early, returning what it found so far, once `cancelled` returns true.
    // Slow (each match re-reads the page's text), so call it off the UI thread.
    std::vector<RectPts> search(int page, const std::string& query, bool matchCase,
                                const std::function<bool()>& cancelled = {}) const;

private:
    PdfDocument(std::filesystem::path path, std::unique_ptr<poppler::document> doc);

    std::filesystem::path m_path;
    std::unique_ptr<poppler::document> m_doc;
};
