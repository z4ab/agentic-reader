#include "chat/ChatPanel.h"

#include "agent/SidecarClient.h"
#include "chat/ChatList.h"
#include "chat/ContextChip.h"
#include "chat/Markdown.h"
#include "chat/PermissionDialog.h"
#include "chat/SetupBanner.h"
#include "chat/ToolFormat.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/html/htmlwin.h>
#include <wx/html/m_templ.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <variant>

namespace
{

constexpr int kGap = 5;
constexpr int kInputHeight = 60;
constexpr int kRenderDelayMs = 50;
constexpr std::size_t kMaxToolLine = 300; // bytes shown of a tool line
constexpr int kChatChoiceMinWidth = 150;
const char* const kUntitled = "New chat";

// Colours that read on the system theme, light or dark.
struct Palette
{
    std::string userBackground;
    std::string error;
};

const Palette& palette()
{
    static const Palette colours = [] {
        const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        const wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        auto mix = [](unsigned char a, unsigned char b) {
            return static_cast<unsigned char>((a * 88 + b * 12) / 100);
        };
        const wxColour user(mix(bg.Red(), fg.Red()), mix(bg.Green(), fg.Green()),
                            mix(bg.Blue(), fg.Blue()));
        const bool dark = wxSystemSettings::GetAppearance().IsDark();
        return Palette{user.GetAsString(wxC2S_HTML_SYNTAX).utf8_string(),
                       dark ? "#ff7b7b" : "#b00020"};
    }();
    return colours;
}

std::string userHtml(const std::string& text)
{
    // The <p> keeps the box clear of the line above it.
    return "<p><table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\" bgcolor=\""
           + palette().userBackground + "\"><tr><td>" + markdown::escapeText(text)
           + "</td></tr></table>";
}

std::string greyHtml(const std::string& escaped)
{
    return "<p><font size=\"-1\" color=\"#808080\">" + escaped + "</font></p>";
}

std::string redHtml(const std::string& escaped)
{
    return "<p><font color=\"" + palette().error + "\">" + escaped + "</font></p>";
}

// A tool call as one grey line, cut short when very long.
std::string toolHtml(const protocol::ToolUse& tool)
{
    std::string text = toolformat::line(tool.name, tool.input);
    if (text.size() > kMaxToolLine) {
        std::size_t cut = kMaxToolLine;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut; // not inside a UTF-8 sequence
        text.resize(cut);
        text += "\xe2\x80\xa6"; // "…"
    }
    return "<div><font size=\"-1\" color=\"#808080\">" + markdown::escapeText(text)
           + "</font></div>";
}

// Adds <wbr> break points inside long words, such as paths and URLs, so they
// wrap instead of running past the view. Skips tags, entities and <pre>.
std::string addBreakPoints(std::string_view html)
{
    constexpr int kMinWord = 16; // break after punctuation once a word is this long
    constexpr int kMaxRun = 32;  // force a break after this many characters
    auto isSpace = [](char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; };
    auto startsWith = [&](std::size_t i, std::string_view tag) {
        if (html.compare(i, tag.size(), tag) != 0)
            return false;
        const std::size_t end = i + tag.size();
        return end < html.size() && (html[end] == '>' || isSpace(html[end]));
    };

    std::string out;
    out.reserve(html.size() + html.size() / 16);
    bool inPre = false;
    int word = 0; // characters since the last space
    int run = 0;  // characters since the last break point
    for (std::size_t i = 0; i < html.size();) {
        const char c = html[i];
        if (c == '<') {
            if (startsWith(i, "<pre"))
                inPre = true;
            else if (startsWith(i, "</pre"))
                inPre = false;
            const std::size_t end = html.find('>', i);
            const std::size_t stop = end == std::string_view::npos ? html.size() : end + 1;
            out.append(html.substr(i, stop - i));
            i = stop;
            continue;
        }
        std::size_t len = 1;
        if (c == '&') {
            const std::size_t semi = html.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10)
                len = semi - i + 1;
        } else {
            // A whole UTF-8 sequence.
            while (i + len < html.size()
                   && (static_cast<unsigned char>(html[i + len]) & 0xC0) == 0x80)
                ++len;
        }
        out.append(html.substr(i, len));
        i += len;
        if (inPre || isSpace(c)) {
            word = run = 0;
            continue;
        }
        ++word;
        ++run;
        const bool more = i < html.size() && html[i] != '<' && !isSpace(html[i]);
        // Not after the dot in a number or version, such as "m5.2".
        const bool decimal = c == '.' && i < html.size() && html[i] >= '0' && html[i] <= '9';
        const bool punct = len == 1 && !decimal
                           && std::string_view("/\\-_.?=").find(c) != std::string_view::npos;
        if (more && ((punct && word >= kMinWord) || run >= kMaxRun)) {
            out += "<wbr>";
            run = 0;
        }
    }
    return out;
}

