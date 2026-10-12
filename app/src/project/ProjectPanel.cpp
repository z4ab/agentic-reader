#include "project/ProjectPanel.h"

#include "project/ProjectTree.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>

#include <utility>

wxDEFINE_EVENT(EVT_PROJECT_FILE_ACTIVATED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PROJECT_SAVE_REQUESTED, wxCommandEvent);
wxDEFINE_EVENT(EVT_PROJECT_PATH_DROPPED, wxCommandEvent);

namespace fs = std::filesystem;

namespace {

// Changes outside the app are shown this long after the last one.
constexpr int kRefreshDelayMs = 300;
constexpr int kHeaderBorder = 6;

enum RowImage
{
    kFolderImage = 0,
    kPdfImage = 1,
};

wxString toWx(const fs::path& path)
{
#ifdef _WIN32
    return wxString(path.wstring());
#else
    return wxString::FromUTF8(path.string());
#endif
}

fs::path toPath(const wxString& path)
{
#ifdef _WIN32
    return fs::path(path.ToStdWstring());
#else
    return fs::path(path.utf8_string());
#endif
}

class RowData : public wxTreeItemData
{
public:
    RowData(std::string rel, bool folder) : rel(std::move(rel)), folder(folder) {}

    std::string rel;
    bool folder;
};

// Queues EVT_PROJECT_PATH_DROPPED on the panel for the first dropped path.
class PathDropTarget : public wxFileDropTarget
{
public:
    explicit PathDropTarget(wxWindow* panel) : m_panel(panel) {}

    bool OnDropFiles(wxCoord /*x*/, wxCoord /*y*/, const wxArrayString& files) override
    {
        if (files.empty())
            return false;
        auto* event = new wxCommandEvent(EVT_PROJECT_PATH_DROPPED, m_panel->GetId());
        event->SetEventObject(m_panel);
        event->SetString(files[0]);
        m_panel->GetEventHandler()->QueueEvent(event);
        return true;
    }

private:
    wxWindow* m_panel;
};

} // namespace

ProjectPanel::ProjectPanel(wxWindow* parent)
    : wxPanel(parent),
      m_refreshTimer(this)
{
    m_title = new wxStaticText(this, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_END);
    m_title->SetFont(m_title->GetFont().Bold());
    m_unsaved = new wxStaticText(this, wxID_ANY, "Unsaved project");
    m_save = new wxButton(this, wxID_ANY, "&Save as project");
    m_save->SetToolTip("Create a .reader folder here so chats and reading positions are kept");
    m_tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT | wxTR_SINGLE
                                | wxTR_LINES_AT_ROOT | wxBORDER_NONE);
    m_tree->SetImages({wxArtProvider::GetBitmapBundle(wxART_FOLDER, wxART_LIST),
                       wxArtProvider::GetBitmapBundle(wxART_NORMAL_FILE, wxART_LIST)});

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_title, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kHeaderBorder);
    sizer->Add(m_unsaved, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kHeaderBorder);
    sizer->Add(m_save, 0, wxLEFT | wxRIGHT | wxTOP, kHeaderBorder);
    sizer->AddSpacer(kHeaderBorder);
    sizer->Add(m_tree, 1, wxEXPAND);
    SetSizer(sizer);

    SetDropTarget(new PathDropTarget(this));
    m_tree->SetDropTarget(new PathDropTarget(this));

    m_tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &ProjectPanel::onActivated, this);
    m_tree->Bind(wxEVT_TREE_SEL_CHANGED, &ProjectPanel::onSelChanged, this);
    m_save->Bind(wxEVT_BUTTON, &ProjectPanel::onSave, this);
    Bind(wxEVT_FSWATCHER, &ProjectPanel::onFileSystem, this);
    Bind(wxEVT_TIMER, &ProjectPanel::onRefreshTimer, this, m_refreshTimer.GetId());

    updateHeader();
}

ProjectPanel::~ProjectPanel() = default;

void ProjectPanel::setProject(const fs::path& root, bool saved)
{
    m_root = root;
    m_saved = saved;
    m_selected.clear();
    m_refreshTimer.Stop();
    // wxFileSystemWatcher attaches to the event loop running when it is made.
    if (!m_watcher && !m_root.empty()) {
        m_watcher = std::make_unique<wxFileSystemWatcher>();
        m_watcher->SetOwner(this);
    }
    updateHeader();
    refresh();
}

void ProjectPanel::setSaved(bool saved)
{
    m_saved = saved;
    updateHeader();
}

