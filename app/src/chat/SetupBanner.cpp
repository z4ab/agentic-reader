#include "chat/SetupBanner.h"

#include "settings/Settings.h"

#include <wx/button.h>
#include <wx/hyperlink.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace
{
constexpr int kGap = 5;
} // namespace

ErrorAction errorAction(std::string_view kind)
{
    if (kind == "busy")
        return ErrorAction::Ignore;
    if (kind == "interrupted")
        return ErrorAction::Stopped;
    if (kind == "auth")
        return ErrorAction::AuthBanner;
    if (kind == "rate_limit")
        return ErrorAction::RetryNotice;
    if (kind == "cli_missing")
        return ErrorAction::Reinstall;
    if (kind == "process")
        return ErrorAction::Restarting;
    return ErrorAction::Inline;
}

bool errorClosesChat(std::string_view kind)
{
    return kind == "unknown_chat" || kind == "cli_missing" || kind == "process";
}

bool needsSetup(const Settings& settings)
{
    // The local login never needs the key store, so it is checked first.
    return !settings.useLocalLogin() && settings.apiKey().empty();
}

SetupBanner::SetupBanner(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME | wxTAB_TRAVERSAL)
{
    m_text = new wxStaticText(this, wxID_ANY, wxString());
    m_text->SetFont(m_text->GetFont().Bold());
    auto* hint = new wxStaticText(this, wxID_ANY, "Get a key from the Anthropic Console:");
    m_link = new wxHyperlinkCtrl(this, wxID_ANY, kKeysUrl, kKeysUrl);
    m_openSettings = new wxButton(this, wxID_ANY, "Open Settings");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_text, 0, wxLEFT | wxRIGHT | wxTOP, kGap * 2);
    sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, kGap * 2);
    sizer->Add(m_link, 0, wxLEFT | wxRIGHT, kGap * 2);
    sizer->Add(m_openSettings, 0, wxALIGN_RIGHT | wxALL, kGap);
    SetSizer(sizer);

    m_openSettings->Bind(wxEVT_BUTTON, &SetupBanner::onOpenSettings, this);
    setReason(Reason::FirstRun);
}

void SetupBanner::setReason(Reason reason)
{
    m_text->SetLabel(reason == Reason::FirstRun ? "Add your Anthropic API key to start"
                                                : "API key rejected");
    Layout();
}

void SetupBanner::onOpenSettings(wxCommandEvent& /*event*/)
{
    // Queued, so the dialog's modal loop does not run inside this handler.
    if (wxWindow* top = wxGetTopLevelParent(this))
        wxQueueEvent(top, new wxCommandEvent(wxEVT_MENU, wxID_PREFERENCES));
}