std::int64_t tokenCount(const nlohmann::json& usage, const char* key)
{
    const auto it = usage.find(key);
    return (it != usage.end() && it->is_number_integer()) ? it->get<std::int64_t>() : 0;
}

// "$0.0123 · 1234 in · 56 out tokens". Input counts cached tokens too.
std::string resultFooter(const protocol::Result& result)
{
    const auto& usage = result.usage;
    const std::int64_t in = tokenCount(usage, "input_tokens")
                            + tokenCount(usage, "cache_creation_input_tokens")
                            + tokenCount(usage, "cache_read_input_tokens");
    const std::int64_t out = tokenCount(usage, "output_tokens");
    char buf[128];
    std::snprintf(buf, sizeof buf,
                  "$%.4f \xc2\xb7 %" PRId64 " in \xc2\xb7 %" PRId64 " out tokens",
                  result.costUsd, in, out);
    return greyHtml(buf);
}

// "$0.0123", the cost line of a reply loaded from a transcript.
std::string costHtml(double costUsd)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "$%.4f", costUsd);
    return greyHtml(buf);
}

// A saved transcript as the view shows it.
std::string transcriptHtml(const std::vector<ChatMessage>& messages)
{
    std::string html;
    for (const ChatMessage& message : messages) {
        if (message.role == "user") {
            html += userHtml(message.text);
        } else if (message.role == "assistant") {
            html += markdown::toHtml(message.text);
            if (message.costUsd)
                html += costHtml(*message.costUsd);
        }
    }
    return html;
}

std::string cannotResumeHtml(const std::string& message)
{
    return redHtml(markdown::escapeText("This chat cannot resume: " + message))
           + greyHtml("Its messages stay here. Press Continue in new chat to go on in a "
                      "new chat that gets the last messages as context.");
}

// Error kinds that say a resumed session failed when they come before the
// chat's first `result`. The others are about the request (busy,
// interrupted, auth, rate_limit), the app (protocol, no_project,
// chat_exists, unknown_chat) or the installation (cli_missing).
bool failsResume(const std::string& kind)
{
    return kind != "busy" && kind != "interrupted" && kind != "auth" && kind != "rate_limit"
           && kind != "protocol" && kind != "no_project" && kind != "chat_exists"
           && kind != "unknown_chat" && kind != "cli_missing";
}

bool isWebLink(const wxString& href)
{
    const wxString lower = href.Lower();
    return lower.StartsWith("http://") || lower.StartsWith("https://")
           || lower.StartsWith("mailto:");
}

// <wbr>: a zero-size cell before which wxHtml may break a line. wxHtml breaks
// only at spaces otherwise.
class ChatBreakCell : public wxHtmlCell
{
public:
    ChatBreakCell()
    {
        m_Width = 0;
        m_Height = 0;
        m_Descent = 0;
    }

    // wxHtml treats zero-size cells as formatting cells, which forbid a break.
    bool IsLinebreakAllowed() const override { return true; }
};

TAG_HANDLER_BEGIN(CHAT_WBR, "WBR")
    TAG_HANDLER_CONSTR(CHAT_WBR) {}

    TAG_HANDLER_PROC(tag)
    {
        wxUnusedVar(tag);
        m_WParser->GetContainer()->InsertCell(new ChatBreakCell);
        return false;
    }
TAG_HANDLER_END(CHAT_WBR)

TAGS_MODULE_BEGIN(ChatWbr)
    TAGS_MODULE_ADD(CHAT_WBR)
TAGS_MODULE_END(ChatWbr)

} // namespace

