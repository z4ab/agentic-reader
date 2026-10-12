#include "MainFrame.h"

#include "agent/SidecarClient.h"
#include "chat/ChatPanel.h"
#include "chat/SetupBanner.h"
#include "pdf/FindBar.h"
#include "pdf/OpenPdf.h"
#include "pdf/PdfEvents.h"
#include "pdf/PdfView.h"
#include "project/Instructions.h"
#include "project/InstructionsDialog.h"
#include "project/Project.h"
#include "project/ProjectPanel.h"
#include "project/ProjectTree.h"
#include "settings/Settings.h"
#include "settings/SettingsDialog.h"

#include <wx/aboutdlg.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/config.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filehistory.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>

#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace fs = std::filesystem;

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
// File > Recent: wxFileHistory items take consecutive IDs from the base.
constexpr int kRecentFileCount = 10;
constexpr int kRecentFileIdBase = wxID_HIGHEST + 1;
// wxConfig group of the recent files, saved as file1..file10.
const char* const kRecentFilesPath = "/RecentFiles/";
// File > Recent Projects, the same way, with the IDs after Recent's.
constexpr int kRecentProjectCount = 10;
constexpr int kRecentProjectIdBase = kRecentFileIdBase + kRecentFileCount;
const char* const kRecentProjectsPath = "/RecentProjects/";
// The project open at the last exit, saved or unsaved.
const char* const kLastProjectKey = "/Session/LastProject";

wxString toWx(const fs::path& path)
{
#ifdef _WIN32
    return wxString(path.wstring());
#else
    return wxString::FromUTF8(path.string());
#endif
}

// Absolute and lexically normal, without a trailing separator.
fs::path normalPath(const wxString& path)
{
#ifdef _WIN32
    fs::path p(path.ToStdWstring());
#else
    fs::path p(path.utf8_string());
#endif
    std::error_code ec;
    p = fs::absolute(p, ec).lexically_normal();
    if (p.has_relative_path() && !p.has_filename())
        p = p.parent_path();
    return p;
}

bool isSavedProject(const fs::path& root)
{
    std::error_code ec;
    return fs::is_directory(root / ".reader", ec);
}

// `path` relative to `root` with '/' separators, or empty when it is not
// below `root`.
std::string relativeTo(const fs::path& path, const fs::path& root)
{
    const fs::path rel = path.lexically_relative(root);
    if (rel.empty() || rel == "." || *rel.begin() == "..")
        return {};
    return rel.generic_string();
}
} // namespace

MainFrame::MainFrame()
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
    showPage();
    showZoom();
    m_pdfView->SetFocus();
}

MainFrame::~MainFrame() = default;

void MainFrame::openFile(const wxString& path)
{
    wxFileName file(path);
    file.MakeAbsolute();
    const wxString fullPath = file.GetFullPath();
    const wxString name = file.GetFullName();

    auto askPassword = [this, &name](bool wrongPassword) -> std::optional<std::string> {
        wxString prompt = wxString::Format("\"%s\" is protected. Enter its password:", name);
        if (wrongPassword)
            prompt = "The password is incorrect. Try again.\n\n" + prompt;
        wxPasswordEntryDialog dialog(this, prompt, "Password Required");
        if (dialog.ShowModal() != wxID_OK)
            return std::nullopt;
        return dialog.GetValue().utf8_string();
    };
    OpenPdfResult result = openPdf(fullPath.utf8_string(), askPassword);
    if (result.cancelled)
        return;
    if (!result.doc) {
        wxMessageBox(wxString::Format("Cannot open \"%s\": %s.", name,
                                      wxString::FromUTF8(result.error)),
                     "Cannot Open File", wxOK | wxICON_ERROR, this);
        return;
    }

    // A PDF of the open project keeps it and its chat. Any other PDF opens
    // the saved project it belongs to, or its folder as an unsaved project.
    // The project being left records its open file first.
    const fs::path pdf = normalPath(fullPath);
    const fs::path folder = projectFolderFor(pdf, m_projectRoot);
    if (folder != m_projectRoot)
        enterProject(folder, false);

    m_findBar->dismiss();
    m_pdfView->setDocument(std::move(result.doc), std::move(result.password));
    m_fileHistory->AddFileToHistory(fullPath);
    saveFileHistory();
    showOpenedFile();
    m_projectPanel->selectFile(relativeTo(pdf, m_projectRoot));
    saveProjectState();
}

