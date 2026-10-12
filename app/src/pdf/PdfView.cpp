#include "pdf/PdfView.h"

#include "pdf/PdfDocument.h"
#include "pdf/RenderWorker.h"

#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/settings.h>
#include <wx/textdlg.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

wxDEFINE_EVENT(EVT_PDF_PAGE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_ZOOM_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_FILE_DROPPED, wxCommandEvent);

namespace {
// One mouse wheel line. Scrolling itself is per pixel (scroll rate 1).
constexpr int kWheelLineStep = 20;
// Up/Down/Left/Right.
constexpr int kArrowStep = 40;
const wxColour kBackground(0x80, 0x80, 0x80);
const wxColour kBorder(0x40, 0x40, 0x40);

// Queues EVT_PDF_FILE_DROPPED on the view for the first dropped file.
class FileDropTarget : public wxFileDropTarget
{
public:
    explicit FileDropTarget(wxWindow* view) : m_view(view) {}

    bool OnDropFiles(wxCoord /*x*/, wxCoord /*y*/, const wxArrayString& files) override
    {
        if (files.empty())
            return false;
        auto* event = new wxCommandEvent(EVT_PDF_FILE_DROPPED, m_view->GetId());
        event->SetEventObject(m_view);
        event->SetString(files[0]);
        m_view->GetEventHandler()->QueueEvent(event);
        return true;
    }

private:
    wxWindow* m_view;
};
} // namespace

PdfView::PdfView(wxWindow* parent)
    : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL | wxHSCROLL | wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(1, 1);
    m_viewport = GetClientSize();
    SetDropTarget(new FileDropTarget(this));
    Bind(wxEVT_PAINT, &PdfView::onPaint, this);
    Bind(wxEVT_SIZE, &PdfView::onSize, this);
    Bind(wxEVT_DPI_CHANGED, &PdfView::onDpiChanged, this);
    Bind(EVT_PAGE_RENDERED, &PdfView::onPageRendered, this);
    Bind(wxEVT_KEY_DOWN, &PdfView::onKeyDown, this);
    Bind(wxEVT_MOUSEWHEEL, &PdfView::onMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &PdfView::onLeftDown, this);
    Bind(wxEVT_SCROLLWIN_TOP, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_BOTTOM, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_LINEUP, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_LINEDOWN, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_PAGEUP, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_PAGEDOWN, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_THUMBTRACK, &PdfView::onScrollWin, this);
    Bind(wxEVT_SCROLLWIN_THUMBRELEASE, &PdfView::onScrollWin, this);
}

PdfView::~PdfView()
{
    // Stops and joins the render thread before this handler goes away.
    m_worker.reset();
}

void PdfView::setDocument(std::unique_ptr<PdfDocument> doc, std::string password)
{
    m_doc = std::move(doc);
    m_path = m_doc->path();
    m_sizes.clear();
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const wxRealPoint pts = m_doc->pageSizePts(i);
        m_sizes.push_back({pts.x, pts.y});
    }
    m_currentPage = -1;
    ++m_generation;
    m_worker.reset();
    m_worker = std::make_unique<RenderWorker>(this, m_path, std::move(password));
    m_cache.clear();
    m_textLayer.clear();
    if (m_zoomMode != ZoomMode::Custom)
        m_zoom = fitZoom(m_zoomMode);
    relayout();
    scrollToOrigin(0, 0);
    sendZoomChanged();
}

void PdfView::setZoom(double zoom)
{
    applyZoom(zoom, ZoomMode::Custom, paneCenter());
}

void PdfView::fitWidth()
{
    applyZoom(fitZoom(ZoomMode::FitWidth), ZoomMode::FitWidth, paneCenter());
}

void PdfView::fitPage()
{
    const int page = m_currentPage;
    applyZoom(fitZoom(ZoomMode::FitPage), ZoomMode::FitPage, paneCenter());
    goToPage(page);
}

void PdfView::goToPage(int page)
{
    if (!m_doc)
        return;
    const PageRect r = m_layout.pageRect(std::clamp(page, 0, pageCount() - 1));
    scrollToOrigin(CalcUnscrolledPosition(wxPoint(0, 0)).x, r.y - PageLayout::kGap);
}

void PdfView::showGoToPageDialog()
{
    if (!m_doc)
        return;
    wxTextEntryDialog dialog(this, wxString::Format("Page number (1-%d):", pageCount()),
                             "Go to Page", wxString::Format("%d", m_currentPage + 1));
    if (dialog.ShowModal() != wxID_OK)
        return;
    long page = 0;
    if (dialog.GetValue().Trim().Trim(false).ToLong(&page))
        goToPage(static_cast<int>(page) - 1);
}