ChatPanel::ChatPanel(wxWindow* parent, SidecarClient& sidecar)
    : wxPanel(parent), m_sidecar(sidecar), m_renderTimer(this)
{
    // The dropdown's width follows the pane, not its longest title.
    m_chatChoice = new wxChoice(this, wxID_ANY);
    m_chatChoice->SetMinSize(wxSize(FromDIP(kChatChoiceMinWidth), -1));
    m_newChat = new wxButton(this, wxID_ANY, "&New chat", wxDefaultPosition, wxDefaultSize,
                             wxBU_EXACTFIT);
    m_rename = new wxButton(this, wxID_ANY, "&Rename", wxDefaultPosition, wxDefaultSize,
                            wxBU_EXACTFIT);
    m_delete = new wxButton(this, wxID_ANY, "&Delete", wxDefaultPosition, wxDefaultSize,
                            wxBU_EXACTFIT);
    auto* chatRow = new wxBoxSizer(wxHORIZONTAL);
    chatRow->Add(m_chatChoice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, kGap);
    chatRow->Add(m_newChat, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kGap);
    chatRow->Add(m_rename, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kGap);
    chatRow->Add(m_delete, 0, wxALIGN_CENTER_VERTICAL);

    m_view = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxHW_SCROLLBAR_AUTO | wxBORDER_THEME);
    m_chip = new ContextChip(this);
    m_input = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition,
                             wxSize(-1, kInputHeight), wxTE_MULTILINE);
    m_retry = new wxButton(this, wxID_ANY, "Retry");
    m_continue = new wxButton(this, wxID_ANY, "&Continue in new chat");
    m_send = new wxButton(this, wxID_ANY, "Send");
    m_stop = new wxButton(this, wxID_ANY, "Stop");
    m_banner = new SetupBanner(this);

    // Retry or Continue in new chat at the left end, Send and Stop at the
    // right.
    m_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_buttons->Add(m_retry);
    m_buttons->Add(m_continue);
    m_buttons->AddStretchSpacer();
    m_buttons->Add(m_send, 0, wxRIGHT, kGap);
    m_buttons->Add(m_stop);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(chatRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_view, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_chip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_buttons, 0, wxEXPAND | wxALL, kGap);
    // In place of the input and the buttons.
    sizer->Add(m_banner, 0, wxEXPAND | wxALL, kGap);
    SetSizer(sizer);

    m_input->Bind(wxEVT_KEY_DOWN, &ChatPanel::onInputKey, this);
    m_send->Bind(wxEVT_BUTTON, &ChatPanel::onSend, this);
    m_stop->Bind(wxEVT_BUTTON, &ChatPanel::onStop, this);
    m_retry->Bind(wxEVT_BUTTON, &ChatPanel::onRetry, this);
    m_continue->Bind(wxEVT_BUTTON, &ChatPanel::onContinue, this);
    m_chatChoice->Bind(wxEVT_CHOICE, &ChatPanel::onChatChosen, this);
    m_newChat->Bind(wxEVT_BUTTON, &ChatPanel::onNewChat, this);
    m_rename->Bind(wxEVT_BUTTON, &ChatPanel::onRename, this);
    m_delete->Bind(wxEVT_BUTTON, &ChatPanel::onDelete, this);
    m_chip->Bind(wxEVT_BUTTON, &ChatPanel::onChipClose, this);
    m_view->Bind(wxEVT_HTML_LINK_CLICKED, &ChatPanel::onLink, this);
    Bind(wxEVT_TIMER, &ChatPanel::onRenderTimer, this);

    m_continue->Hide();
    updateControls();
    render();
}

ChatPanel::~ChatPanel() = default;

void ChatPanel::openProject(const std::string& path, Project* project)
{
    closeProject();
    // The sidecar closes every chat of another project.
    m_sidecar.send(protocol::OpenProject{path, std::nullopt});
    m_hasProject = true;
    m_chats = std::make_unique<ChatList>(project);
    const std::vector<ChatInfo> chats = m_chats->chats();
    if (chats.empty())
        newChat();
    else
        selectChat(chats.front().id);
}

void ChatPanel::closeProject()
{
    interruptReply();
    dropPermissions({});
    m_hasProject = false;
    m_saveErrorShown = false;
    m_chats.reset();
    m_chatId.clear();
    m_openChats.clear();
    m_resuming.clear();
    m_cannotResume.clear();
    m_continuations.clear();
    m_retryText.clear();
    refreshChatList();
    updateControls();
}

void ChatPanel::attachProject(Project* project)
{
    if (!m_chats)
        return;
    std::string error;
    if (!m_chats->attach(project, error))
        noteSaveError(error);
}

void ChatPanel::setUnavailable(const wxString& reason)
{
    m_available = false;
    m_historyHtml += redHtml(markdown::escapeText(reason.utf8_string()));
    updateButtons();
    render(true);
}

void ChatPanel::setSetupNeeded(bool setupNeeded)
{
    m_setupNeeded = setupNeeded;
    m_keyRejected = false;
    updateControls();
}

