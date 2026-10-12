#include "chat/PermissionDialog.h"

#include "chat/ToolFormat.h"

#include <wx/button.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace
{
constexpr int kGap = 10;
constexpr int kDetailsWidth = 560;
constexpr int kDetailsHeight = 240;
} // namespace

PermissionDialog::PermissionDialog(wxWindow* parent, const std::string& tool,
                                   const nlohmann::json& input)
    : wxDialog(parent, wxID_ANY, "Allow this action?", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* intro = new wxStaticText(this, wxID_ANY, "The assistant wants to use this tool:");
    auto* name = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(tool));
    name->SetFont(name->GetFont().Bold().Larger());

    auto* details = new wxTextCtrl(this, wxID_ANY,
                                   wxString::FromUTF8(toolformat::details(tool, input)),
                                   wxDefaultPosition, wxSize(kDetailsWidth, kDetailsHeight),
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP);
    details->SetFont(wxFont(wxFontInfo(details->GetFont().GetPointSize()).Family(
        wxFONTFAMILY_TELETYPE)));

    // Esc and the close box press the wxID_CANCEL button.
    auto* allow = new wxButton(this, wxID_OK, "&Allow");
    auto* deny = new wxButton(this, wxID_CANCEL, "&Deny");
    deny->SetDefault();
    m_deny = deny;

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(allow, 0, wxRIGHT, kGap);
    buttons->Add(deny);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(name, 0, wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(details, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxALL, kGap);
    SetSizerAndFit(sizer);
    CentreOnParent();

    Bind(wxEVT_INIT_DIALOG, &PermissionDialog::onInit, this);
    deny->SetFocus();
}

void PermissionDialog::dismiss()
{
    if (IsModal())
        EndModal(kDismissed);
}

void PermissionDialog::onInit(wxInitDialogEvent& event)
{
    event.Skip();
    m_deny->SetFocus();
}
