#include "project/InstructionsDialog.h"

#include "project/Instructions.h"

#include <wx/accel.h>
#include <wx/button.h>
#include <wx/font.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <string>

namespace
{
constexpr int kBorder = 10;
constexpr int kNoteWrapWidth = 640;
const wxSize kTextSize(680, 420);

wxString folderName(const std::filesystem::path& root)
{
#ifdef _WIN32
    return wxString(root.filename().wstring());
#else
    return wxString::FromUTF8(root.filename().string());
#endif
}
} // namespace

InstructionsDialog::InstructionsDialog(wxWindow* parent, const std::filesystem::path& root,
                                       const wxString& text)
    : wxDialog(parent, wxID_ANY, wxString::Format("Instructions for \"%s\"", folderName(root)),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_root(root)
{
    auto* note = new wxStaticText(
        this, wxID_ANY,
        "Instructions for the assistant in this project, kept in CLAUDE.md in the project "
        "folder. New chats read them when they start.");
    note->Wrap(FromDIP(kNoteWrapWidth));

    m_text = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, FromDIP(kTextSize),
                            wxTE_MULTILINE | wxTE_RICH2);
    const int pointSize = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetPointSize();
    m_text->SetFont(wxFont(wxFontInfo(pointSize).Family(wxFONTFAMILY_TELETYPE)));

    auto* buttons = new wxStdDialogButtonSizer();
    buttons->AddButton(new wxButton(this, wxID_SAVE, "&Save"));
    buttons->AddButton(new wxButton(this, wxID_CANCEL));
    buttons->Realize();

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(note, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP, FromDIP(kBorder)));
    sizer->Add(m_text, wxSizerFlags(1).Expand().Border(wxALL, FromDIP(kBorder)));
    sizer->Add(buttons, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(kBorder)));
    SetSizerAndFit(sizer);
    CentreOnParent();

    // Ctrl+S saves too. Return stays a newline in the text area.
    wxAcceleratorEntry save(wxACCEL_CTRL, 'S', wxID_SAVE);
    SetAcceleratorTable(wxAcceleratorTable(1, &save));
    SetEscapeId(wxID_CANCEL);
    Bind(wxEVT_BUTTON, &InstructionsDialog::onSave, this, wxID_SAVE);
    Bind(wxEVT_MENU, &InstructionsDialog::onSave, this, wxID_SAVE);

    m_text->SetInsertionPoint(0);
    m_text->SetFocus();
}

void InstructionsDialog::onSave(wxCommandEvent& /*event*/)
{
    std::string error;
    if (!saveInstructions(m_root, m_text->GetValue().utf8_string(), error)) {
        wxMessageBox(wxString::Format("Could not save the instructions.\n\n%s",
                                      wxString::FromUTF8(error)),
                     "Cannot Save Instructions", wxOK | wxICON_ERROR, this);
        return;
    }
    EndModal(wxID_SAVE);
}