void ChatPanel::selectChat(const std::string& id)
{
    if (m_running || !m_chats)
        return;
    const ChatInfo* chat = m_chats->find(id);
    if (!chat)
        return;
    m_chatId = id;
    m_retryText.clear();
    m_historyHtml = transcriptHtml(m_chats->transcript(id));
    if (const auto it = m_cannotResume.find(id); it != m_cannotResume.end())
        m_historyHtml += cannotResumeHtml(it->second);
    if (const auto it = m_continuations.find(id); it != m_continuations.end()) {
        m_historyHtml += greyHtml(markdown::escapeText(
            "Continued from \xe2\x80\x9c" + it->second.from + "\xe2\x80\x9d. Your first message here "
            "goes with its last " + std::to_string(std::min(it->second.messages.size(),
                                                            chatlist::kContextMessages))
            + " messages as context."));
    }
    openChat(id);
    refreshChatList();
    updateControls();
    render(true);
}

void ChatPanel::newChat()
{
    if (m_running || !m_chats)
        return;
    std::string error;
    const std::string id = m_chats->create(error);
    selectChat(id);
    // After selectChat(), which replaces the view.
    if (!error.empty())
        noteSaveError(error);
}

void ChatPanel::nextChat()
{
    stepChat(1);
}

void ChatPanel::prevChat()
{
    stepChat(-1);
}

void ChatPanel::stepChat(int step)
{
    if (!canNewChat())
        return;
    const std::string id = chatlist::neighbor(m_choiceIds, m_chatId, step);
    if (id != m_chatId)
        selectChat(id);
}

void ChatPanel::openChat(const std::string& id)
{
    if (!m_hasProject || !m_chats || m_openChats.contains(id) || m_cannotResume.contains(id))
        return;
    const ChatInfo* chat = m_chats->find(id);
    if (!chat)
        return;
    const protocol::Inbound msg = chatlist::openMessage(*chat);
    if (std::holds_alternative<protocol::ResumeChat>(msg))
        m_resuming.insert(id);
    m_openChats.insert(id);
    m_sidecar.send(msg);
}

void ChatPanel::forgetOpenChats()
{
    m_openChats.clear();
    m_resuming.clear();
}

std::string ChatPanel::chatLabel(const ChatInfo& chat) const
{
    std::string label = chat.title.empty() ? kUntitled : chat.title;
    if (m_cannotResume.contains(chat.id))
        label += " (cannot resume)";
    return label;
}

void ChatPanel::refreshChatList()
{
    std::vector<std::string> ids;
    wxArrayString labels;
    if (m_chats) {
        for (const ChatInfo& chat : m_chats->chats()) {
            ids.push_back(chat.id);
            labels.Add(wxString::FromUTF8(chatLabel(chat)));
        }
    }
    // Rebuilding the GTK combo box while it handles a selection is not safe,
    // and a selection changes no entry.
    if (ids != m_choiceIds || labels != m_chatChoice->GetStrings()) {
        m_chatChoice->Set(labels);
        m_choiceIds = std::move(ids);
    }
    const auto it = std::find(m_choiceIds.begin(), m_choiceIds.end(), m_chatId);
    const int index = it == m_choiceIds.end() ? wxNOT_FOUND
                                              : static_cast<int>(it - m_choiceIds.begin());
    if (m_chatChoice->GetSelection() != index)
        m_chatChoice->SetSelection(index);
}

void ChatPanel::noteSaveError(const std::string& error)
{
    if (m_saveErrorShown)
        return;
    m_saveErrorShown = true;
    m_historyHtml += redHtml(markdown::escapeText("This chat is not saved: " + error));
    render(true);
}

void ChatPanel::setContextSource(std::function<readercontext::Snapshot()> source)
{
    m_contextSource = std::move(source);
    refreshChip();
}

void ChatPanel::updateContext(bool selectionChanged)
{
    if (selectionChanged)
        m_chipState.includeSelection();
    refreshChip();
}

void ChatPanel::askAbout(const std::string& selection)
{
    const wxString prefill = wxString::FromUTF8(readercontext::askPrefill(selection));
    if (prefill.empty() || m_setupNeeded || m_keyRejected)
        return;
    m_chipState.includeSelection();
    refreshChip();
    // A draft the user typed stays, after the quote.
    const wxString draft = m_input->GetValue();
    m_input->ChangeValue(draft.Strip(wxString::both).empty() ? prefill : prefill + draft);
    m_input->SetFocus();
    m_input->SetInsertionPoint(static_cast<long>(prefill.length()));
    m_input->ShowPosition(m_input->GetInsertionPoint());
}

readercontext::Snapshot ChatPanel::snapshot() const
{
    return m_contextSource ? m_contextSource() : readercontext::Snapshot{};
}

