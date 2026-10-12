#pragma once

#include "agent/Protocol.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <deque>
#include <string>

class PermissionDialog;
class SetupBanner;
class SidecarClient;
class wxButton;
class wxCommandEvent;
class wxHtmlLinkEvent;
class wxHtmlWindow;
class wxKeyEvent;
class wxSizer;
class wxTextCtrl;
class wxTimerEvent;

// The chat pane: message view, input, Send and Stop. One chat per project.
// A SetupBanner takes the place of the input on first run and after the API
// key was rejected. A rate limit adds a Retry button.
//
// The owner forwards sidecar events to onSidecarMessage() and
// onSidecarDown(). Everything runs on the UI thread.
class ChatPanel : public wxPanel
{
public:
    // `sidecar` must outlive the panel.
    ChatPanel(wxWindow* parent, SidecarClient& sidecar);

    // Sends `open_project` for the absolute folder `path` and starts a chat.
    void openProject(const std::string& path);

    // No sidecar can run. Shows `reason` and disables Send.
    void setUnavailable(const wxString& reason);
    bool available() const { return m_available; }

    // Settings were loaded or saved. `setupNeeded` (needsSetup()) shows the
    // first-run banner in place of the input. Clears the "API key rejected"
    // banner, since the user has been to Settings.
    void setSetupNeeded(bool setupNeeded);

    void onSidecarMessage(const protocol::Outbound& msg);

    // The sidecar exited. Ends a running reply with a notice and reopens the
    // chat in the restarted sidecar.
    void onSidecarDown(const wxString& reason);

    // The owner is about to restart the sidecar on purpose. Interrupts a
    // running reply and ends it with "Stopped.".
    void interruptReply();

    // The owner restarted the sidecar. Shows `notice`, makes the pane
    // available again and reopens the chat in the new sidecar.
    void onSidecarRestarted(const wxString& notice);

private:
    bool canSend() const;
    void send();
    // Shows `text` as the user's message and sends it. By value: Retry
    // passes m_retryText, which this clears.
    void startReply(std::string text);
    void retry();
    void stop();
    void startChat();
    void endReply(const std::string& trailerHtml);
    void updateButtons();
    // Shows the banner or the input, and Retry when it applies.
    void updateControls();
    void scheduleRender();
    // Rebuilds the view. Keeps the scroll position unless the view was at
    // the bottom or `toBottom` is set.
    void render(bool toBottom = false);
    bool atBottom() const;

    void onResult(const protocol::Result& result);
    void onError(const protocol::Error& error);
    void onToolUse(const protocol::ToolUse& tool);
    void onPermissionRequest(const protocol::PermissionRequest& request);

    // Shows the next queued permission request, modal, and replies with the
    // user's answer unless the request ended first.
    void showNextPermission();
    // The sidecar sends nothing when a request ends without an answer. Drops
    // the requests of `chatId` (all when empty) and closes their dialog
    // without replying.
    void dropPermissions(const std::string& chatId);

    void onInputKey(wxKeyEvent& event);
    void onSend(wxCommandEvent& event);
    void onStop(wxCommandEvent& event);
    void onRetry(wxCommandEvent& event);
    void onLink(wxHtmlLinkEvent& event);
    void onRenderTimer(wxTimerEvent& event);

    SidecarClient& m_sidecar;

    wxHtmlWindow* m_view = nullptr;
    wxTextCtrl* m_input = nullptr;
    wxButton* m_send = nullptr;
    wxButton* m_stop = nullptr;
    wxButton* m_retry = nullptr;
    wxSizer* m_buttons = nullptr;
    SetupBanner* m_banner = nullptr;
    wxTimer m_renderTimer;

    bool m_available = true;
    bool m_setupNeeded = false;
    bool m_keyRejected = false;
    bool m_hasProject = false;
    std::string m_chatId;
    // From the last `result`. A reopened chat resumes this session.
    std::string m_sessionId;

    bool m_running = false;
    bool m_stopRequested = false;
    // The text of the running or last reply's prompt.
    std::string m_lastPrompt;
    // The prompt Retry sends again. Set while the last reply ended with
    // rate_limit.
    std::string m_retryText;
    // Finished messages as HTML. The running reply is HTML up to its last
    // tool line, then Markdown.
    std::string m_historyHtml;
    std::string m_replyHtml;
    std::string m_reply;

    // Permission requests waiting for the dialog, and the one it shows.
    std::deque<protocol::PermissionRequest> m_permissions;
    PermissionDialog* m_permissionDialog = nullptr;
    std::string m_permissionChatId;
};
