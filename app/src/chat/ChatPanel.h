#pragma once

#include "agent/Protocol.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <string>

class SidecarClient;
class wxButton;
class wxCommandEvent;
class wxHtmlLinkEvent;
class wxHtmlWindow;
class wxKeyEvent;
class wxTextCtrl;
class wxTimerEvent;

// The chat pane: message view, input, Send and Stop. One chat per project.
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

    void onSidecarMessage(const protocol::Outbound& msg);

    // The sidecar exited. Ends a running reply with a notice and reopens the
    // chat in the restarted sidecar.
    void onSidecarDown(const wxString& reason);

private:
    void send();
    void stop();
    void startChat();
    void endReply(const std::string& trailerHtml);
    void updateButtons();
    void scheduleRender();
    // Rebuilds the view. Keeps the scroll position unless the view was at
    // the bottom or `toBottom` is set.
    void render(bool toBottom = false);
    bool atBottom() const;

    void onResult(const protocol::Result& result);
    void onError(const protocol::Error& error);

    void onInputKey(wxKeyEvent& event);
    void onSend(wxCommandEvent& event);
    void onStop(wxCommandEvent& event);
    void onLink(wxHtmlLinkEvent& event);
    void onRenderTimer(wxTimerEvent& event);

    SidecarClient& m_sidecar;

    wxHtmlWindow* m_view = nullptr;
    wxTextCtrl* m_input = nullptr;
    wxButton* m_send = nullptr;
    wxButton* m_stop = nullptr;
    wxTimer m_renderTimer;

    bool m_available = true;
    bool m_hasProject = false;
    std::string m_chatId;
    // From the last `result`. A reopened chat resumes this session.
    std::string m_sessionId;

    bool m_running = false;
    bool m_stopRequested = false;
    // Finished messages as HTML, and the running reply as Markdown.
    std::string m_historyHtml;
    std::string m_reply;
};
