#pragma once

#include <nlohmann/json.hpp>

#include <wx/dialog.h>

#include <string>

// Asks the user to allow or deny a tool Claude wants to run. Shows the tool
// name and its formatted input (toolformat::details) with Allow and Deny
// buttons. Deny is the default button and has the focus, so Return denies;
// Esc and the close box deny too.
//
// ShowModal() returns wxID_OK for Allow, wxID_CANCEL for Deny, or
// kDismissed when dismiss() closed the dialog without an answer.
class PermissionDialog : public wxDialog
{
public:
    static constexpr int kDismissed = wxID_ABORT;

    PermissionDialog(wxWindow* parent, const std::string& tool, const nlohmann::json& input);

    // The request is over (its reply ended or the sidecar restarted). Ends a
    // running ShowModal() with kDismissed. Does nothing otherwise.
    void dismiss();

private:
    void onInit(wxInitDialogEvent& event);

    wxWindow* m_deny = nullptr;
};
