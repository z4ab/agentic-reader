#pragma once

#include <wx/frame.h>
#include <wx/string.h>

class PdfView;
class wxCommandEvent;
class wxPanel;
class wxWindow;

// Main application window: menu bar, three-pane layout, status bar.
class MainFrame : public wxFrame
{
public:
    // Opens pdfPath in the PDF view when it is not empty.
    explicit MainFrame(const wxString& pdfPath = wxString());

private:
    void buildMenuBar();
    void buildLayout();
    wxPanel* makePane(wxWindow* parent, const wxString& label);

    void onQuit(wxCommandEvent& event);
    void onAbout(wxCommandEvent& event);

    PdfView* m_pdfView = nullptr;
};
