#pragma once

#include <wx/event.h>
#include <wx/fswatcher.h>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/treebase.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ProjectTreeNode;
class wxButton;
class wxStaticText;
class wxTimerEvent;
class wxTreeCtrl;
class wxTreeEvent;

// Queued by ProjectPanel when a PDF row is activated (double click or
// Return). GetString() is the PDF's absolute path.
wxDECLARE_EVENT(EVT_PROJECT_FILE_ACTIVATED, wxCommandEvent);
// Queued by ProjectPanel when "Save as project" is pressed.
wxDECLARE_EVENT(EVT_PROJECT_SAVE_REQUESTED, wxCommandEvent);
// Queued by ProjectPanel when files or folders are dropped on it. GetString()
// is the first dropped path.
wxDECLARE_EVENT(EVT_PROJECT_PATH_DROPPED, wxCommandEvent);

// The left pane: a header naming the project, and a tree of its folders and
// PDFs (buildProjectTree()). An unsaved project's header says "Unsaved
// project" and has a "Save as project" button. The tree follows changes made
// outside the app through a wxFileSystemWatcher, refreshed 300 ms after the
// last change.
class ProjectPanel : public wxPanel
{
public:
    explicit ProjectPanel(wxWindow* parent);
    ~ProjectPanel() override;

    // Shows the project at the absolute folder `root`, or no project when
    // `root` is empty. Starts watching it, which needs a running event loop.
    void setProject(const std::filesystem::path& root, bool saved);
    // The open project gained (or lost) its .reader/ folder.
    void setSaved(bool saved);
    // Selects the PDF row with this path relative to the root, if shown.
    void selectFile(const std::string& rel);

    const std::filesystem::path& root() const { return m_root; }

private:
    void updateHeader();
    // Rebuilds the tree from disk and watches every folder that can hold a
    // PDF, so folders created later are seen too.
    void refresh();
    void watchFolders();
    void addRows(const wxTreeItemId& parent, const std::vector<ProjectTreeNode>& nodes);
    // The row of `rel` under `parent`, or an invalid id.
    wxTreeItemId findRow(const wxTreeItemId& parent, const std::string& rel) const;

    void onActivated(wxTreeEvent& event);
    void onSelChanged(wxTreeEvent& event);
    void onSave(wxCommandEvent& event);
    void onFileSystem(wxFileSystemWatcherEvent& event);
    void onRefreshTimer(wxTimerEvent& event);

    wxStaticText* m_title = nullptr;
    wxStaticText* m_unsaved = nullptr;
    wxButton* m_save = nullptr;
    wxTreeCtrl* m_tree = nullptr;
    std::unique_ptr<wxFileSystemWatcher> m_watcher;
    wxTimer m_refreshTimer;

    std::filesystem::path m_root;
    bool m_saved = false;
    // The selected PDF, kept across refreshes.
    std::string m_selected;
};