void ChatPanel::refreshChip()
{
    const readercontext::Snapshot view = snapshot();
    const std::string label = m_chipState.label(view);
    const bool removable = !m_chipState.selectionExcluded() && view.selection
                           && !view.selection->text.empty();
    m_chip->setContent(wxString::FromUTF8(label), removable);
    const bool shown = !label.empty() && !m_setupNeeded && !m_keyRejected;
    if (GetSizer()->IsShown(m_chip) != shown) {
        GetSizer()->Show(m_chip, shown);
        Layout();
    }
}

void ChatPanel::onSidecarMessage(const protocol::Outbound& msg)
{
    if (const auto* delta = std::get_if<protocol::TextDelta>(&msg)) {
        if (m_running && delta->chatId == m_chatId) {
            m_reply += delta->text;
            m_replyText += delta->text;
            scheduleRender();
        }
    } else if (const auto* tool = std::get_if<protocol::ToolUse>(&msg)) {
        onToolUse(*tool);
    } else if (const auto* request = std::get_if<protocol::PermissionRequest>(&msg)) {
        onPermissionRequest(*request);
    } else if (const auto* result = std::get_if<protocol::Result>(&msg)) {
        onResult(*result);
    } else if (const auto* error = std::get_if<protocol::Error>(&msg)) {
        onError(*error);
    }
}

void ChatPanel::onSidecarDown(const wxString& reason)
{
    // Requests to the old process cannot be answered any more.
    dropPermissions({});
    if (m_running) {
        endReply(redHtml(markdown::escapeText("Agent stopped, restarting\xe2\x80\xa6 ("
                                              + reason.utf8_string() + ")"))
                 + greyHtml("Send your message again."));
    }
    // The open chats died with the process. The selected one is reopened,
    // queued until the restarted sidecar is ready.
    forgetOpenChats();
    openChat(m_chatId);
}

void ChatPanel::interruptReply()
{
    if (!m_running)
        return;
    if (!m_stopRequested)
        m_sidecar.send(protocol::Interrupt{m_chatId, std::nullopt});
    endReply(greyHtml("Stopped."));
}

void ChatPanel::onSidecarRestarted(const wxString& notice)
{
    m_available = true;
    m_historyHtml += greyHtml(markdown::escapeText(notice.utf8_string()));
    updateButtons();
    render(true);
    // Queued until the new sidecar is ready.
    forgetOpenChats();
    openChat(m_chatId);
}

void ChatPanel::onResult(const protocol::Result& result)
{
    dropPermissions(result.chatId);
    // The session resumed, and a continued chat has its context now.
    m_resuming.erase(result.chatId);
    m_continuations.erase(result.chatId);
    if (result.chatId != m_chatId || !m_running || !m_chats)
        return;
    std::string error;
    if (!m_chats->addResult(m_chatId, m_replyText, result, error))
        noteSaveError(error);
    // A reply the user stopped may still end with `result` after the interrupt.
    endReply(m_stopRequested ? greyHtml("Stopped.") : resultFooter(result));
    refreshChatList();
}

void ChatPanel::onError(const protocol::Error& error)
{
    const ErrorAction action = errorAction(error.kind);
    // busy cannot come from this pane, which never sends while a reply runs.
    // A running reply goes on.
    if (action == ErrorAction::Ignore)
        return;
    // A chat's error ends its reply, and with it any permission request.
    if (error.chatId)
        dropPermissions(*error.chatId);
    if (error.chatId && m_resuming.contains(*error.chatId) && failsResume(error.kind)) {
        onResumeFailed(*error.chatId, error.message);
        return;
    }
    if (error.chatId && errorClosesChat(error.kind)) {
        m_openChats.erase(*error.chatId);
        m_resuming.erase(*error.chatId);
    }
    if (error.chatId && *error.chatId != m_chatId)
        return;

    // An error without a chat_id is not about the running reply.
    const bool endsReply = m_running && error.chatId;
    const std::string message = markdown::escapeText(error.message);
    std::string notice;
    switch (action) {
    case ErrorAction::Stopped:
        notice = greyHtml("Stopped.");
        break;
    case ErrorAction::AuthBanner:
        notice = redHtml("API key rejected: " + message);
        m_keyRejected = true;
        break;
    case ErrorAction::RetryNotice:
        notice = redHtml("The rate limit was reached. Wait a moment, then press Retry.");
        if (endsReply)
            m_retryText = m_lastPrompt;
        break;
    case ErrorAction::Reinstall:
        notice = redHtml("Installation is incomplete, reinstall the app.");
        break;
    case ErrorAction::Restarting:
        notice = redHtml("Agent stopped, restarting\xe2\x80\xa6");
        break;
    case ErrorAction::Inline:
    case ErrorAction::Ignore:
        notice = redHtml("Error: " + message);
        break;
    }

    // These leave the chat closed in the sidecar. Reopen it for the next send.
    if (errorClosesChat(error.kind) && m_hasProject && m_chats && m_chats->find(m_chatId)) {
        if (action != ErrorAction::Reinstall)
            notice += greyHtml("The chat was reopened. Send your message again.");
        openChat(m_chatId);
    }

    if (endsReply) {
        endReply(notice);
    } else {
        m_historyHtml += notice;
        render();
    }
    updateControls();
}