void ProjectPanel::selectFile(const std::string& rel)
{
    m_selected = rel;
    const wxTreeItemId row = rel.empty() ? wxTreeItemId() : findRow(m_tree->GetRootItem(), rel);
    if (row.IsOk())
        m_tree->SelectItem(row);
    else
        m_tree->UnselectAll();
}

void ProjectPanel::updateHeader()
{
    const bool open = !m_root.empty();
    m_title->SetLabel(open ? toWx(m_root.filename()) : wxString("No project"));
    m_title->SetToolTip(open ? toWx(m_root) : wxString("Open a folder with File > Open Project"));
    m_unsaved->Show(open && !m_saved);
    m_save->Show(open && !m_saved);
    Layout();
}

void ProjectPanel::refresh()
{
    m_tree->Freeze();
    m_tree->DeleteAllItems();
    const wxTreeItemId root = m_tree->AddRoot(wxString());
    if (!m_root.empty())
        addRows(root, buildProjectTree(m_root));
    if (!m_selected.empty()) {
        const wxTreeItemId row = findRow(root, m_selected);
        if (row.IsOk())
            m_tree->SelectItem(row);
    }
    m_tree->Thaw();
    watchFolders();
}

void ProjectPanel::watchFolders()
{
    if (!m_watcher)
        return;
    // A folder that vanished between the scan and Add() is not an error worth
    // a message box. The next refresh catches up.
    wxLogNull quiet;
    m_watcher->RemoveAll();
    for (const fs::path& folder : projectFolders(m_root)) {
        m_watcher->Add(wxFileName::DirName(toWx(folder)),
                       wxFSW_EVENT_CREATE | wxFSW_EVENT_DELETE | wxFSW_EVENT_RENAME
                           | wxFSW_EVENT_WARNING | wxFSW_EVENT_ERROR);
    }
}

void ProjectPanel::addRows(const wxTreeItemId& parent, const std::vector<ProjectTreeNode>& nodes)
{
    for (const ProjectTreeNode& node : nodes) {
        const wxTreeItemId row =
            m_tree->AppendItem(parent, wxString::FromUTF8(node.name),
                               node.folder ? kFolderImage : kPdfImage, -1,
                               new RowData(node.rel, node.folder));
        if (node.folder) {
            addRows(row, node.children);
            m_tree->Expand(row);
        }
    }
}

wxTreeItemId ProjectPanel::findRow(const wxTreeItemId& parent, const std::string& rel) const
{
    if (!parent.IsOk())
        return {};
    wxTreeItemIdValue cookie;
    for (wxTreeItemId row = m_tree->GetFirstChild(parent, cookie); row.IsOk();
         row = m_tree->GetNextChild(parent, cookie)) {
        const auto* data = static_cast<const RowData*>(m_tree->GetItemData(row));
        if (data->rel == rel)
            return row;
        if (data->folder && rel.starts_with(data->rel + "/"))
            return findRow(row, rel);
    }
    return {};
}

void ProjectPanel::onActivated(wxTreeEvent& event)
{
    const auto* data = static_cast<const RowData*>(m_tree->GetItemData(event.GetItem()));
    if (!data || data->folder) {
        // Folders keep the tree's own expand and collapse.
        event.Skip();
        return;
    }
    // Queued, so a password prompt or error box does not run inside the
    // tree's own event handler.
    auto* opened = new wxCommandEvent(EVT_PROJECT_FILE_ACTIVATED, GetId());
    opened->SetEventObject(this);
    opened->SetString(toWx(m_root / toPath(wxString::FromUTF8(data->rel))));
    GetEventHandler()->QueueEvent(opened);
}

void ProjectPanel::onSelChanged(wxTreeEvent& event)
{
    const wxTreeItemId row = event.GetItem();
    if (!row.IsOk())
        return;
    const auto* data = static_cast<const RowData*>(m_tree->GetItemData(row));
    if (data && !data->folder)
        m_selected = data->rel;
}

void ProjectPanel::onSave(wxCommandEvent& /*event*/)
{
    auto* save = new wxCommandEvent(EVT_PROJECT_SAVE_REQUESTED, GetId());
    save->SetEventObject(this);
    GetEventHandler()->QueueEvent(save);
}

void ProjectPanel::onFileSystem(wxFileSystemWatcherEvent& /*event*/)
{
    if (!m_root.empty())
        m_refreshTimer.StartOnce(kRefreshDelayMs);
}

void ProjectPanel::onRefreshTimer(wxTimerEvent& /*event*/)
{
    refresh();
}