void MainFrame::restoreSession()
{
    const wxString last = wxConfigBase::Get()->Read(kLastProjectKey, wxString());
    if (last.empty() || !wxFileName::DirExists(last))
        return;
    const fs::path root = normalPath(last);
    if (isSavedProject(root)) {
        openProjectFolder(root, false);
        return;
    }
    // An unsaved project keeps no open_file. The most recent file stands in
    // when it belongs there.
    if (m_fileHistory->GetCount() > 0) {
        const wxString file = m_fileHistory->GetHistoryFile(0);
        if (wxFileName::FileExists(file) && projectFolderFor(normalPath(file), {}) == root) {
            openFile(file);
            return;
        }
    }
    enterProject(root, false);
    closeFile();
}

void MainFrame::openProjectFolder(const fs::path& root, bool save)
{
    if (root == m_projectRoot) {
        if (save && !m_project)
            saveAsProject();
        return;
    }
    enterProject(root, save);
    if (m_project && !m_project->openFile().empty()) {
        const fs::path file = root / fs::path(m_project->openFile());
        std::error_code ec;
        if (fs::is_regular_file(file, ec))
            openFile(toWx(file));
    }
    // With no open_file, or one that failed to open, the PDF of the project
    // left behind must not stay on screen.
    if (relativeTo(m_pdfView->filePath(), m_projectRoot).empty())
        closeFile();
}

void MainFrame::enterProject(const fs::path& root, bool save)
{
    saveProjectState();
    // The chat pane lets go of the project before it is closed.
    m_chat->closeProject();
    m_project.reset();
    m_projectRoot = root;

    if (save || isSavedProject(root)) {
        std::string error;
        m_project = Project::open(root, error);
        if (!m_project) {
            wxMessageBox(wxString::Format("Cannot save \"%s\" as a project: %s.\n\n"
                                          "It is open as an unsaved project.",
                                          toWx(root.filename()), wxString::FromUTF8(error)),
                         "Cannot Open Project", wxOK | wxICON_WARNING, this);
        } else if (!m_project->warnings().empty()) {
            wxString text;
            for (const std::string& warning : m_project->warnings())
                text += wxString::FromUTF8(warning) + "\n";
            wxMessageBox(text, "Project", wxOK | wxICON_WARNING, this);
        }
    }

    m_projectPanel->setProject(root, m_project != nullptr);
    // The chat pane shows the project's chats. An unsaved project's chats
    // stay in memory only.
    m_chat->openProject(toWx(root).utf8_string(), m_project.get());
    if (m_project)
        addRecentProject();

    wxConfigBase* config = wxConfigBase::Get();
    config->Write(kLastProjectKey, toWx(root));
    config->Flush();
}

void MainFrame::saveProjectState()
{
    if (!m_project || m_project->readOnly())
        return;
    const std::string rel = relativeTo(m_pdfView->filePath(), m_projectRoot);
    if (rel.empty() || rel == m_project->openFile())
        return;
    m_project->setOpenFile(rel);
    saveProject();
}

void MainFrame::saveProject()
{
    std::string error;
    if (!m_project->save(error))
        wxMessageBox(wxString::Format("Cannot save the project \"%s\": %s.",
                                      toWx(m_projectRoot.filename()), wxString::FromUTF8(error)),
                     "Cannot Save Project", wxOK | wxICON_ERROR, this);
}

void MainFrame::saveAsProject()
{
    if (m_projectRoot.empty() || m_project)
        return;
    std::string error;
    m_project = Project::open(m_projectRoot, error);
    if (!m_project) {
        wxMessageBox(wxString::Format("Cannot save \"%s\" as a project: %s.",
                                      toWx(m_projectRoot.filename()), wxString::FromUTF8(error)),
                     "Cannot Save Project", wxOK | wxICON_ERROR, this);
        return;
    }
    // The chats go on and are saved from here.
    m_chat->attachProject(m_project.get());
    m_projectPanel->setSaved(true);
    addRecentProject();
    m_project->setOpenFile(relativeTo(m_pdfView->filePath(), m_projectRoot));
    saveProject();
}

void MainFrame::addRecentProject()
{
    m_projectHistory->AddFileToHistory(toWx(m_projectRoot));
    saveProjectHistory();
}

void MainFrame::showOpenedFile()
{
    SetStatusText(wxString::FromUTF8(m_pdfView->filePath().filename().string()), kFileField);
    m_pageTotal->SetLabel(wxString::Format("/ %d", m_pdfView->pageCount()));
    // The total's width changed.
    GetToolBar()->Realize();
    showPage();
    showZoom();
    m_pdfView->SetFocus();
}

