#include "MainFrame.h"

#include "pdf/PdfView.h"

#include <wx/aboutdlg.h>
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
    : wxFrame(nullptr, wxID_ANY, "Agentic Reader", wxDefaultPosition, wxSize(1400, 900))
{
    buildMenuBar();
    buildLayout();
    CreateStatusBar();
    SetStatusText("Ready");

    // Temporary until the project panel opens files (M6).
    if (!pdfPath.empty()) {
        std::string error;
        if (!m_pdfView->openFile(pdfPath.utf8_string(), error))
            wxLogError("Cannot open %s: %s", pdfPath, error);
    }
}

void MainFrame::buildMenuBar()
{
    auto* fileMenu = new wxMenu();
    fileMenu->Append(wxID_EXIT, "&Quit\tCtrl+Q");

    auto* helpMenu = new wxMenu();
    helpMenu->Append(wxID_ABOUT, "&About");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(helpMenu, "&Help");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::onQuit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::onAbout, this, wxID_ABOUT);
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
    wxPanel* right = makePane(inner, "Chat");

    // Resizing the window grows or shrinks only the center pane.
    outer->SetSashGravity(0.0);
    inner->SetSashGravity(1.0);

    inner->SplitVertically(m_pdfView, right, -kRightPaneWidth);
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

void MainFrame::onAbout(wxCommandEvent& /*event*/)
{
    wxAboutDialogInfo info;
    info.SetName("Agentic Reader");
    info.SetDescription("A PDF reader with a chat pane that knows what you are reading.");
    wxAboutBox(info, this);
}
