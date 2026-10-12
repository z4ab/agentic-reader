#include "MainFrame.h"

#include "agent/SidecarClient.h"
#include "chat/ChatPanel.h"
#include "pdf/PdfView.h"
#include "settings/Settings.h"
#include "settings/SettingsDialog.h"

#include <wx/aboutdlg.h>
#include <wx/config.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>

namespace
{
constexpr int kLeftPaneWidth = 220;
constexpr int kRightPaneWidth = 420;
constexpr int kMinPaneWidth = 100;
} // namespace

MainFrame::MainFrame(const wxString& pdfPath)
    : wxFrame(nullptr, wxID_ANY, "Agentic Reader", wxDefaultPosition, wxSize(1400, 900)),
      m_keyStore(makeSystemKeyStore()),
      m_settings(std::make_unique<Settings>(*wxConfigBase::Get(), *m_keyStore))
{
    // The chat pane needs the client, so it exists before the layout.
    m_sidecar = std::make_unique<SidecarClient>(this);
    buildMenuBar();
    buildLayout();
    CreateStatusBar();
    SetStatusText("Ready");
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::onClose, this);
    startSidecar();

    // Temporary until the project panel opens files (M6).
    if (!pdfPath.empty()) {
        std::string error;
        if (m_pdfView->openFile(pdfPath.utf8_string(), error)) {
            // The PDF's folder is the project until M6 adds real projects.
            wxFileName file(pdfPath);
            file.MakeAbsolute();
            m_chat->openProject(file.GetPath().utf8_string());
        } else {
            wxLogError("Cannot open %s: %s", pdfPath, error);
        }
    }
}

MainFrame::~MainFrame() = default;

void MainFrame::startSidecar()
{
    Bind(EVT_SIDECAR_READY, &MainFrame::onSidecarReady, this);
    Bind(EVT_SIDECAR_DOWN, &MainFrame::onSidecarDown, this);
    Bind(EVT_SIDECAR_MESSAGE, &MainFrame::onSidecarMessage, this);
    if (m_sidecar->start()) {
        SetStatusText("Starting assistant...");
    } else {
        SetStatusText("Assistant unavailable: reader-sidecar not found");
        m_chat->setUnavailable("The assistant is unavailable: reader-sidecar was not found.");
    }
}

void MainFrame::buildMenuBar()
{
    auto* fileMenu = new wxMenu();
    fileMenu->Append(wxID_EXIT, "&Quit\tCtrl+Q");

    // On macOS wx moves wxID_PREFERENCES to the app menu.
    auto* editMenu = new wxMenu();
    editMenu->Append(wxID_PREFERENCES, "&Preferences...");

    auto* helpMenu = new wxMenu();
    helpMenu->Append(wxID_ABOUT, "&About");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(editMenu, "&Edit");
    menuBar->Append(helpMenu, "&Help");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::onQuit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::onAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::onPreferences, this, wxID_PREFERENCES);
}

void MainFrame::buildLayout()
{
    // outer: [left | inner], inner: [center | right].
    auto* outer = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxSP_LIVE_UPDATE);
    auto* inner = new wxSplitterWindow(outer, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxSP_LIVE_UPDATE);
    outer->SetMinimumPaneSize(kMinPaneWidth);
    inner->SetMinimumPaneSize(kMinPaneWidth);

    wxPanel* left = makePane(outer, "Project");
    m_pdfView = new PdfView(inner);
    m_chat = new ChatPanel(inner, *m_sidecar);

    // Resizing the window grows or shrinks only the center pane.
    outer->SetSashGravity(0.0);
    inner->SetSashGravity(1.0);

    inner->SplitVertically(m_pdfView, m_chat, -kRightPaneWidth);
    outer->SplitVertically(left, inner, kLeftPaneWidth);
}

wxPanel* MainFrame::makePane(wxWindow* parent, const wxString& label)
{
    auto* panel = new wxPanel(parent);
    auto* text = new wxStaticText(panel, wxID_ANY, label);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(text, 0, wxALIGN_CENTER_HORIZONTAL);
    sizer->AddStretchSpacer();
    panel->SetSizer(sizer);
    return panel;
}

void MainFrame::onQuit(wxCommandEvent& /*event*/)
{
    Close(true);
}

void MainFrame::onClose(wxCloseEvent& event)
{
    m_sidecar->shutdown();
    event.Skip();
}

void MainFrame::onSidecarReady(wxThreadEvent& /*event*/)
{
    SetStatusText("Assistant ready");
}

void MainFrame::onSidecarDown(wxThreadEvent& event)
{
    SetStatusText("Assistant stopped, restarting...");
    m_chat->onSidecarDown(event.GetString());
}

void MainFrame::onSidecarMessage(wxThreadEvent& event)
{
    m_chat->onSidecarMessage(event.GetPayload<protocol::Outbound>());
}

void MainFrame::onPreferences(wxCommandEvent& /*event*/)
{
    SettingsDialog dialog(this, *m_settings);
    dialog.ShowModal();
}

void MainFrame::onAbout(wxCommandEvent& /*event*/)
{
    wxAboutDialogInfo info;
    info.SetName("Agentic Reader");
    info.SetDescription("A PDF reader with a chat pane that knows what you are reading.");
    wxAboutBox(info, this);
}
