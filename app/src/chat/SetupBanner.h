#pragma once

#include <wx/panel.h>

#include <string_view>

class Settings;
class wxButton;
class wxCommandEvent;
class wxHyperlinkCtrl;
class wxStaticText;

// What the chat pane does with a sidecar `error`, by its kind
// (sidecar/PROTOCOL.md).
enum class ErrorAction
{
    Ignore,      // busy: no notice, a running reply keeps going
    Stopped,     // interrupted: the grey "Stopped." line
    AuthBanner,  // auth: SetupBanner "API key rejected" + Open Settings
    RetryNotice, // rate_limit: inline notice with a Retry button
    Reinstall,   // cli_missing: "Installation is incomplete, reinstall the app"
    Restarting,  // process: "Agent stopped, restarting…"
    Inline,      // every other kind, unknown kinds included
};

// Kinds match exactly, as PROTOCOL.md spells them.
ErrorAction errorAction(std::string_view kind);

// True when the sidecar closed the chat, so the pane reopens it:
// unknown_chat, cli_missing and process.
bool errorClosesChat(std::string_view kind);

// First run: no API key is stored (or the key store is unavailable) and the
// developer login is off.
bool needsSetup(const Settings& settings);

// Shown in the chat pane in place of the input until the assistant can be
// used: a line of text, the console link for API keys, and "Open Settings",
// which opens Edit > Preferences (it queues wxID_PREFERENCES to the
// top-level window).
class SetupBanner : public wxPanel
{
public:
    // Target of the console link.
    static constexpr const char* kKeysUrl = "https://console.anthropic.com/settings/keys";

    enum class Reason
    {
        FirstRun,    // "Add your Anthropic API key to start"
        KeyRejected, // "API key rejected"
    };

    explicit SetupBanner(wxWindow* parent);

    void setReason(Reason reason);

private:
    void onOpenSettings(wxCommandEvent& event);

    wxStaticText* m_text = nullptr;
    wxHyperlinkCtrl* m_link = nullptr;
    wxButton* m_openSettings = nullptr;
};
