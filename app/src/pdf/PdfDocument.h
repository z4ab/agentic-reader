#pragma once

#include <wx/gdicmn.h>
#include <wx/image.h>

#include <filesystem>
#include <memory>
#include <string>

namespace poppler {
class document;
}

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

private:
    PdfDocument(std::filesystem::path path, std::unique_ptr<poppler::document> doc);

    std::filesystem::path m_path;
    std::unique_ptr<poppler::document> m_doc;
};
