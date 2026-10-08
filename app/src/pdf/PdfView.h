#pragma once

#include "pdf/PageLayout.h"

#include <wx/bitmap.h>
#include <wx/scrolwin.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

class PdfDocument;
class wxPaintEvent;
class wxSizeEvent;

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
    void onPaint(wxPaintEvent& event);
    void onSize(wxSizeEvent& event);
    void onDpiChanged(wxDPIChangedEvent& event);

    std::unique_ptr<PdfDocument> m_doc;
    std::vector<PageSizePts> m_sizes;
    PageLayout m_layout;
    double m_zoom = 1.0;
    // Synchronous render cache keyed by page. M2.3 replaces this.
    std::map<int, wxBitmap> m_cache;
};
