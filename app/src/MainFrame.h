#pragma once

#include "chat/ReaderContext.h"

#include <wx/frame.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <filesystem>
#include <memory>

class ChatPanel;
struct FileState;
class FindBar;
class KeyStore;
class PdfView;
class Project;
class ProjectPanel;
class Settings;
class SidecarClient;
class wxButton;
class wxCloseEvent;
class wxComboBox;
class wxCommandEvent;
class wxFileHistory;
class wxSplitterWindow;
class wxStaticText;
class wxTextCtrl;
class wxTimerEvent;
class wxToggleButton;
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
    // Open, File > Recent, drag and drop, the project panel and the command
    // line. A PDF outside the open project switches projects (projectFolderFor()).
    void openFile(const wxString& path);

    // Reopens the last project and its open file. Called once at start, after
    // the event loop runs, when no file was given on the command line.
    void restoreSession();

private:
    void buildMenuBar();
    void buildToolBar();
    void buildLayout();

    void startSidecar();
    void restartSidecar();
    // No sidecar was found: the status bar and the chat pane say so.
    void showSidecarMissing();

    // Shows the open document's name in the status bar and its page count
    // and zoom in the toolbar.
    void showOpenedFile();
    // Empties the PDF view, the file name in the status bar and the page
    // total.
    void closeFile();
    void saveFileHistory();
    void saveProjectHistory();

    // Makes the folder `root` the open project and opens its open_file, if
    // any. `save` creates its .reader/ (Open Project, Recent Projects, a
    // dropped folder). Without it a folder with no .reader/ opens unsaved.
    void openProjectFolder(const std::filesystem::path& root, bool save);
    // Leaves the open project and makes `root` the open one: the panel shows
    // it and the chat moves to it. Does not open a file.
    void enterProject(const std::filesystem::path& root, bool save);
    // Records the open file as the open project's open_file, with its page
    // and zoom, and writes .reader/project.json when that changed. Does
    // nothing for an unsaved project or a file outside the project. Cancels a
    // pending debounced save.
    void saveProjectState();
    // The open file's page and zoom as kept in project.json.
    FileState currentFileState() const;
    // Shows the just opened file at the page and zoom saved in the open
    // project. A file with no saved state keeps page 1 and the view's zoom.
    void restoreFileState();
    // Writes .reader/project.json, with a message box on failure.
    void saveProject();
    // Creates .reader/ in the open unsaved project.
    void saveAsProject();
    void addRecentProject();

    void onOpen(wxCommandEvent& event);
    void onRecent(wxCommandEvent& event);
    void onOpenProject(wxCommandEvent& event);
    void onRecentProject(wxCommandEvent& event);
    void onPathDropped(wxCommandEvent& event);
    void onProjectFileActivated(wxCommandEvent& event);
    void onSaveAsProject(wxCommandEvent& event);
    // Project > Instructions: edits the open project's CLAUDE.md.
    void onInstructions(wxCommandEvent& event);
    void onQuit(wxCommandEvent& event);
    void onAbout(wxCommandEvent& event);
    void onPreferences(wxCommandEvent& event);
    void onClose(wxCloseEvent& event);
    void onSidecarReady(wxThreadEvent& event);
    void onSidecarDown(wxThreadEvent& event);
    void onSidecarMessage(wxThreadEvent& event);

    // What the PDF view shows, for the chat's reader context. Loads the text
    // of selected pages not rendered yet first.
    readercontext::Snapshot readerSnapshot();
    // "Ask about selection" (Ctrl+Shift+A, the PDF context menu): quotes the
    // selection in the chat input. Does nothing without a selection.
    void askAboutSelection();

    // F11 fullscreen: entering hides the project pane, the chat pane and the
    // toolbar, leaving restores them at the sash positions they had.
    void setFullscreen(bool on);

    void onZoomEntered(wxCommandEvent& event);
    void onPageEntered(wxCommandEvent& event);
    void onPdfPageChanged(wxCommandEvent& event);
    void onPdfZoomChanged(wxCommandEvent& event);
    // Page and zoom changes are written this long after the last one.
    void schedulePositionSave();
    void onPositionTimer(wxTimerEvent& event);
    void showPage();
    void showZoom();
    // Syncs the toolbar toggle with the view's layout mode.
    void showLayout();

    // [project | inner] and [PDF | chat], for fullscreen's unsplit and restore.
    wxSplitterWindow* m_outerSplitter = nullptr;
    wxSplitterWindow* m_innerSplitter = nullptr;
    bool m_fullscreen = false;
    // Sash positions kept while fullscreen hides the panes.
    int m_outerSash = 0;
    int m_innerSash = 0;
    PdfView* m_pdfView = nullptr;
    // Above the PDF view, hidden until Edit > Find (Ctrl+F).
    FindBar* m_findBar = nullptr;
    wxComboBox* m_zoomCombo = nullptr;
    wxTextCtrl* m_pageEntry = nullptr;
    // Toolbar toggle, on in SinglePage.
    wxToggleButton* m_layoutToggle = nullptr;
    // Toolbar Previous and Next page buttons, on with a document in both layouts.
    wxButton* m_prevPageButton = nullptr;
    wxButton* m_nextPageButton = nullptr;
    wxStaticText* m_pageTotal = nullptr;
    // Edit > Ask about selection, also in the PDF context menu.
    int m_askAboutId = wxID_NONE;
    std::unique_ptr<KeyStore> m_keyStore;
    std::unique_ptr<Settings> m_settings;
    ChatPanel* m_chat = nullptr;
    ProjectPanel* m_projectPanel = nullptr;
    std::unique_ptr<SidecarClient> m_sidecar;
    std::unique_ptr<wxFileHistory> m_fileHistory;
    std::unique_ptr<wxFileHistory> m_projectHistory;
    // Root of the open project, saved or unsaved. Empty until one opens.
    std::filesystem::path m_projectRoot;
    // The open project's .reader/ state. Null for an unsaved project, which
    // keeps nothing on disk.
    std::unique_ptr<Project> m_project;
    // Debounces saving the reading position (schedulePositionSave()).
    wxTimer m_positionTimer;
};