void ChatPanel::onResumeFailed(const std::string& chatId, const std::string& message)
{
    // Not reopened: another resume would fail the same way.
    m_resuming.erase(chatId);
    m_openChats.erase(chatId);
    m_cannotResume[chatId] = message;
    if (chatId == m_chatId) {
        const std::string notice = cannotResumeHtml(message);
        if (m_running) {
            endReply(notice);
        } else {
            m_historyHtml += notice;
            render();
        }
    }
    refreshChatList();
    updateControls();
}

void ChatPanel::onToolUse(const protocol::ToolUse& tool)
{
    if (!m_running || tool.chatId != m_chatId)
        return;
    // Text before and after a tool call are separate paragraphs.
    if (!m_replyText.empty() && !m_replyText.ends_with("\n\n"))
        m_replyText += "\n\n";
    // The line goes where the call happened, between the text around it.
    m_replyHtml += markdown::toHtml(m_reply) + toolHtml(tool);
    m_reply.clear();
    scheduleRender();
}

void ChatPanel::onPermissionRequest(const protocol::PermissionRequest& request)
{
    if (request.chatId != m_chatId) {
        // A chat this panel no longer shows. Nobody can judge the request.
        m_sidecar.send(protocol::PermissionReply{request.reqId, false, std::nullopt});
        return;
    }
    m_permissions.push_back(request);
    // Not from inside the sidecar's event handler: ShowModal runs an event loop.
    if (!m_permissionDialog)
        CallAfter(&ChatPanel::showNextPermission);
}

void ChatPanel::showNextPermission()
{
    if (m_permissionDialog || m_permissions.empty())
        return;
    const protocol::PermissionRequest request = std::move(m_permissions.front());
    m_permissions.pop_front();

    render(); // show the tool line behind the dialog
    PermissionDialog dialog(wxGetTopLevelParent(this), request.tool, request.input);
    m_permissionDialog = &dialog;
    m_permissionChatId = request.chatId;
    const int answer = dialog.ShowModal();
    m_permissionDialog = nullptr;
    m_permissionChatId.clear();

    if (answer != PermissionDialog::kDismissed)
        m_sidecar.send(protocol::PermissionReply{request.reqId, answer == wxID_OK, std::nullopt});
    if (!m_permissions.empty())
        CallAfter(&ChatPanel::showNextPermission);
}

void ChatPanel::dropPermissions(const std::string& chatId)
{
    std::erase_if(m_permissions, [&](const protocol::PermissionRequest& request) {
        return chatId.empty() || request.chatId == chatId;
    });
    if (m_permissionDialog && (chatId.empty() || m_permissionChatId == chatId))
        m_permissionDialog->dismiss();
}

bool ChatPanel::canSend() const
{
    return m_available && !m_running && !m_setupNeeded && !m_keyRejected;
}

void ChatPanel::send()
{
    if (!canSend())
        return;
    const wxString value = m_input->GetValue();
    if (value.Strip(wxString::both).empty())
        return;
    if (!m_hasProject || !m_chats || !m_chats->find(m_chatId)) {
        m_historyHtml += redHtml("Open a PDF to start a chat.");
        render(true);
        return;
    }
    if (m_cannotResume.contains(m_chatId)) {
        m_historyHtml += redHtml("This chat cannot resume. Press Continue in new chat to go on.");
        render(true);
        return;
    }
    m_input->Clear();
    // The x on the chip covers this prompt only.
    std::optional<protocol::Context> context = m_chipState.takeForPrompt(snapshot());
    refreshChip();
    startReply(value.utf8_string(), std::move(context));
}

