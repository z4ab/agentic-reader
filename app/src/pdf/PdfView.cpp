#include "pdf/PdfView.h"

#include "pdf/PdfDocument.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>

namespace {
constexpr int kScrollStep = 20;
const wxColour kBackground(0x80, 0x80, 0x80);
const wxColour kBorder(0x40, 0x40, 0x40);
} // namespace

PdfView::PdfView(wxWindow* parent)
    : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL | wxHSCROLL | wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(kScrollStep, kScrollStep);
    Bind(wxEVT_PAINT, &PdfView::onPaint, this);
    Bind(wxEVT_SIZE, &PdfView::onSize, this);
    Bind(wxEVT_DPI_CHANGED, &PdfView::onDpiChanged, this);
}

PdfView::~PdfView() = default;

bool PdfView::openFile(const std::filesystem::path& path, std::string& error)
{
    auto doc = PdfDocument::open(path, "", error);
    if (!doc)
        return false;

    m_doc = std::move(doc);
    m_sizes.clear();
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const wxRealPoint pts = m_doc->pageSizePts(i);
        m_sizes.push_back({pts.x, pts.y});
    }
    m_cache.clear();
    Scroll(0, 0);
    relayout();
    return true;
}

void PdfView::relayout()
{
    m_layout.compute(m_sizes, m_zoom, GetClientSize().x, GetDPIScaleFactor());
    SetVirtualSize(m_layout.totalWidth(), m_layout.totalHeight());
    Refresh();
}

void PdfView::onSize(wxSizeEvent& event)
{
    relayout();
    event.Skip();
}

void PdfView::onDpiChanged(wxDPIChangedEvent& event)
{
    m_cache.clear();
    relayout();
    event.Skip();
}

void PdfView::onPaint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);

    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const wxSize client = GetClientSize();
    dc.SetBackground(wxBrush(kBackground));
    dc.Clear();

    if (!m_doc)
        return;

    const double renderScale = m_zoom * GetDPIScaleFactor();
    const PageRect view{origin.x, origin.y, client.x, client.y};
    for (int page : m_layout.visiblePages(view)) {
        const PageRect r = m_layout.pageRect(page);
        dc.SetPen(wxPen(kBorder, 1));
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawRectangle(r.x - 1, r.y - 1, r.w + 2, r.h + 2);

        auto it = m_cache.find(page);
        if (it == m_cache.end()) {
            const wxImage img = m_doc->renderPage(page, renderScale);
            if (!img.IsOk())
                continue;
            wxBitmap bmp(img);
            // Logical size equals the page rect, physical pixels stay at full DPI.
            bmp.SetScaleFactor(GetDPIScaleFactor());
            it = m_cache.emplace(page, bmp).first;
        }
        dc.DrawBitmap(it->second, r.x, r.y, true);
    }
}
