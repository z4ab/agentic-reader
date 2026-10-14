#include "pdf/PdfView.h"

#include "chat/ReaderContext.h"
#include "pdf/LayoutDetect.h"
#include "pdf/PdfDocument.h"
#include "pdf/RenderWorker.h"
#include "pdf/SearchWorker.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/textdlg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>

wxDEFINE_EVENT(EVT_PDF_PAGE_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_ZOOM_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_LAYOUT_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_FILE_DROPPED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_SELECTION_CHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PDF_FIND_CHANGED, wxCommandEvent);

namespace {
// One mouse wheel line. Scrolling itself is per pixel (scroll rate 1).
constexpr int kWheelLineStep = 20;
// Up/Down/Left/Right.
constexpr int kArrowStep = 40;
// The zoom a layout starts with.
PdfView::ZoomMode defaultZoomMode(LayoutMode mode)
{
    return mode == LayoutMode::SinglePage ? PdfView::ZoomMode::FitPage
                                          : PdfView::ZoomMode::FitWidth;
}

const wxColour kBackground(0x80, 0x80, 0x80);
// Around the slide in fullscreen SinglePage.
const wxColour kPresentationBackground(0x00, 0x00, 0x00);
const wxColour kBorder(0x40, 0x40, 0x40);
// 35% of 255: the selection highlight's alpha over the system highlight colour.
constexpr unsigned char kHighlightAlpha = 89;
// Find highlights, translucent so the text stays readable: every match, and the
// current one.
const wxColour kMatchFill(0xff, 0xdc, 0x00, 0x73);
const wxColour kCurrentMatchFill(0xff, 0x78, 0x00, 0xa6);
// Pointer travel in pixels before a left press becomes a drag.
constexpr int kDragThreshold = 3;
// How far a click may land from the double-click and still be a triple-click.
constexpr int kMultiClickSlop = 4;

// Clear space kept between a revealed rect and the pane edge (SinglePage).
constexpr int kRevealMargin = 24;

// Scroll origin along one axis that shows [start, start + length) with
// kRevealMargin to spare, moving the origin least. A span longer than the pane
// keeps its start in view.
int revealOrigin(int origin, int start, int length, int size)
{
    const int top = start - kRevealMargin;
    const int bottom = start + length + kRevealMargin - size;
    if (top < origin)
        return top;
    if (bottom > origin)
        return std::min(bottom, top);
    return origin;
}

// Milliseconds on a clock that never goes back, for the wheel lockout.
long long monotonicMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

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
    Bind(EVT_SEARCH_RESULT, &PdfView::onSearchResult, this);
    Bind(wxEVT_KEY_DOWN, &PdfView::onKeyDown, this);
    Bind(wxEVT_MOUSEWHEEL, &PdfView::onMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &PdfView::onLeftDown, this);
    Bind(wxEVT_LEFT_DCLICK, &PdfView::onLeftDClick, this);
    Bind(wxEVT_LEFT_UP, &PdfView::onLeftUp, this);
    Bind(wxEVT_MOTION, &PdfView::onMotion, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PdfView::onCaptureLost, this);
    Bind(wxEVT_CONTEXT_MENU, &PdfView::onContextMenu, this);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { copySelection(); }, wxID_COPY);
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
    // Stops and joins the worker threads before this handler goes away.
    m_worker.reset();
    m_searchWorker.reset();
}

void PdfView::setDocument(std::unique_ptr<PdfDocument> doc, std::string password,
                          std::optional<LayoutMode> layout)
{
    m_doc = std::move(doc);
    m_path = m_doc->path();
    m_sizes.clear();
    for (int i = 0; i < m_doc->pageCount(); ++i) {
        const wxRealPoint pts = m_doc->pageSizePts(i);
        m_sizes.push_back({pts.x, pts.y});
    }
    m_currentPage = -1;
    m_shownPage = 0;
    endDrag();
    ++m_generation;
    m_documentGeneration = m_generation;
    endFind();
    m_searchWorker.reset();
    m_worker.reset();
    m_searchWorker = std::make_unique<SearchWorker>(this, m_path, password);
    m_worker = std::make_unique<RenderWorker>(this, m_path, std::move(password));
    m_cache.clear();
    m_textLayer.clear();
    m_detectedLayout = detectLayout(m_sizes);
    const LayoutMode previous = m_layoutMode;
    m_layoutMode = layout.value_or(m_detectedLayout);
    // A fit zoom follows the new mode's default. A custom zoom is kept.
    if (m_layoutMode != previous && m_zoomMode != ZoomMode::Custom)
        m_zoomMode = defaultZoomMode(m_layoutMode);
    if (m_zoomMode != ZoomMode::Custom)
        m_zoom = fitZoom(m_zoomMode);
    relayout();
    scrollToOrigin(0, 0);
    setSelection(std::nullopt);
    sendZoomChanged();
}