void ChatPanel::retry()
{
    if (!canSend() || m_retryText.empty() || !m_hasProject || !m_chats
        || m_cannotResume.contains(m_chatId))
        return;
    // The same prompt again, with what the reader showed when it was sent.
    startReply(m_retryText, m_lastContext);
}

void ChatPanel::startReply(std::string text, std::optional<protocol::Context> context)
{
    // A continued chat sends the old chat's messages with each prompt until
    // a reply came through.
    std::string prompt = text;
    if (const auto it = m_continuations.find(m_chatId); it != m_continuations.end())
        prompt = chatlist::continuePrompt(it->second.messages, text);
    std::string error;
    if (!m_chats->addUserMessage(m_chatId, text, error))
        noteSaveError(error);
    m_historyHtml += userHtml(text);
    m_lastPrompt = text;
    m_lastContext = context;
    m_retryText.clear();
    m_replyHtml.clear();
    m_reply.clear();
    m_replyText.clear();
    m_running = true;
    m_stopRequested = false;
    refreshChatList();
    updateControls();
    m_sidecar.send(protocol::Prompt{m_chatId, prompt, std::move(context), std::nullopt});
    render(true);
}

void ChatPanel::stop()
{
    if (!m_running || m_stopRequested)
        return;
    m_stopRequested = true;
    m_sidecar.send(protocol::Interrupt{m_chatId, std::nullopt});
    updateButtons();
}

bool ChatPanel::canNewChat() const
{
    return m_chats != nullptr && !m_running;
}

bool ChatPanel::canStop() const
{
    return m_running && !m_stopRequested;
}

void ChatPanel::focusInput()
{
    if (m_input->IsShown())
        m_input->SetFocus();
}

void ChatPanel::endReply(const std::string& trailerHtml)
{
    m_historyHtml += m_replyHtml + markdown::toHtml(m_reply) + trailerHtml;
    m_replyHtml.clear();
    m_reply.clear();
    m_replyText.clear();
    m_running = false;
    m_stopRequested = false;
    updateButtons();
    render();
}

void ChatPanel::updateButtons()
{
    m_send->Enable(m_available && !m_running);
    m_stop->Enable(canStop());
    m_retry->Enable(m_available && !m_running);
    const bool retry = !m_retryText.empty() && !m_running && !m_setupNeeded && !m_keyRejected;
    const bool cont = !m_running && m_cannotResume.contains(m_chatId);
    if (m_retry->IsShown() != retry || m_continue->IsShown() != cont) {
        m_retry->Show(retry);
        m_continue->Show(cont);
        m_buttons->Layout();
    }
    // Chats change only between replies.
    const bool chats = canNewChat();
    m_chatChoice->Enable(chats && !m_choiceIds.empty());
    m_newChat->Enable(chats);
    m_rename->Enable(chats && m_chats->find(m_chatId));
    m_delete->Enable(chats && m_chats->find(m_chatId));
}

void ChatPanel::updateControls()
{
    const bool banner = m_setupNeeded || m_keyRejected;
    if (banner) {
        m_banner->setReason(m_setupNeeded ? SetupBanner::Reason::FirstRun
                                          : SetupBanner::Reason::KeyRejected);
    }
    wxSizer* sizer = GetSizer();
    sizer->Show(m_input, !banner);
    sizer->Show(m_buttons, !banner);
    sizer->Show(m_banner, banner);
    updateButtons();
    refreshChip();
    Layout();
}

void ChatPanel::scheduleRender()
{
    if (!m_renderTimer.IsRunning())
        m_renderTimer.StartOnce(kRenderDelayMs);
}

void ChatPanel::render(bool toBottom)
{
    m_renderTimer.Stop();
    const bool follow = toBottom || atBottom();
    int x = 0;
    int y = 0;
    m_view->GetViewStart(&x, &y);

    std::string html = "<html><body>" + m_historyHtml;
    if (m_running)
        html += m_replyHtml + markdown::toHtml(m_reply);
    html += "</body></html>";
    html = addBreakPoints(html);

    // SetPage scrolls to the top. Put the view back where it was, or at the
    // bottom when it was following the newest text.
    m_view->Freeze();
    m_view->SetPage(wxString::FromUTF8(html));
    if (follow) {
        int unitX = 0;
        int unitY = 0;
        m_view->GetScrollPixelsPerUnit(&unitX, &unitY);
        if (unitY > 0)
            m_view->Scroll(-1, m_view->GetVirtualSize().y / unitY);
    } else {
        m_view->Scroll(x, y);
    }
    m_view->Thaw();
}

