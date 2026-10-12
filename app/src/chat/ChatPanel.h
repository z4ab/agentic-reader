#pragma once

#include "agent/Protocol.h"
#include "project/Project.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ChatList;
class PermissionDialog;
class SetupBanner;
class SidecarClient;
class wxButton;
class wxChoice;
class wxCommandEvent;
class wxHtmlLinkEvent;
class wxHtmlWindow;
class wxKeyEvent;
class wxSizer;
class wxTextCtrl;
class wxTimerEvent;

// The chat pane: the chat list (a dropdown of the project's chats, newest
// first, with New chat, Rename and Delete), the message view, input, Send and
// Stop. A SetupBanner takes the place of the input on first run and after the
// API key was rejected. A rate limit adds a Retry button, a chat that cannot
// resume a "Continue in new chat" button.
//
// Chats and their transcripts are kept by a ChatList: in the project's
// .reader/ for a saved project, in memory for an unsaved one. Selecting a
// chat shows its transcript and opens it in the sidecar (resume_chat, or
// new_chat under its own id when it has no session yet), once per sidecar
// process.
//
// The owner forwards sidecar events to onSidecarMessage() and
// onSidecarDown(). Everything runs on the UI thread.
class ChatPanel : public wxPanel
{
public:
    // `sidecar` must outlive the panel.
    ChatPanel(wxWindow* parent, SidecarClient& sidecar);
    ~ChatPanel() override;

    // Sends `open_project` for the absolute folder `path` and shows the chats
    // of `project` (null for an unsaved project, whose chats stay in memory):
    // the most recently updated one, or a new chat when there is none.
    // `project` must stay valid until closeProject() or the next
    // openProject().
    void openProject(const std::string& path, Project* project);

    // The open project is about to be closed. Ends a running reply and drops
    // the chat list.
    void closeProject();

    // The open unsaved project was saved as `project`. Its chats so far are
    // written there, and every later change too.
    void attachProject(Project* project);

    // No sidecar can run. Shows `reason` and disables Send.
    void setUnavailable(const wxString& reason);
    bool available() const { return m_available; }

    // Settings were loaded or saved. `setupNeeded` (needsSetup()) shows the
    // first-run banner in place of the input. Clears the "API key rejected"
    // banner, since the user has been to Settings.
    void setSetupNeeded(bool setupNeeded);

    void onSidecarMessage(const protocol::Outbound& msg);

    // The sidecar exited. Ends a running reply with a notice and reopens the
    // selected chat in the restarted sidecar.
    void onSidecarDown(const wxString& reason);

    // The owner is about to restart the sidecar on purpose. Interrupts a
    // running reply and ends it with "Stopped.".
    void interruptReply();

    // The owner restarted the sidecar. Shows `notice`, makes the pane
    // available again and reopens the selected chat in the new sidecar.
    void onSidecarRestarted(const wxString& notice);

private:
    bool canSend() const;
    void send();
    // Shows `text` as the user's message and sends it. By value: Retry
    // passes m_retryText, which this clears.
    void startReply(std::string text);
    void retry();
    void stop();

    // Shows chat `id` with its transcript and opens it in the sidecar unless
    // this sidecar has it open already.
    void selectChat(const std::string& id);
    // Creates a chat and selects it.
    void newChat();
    // Sends new_chat or resume_chat for `id`, unless the sidecar has it open
    // or it cannot resume.
    void openChat(const std::string& id);
    // The sidecar has no chat open any more (it restarted).
    void forgetOpenChats();
    // Rebuilds the dropdown when its entries changed and selects m_chatId.
    void refreshChatList();
    // Shows a failed write of the chat list once per project.
    void noteSaveError(const std::string& error);
    std::string chatLabel(const ChatInfo& chat) const;
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
    // An `error` for a chat opened with resume_chat, before its first
    // `result`: the session cannot be resumed.
    void onResumeFailed(const std::string& chatId, const std::string& message);
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
    void onChatChosen(wxCommandEvent& event);
    void onNewChat(wxCommandEvent& event);
    void onRename(wxCommandEvent& event);
    void onDelete(wxCommandEvent& event);
    void onContinue(wxCommandEvent& event);
    void onLink(wxHtmlLinkEvent& event);
    void onRenderTimer(wxTimerEvent& event);

    SidecarClient& m_sidecar;

    wxChoice* m_chatChoice = nullptr;
    wxButton* m_newChat = nullptr;
    wxButton* m_rename = nullptr;
    wxButton* m_delete = nullptr;
    wxButton* m_continue = nullptr;
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
    bool m_saveErrorShown = false;

    std::unique_ptr<ChatList> m_chats;
    // The selected chat.
    std::string m_chatId;
    // The chat id of each dropdown entry, in order.
    std::vector<std::string> m_choiceIds;
    // Chats the running sidecar has open: new_chat or resume_chat was sent.
    std::set<std::string> m_openChats;
    // Chats opened with resume_chat that have had no `result` since.
    std::set<std::string> m_resuming;
    // Chats whose session cannot be resumed, with the sidecar's message.
    std::map<std::string, std::string> m_cannotResume;

    // A chat made by "Continue in new chat": its prompts carry the old
    // chat's messages until its first `result`.
    struct Continuation
    {
        std::string from; // the old chat's label
        std::vector<ChatMessage> messages;
    };
    std::map<std::string, Continuation> m_continuations;

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
    // The whole text of the running reply, saved at its `result`.
    std::string m_replyText;

    // Permission requests waiting for the dialog, and the one it shows.
    std::deque<protocol::PermissionRequest> m_permissions;
    PermissionDialog* m_permissionDialog = nullptr;
    std::string m_permissionChatId;
};
