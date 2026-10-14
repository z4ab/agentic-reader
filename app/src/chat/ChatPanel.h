#pragma once

#include "agent/Protocol.h"
#include "chat/ReaderContext.h"
#include "project/Project.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class ChatList;
class ContextChip;
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
// Stop, with the context chip above the input. A SetupBanner takes the place
// of the chip and the input on first run and after the API key was rejected.
// A rate limit adds a Retry button, a chat that cannot resume a "Continue in
// new chat" button.
//
// Chats and their transcripts are kept by a ChatList: in the project's
// .reader/ for a saved project, in memory for an unsaved one. Selecting a
// chat shows its transcript and opens it in the sidecar (resume_chat, or
// new_chat under its own id when it has no session yet), once per sidecar
// process.
//
// Each prompt carries the reader context of the context source: the open
// PDF, its current page and the selection. The chip above the input shows it,
// and its x leaves the selection out of the next prompt.
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

    // Called at every send and chip update for what the reader shows. Without
    // a source prompts carry no context.
    void setContextSource(std::function<readercontext::Snapshot()> source);
    // The reader context changed (file, page or selection). A new selection
    // takes back the chip's x.
    void updateContext(bool selectionChanged = false);
    // "Ask about selection": puts the quoted start of `selection` (UTF-8)
    // before the input's text, the caret after it, and focuses the input.
    // The next prompt carries the selection, even after the chip's x.
    void askAbout(const std::string& selection);

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

    // Creates a chat and selects it. Does nothing while a reply runs.
    void newChat();
    // Selects the chat after the selected one in the dropdown, or before it,
    // wrapping. Does nothing while a reply runs and with no project.
    void nextChat();
    void prevChat();
    // Interrupts the running reply (the Stop button, Esc). Does nothing when
    // no reply runs or a stop was already sent.
    void stop();
    // True while New chat can run (a project is open and no reply runs).
    bool canNewChat() const;
    // True while Stop can run: a reply runs and no stop was sent yet.
    bool canStop() const;
    // Moves the caret into the input. Does nothing while the input is hidden
    // behind the setup or key banner.
    void focusInput();

private:
    bool canSend() const;
    void send();
    // Shows `text` as the user's message and sends it with `context`. By
    // value: Retry passes m_retryText, which this clears.
    void startReply(std::string text, std::optional<protocol::Context> context);
    void retry();

    // Shows chat `id` with its transcript and opens it in the sidecar unless
    // this sidecar has it open already.
    void selectChat(const std::string& id);
    // nextChat() for step 1, prevChat() for -1.
    void stepChat(int step);
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
    // Shows the banner or the chip and the input, and Retry when it applies.
    void updateControls();
    // Shows the chip with the context source's label, or hides it with no PDF.
    void refreshChip();
    readercontext::Snapshot snapshot() const;
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
    void onChipClose(wxCommandEvent& event);
    void onLink(wxHtmlLinkEvent& event);
    void onRenderTimer(wxTimerEvent& event);

    SidecarClient& m_sidecar;

    wxChoice* m_chatChoice = nullptr;
    wxButton* m_newChat = nullptr;
    wxButton* m_rename = nullptr;
    wxButton* m_delete = nullptr;
    wxButton* m_continue = nullptr;
    wxHtmlWindow* m_view = nullptr;
    ContextChip* m_chip = nullptr;
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
    std::function<readercontext::Snapshot()> m_contextSource;
    readercontext::ChipState m_chipState;

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
    // The text and context of the running or last reply's prompt.
    std::string m_lastPrompt;
    std::optional<protocol::Context> m_lastContext;
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