bool ChatPanel::atBottom() const
{
    int unitX = 0;
    int unitY = 0;
    m_view->GetScrollPixelsPerUnit(&unitX, &unitY);
    if (unitY <= 0)
        return true;
    int x = 0;
    int y = 0;
    m_view->GetViewStart(&x, &y);
    // Within one scroll step of the end counts as the bottom.
    return y * unitY + m_view->GetClientSize().y >= m_view->GetVirtualSize().y - unitY;
}

void ChatPanel::onInputKey(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    const bool enter = key == WXK_RETURN || key == WXK_NUMPAD_ENTER;
    if (enter && !event.ShiftDown() && !event.ControlDown() && !event.AltDown()) {
        send();
        return;
    }
    // Shift+Enter falls through and the control inserts a newline.
    event.Skip();
}

void ChatPanel::onSend(wxCommandEvent& /*event*/)
{
    send();
    m_input->SetFocus();
}

void ChatPanel::onStop(wxCommandEvent& /*event*/)
{
    stop();
}

void ChatPanel::onRetry(wxCommandEvent& /*event*/)
{
    retry();
    m_input->SetFocus();
}

void ChatPanel::onChatChosen(wxCommandEvent& event)
{
    const int index = event.GetSelection();
    if (index < 0 || static_cast<std::size_t>(index) >= m_choiceIds.size())
        return;
    if (m_running) {
        refreshChatList(); // put the selection back
        return;
    }
    if (m_choiceIds[static_cast<std::size_t>(index)] != m_chatId)
        selectChat(m_choiceIds[static_cast<std::size_t>(index)]);
}

void ChatPanel::onNewChat(wxCommandEvent& /*event*/)
{
    newChat();
    m_input->SetFocus();
}

void ChatPanel::onRename(wxCommandEvent& /*event*/)
{
    if (!m_chats || !m_chats->find(m_chatId))
        return;
    const std::string id = m_chatId;
    wxTextEntryDialog dialog(wxGetTopLevelParent(this), "Name of this chat:", "Rename Chat",
                             wxString::FromUTF8(m_chats->find(id)->title));
    if (dialog.ShowModal() != wxID_OK)
        return;
    const std::string title = dialog.GetValue().Strip(wxString::both).utf8_string();
    // The chat may be gone, after a project switch during the dialog.
    if (title.empty() || !m_chats || !m_chats->find(id))
        return;
    std::string error;
    if (!m_chats->rename(id, title, error))
        noteSaveError(error);
    refreshChatList();
}

void ChatPanel::onDelete(wxCommandEvent& /*event*/)
{
    if (m_running || !m_chats || !m_chats->find(m_chatId))
        return;
    const std::string id = m_chatId;
    const std::string label = chatLabel(*m_chats->find(id));
    wxMessageDialog dialog(wxGetTopLevelParent(this),
                           wxString::FromUTF8("Delete the chat \xe2\x80\x9c" + label
                                              + "\xe2\x80\x9d?\n\nIts messages are removed from "
                                                "this project. This cannot be undone."),
                           "Delete Chat", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (dialog.ShowModal() != wxID_YES || m_running || !m_chats || !m_chats->find(id))
        return;
    std::string error;
    const bool removed = m_chats->remove(id, error);
    m_openChats.erase(id);
    m_resuming.erase(id);
    m_cannotResume.erase(id);
    m_continuations.erase(id);
    const std::vector<ChatInfo> chats = m_chats->chats();
    if (chats.empty())
        newChat();
    else
        selectChat(chats.front().id);
    if (!removed)
        noteSaveError(error);
}

void ChatPanel::onContinue(wxCommandEvent& /*event*/)
{
    if (m_running || !m_chats || !m_chats->find(m_chatId) || !m_cannotResume.contains(m_chatId))
        return;
    const ChatInfo& old = *m_chats->find(m_chatId);
    Continuation continuation{old.title.empty() ? kUntitled : old.title,
                              m_chats->transcript(m_chatId)};
    std::string error;
    const std::string id = m_chats->create(error);
    m_continuations[id] = std::move(continuation);
    selectChat(id);
    if (!error.empty())
        noteSaveError(error);
    m_input->SetFocus();
}

void ChatPanel::onChipClose(wxCommandEvent& /*event*/)
{
    m_chipState.excludeSelection();
    refreshChip();
}

void ChatPanel::onLink(wxHtmlLinkEvent& event)
{
    const wxString href = event.GetLinkInfo().GetHref();
    if (isWebLink(href))
        wxLaunchDefaultBrowser(href);
}

void ChatPanel::onRenderTimer(wxTimerEvent& /*event*/)
{
    render();
}
