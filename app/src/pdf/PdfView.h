#pragma once

#include "pdf/PageCache.h"
#include "pdf/PageLayout.h"

#include <wx/scrolwin.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class PdfDocument;
class RenderWorker;
class wxPaintEvent;
class wxSizeEvent;
class wxThreadEvent;

// Continuous vertical scroll of every page of one document.
class PdfView : public wxScrolledCanvas
{
public:
    explicit PdfView(wxWindow* parent);
    ~PdfView() override;

    // Returns false and sets `error` on failure. The previous document stays open.
    bool openFile(const std::filesystem::path& path, std::string& error);

private:
    void relayout();
    double renderScale() const;
    // Drops queued renders and results in flight. Call when the render scale changes.
    void invalidateRenders();
    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onDpiChanged(wxDPIChangedEvent& event);
    void onPageRendered(wxThreadEvent& event);

    std::unique_ptr<PdfDocument> m_doc;
    std::vector<PageSizePts> m_sizes;
    PageLayout m_layout;
    double m_zoom = 1.0;
    PageCache m_cache;
    // Results from an older generation are dropped.
    unsigned m_generation = 0;
    std::unique_ptr<RenderWorker> m_worker;
};