void PdfView::relayout()
{
    m_layout.compute(m_sizes, m_zoom, m_viewport.x, GetDPIScaleFactor());
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

PageSizePts PdfView::currentPageSize() const
{
    return m_sizes[static_cast<std::size_t>(std::max(m_currentPage, 0))];
}

double PdfView::fitZoom(ZoomMode mode) const
{
    if (m_sizes.empty())
        return m_zoom;
    const wxSize& client = m_viewport;
    switch (mode) {
    case ZoomMode::FitWidth:
        return fitWidthZoom(currentPageSize(), client.x);
    case ZoomMode::FitPage:
        return fitPageZoom(currentPageSize(), client.x, client.y);
    case ZoomMode::Custom:
        break;
    }
    return m_zoom;
}

void PdfView::applyZoom(double zoom, ZoomMode mode, ViewPoint anchor)
{
    if (!m_doc)
        return;
    m_zoomMode = mode;
    zoom = clampZoom(zoom);
    if (zoom == m_zoom)
        return;

    const PageLayout before = m_layout;
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    m_zoom = zoom;
    invalidateRenders();
    relayout();
    const ViewPoint to = zoomScrollOrigin(before, m_layout, {origin.x, origin.y}, anchor);
    scrollToOrigin(to.x, to.y);
    sendZoomChanged();
}

ViewPoint PdfView::paneCenter() const
{
    return {m_viewport.x / 2, m_viewport.y / 2};
}

void PdfView::scrollToOrigin(int x, int y)
{
    const wxSize& client = m_viewport;
    x = std::clamp(x, 0, std::max(0, m_layout.totalWidth() - client.x));
    y = std::clamp(y, 0, std::max(0, m_layout.totalHeight() - client.y));
    Scroll(x, y);
    updateCurrentPage();
}

void PdfView::scrollByPixels(int dx, int dy)
{
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    scrollToOrigin(origin.x + dx, origin.y + dy);
}

void PdfView::updateCurrentPage()
{
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const wxSize& client = m_viewport;
    const int page = mostVisiblePage(m_layout, {origin.x, origin.y, client.x, client.y});
    if (page < 0 || page == m_currentPage)
        return;

    m_currentPage = page;
    wxCommandEvent event(EVT_PDF_PAGE_CHANGED, GetId());
    event.SetEventObject(this);
    event.SetInt(page);
    ProcessWindowEvent(event);
}

void PdfView::sendZoomChanged()
{
    wxCommandEvent event(EVT_PDF_ZOOM_CHANGED, GetId());
    event.SetEventObject(this);
    ProcessWindowEvent(event);
}

void PdfView::onSize(wxSizeEvent& event)
{
    event.Skip();
    m_viewport = GetClientSize();
    if (m_doc && m_zoomMode != ZoomMode::Custom) {
        const double zoom = fitZoom(m_zoomMode);
        if (zoom != m_zoom) {
            // Keep the top edge of the view in place while the fit zoom follows the size.
            applyZoom(zoom, m_zoomMode, {m_viewport.x / 2, 0});
            return;
        }
    }
    relayout();
    updateCurrentPage();
}

void PdfView::onDpiChanged(wxDPIChangedEvent& event)
{
    invalidateRenders();
    relayout();
    event.Skip();
}

void PdfView::onKeyDown(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    const int pageStep = m_viewport.y;

    if (event.ControlDown()) {
        switch (key) {
        case '+':
        case '=':
        case WXK_NUMPAD_ADD:
            applyZoom(stepZoom(m_zoom, 1), ZoomMode::Custom, paneCenter());
            return;
        case '-':
        case WXK_NUMPAD_SUBTRACT:
            applyZoom(stepZoom(m_zoom, -1), ZoomMode::Custom, paneCenter());
            return;
        case '0':
        case WXK_NUMPAD0:
            fitWidth();
            return;
        case 'G':
            showGoToPageDialog();
            return;
        default:
            event.Skip();
            return;
        }
    }
    if (event.HasAnyModifiers()) {
        event.Skip();
        return;
    }

    switch (key) {
    case WXK_PAGEDOWN:
    case WXK_NUMPAD_PAGEDOWN:
        scrollByPixels(0, pageStep);
        break;
    case WXK_PAGEUP:
    case WXK_NUMPAD_PAGEUP:
        scrollByPixels(0, -pageStep);
        break;
    case WXK_HOME:
    case WXK_NUMPAD_HOME:
        scrollToOrigin(CalcUnscrolledPosition(wxPoint(0, 0)).x, 0);
        break;
    case WXK_END:
    case WXK_NUMPAD_END:
        scrollToOrigin(CalcUnscrolledPosition(wxPoint(0, 0)).x, m_layout.totalHeight());
        break;
    case WXK_DOWN:
    case WXK_NUMPAD_DOWN:
        scrollByPixels(0, kArrowStep);
        break;
    case WXK_UP:
    case WXK_NUMPAD_UP:
        scrollByPixels(0, -kArrowStep);
        break;
    case WXK_RIGHT:
    case WXK_NUMPAD_RIGHT:
        scrollByPixels(kArrowStep, 0);
        break;
    case WXK_LEFT:
    case WXK_NUMPAD_LEFT:
        scrollByPixels(-kArrowStep, 0);
        break;
    default:
        event.Skip();
        break;
    }
}

void PdfView::onMouseWheel(wxMouseEvent& event)
{
    const int delta = event.GetWheelDelta() > 0 ? event.GetWheelDelta() : 120;
    const int rotation = event.GetWheelRotation();

    if (event.ControlDown()) {
        if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL)
            return;
        // One notch (one wheel delta) is one zoom step, toward the cursor.
        m_zoomWheelRotation += rotation;
        const int steps = m_zoomWheelRotation / delta;
        m_zoomWheelRotation -= steps * delta;
        if (steps != 0)
            applyZoom(stepZoom(m_zoom, steps), ZoomMode::Custom, {event.GetX(), event.GetY()});
        return;
    }

    const double notches = static_cast<double>(rotation) / delta;
    int pixels = 0;
    if (event.IsPageScroll())
        pixels = static_cast<int>(std::lround(notches * m_viewport.y));
    else
        pixels = static_cast<int>(std::lround(notches * event.GetLinesPerAction() * kWheelLineStep));

    // Positive rotation is up for the vertical wheel and right for the horizontal one.
    if (event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL)
        scrollByPixels(pixels, 0);
    else
        scrollByPixels(0, -pixels);
}