std::optional<SelectionInfo> PdfView::selection() const
{
    if (!m_selection)
        return std::nullopt;
    return m_selection->info(m_textLayer);
}

int PdfView::loadSelectionPages()
{
    if (!m_selection || !m_doc)
        return 0;
    const int loaded = readercontext::loadSelectionPages(*m_selection, *m_doc, m_textLayer);
    if (loaded > 0)
        Refresh(); // the highlight covers the loaded pages' words too
    return loaded;
}

void PdfView::clearSelection()
{
    setSelection(std::nullopt);
}

void PdfView::copySelection()
{
    if (!m_selection)
        return;
    const std::string text = m_selection->text(m_textLayer);
    if (text.empty())
        return;
    wxClipboardLocker lock;
    if (!lock)
        return;
    wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
}

void PdfView::setContextMenuHook(std::function<void(wxMenu&)> hook)
{
    m_contextMenuHook = std::move(hook);
}

void PdfView::setSelection(std::optional<Selection> selection)
{
    const bool same = selection.has_value() == m_selection.has_value() &&
                      (!selection || (selection->start() == m_selection->start() &&
                                      selection->end() == m_selection->end()));
    if (same)
        return;
    m_selection = selection;
    Refresh();

    wxCommandEvent event(EVT_PDF_SELECTION_CHANGED, GetId());
    event.SetEventObject(this);
    ProcessWindowEvent(event);
}

std::optional<TextPos> PdfView::textPosAt(wxPoint client, bool nearest) const
{
    if (!m_doc)
        return std::nullopt;
    const wxPoint v = CalcUnscrolledPosition(client);
    ViewPoint point{v.x, v.y};
    if (nearest) {
        // Beside a page: snap into it horizontally, so a drag into the margin
        // still selects to the line's end.
        const int page = m_layout.pageAt(point.y);
        if (page >= 0) {
            const PageRect r = m_layout.pageRect(page);
            point.x = std::clamp(point.x, r.x, r.x + r.w - 1);
        }
    }
    const PagePoint hit = m_layout.viewToPage(point);
    if (hit.page < 0)
        return std::nullopt;
    const int box = nearest ? m_textLayer.nearestBox(hit.page, hit.pt)
                            : m_textLayer.boxAt(hit.page, hit.pt);
    if (box < 0)
        return std::nullopt;
    return TextPos{hit.page, box};
}

void PdfView::endDrag()
{
    m_dragAnchor.reset();
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
}

void PdfView::closeDocument()
{
    if (!m_doc)
        return;
    endDrag();
    setSelection(std::nullopt);
    endFind();
    m_searchWorker.reset();
    // Results already queued from the old worker carry the old generation.
    ++m_generation;
    m_documentGeneration = m_generation;
    m_worker.reset();
    m_doc.reset();
    m_path.clear();
    m_sizes.clear();
    m_currentPage = -1;
    m_cache.clear();
    m_textLayer.clear();
    relayout();
    Scroll(0, 0);
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
    page = std::clamp(page, 0, pageCount() - 1);
    if (m_layoutMode == LayoutMode::SinglePage && page != m_shownPage) {
        m_shownPage = page;
        relayout();
    }
    const PageRect r = m_layout.pageRect(page);
    scrollToOrigin(CalcUnscrolledPosition(wxPoint(0, 0)).x, r.y - PageLayout::kGap);
}

void PdfView::previousPage()
{
    if (!m_doc)
        return;
    if (m_layoutMode == LayoutMode::SinglePage)
        flipPage(-1);
    else
        goToPage(m_currentPage - 1);
}

