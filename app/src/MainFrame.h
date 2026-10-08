#pragma once

#include <wx/frame.h>

class wxCommandEvent;
class wxPanel;
class wxString;
class wxWindow;

// Main application window: menu bar, three-pane layout, status bar.
class MainFrame : public wxFrame
{
public:
    MainFrame();

private:
    void buildMenuBar();
    void buildLayout();
    wxPanel* makePane(wxWindow* parent, const wxString& label);

    void onQuit(wxCommandEvent& event);
    void onAbout(wxCommandEvent& event);
};
