#pragma once

#include "pdf/PageCache.h"
#include "pdf/PageLayout.h"
#include "pdf/PdfEvents.h"
#include "pdf/Selection.h"
#include "pdf/TextLayer.h"
#include "pdf/ViewNav.h"

#include <wx/scrolwin.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PdfDocument;
class RenderWorker;
class wxContextMenuEvent;
class wxGraphicsContext;
class wxKeyEvent;
class wxMenu;
class wxMouseCaptureLostEvent;
class wxMouseEvent;
class wxPaintEvent;
class wxScrollWinEvent;
class wxSizeEvent;
class wxThreadEvent;

// Continuous vertical scroll of every page of one document. Sends
// EVT_PDF_PAGE_CHANGED, EVT_PDF_ZOOM_CHANGED and EVT_PDF_SELECTION_CHANGED, and
// queues EVT_PDF_FILE_DROPPED when a file is dropped on it.
//
// Text selection: left drag selects, double-click selects a word, triple-click
// a line, a click without a drag clears. Ctrl+C and the context menu "Copy" put
// the selected text on the clipboard.
class PdfView : public wxScrolledCanvas
{
public:
    explicit PdfView(wxWindow* parent);
    ~PdfView() override;

    // Shows `doc` (not null), replacing the current document and clearing the
    // selection. `password` is the one that opened it (empty for an unencrypted
    // file), so the render thread can open its own copy.
    void setDocument(std::unique_ptr<PdfDocument> doc, std::string password);
    // Shows no document: stops the render thread, drops cached pages, word
    // boxes and results in flight, and paints the empty pane. Does nothing
    // when no document is open.
    void closeDocument();

    // 0-based page with the most visible area, or -1 with no document.
    int currentPage() const { return m_currentPage; }
    int pageCount() const { return static_cast<int>(m_sizes.size()); }
    // Scale factor, 1.0 = 100%.
    double zoom() const { return m_zoom; }
    // Empty with no document.
    const std::filesystem::path& filePath() const { return m_path; }
    // Word boxes of the pages rendered so far. A page's boxes arrive with its
    // first rendered image.
    const TextLayer& textLayer() const { return m_textLayer; }

    // The selected text and its 0-based page range, nullopt without a selection.
    std::optional<SelectionInfo> selection() const;
    // Removes the selection (sends EVT_PDF_SELECTION_CHANGED if there was one).
    void clearSelection();
    // Puts the selected text on the clipboard. Does nothing without a selection.
    void copySelection();
    // Called with the context menu before it shows, after its "Copy" item, so
    // callers can append items and bind their handlers on the menu.
    void setContextMenuHook(std::function<void(wxMenu&)> hook);

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

    // The word box under the client point. With `nearest`, the closest box on the
    // page under the point when no box contains it. nullopt off the pages or on a
    // page whose text is not loaded.
    std::optional<TextPos> textPosAt(wxPoint client, bool nearest) const;
    void setSelection(std::optional<Selection> selection);
    void paintSelection(wxGraphicsContext& gc, const std::vector<int>& visible);
    void endDrag();

    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onDpiChanged(wxDPIChangedEvent& event);
    void onPageRendered(wxThreadEvent& event);
    void onKeyDown(wxKeyEvent& event);
    void onMouseWheel(wxMouseEvent& event);
    void onLeftDown(wxMouseEvent& event);
    void onLeftDClick(wxMouseEvent& event);
    void onLeftUp(wxMouseEvent& event);
    void onMotion(wxMouseEvent& event);
    void onCaptureLost(wxMouseCaptureLostEvent& event);
    void onContextMenu(wxContextMenuEvent& event);
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
    TextLayer m_textLayer;
    // Results from an older generation are dropped. Their text is kept when the
    // result is from this document (generation >= m_documentGeneration), since
    // text does not depend on zoom.
    unsigned m_generation = 0;
    unsigned m_documentGeneration = 0;
    std::unique_ptr<RenderWorker> m_worker;

    std::optional<Selection> m_selection;
    std::function<void(wxMenu&)> m_contextMenuHook;
    // Box under the left press while a drag may be in progress.
    std::optional<TextPos> m_dragAnchor;
    wxPoint m_dragStart;
    bool m_dragging = false;
    // Time and position of the last double-click, to recognize a triple-click.
    long m_lastDClickTime = 0;
    wxPoint m_lastDClickPos;
    bool m_overText = false;
};