void PdfView::nextPage()
{
    if (!m_doc)
        return;
    if (m_layoutMode == LayoutMode::SinglePage)
        flipPage(1);
    else
        goToPage(m_currentPage + 1);
}

void PdfView::setLayoutMode(LayoutMode mode)
{
    if (!m_doc || mode == m_layoutMode)
        return;
    const int page = std::max(m_currentPage, 0);
    m_layoutMode = mode;
    m_shownPage = page;
    m_currentPage = page;
    m_zoomMode = defaultZoomMode(mode);
    const double zoom = clampZoom(fitZoom(m_zoomMode));
    if (zoom != m_zoom) {
        m_zoom = zoom;
        invalidateRenders();
    }
    relayout();
    goToPage(page);
    sendZoomChanged();

    wxCommandEvent event(EVT_PDF_LAYOUT_CHANGED, GetId());
    event.SetEventObject(this);
    event.SetInt(static_cast<int>(mode));
    ProcessWindowEvent(event);
}

void PdfView::setPresentation(bool presentation)
{
    if (presentation == m_presentation)
        return;
    m_presentation = presentation;
    Refresh();
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
    m_layout.compute(m_sizes, m_zoom, m_viewport.x, GetDPIScaleFactor(), m_layoutMode,
                     m_shownPage, m_viewport.y);
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

int PdfView::displayedPage() const
{
    return m_layoutMode == LayoutMode::SinglePage ? m_shownPage : std::max(m_currentPage, 0);
}

PageSizePts PdfView::currentPageSize() const
{
    return m_sizes[static_cast<std::size_t>(displayedPage())];
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

bool PdfView::singlePageKey(int key, bool shift)
{
    switch (key) {
    case WXK_RIGHT:
    case WXK_NUMPAD_RIGHT:
    case WXK_PAGEDOWN:
    case WXK_NUMPAD_PAGEDOWN:
    case 'N':
        flipPage(1);
        return true;
    case WXK_LEFT:
    case WXK_NUMPAD_LEFT:
    case WXK_PAGEUP:
    case WXK_NUMPAD_PAGEUP:
    case 'P':
        flipPage(-1);
        return true;
    case WXK_SPACE:
        flipPage(shift ? -1 : 1);
        return true;
    case WXK_HOME:
    case WXK_NUMPAD_HOME:
        goToPage(0);
        return true;
    case WXK_END:
    case WXK_NUMPAD_END:
        goToPage(pageCount() - 1);
        return true;
    case WXK_DOWN:
    case WXK_NUMPAD_DOWN:
        stepSinglePage(1);
        return true;
    case WXK_UP:
    case WXK_NUMPAD_UP:
        stepSinglePage(-1);
        return true;
    default:
        return false;
    }
}

void PdfView::flipPage(int delta)
{
    const int target = m_shownPage + delta;
    if (!m_doc || target < 0 || target >= pageCount())
        return;
    const int x = CalcUnscrolledPosition(wxPoint(0, 0)).x;
    m_shownPage = target;
    relayout();
    const PageRect r = m_layout.pageRect(target);
    // Forward shows the new page's top edge, back shows its bottom edge.
    const int y = delta > 0 ? r.y - PageLayout::kGap
                            : r.y + r.h + PageLayout::kGap - m_viewport.y;
    scrollToOrigin(x, y);
}

void PdfView::stepSinglePage(int dir)
{
    const int totalHeight = m_layout.totalHeight();
    if (totalHeight <= m_viewport.y) {
        flipPage(dir);
        return;
    }
    const int y = CalcUnscrolledPosition(wxPoint(0, 0)).y;
    if (dir > 0 && y >= totalHeight - m_viewport.y) {
        flipPage(1);
        return;
    }
    if (dir < 0 && y <= 0) {
        flipPage(-1);
        return;
    }
    scrollByPixels(0, dir * kArrowStep);
}

void PdfView::updateCurrentPage()
{
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const wxSize& client = m_viewport;
    const int page = m_layoutMode == LayoutMode::SinglePage
                         ? m_shownPage
                         : mostVisiblePage(m_layout, {origin.x, origin.y, client.x, client.y});
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
            // The current mode's default zoom.
            if (m_layoutMode == LayoutMode::SinglePage)
                fitPage();
            else
                fitWidth();
            return;
        case 'G':
            showGoToPageDialog();
            return;
        case 'C':
            copySelection();
            return;
        default:
            event.Skip();
            return;
        }
    }
    // SinglePage takes plain keys, and Shift+Space for Previous.
    if (m_layoutMode == LayoutMode::SinglePage && m_doc
        && (!event.HasAnyModifiers() || (event.GetModifiers() == wxMOD_SHIFT && key == WXK_SPACE))
        && singlePageKey(key, event.ShiftDown())) {
        return;
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

    if (m_layoutMode == LayoutMode::SinglePage && m_doc
        && event.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL) {
        const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
        const int maxY = std::max(0, m_layout.totalHeight() - m_viewport.y);
        const WheelFlip::Step step = m_wheelFlip.onWheel(
            rotation, delta, monotonicMs(), m_layout.totalHeight() <= m_viewport.y,
            origin.y <= 0, origin.y >= maxY, m_shownPage, pageCount());
        if (step.flip != 0) {
            flipPage(step.flip);
            return;
        }
        if (!step.pan)
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
    const wxPoint p = event.GetPosition();

    // wxGTK reports the third click of a triple-click as a plain press (sometimes
    // two), so recognize it by its distance in time and space from the double-click.
    int interval = wxSystemSettings::GetMetric(wxSYS_DCLICK_MSEC, this);
    if (interval <= 0)
        interval = 500;
    const long sinceDClick = event.GetTimestamp() - m_lastDClickTime;
    if (m_lastDClickTime != 0 && sinceDClick >= 0 && sinceDClick <= interval &&
        std::abs(p.x - m_lastDClickPos.x) <= kMultiClickSlop &&
        std::abs(p.y - m_lastDClickPos.y) <= kMultiClickSlop) {
        endDrag();
        if (const std::optional<TextPos> pos = textPosAt(p, false))
            setSelection(Selection::line(m_textLayer, *pos));
        return;
    }

    // A click clears the selection. A drag from here makes a new one.
    setSelection(std::nullopt);
    endDrag();
    m_dragAnchor = textPosAt(p, true);
    m_dragStart = p;
    if (m_dragAnchor)
        CaptureMouse();
}

void PdfView::onLeftDClick(wxMouseEvent& event)
{
    SetFocus();
    endDrag();
    m_lastDClickTime = event.GetTimestamp();
    m_lastDClickPos = event.GetPosition();
    if (const std::optional<TextPos> pos = textPosAt(event.GetPosition(), false))
        setSelection(Selection::word(*pos));
    else
        setSelection(std::nullopt);
}

void PdfView::onLeftUp(wxMouseEvent& event)
{
    endDrag();
    event.Skip();
}

void PdfView::onMotion(wxMouseEvent& event)
{
    event.Skip();
    const wxPoint p = event.GetPosition();

    const bool overText = textPosAt(p, false).has_value();
    if (overText != m_overText) {
        m_overText = overText;
        SetCursor(overText ? wxCursor(wxCURSOR_IBEAM) : wxNullCursor);
    }

    if (!m_dragAnchor || !event.LeftIsDown())
        return;
    if (!m_dragging && std::abs(p.x - m_dragStart.x) < kDragThreshold &&
        std::abs(p.y - m_dragStart.y) < kDragThreshold)
        return;
    m_dragging = true;

    // Dragging above or below the pane scrolls toward the pointer.
    if (p.y < 0)
        scrollByPixels(0, p.y);
    else if (p.y >= m_viewport.y)
        scrollByPixels(0, p.y - m_viewport.y + 1);

    if (const std::optional<TextPos> pos = textPosAt(p, true))
        setSelection(Selection(*m_dragAnchor, *pos));
}

void PdfView::onCaptureLost(wxMouseCaptureLostEvent& /*event*/)
{
    m_dragAnchor.reset();
    m_dragging = false;
}

void PdfView::onContextMenu(wxContextMenuEvent& event)
{
    wxMenu menu;
    menu.Append(wxID_COPY, "&Copy\tCtrl+C");
    menu.Enable(wxID_COPY, m_selection.has_value());
    if (m_contextMenuHook)
        m_contextMenuHook(menu);

    // From the keyboard the position is unset and the menu opens at the pointer.
    wxPoint pos = event.GetPosition();
    if (pos != wxDefaultPosition)
        pos = ScreenToClient(pos);
    PopupMenu(&menu, pos);
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
    const bool black = m_presentation && m_layoutMode == LayoutMode::SinglePage;
    dc.SetBackground(wxBrush(black ? kPresentationBackground : kBackground));
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

    const bool matchesVisible =
        std::any_of(visible.begin(), visible.end(),
                    [this](int page) { return m_findMatches.count(page) != 0; });
    if (m_selection || matchesVisible) {
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc && matchesVisible)
            paintMatches(*gc, visible);
        if (gc && m_selection)
            paintSelection(*gc, visible);
    }
}

void PdfView::paintMatches(wxGraphicsContext& gc, const std::vector<int>& visible)
{
    const auto [currentPage, current] = findMatch(m_findCurrent);
    gc.SetPen(*wxTRANSPARENT_PEN);
    wxGraphicsPath path = gc.CreatePath();
    for (int page : visible) {
        const auto it = m_findMatches.find(page);
        if (it == m_findMatches.end())
            continue;
        for (const RectPts& match : it->second) {
            if (&match == current)
                continue;
            const PageRect r = m_layout.pageToView(page, match);
            path.AddRectangle(r.x, r.y, r.w, r.h);
        }
    }
    gc.SetBrush(wxBrush(kMatchFill));
    gc.FillPath(path, wxWINDING_RULE);

    if (current && std::binary_search(visible.begin(), visible.end(), currentPage)) {
        const PageRect r = m_layout.pageToView(currentPage, *current);
        gc.SetBrush(wxBrush(kCurrentMatchFill));
        gc.DrawRectangle(r.x, r.y, r.w, r.h);
    }
}

void PdfView::paintSelection(wxGraphicsContext& gc, const std::vector<int>& visible)
{
    const wxColour system = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(wxColour(system.Red(), system.Green(), system.Blue(), kHighlightAlpha)));

    // One path for all boxes, filled with the winding rule, so overlapping box
    // edges are not painted twice.
    wxGraphicsPath path = gc.CreatePath();
    const TextPos start = m_selection->start();
    const TextPos end = m_selection->end();
    for (int page : visible) {
        if (page < start.page || page > end.page)
            continue;
        const std::vector<TextBox>* boxes = m_textLayer.boxes(page);
        if (!boxes || boxes->empty())
            continue;
        const int last = static_cast<int>(boxes->size()) - 1;
        const int from = std::max(page == start.page ? start.boxIndex : 0, 0);
        const int to = std::min(page == end.page ? end.boxIndex : last, last);
        for (int i = from; i <= to; ++i) {
            const RectPts& box = (*boxes)[static_cast<std::size_t>(i)].bbox;
            const PageRect r = m_layout.pageToView(page, box);
            path.AddRectangle(r.x, r.y, r.w, r.h);
        }
    }
    gc.FillPath(path, wxWINDING_RULE);
}