void PdfView::onLeftDown(wxMouseEvent& event)
{
    SetFocus();
    event.Skip();
}

void PdfView::onScrollWin(wxScrollWinEvent& event)
{
    // The scroll helper moves the view after this handler returns.
    event.Skip();
    CallAfter(&PdfView::updateCurrentPage);
}

void PdfView::onPaint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    DoPrepareDC(dc);

    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const wxSize& client = m_viewport;
    dc.SetBackground(wxBrush(kBackground));
    dc.Clear();

    if (!m_doc)
        return;

    const double scale = renderScale();
    // Same size as PdfDocument::renderPage's image, 4 bytes per pixel.
    const auto pageBytes = [&](int page) {
        const PageSizePts& s = m_sizes[static_cast<std::size_t>(page)];
        return static_cast<std::size_t>(std::ceil(s.w * scale)) *
               static_cast<std::size_t>(std::ceil(s.h * scale)) * 4;
    };

    const PageRect view{origin.x, origin.y, client.x, client.y};
    const std::vector<int> visible = m_layout.visiblePages(view);

    // At high zoom the visible pages alone can exceed the default budget. Raise it
    // so they never evict each other, which would re-render them forever.
    std::size_t wantedBytes = 0;
    for (int page : visible)
        wantedBytes += pageBytes(page);
    m_cache.setBudget(std::max(PageCache::kDefaultBudgetBytes, wantedBytes));

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
            wanted.push_back({page, scale, m_generation, !m_textLayer.hasPage(page)});
    }

    // Then one page above and one below, but only while they fit in the budget
    // next to the visible pages. Otherwise caching them evicts a visible page.
    if (!visible.empty()) {
        for (int page : {visible.front() - 1, visible.back() + 1}) {
            if (page < 0 || page >= m_layout.pageCount())
                continue;
            const std::size_t bytes = pageBytes(page);
            if (wantedBytes + bytes > m_cache.budget())
                continue;
            wantedBytes += bytes;
            if (!m_cache.contains(page, scale))
                wanted.push_back({page, scale, m_generation, !m_textLayer.hasPage(page)});
        }
    }
    m_worker->setRequests(std::move(wanted));
}

void PdfView::onPageRendered(wxThreadEvent& event)
{
    auto result = event.GetPayload<RenderResult>();
    if (result.generation != m_generation)
        return;
    if (result.textBoxes)
        m_textLayer.setBoxes(result.page, std::move(*result.textBoxes));
    if (!result.image.IsOk())
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
