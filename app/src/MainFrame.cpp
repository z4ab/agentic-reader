#include "MainFrame.h"

#include "agent/SidecarClient.h"
#include "chat/ChatPanel.h"
#include "chat/SetupBanner.h"
#include "pdf/PdfEvents.h"
#include "pdf/PdfView.h"
#include "settings/Settings.h"
#include "settings/SettingsDialog.h"

#include <wx/aboutdlg.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/config.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>

#include <cmath>
#include <iterator>
#include <map>
#include <tuple>

namespace
{
constexpr int kLeftPaneWidth = 220;
constexpr int kRightPaneWidth = 420;
constexpr int kMinPaneWidth = 100;
constexpr int kZoomComboWidth = 100;
constexpr int kPageEntryWidth = 60;
// Status bar: the open file's name, then the assistant's state.
constexpr int kFileField = 0;
constexpr int kAssistantField = 1;
constexpr int kStatusFieldCount = 2;
} // namespace

MainFrame::MainFrame(const wxString& pdfPath)
    : wxFrame(nullptr, wxID_ANY, "Agentic Reader", wxDefaultPosition, wxSize(1400, 900)),
      m_keyStore(makeSystemKeyStore()),
      m_settings(std::make_unique<Settings>(*wxConfigBase::Get(), *m_keyStore))
{
    // The chat pane needs the client, so it exists before the layout.
    m_sidecar = std::make_unique<SidecarClient>(this);
    buildMenuBar();
    buildToolBar();
    buildLayout();
    CreateStatusBar(kStatusFieldCount);
    SetStatusText("Ready", kFileField);
    m_chat->setSetupNeeded(needsSetup(*m_settings));
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::onClose, this);
    startSidecar();

    // Temporary until the project panel opens files (M6).
    if (!pdfPath.empty()) {
        std::string error;
        if (m_pdfView->openFile(pdfPath.utf8_string(), error)) {
            SetStatusText(wxString::FromUTF8(m_pdfView->filePath().filename().string()),
                          kFileField);
            // The PDF's folder is the project until M6 adds real projects.
            wxFileName file(pdfPath);
            file.MakeAbsolute();
            m_chat->openProject(file.GetPath().utf8_string());
        } else {
            wxLogError("Cannot open %s: %s", pdfPath, error);
        }
    }
    m_pageTotal->SetLabel(wxString::Format("/ %d", m_pdfView->pageCount()));
    GetToolBar()->Realize();
    showPage();
    showZoom();
    m_pdfView->SetFocus();
}

MainFrame::~MainFrame() = default;

void MainFrame::startSidecar()
{
    Bind(EVT_SIDECAR_READY, &MainFrame::onSidecarReady, this);
    Bind(EVT_SIDECAR_DOWN, &MainFrame::onSidecarDown, this);
    Bind(EVT_SIDECAR_MESSAGE, &MainFrame::onSidecarMessage, this);
    // Built at every spawn, so restarts pick up saved settings.
    m_sidecar->setEnvironmentSource(
        [this](const std::map<wxString, wxString>& inherited) {
            return m_settings->sidecarEnv(inherited);
        });
    if (m_sidecar->start())
        SetStatusText("Starting assistant...", kAssistantField);
    else
        showSidecarMissing();
}