void PdfView::onPageRendered(wxThreadEvent& event)
{
    auto result = event.GetPayload<RenderResult>();
    // From a document shown before this one.
    if (result.generation < m_documentGeneration)
        return;
    // Text does not depend on the render scale, so keep it even from a stale result.
    if (result.textBoxes)
        m_textLayer.setBoxes(result.page, std::move(*result.textBoxes));
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

void PdfView::find(const std::string& query, bool matchCase)
{
    if (query.empty()) {
        endFind();
        return;
    }
    if (!m_doc || !m_searchWorker)
        return;
    resetFind();
    m_findQuery = query;
    m_findMatchCase = matchCase;
    m_searchWorker->start(query, matchCase, m_findGeneration);
    Refresh();
    sendFindChanged();
}

void PdfView::findStep(bool forward)
{
    const int next = stepMatch(m_findCurrent, m_findTotal, forward);
    if (next < 0)
        return;
    m_findCurrent = next;
    scrollToCurrentMatch();
    Refresh();
    sendFindChanged();
}

void PdfView::endFind()
{
    if (m_findQuery.empty())
        return;
    if (m_searchWorker)
        m_searchWorker->cancel();
    resetFind();
    Refresh();
    sendFindChanged();
}

void PdfView::resetFind()
{
    // Results of the old search may already be queued. They carry the old generation.
    ++m_findGeneration;
    m_findQuery.clear();
    m_findMatchCase = false;
    m_findDone = false;
    m_findTotal = 0;
    m_findCurrent = -1;
    m_findMatches.clear();
    m_findPages.clear();
    m_findFirstIndex.clear();
}

void PdfView::sendFindChanged()
{
    wxCommandEvent event(EVT_PDF_FIND_CHANGED, GetId());
    event.SetEventObject(this);
    ProcessWindowEvent(event);
}

std::pair<int, const RectPts*> PdfView::findMatch(int index) const
{
    if (index < 0 || index >= m_findTotal)
        return {-1, nullptr};
    // The last page whose first match is at or before `index`.
    const auto it = std::upper_bound(m_findFirstIndex.begin(), m_findFirstIndex.end(), index) - 1;
    const auto slot = static_cast<std::size_t>(it - m_findFirstIndex.begin());
    const int page = m_findPages[slot];
    const std::vector<RectPts>& matches = m_findMatches.at(page);
    return {page, &matches[static_cast<std::size_t>(index - *it)]};
}

bool PdfView::isPageVisible(int page) const
{
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const PageRect r = m_layout.pageRect(page);
    return r.y < origin.y + m_viewport.y && r.y + r.h > origin.y &&
           r.x < origin.x + m_viewport.x && r.x + r.w > origin.x;
}

void PdfView::revealRect(int page, const RectPts& rectPts)
{
    if (!m_doc || page < 0 || page >= pageCount())
        return;

    if (m_layoutMode == LayoutMode::Continuous) {
        const PageRect r = m_layout.pageToView(page, rectPts);
        const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
        int x = origin.x;
        int y = origin.y;
        if (r.x < origin.x || r.x + r.w > origin.x + m_viewport.x)
            x = r.x + r.w / 2 - m_viewport.x / 2;
        if (r.y < origin.y || r.y + r.h > origin.y + m_viewport.y)
            y = r.y + r.h / 2 - m_viewport.y / 2;
        if (x != origin.x || y != origin.y)
            scrollToOrigin(x, y);
        return;
    }

    if (page != m_shownPage) {
        // Shows the page at its top edge, as goToPage does. The zoom is kept.
        m_shownPage = page;
        relayout();
        const PageRect pageRect = m_layout.pageRect(page);
        scrollToOrigin(CalcUnscrolledPosition(wxPoint(0, 0)).x,
                       pageRect.y - PageLayout::kGap);
    }
    const PageRect r = m_layout.pageToView(page, rectPts);
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const int x = revealOrigin(origin.x, r.x, r.w, m_viewport.x);
    const int y = revealOrigin(origin.y, r.y, r.h, m_viewport.y);
    if (x != origin.x || y != origin.y)
        scrollToOrigin(x, y);
}

void PdfView::scrollToCurrentMatch()
{
    const auto [page, match] = findMatch(m_findCurrent);
    if (match)
        revealRect(page, *match);
}

void PdfView::onSearchResult(wxThreadEvent& event)
{
    auto result = event.GetPayload<SearchResult>();
    if (result.generation != m_findGeneration || m_findQuery.empty())
        return;
    if (result.done) {
        m_findDone = true;
        sendFindChanged();
        return;
    }
    if (result.matches.empty() || m_findMatches.count(result.page) != 0)
        return;

    const int page = result.page;
    m_findPages.push_back(page);
    m_findFirstIndex.push_back(m_findTotal);
    m_findTotal += static_cast<int>(result.matches.size());
    m_findMatches.emplace(page, std::move(result.matches));
    if (m_findCurrent < 0) {
        // The first match of a new search becomes the current one.
        m_findCurrent = 0;
        scrollToCurrentMatch();
    }
    if (isPageVisible(page)) {
        const PageRect r = m_layout.pageRect(page);
        RefreshRect(wxRect(CalcScrolledPosition(wxPoint(r.x, r.y)), wxSize(r.w, r.h)));
    }
    sendFindChanged();
}