void MainFrame::closeFile()
{
    m_findBar->dismiss();
    m_pdfView->closeDocument();
    m_projectPanel->selectFile(std::string());
    SetStatusText("Ready", kFileField);
    m_pageTotal->SetLabel("/ 0");
    GetToolBar()->Realize();
    showPage();
    showZoom();
}

void MainFrame::saveFileHistory()
{
    wxConfigBase* config = wxConfigBase::Get();
    {
        wxConfigPathChanger group(config, kRecentFilesPath);
        m_fileHistory->Save(*config);
    }
    config->Flush();
}

void MainFrame::saveProjectHistory()
{
    wxConfigBase* config = wxConfigBase::Get();
    {
        wxConfigPathChanger group(config, kRecentProjectsPath);
        m_projectHistory->Save(*config);
    }
    config->Flush();
}

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
    fileMenu->Append(wxID_OPEN, "&Open...\tCtrl+O");
    const int openProjectId = wxWindow::NewControlId();
    fileMenu->Append(openProjectId, "Open &Project...\tCtrl+Shift+O");
    auto* recentMenu = new wxMenu();
    fileMenu->AppendSubMenu(recentMenu, "&Recent");
    auto* recentProjectsMenu = new wxMenu();
    fileMenu->AppendSubMenu(recentProjectsMenu, "Recent Pro&jects");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "&Quit\tCtrl+Q");

    m_fileHistory = std::make_unique<wxFileHistory>(kRecentFileCount, kRecentFileIdBase);
    m_fileHistory->UseMenu(recentMenu);
    {
        wxConfigBase* config = wxConfigBase::Get();
        wxConfigPathChanger group(config, kRecentFilesPath);
        m_fileHistory->Load(*config);
    }
    m_projectHistory = std::make_unique<wxFileHistory>(kRecentProjectCount, kRecentProjectIdBase);
    m_projectHistory->UseMenu(recentProjectsMenu);
    {
        wxConfigBase* config = wxConfigBase::Get();
        wxConfigPathChanger group(config, kRecentProjectsPath);
        m_projectHistory->Load(*config);
    }

    // On macOS wx moves wxID_PREFERENCES to the app menu.
    auto* editMenu = new wxMenu();
    editMenu->Append(wxID_FIND, "&Find...\tCtrl+F");
    editMenu->AppendSeparator();
    editMenu->Append(wxID_PREFERENCES, "&Preferences...");

    auto* projectMenu = new wxMenu();
    const int instructionsId = wxWindow::NewControlId();
    projectMenu->Append(instructionsId, "&Instructions...");

    auto* helpMenu = new wxMenu();
    helpMenu->Append(wxID_ABOUT, "&About");

    auto* menuBar = new wxMenuBar();
    menuBar->Append(fileMenu, "&File");
    menuBar->Append(editMenu, "&Edit");
    menuBar->Append(projectMenu, "&Project");
    menuBar->Append(helpMenu, "&Help");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::onOpen, this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::onRecent, this, kRecentFileIdBase,
         kRecentFileIdBase + kRecentFileCount - 1);
    Bind(wxEVT_MENU, &MainFrame::onOpenProject, this, openProjectId);
    Bind(wxEVT_MENU, &MainFrame::onRecentProject, this, kRecentProjectIdBase,
         kRecentProjectIdBase + kRecentProjectCount - 1);
    Bind(wxEVT_MENU, &MainFrame::onQuit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::onAbout, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::onPreferences, this, wxID_PREFERENCES);
    Bind(wxEVT_MENU, &MainFrame::onInstructions, this, instructionsId);
    // CLAUDE.md lives in the project folder, so any open project, saved or
    // unsaved, can have one.
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) { event.Enable(!m_projectRoot.empty()); },
         instructionsId);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_findBar->activate(); }, wxID_FIND);
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

    m_projectPanel = new ProjectPanel(outer);
    m_projectPanel->Bind(EVT_PROJECT_FILE_ACTIVATED, &MainFrame::onProjectFileActivated, this);
    m_projectPanel->Bind(EVT_PROJECT_SAVE_REQUESTED, &MainFrame::onSaveAsProject, this);
    m_projectPanel->Bind(EVT_PROJECT_PATH_DROPPED, &MainFrame::onPathDropped, this);
    // The find bar sits above the PDF view in the center pane.
    auto* center = new wxPanel(inner, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0);
    m_pdfView = new PdfView(center);
    m_findBar = new FindBar(center, m_pdfView);
    m_findBar->MoveBeforeInTabOrder(m_pdfView);
    auto* centerSizer = new wxBoxSizer(wxVERTICAL);
    centerSizer->Add(m_findBar, 0, wxEXPAND);
    centerSizer->Add(m_pdfView, 1, wxEXPAND);
    center->SetSizer(centerSizer);
    m_pdfView->Bind(EVT_PDF_PAGE_CHANGED, &MainFrame::onPdfPageChanged, this);
    m_pdfView->Bind(EVT_PDF_ZOOM_CHANGED, &MainFrame::onPdfZoomChanged, this);
    m_pdfView->Bind(EVT_PDF_FILE_DROPPED, &MainFrame::onPathDropped, this);
    m_chat = new ChatPanel(inner, *m_sidecar);

    // Resizing the window grows or shrinks only the center pane.
    outer->SetSashGravity(0.0);
    inner->SetSashGravity(1.0);

    inner->SplitVertically(center, m_chat, -kRightPaneWidth);
    outer->SplitVertically(m_projectPanel, inner, kLeftPaneWidth);
}

