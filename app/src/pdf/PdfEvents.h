#pragma once

#include <wx/event.h>

// Sent by PdfView when its current page (the page with the most visible area)
// changes. GetInt() is the new 0-based page.
wxDECLARE_EVENT(EVT_PDF_PAGE_CHANGED, wxCommandEvent);

// Sent by PdfView when its zoom changes. Read the new value from PdfView::zoom().
wxDECLARE_EVENT(EVT_PDF_ZOOM_CHANGED, wxCommandEvent);
