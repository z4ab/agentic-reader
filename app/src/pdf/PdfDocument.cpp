#include "pdf/PdfDocument.h"

#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>

namespace {

std::unique_ptr<poppler::page> loadPage(const poppler::document& doc, int page)
{
    if (page < 0 || page >= doc.pages()) {
        return nullptr;
    }
    return std::unique_ptr<poppler::page>(doc.create_page(page));
}

} // namespace

std::unique_ptr<PdfDocument> PdfDocument::open(const std::filesystem::path& path,
                                               const std::string& password,
                                               std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "file not found";
        return nullptr;
    }

    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_file(path.string(), password, password));
    if (!doc) {
        error = "not a PDF";
        return nullptr;
    }
    // poppler-cpp returns an encrypted document it could not decrypt as locked
    // instead of failing the load.
    if (doc->is_locked()) {
        error = "password required";
        return nullptr;
    }

    return std::unique_ptr<PdfDocument>(new PdfDocument(path, std::move(doc)));
}

PdfDocument::PdfDocument(std::filesystem::path path, std::unique_ptr<poppler::document> doc)
    : m_path(std::move(path))
    , m_doc(std::move(doc))
{
}

PdfDocument::~PdfDocument() = default;

int PdfDocument::pageCount() const
{
    return m_doc->pages();
}

wxRealPoint PdfDocument::pageSizePts(int page) const
{
    const auto p = loadPage(*m_doc, page);
    if (!p) {
        return {};
    }
    const poppler::rectf box = p->page_rect(poppler::media_box);
    switch (p->orientation()) {
    case poppler::page::landscape:
    case poppler::page::seascape:
        return {box.height(), box.width()};
    default:
        return {box.width(), box.height()};
    }
}

wxImage PdfDocument::renderPage(int page, double scale) const
{
    const auto p = loadPage(*m_doc, page);
    if (!p || scale <= 0.0) {
        return {};
    }

    // Pass the size explicitly: poppler rounds the default size to nearest.
    const wxRealPoint pts = pageSizePts(page);
    const int width = static_cast<int>(std::ceil(pts.x * scale));
    const int height = static_cast<int>(std::ceil(pts.y * scale));

    poppler::page_renderer renderer;
    renderer.set_image_format(poppler::image::format_argb32);
    renderer.set_render_hint(poppler::page_renderer::antialiasing);
    renderer.set_render_hint(poppler::page_renderer::text_antialiasing);
    renderer.set_render_hint(poppler::page_renderer::text_hinting);

    const double dpi = 72.0 * scale;
    const poppler::image src = renderer.render_page(p.get(), dpi, dpi, 0, 0, width, height);
    if (!src.is_valid() || src.format() != poppler::image::format_argb32) {
        return {};
    }

    const int w = src.width();
    const int h = src.height();
    wxImage img(w, h, false);
    img.SetAlpha();
    unsigned char* rgb = img.GetData();
    unsigned char* alpha = img.GetAlpha();

    // Each argb32 pixel is a native-endian 32-bit 0xAARRGGBB word (BGRA bytes on
    // little-endian). Rows may be padded, so step by bytes_per_row().
    const char* data = src.const_data();
    for (int y = 0; y < h; ++y) {
        const char* row = data + static_cast<std::ptrdiff_t>(y) * src.bytes_per_row();
        for (int x = 0; x < w; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, row + static_cast<std::ptrdiff_t>(x) * 4, 4);
            *rgb++ = static_cast<unsigned char>(pixel >> 16);
            *rgb++ = static_cast<unsigned char>(pixel >> 8);
            *rgb++ = static_cast<unsigned char>(pixel);
            *alpha++ = static_cast<unsigned char>(pixel >> 24);
        }
    }
    return img;
}