void MainFrame::showSidecarMissing()
{
    SetStatusText("Assistant unavailable: reader-sidecar not found", kAssistantField);
    m_chat->setUnavailable("The assistant is unavailable: reader-sidecar was not found.");
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

void MainFrame::buildToolBar()
{
    wxToolBar* toolBar = CreateToolBar(wxTB_HORIZONTAL | wxTB_FLAT);

    const wxString zoomChoices[] = {"25%", "50%", "75%", "100%", "125%",
                                    "150%", "200%", "400%", "800%"};
    m_zoomCombo = new wxComboBox(toolBar, wxID_ANY, "", wxDefaultPosition,
                                 FromDIP(wxSize(kZoomComboWidth, -1)), std::size(zoomChoices),
                                 zoomChoices, wxTE_PROCESS_ENTER);
    toolBar->AddControl(m_zoomCombo);
    auto* fitWidth = new wxButton(toolBar, wxID_ANY, "Fit Width");
    toolBar->AddControl(fitWidth);
    auto* fitPage = new wxButton(toolBar, wxID_ANY, "Fit Page");
    toolBar->AddControl(fitPage);
    toolBar->AddSeparator();
    m_pageEntry = new wxTextCtrl(toolBar, wxID_ANY, "", wxDefaultPosition,
                                 FromDIP(wxSize(kPageEntryWidth, -1)), wxTE_PROCESS_ENTER);
    toolBar->AddControl(m_pageEntry);
    m_pageTotal = new wxStaticText(toolBar, wxID_ANY, "/ 0");
    toolBar->AddControl(m_pageTotal);
    toolBar->Realize();

    m_zoomCombo->Bind(wxEVT_COMBOBOX, &MainFrame::onZoomEntered, this);
    m_zoomCombo->Bind(wxEVT_TEXT_ENTER, &MainFrame::onZoomEntered, this);
    m_pageEntry->Bind(wxEVT_TEXT_ENTER, &MainFrame::onPageEntered, this);
    fitWidth->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_pdfView->fitWidth();
        m_pdfView->SetFocus();
    });
    fitPage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_pdfView->fitPage();
        m_pdfView->SetFocus();
    });
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
    m_pdfView->Bind(EVT_PDF_PAGE_CHANGED, &MainFrame::onPdfPageChanged, this);
    m_pdfView->Bind(EVT_PDF_ZOOM_CHANGED, &MainFrame::onPdfZoomChanged, this);
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
    SetStatusText("Assistant ready", kAssistantField);
}

void MainFrame::onSidecarDown(wxThreadEvent& event)
{
    SetStatusText("Assistant stopped, restarting...", kAssistantField);
    m_chat->onSidecarDown(event.GetString());
}

void MainFrame::onSidecarMessage(wxThreadEvent& event)
{
    m_chat->onSidecarMessage(event.GetPayload<protocol::Outbound>());
}

void MainFrame::onPreferences(wxCommandEvent& /*event*/)
{
    // Restart only when a setting changed. The key is compared in memory
    // and never leaves this function.
    auto snapshot = [this] {
        return std::make_tuple(m_settings->model(), m_settings->useLocalLogin(),
                               m_settings->apiKey());
    };
    const auto before = snapshot();
    SettingsDialog dialog(this, *m_settings);
    if (dialog.ShowModal() != wxID_OK)
        return;
    m_chat->setSetupNeeded(needsSetup(*m_settings));
    // An unavailable assistant gets another try, in case it was reinstalled.
    if (snapshot() != before || !m_chat->available())
        restartSidecar();
}

void MainFrame::restartSidecar()
{
    const wxString notice = "Settings changed, sidecar restarted";
    m_chat->interruptReply();
    if (!m_sidecar->restart()) {
        showSidecarMissing();
        return;
    }
    SetStatusText(notice, kAssistantField);
    m_chat->onSidecarRestarted(notice);
}

void MainFrame::onAbout(wxCommandEvent& /*event*/)
{
    wxAboutDialogInfo info;
    info.SetName("Agentic Reader");
    info.SetVersion(READER_VERSION);
    info.SetDescription("Agentic Reader. Powered by Claude.\n"
                        "A PDF reader with a chat pane that knows what you are reading.");
    wxAboutBox(info, this);
}

void MainFrame::onZoomEntered(wxCommandEvent& event)
{
    wxString text = event.GetString();
    text.Replace("%", "");
    double percent = 0;
    if (text.Trim().Trim(false).ToCDouble(&percent) && percent > 0)
        m_pdfView->setZoom(percent / 100.0);
    showZoom();
    m_pdfView->SetFocus();
}

void MainFrame::onPageEntered(wxCommandEvent& event)
{
    long page = 0;
    if (event.GetString().Trim().Trim(false).ToLong(&page))
        m_pdfView->goToPage(static_cast<int>(page) - 1);
    showPage();
    m_pdfView->SetFocus();
}

void MainFrame::onPdfPageChanged(wxCommandEvent& /*event*/)
{
    showPage();
}

void MainFrame::onPdfZoomChanged(wxCommandEvent& /*event*/)
{
    showZoom();
}

void MainFrame::showPage()
{
    const int page = m_pdfView->currentPage();
    m_pageEntry->ChangeValue(page < 0 ? wxString() : wxString::Format("%d", page + 1));
}

void MainFrame::showZoom()
{
    m_zoomCombo->ChangeValue(wxString::Format("%ld%%", std::lround(m_pdfView->zoom() * 100)));
}
