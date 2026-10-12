#pragma once

#include <wx/dialog.h>

#include <filesystem>

class wxCommandEvent;
class wxTextCtrl;

// Project > Instructions: edits the project's CLAUDE.md (Instructions.h) in a
// monospace text area. Save writes the file and ends the dialog with
// wxID_SAVE; a failed save shows an error and keeps the dialog open. Cancel
// writes nothing.
class InstructionsDialog : public wxDialog
{
public:
    // `text` is the current content of <root>/CLAUDE.md, as UTF-8.
    InstructionsDialog(wxWindow* parent, const std::filesystem::path& root, const wxString& text);

private:
    void onSave(wxCommandEvent& event);

    std::filesystem::path m_root;
    wxTextCtrl* m_text = nullptr;
};
