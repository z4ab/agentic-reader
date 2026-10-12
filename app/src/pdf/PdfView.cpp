#include "pdf/PdfView.h"

#include "pdf/PdfDocument.h"
#include "pdf/RenderWorker.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include <cstddef>

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
    Bind(EVT_PAGE_RENDERED, &PdfView::onPageRendered, this);
}

PdfView::~PdfView()
{
    // Stops and joins the render thread before this handler goes away.
    m_worker.reset();
}

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
    ++m_generation;
    m_worker.reset();
    m_worker = std::make_unique<RenderWorker>(this, path);
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

double PdfView::renderScale() const
{
    return m_zoom * GetDPIScaleFactor();
}

void PdfView::invalidateRenders()
{
    ++m_generation;
    if (m_worker)
        m_worker->clear();
}

void PdfView::onSize(wxSizeEvent& event)
{
    relayout();
    event.Skip();
}

void PdfView::onDpiChanged(wxDPIChangedEvent& event)
{
    invalidateRenders();
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

    const double scale = renderScale();
    const PageRect view{origin.x, origin.y, client.x, client.y};
    const std::vector<int> visible = m_layout.visiblePages(view);
    std::vector<RenderRequest> wanted;
    for (int page : visible) {
        const PageRect r = m_layout.pageRect(page);
        dc.SetPen(wxPen(kBorder, 1));
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawRectangle(r.x - 1, r.y - 1, r.w + 2, r.h + 2);

        // A page not rendered yet stays a plain white rect until its image arrives.
        if (const wxBitmap* bmp = m_cache.get(page, scale))
            dc.DrawBitmap(*bmp, r.x, r.y, true);
        else
            wanted.push_back({page, scale, m_generation});
    }

    // Visible pages first, then one page above and one below.
    if (!visible.empty()) {
        for (int page : {visible.front() - 1, visible.back() + 1}) {
            if (page >= 0 && page < m_layout.pageCount() && !m_cache.contains(page, scale))
                wanted.push_back({page, scale, m_generation});
        }
    }
    m_worker->setRequests(std::move(wanted));
}

void PdfView::onPageRendered(wxThreadEvent& event)
{
    const auto result = event.GetPayload<RenderResult>();
    if (result.generation != m_generation || !result.image.IsOk())
        return;

    // Logical size equals the page rect, physical pixels stay at full DPI.
    wxBitmap bmp(result.image);
    bmp.SetScaleFactor(GetDPIScaleFactor());
    const std::size_t bytes =
        static_cast<std::size_t>(result.image.GetWidth()) * result.image.GetHeight() * 4;
    m_cache.put(result.page, result.scale, bmp, bytes);

    const PageRect r = m_layout.pageRect(result.page);
    RefreshRect(wxRect(CalcScrolledPosition(wxPoint(r.x - 1, r.y - 1)), wxSize(r.w + 2, r.h + 2)));
}
