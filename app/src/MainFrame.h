#pragma once

#include <wx/frame.h>
#include <wx/string.h>

#include <memory>

class ChatPanel;
class KeyStore;
class PdfView;
class Settings;
class SidecarClient;
class wxCloseEvent;
class wxComboBox;
class wxCommandEvent;
class wxPanel;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

// Main application window: menu bar, PDF toolbar, three-pane layout, status bar.
class MainFrame : public wxFrame
{
public:
    // Opens pdfPath in the PDF view when it is not empty.
    explicit MainFrame(const wxString& pdfPath = wxString());
    ~MainFrame() override;

private:
    void buildMenuBar();
    void buildToolBar();
    void buildLayout();
    wxPanel* makePane(wxWindow* parent, const wxString& label);

    void startSidecar();
    void restartSidecar();

    void onQuit(wxCommandEvent& event);
    void onAbout(wxCommandEvent& event);
    void onPreferences(wxCommandEvent& event);
    void onClose(wxCloseEvent& event);
    void onSidecarReady(wxThreadEvent& event);
    void onSidecarDown(wxThreadEvent& event);
    void onSidecarMessage(wxThreadEvent& event);

    void onZoomEntered(wxCommandEvent& event);
    void onPageEntered(wxCommandEvent& event);
    void onPdfPageChanged(wxCommandEvent& event);
    void onPdfZoomChanged(wxCommandEvent& event);
    void showPage();
    void showZoom();

    PdfView* m_pdfView = nullptr;
    wxComboBox* m_zoomCombo = nullptr;
    wxTextCtrl* m_pageEntry = nullptr;
    wxStaticText* m_pageTotal = nullptr;
    std::unique_ptr<KeyStore> m_keyStore;
    std::unique_ptr<Settings> m_settings;
    ChatPanel* m_chat = nullptr;
    std::unique_ptr<SidecarClient> m_sidecar;
};
