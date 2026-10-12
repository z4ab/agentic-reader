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
#include <unordered_map>
#include <utility>
#include <vector>

class PdfDocument;
class RenderWorker;
class SearchWorker;
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
// EVT_PDF_PAGE_CHANGED, EVT_PDF_ZOOM_CHANGED, EVT_PDF_SELECTION_CHANGED and
// EVT_PDF_FIND_CHANGED, and queues EVT_PDF_FILE_DROPPED when a file is dropped
// on it.
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

    // Find: searches the document for `query` (UTF-8) on a worker thread,
    // cancelling the running search. Matches stream in page by page and are
    // highlighted on the visible pages. The first match becomes the current one
    // and is scrolled into view. An empty query ends the find.
    void find(const std::string& query, bool matchCase);
    // Makes the next (`forward`) or previous match current, wrapping at both
    // ends, and scrolls it into view.
    void findStep(bool forward);
    // Cancels the search and removes every find highlight.
    void endFind();
    // The query of the running or finished search, "" without one.
    const std::string& findQuery() const { return m_findQuery; }
    bool findMatchCase() const { return m_findMatchCase; }
    // 0-based current match in document order, -1 without matches.
    int findCurrent() const { return m_findCurrent; }
    // Matches found so far.
    int findTotal() const { return m_findTotal; }
    // True once the whole document was searched.
    bool findDone() const { return m_findDone; }

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
    void paintMatches(wxGraphicsContext& gc, const std::vector<int>& visible);
    // Drops every match and the current one. Does not send EVT_PDF_FIND_CHANGED.
    void resetFind();
    void sendFindChanged();
    // Page and rect of the 0-based match in document order.
    std::pair<int, const RectPts*> findMatch(int index) const;
    // Scrolls the least needed to show the current match, centered on the axis it
    // was off.
    void scrollToCurrentMatch();
    bool isPageVisible(int page) const;
    void endDrag();

    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onDpiChanged(wxDPIChangedEvent& event);
    void onPageRendered(wxThreadEvent& event);
    void onSearchResult(wxThreadEvent& event);
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

    // Find. Results of another generation are dropped.
    std::unique_ptr<SearchWorker> m_searchWorker;
    unsigned m_findGeneration = 0;
    std::string m_findQuery;
    bool m_findMatchCase = false;
    bool m_findDone = false;
    int m_findTotal = 0;
    int m_findCurrent = -1;
    // Matches per page, and the pages with matches in ascending order with the
    // document-order index of each one's first match.
    std::unordered_map<int, std::vector<RectPts>> m_findMatches;
    std::vector<int> m_findPages;
    std::vector<int> m_findFirstIndex;

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
