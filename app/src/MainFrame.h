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
class wxFileHistory;
class wxPanel;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

// Main application window: menu bar, PDF toolbar, three-pane layout, status bar.
class MainFrame : public wxFrame
{
public:
    MainFrame();
    ~MainFrame() override;

    // Opens a PDF in the view, asking for a password when it is encrypted.
    // On failure shows a message box and keeps the current document. A
    // cancelled password prompt ends the attempt silently. Used by File >
    // Open, File > Recent, drag and drop and the command line.
    void openFile(const wxString& path);

private:
    void buildMenuBar();
    void buildToolBar();
    void buildLayout();
    wxPanel* makePane(wxWindow* parent, const wxString& label);

    void startSidecar();
    void restartSidecar();
    // No sidecar was found: the status bar and the chat pane say so.
    void showSidecarMissing();

    // Shows the open document's name in the status bar and its page count
    // and zoom in the toolbar.
    void showOpenedFile();
    void saveFileHistory();

    void onOpen(wxCommandEvent& event);
    void onRecent(wxCommandEvent& event);
    void onFileDropped(wxCommandEvent& event);
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
    std::unique_ptr<wxFileHistory> m_fileHistory;
    // Folder of the open PDF, sent to the chat as the project. Empty until a
    // file opens.
    wxString m_projectDir;
};
