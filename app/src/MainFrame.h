#pragma once

#include <wx/frame.h>
#include <wx/string.h>

#include <memory>

class KeyStore;
class PdfView;
class Settings;
class SidecarClient;
class wxCloseEvent;
class wxCommandEvent;
class wxPanel;
class wxWindow;

// Main application window: menu bar, three-pane layout, status bar.
class MainFrame : public wxFrame
{
public:
    // Opens pdfPath in the PDF view when it is not empty.
    explicit MainFrame(const wxString& pdfPath = wxString());
    ~MainFrame() override;

private:
    void buildMenuBar();
    void buildLayout();
    wxPanel* makePane(wxWindow* parent, const wxString& label);

    void startSidecar();

    void onQuit(wxCommandEvent& event);
    void onAbout(wxCommandEvent& event);
    void onPreferences(wxCommandEvent& event);
    void onClose(wxCloseEvent& event);
    void onSidecarReady(wxThreadEvent& event);
    void onSidecarDown(wxThreadEvent& event);

    PdfView* m_pdfView = nullptr;
    std::unique_ptr<KeyStore> m_keyStore;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<SidecarClient> m_sidecar;
};