void MainFrame::onOpen(wxCommandEvent& /*event*/)
{
    wxString dir;
    if (!m_pdfView->filePath().empty())
        dir = wxString::FromUTF8(m_pdfView->filePath().parent_path().string());
    wxFileDialog dialog(this, "Open PDF", dir, wxString(), "PDF files (*.pdf)|*.pdf",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        openFile(dialog.GetPath());
}

void MainFrame::onRecent(wxCommandEvent& event)
{
    const auto index = static_cast<size_t>(event.GetId() - kRecentFileIdBase);
    if (index >= m_fileHistory->GetCount())
        return;
    const wxString path = m_fileHistory->GetHistoryFile(index);
    openFile(path);
    // A file that is gone leaves the list. The error box already said so.
    if (!wxFileName::FileExists(path)) {
        m_fileHistory->RemoveFileFromHistory(index);
        saveFileHistory();
    }
}

void MainFrame::onOpenProject(wxCommandEvent& /*event*/)
{
    wxDirDialog dialog(this, "Open Project", toWx(m_projectRoot),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        openProjectFolder(normalPath(dialog.GetPath()), true);
}

void MainFrame::onRecentProject(wxCommandEvent& event)
{
    const auto index = static_cast<size_t>(event.GetId() - kRecentProjectIdBase);
    if (index >= m_projectHistory->GetCount())
        return;
    const wxString path = m_projectHistory->GetHistoryFile(index);
    if (!wxFileName::DirExists(path)) {
        wxMessageBox(wxString::Format("Cannot open project \"%s\": folder not found.", path),
                     "Cannot Open Project", wxOK | wxICON_ERROR, this);
        m_projectHistory->RemoveFileFromHistory(index);
        saveProjectHistory();
        return;
    }
    openProjectFolder(normalPath(path), true);
}

void MainFrame::onPathDropped(wxCommandEvent& event)
{
    // A dropped folder opens as a project, like Open Project. A file opens
    // like File > Open, wherever it lands.
    const wxString path = event.GetString();
    if (wxFileName::DirExists(path))
        openProjectFolder(normalPath(path), true);
    else
        openFile(path);
}

void MainFrame::onProjectFileActivated(wxCommandEvent& event)
{
    openFile(event.GetString());
}

void MainFrame::onSaveAsProject(wxCommandEvent& /*event*/)
{
    saveAsProject();
}

void MainFrame::onInstructions(wxCommandEvent& /*event*/)
{
    if (m_projectRoot.empty())
        return;
    std::string text;
    std::string error;
    if (!loadInstructions(m_projectRoot, text, error)) {
        wxMessageBox(wxString::Format("Could not read the instructions.\n\n%s",
                                      wxString::FromUTF8(error)),
                     "Cannot Open Instructions", wxOK | wxICON_ERROR, this);
        return;
    }
    const wxString value = wxString::FromUTF8(text);
    // Saving text that did not convert would lose the file's content.
    if (value.empty() && !text.empty()) {
        wxMessageBox(wxString::Format("Could not read the instructions: %s is not UTF-8 text.",
                                      toWx(instructionsPath(m_projectRoot))),
                     "Cannot Open Instructions", wxOK | wxICON_ERROR, this);
        return;
    }
    InstructionsDialog dialog(this, m_projectRoot, value);
    if (dialog.ShowModal() != wxID_SAVE)
        return;
    // The sidecar is not told: each new chat reads CLAUDE.md when it starts.
    wxMessageBox("The instructions are saved. They apply to new chats. Chats that are already "
                 "open may keep the instructions they started with.",
                 "Instructions Saved", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::onQuit(wxCommandEvent& /*event*/)
{
    Close(true);
}

void MainFrame::onClose(wxCloseEvent& event)
{
    saveProjectState();
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
