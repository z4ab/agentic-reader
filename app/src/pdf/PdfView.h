#pragma once

#include "pdf/PageCache.h"
#include "pdf/PageLayout.h"
#include "pdf/PdfEvents.h"
#include "pdf/ViewNav.h"

#include <wx/scrolwin.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class PdfDocument;
class RenderWorker;
class wxKeyEvent;
class wxMouseEvent;
class wxPaintEvent;
class wxScrollWinEvent;
class wxSizeEvent;
class wxThreadEvent;

// Continuous vertical scroll of every page of one document. Sends
// EVT_PDF_PAGE_CHANGED and EVT_PDF_ZOOM_CHANGED.
class PdfView : public wxScrolledCanvas
{
public:
    explicit PdfView(wxWindow* parent);
    ~PdfView() override;

    // Returns false and sets `error` on failure. The previous document stays open.
    bool openFile(const std::filesystem::path& path, std::string& error);

    // 0-based page with the most visible area, or -1 with no document.
    int currentPage() const { return m_currentPage; }
    int pageCount() const { return static_cast<int>(m_sizes.size()); }
    // Scale factor, 1.0 = 100%.
    double zoom() const { return m_zoom; }
    // Empty with no document.
    const std::filesystem::path& filePath() const { return m_path; }

    // Custom zoom, clamped to kMinZoom..kMaxZoom. Keeps the pane center fixed.
    void setZoom(double zoom);
    void fitWidth();
    // Also scrolls so the whole current page is in view.
    void fitPage();
    // Puts the top edge of the 0-based page at the top of the pane. Clamped.
    void goToPage(int page);

private:
    enum class ZoomMode
    {
        Custom,
        FitWidth,
        FitPage,
    };

    // Asks for a 1-based page number and jumps to it (Ctrl+G).
    void showGoToPageDialog();
    void relayout();
    double renderScale() const;
    // Drops queued renders and results in flight. Call when the render scale changes.
    void invalidateRenders();

    PageSizePts currentPageSize() const;
    double fitZoom(ZoomMode mode) const;
    // Changes the zoom keeping the document point at client position `anchor` fixed.
    void applyZoom(double zoom, ZoomMode mode, ViewPoint anchor);
    ViewPoint paneCenter() const;
    // Scrolls to the virtual DIP origin, clamped to the scroll range.
    void scrollToOrigin(int x, int y);
    void scrollByPixels(int dx, int dy);
    void updateCurrentPage();
    void sendZoomChanged();

    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onDpiChanged(wxDPIChangedEvent& event);
    void onPageRendered(wxThreadEvent& event);
    void onKeyDown(wxKeyEvent& event);
    void onMouseWheel(wxMouseEvent& event);
    void onLeftDown(wxMouseEvent& event);
    void onScrollWin(wxScrollWinEvent& event);

    std::unique_ptr<PdfDocument> m_doc;
    std::filesystem::path m_path;
    std::vector<PageSizePts> m_sizes;
    PageLayout m_layout;
    // Client size from the last size event. wxGTK's GetClientSize() briefly
    // subtracts overlay scrollbars after one shows or hides, and no size event
    // follows to correct it, so layout and scroll clamping use this instead.
    wxSize m_viewport;
    double m_zoom = 1.0;
    ZoomMode m_zoomMode = ZoomMode::FitWidth;
    int m_currentPage = -1;
    // Ctrl+wheel rotation not yet turned into a zoom step.
    int m_zoomWheelRotation = 0;
    PageCache m_cache;
    // Results from an older generation are dropped.
    unsigned m_generation = 0;
    std::unique_ptr<RenderWorker> m_worker;
};
