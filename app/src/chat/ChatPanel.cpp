#include "chat/ChatPanel.h"

#include "agent/SidecarClient.h"
#include "chat/Markdown.h"
#include "chat/PermissionDialog.h"
#include "chat/SetupBanner.h"
#include "chat/ToolFormat.h"

#include <wx/button.h>
#include <wx/html/htmlwin.h>
#include <wx/html/m_templ.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <cinttypes>
#include <cstdio>
#include <random>
#include <variant>

namespace
{

constexpr int kGap = 5;
constexpr int kInputHeight = 60;
constexpr int kRenderDelayMs = 50;
constexpr std::size_t kMaxToolLine = 300; // bytes shown of a tool line

std::string newChatId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & ~std::uint64_t{0xf000}) | 0x4000;                 // version 4
    lo = (lo & ~(std::uint64_t{3} << 62)) | (std::uint64_t{2} << 62); // RFC 4122 variant
    char buf[37];
    std::snprintf(buf, sizeof buf, "%08" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%04" PRIx64
                  "-%012" PRIx64,
                  hi >> 32, (hi >> 16) & 0xffff, hi & 0xffff, lo >> 48,
                  lo & std::uint64_t{0xffffffffffff});
    return buf;
}

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
    m_view = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxHW_SCROLLBAR_AUTO | wxBORDER_THEME);
    m_input = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition,
                             wxSize(-1, kInputHeight), wxTE_MULTILINE);
    m_retry = new wxButton(this, wxID_ANY, "Retry");
    m_send = new wxButton(this, wxID_ANY, "Send");
    m_stop = new wxButton(this, wxID_ANY, "Stop");
    m_banner = new SetupBanner(this);

    // Retry at the left end, Send and Stop at the right.
    m_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_buttons->Add(m_retry);
    m_buttons->AddStretchSpacer();
    m_buttons->Add(m_send, 0, wxRIGHT, kGap);
    m_buttons->Add(m_stop);

    // M5.3 adds the context chip between the view and the input.
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_view, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, kGap);
    sizer->Add(m_buttons, 0, wxEXPAND | wxALL, kGap);
    // In place of the input and the buttons.
    sizer->Add(m_banner, 0, wxEXPAND | wxALL, kGap);
    SetSizer(sizer);

    m_input->Bind(wxEVT_KEY_DOWN, &ChatPanel::onInputKey, this);
    m_send->Bind(wxEVT_BUTTON, &ChatPanel::onSend, this);
    m_stop->Bind(wxEVT_BUTTON, &ChatPanel::onStop, this);
    m_retry->Bind(wxEVT_BUTTON, &ChatPanel::onRetry, this);
    m_view->Bind(wxEVT_HTML_LINK_CLICKED, &ChatPanel::onLink, this);
    Bind(wxEVT_TIMER, &ChatPanel::onRenderTimer, this);

    updateControls();
    render();
}

void ChatPanel::openProject(const std::string& path)
{
    m_sidecar.send(protocol::OpenProject{path, std::nullopt});
    m_hasProject = true;
    m_sessionId.clear();
    startChat();
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

void ChatPanel::startChat()
{
    // The previous chat is closed in the sidecar, or about to be.
    if (!m_chatId.empty())
        dropPermissions(m_chatId);
    m_chatId = newChatId();
    if (m_sessionId.empty())
        m_sidecar.send(protocol::NewChat{m_chatId, std::nullopt});
    else
        m_sidecar.send(protocol::ResumeChat{m_chatId, m_sessionId, std::nullopt});
}

void ChatPanel::onSidecarMessage(const protocol::Outbound& msg)
{
    if (const auto* delta = std::get_if<protocol::TextDelta>(&msg)) {
        if (m_running && delta->chatId == m_chatId) {
            m_reply += delta->text;
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
    // The old chat died with the process. Queued until the restart is ready.
    if (m_hasProject)
        startChat();
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
    if (m_hasProject)
        startChat();
}

void ChatPanel::onResult(const protocol::Result& result)
{
    dropPermissions(result.chatId);
    if (result.chatId != m_chatId)
        return;
    m_sessionId = result.sessionId;
    if (m_running)
        endReply(resultFooter(result));
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
    if (errorClosesChat(error.kind) && m_hasProject) {
        if (action != ErrorAction::Reinstall)
            notice += greyHtml("The chat was reopened. Send your message again.");
        startChat();
    }

    if (endsReply) {
        endReply(notice);
    } else {
        m_historyHtml += notice;
        render();
    }
    updateControls();
}

void ChatPanel::onToolUse(const protocol::ToolUse& tool)
{
    if (!m_running || tool.chatId != m_chatId)
        return;
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
    if (!m_hasProject) {
        m_historyHtml += redHtml("Open a PDF to start a chat.");
        render(true);
        return;
    }
    m_input->Clear();
    startReply(value.utf8_string());
}

void ChatPanel::retry()
{
    if (!canSend() || m_retryText.empty() || !m_hasProject)
        return;
    startReply(m_retryText);
}

void ChatPanel::startReply(std::string text)
{
    m_historyHtml += userHtml(text);
    m_lastPrompt = text;
    m_retryText.clear();
    m_replyHtml.clear();
    m_reply.clear();
    m_running = true;
    m_stopRequested = false;
    updateControls();
    m_sidecar.send(protocol::Prompt{m_chatId, text, std::nullopt, std::nullopt});
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

void ChatPanel::endReply(const std::string& trailerHtml)
{
    m_historyHtml += m_replyHtml + markdown::toHtml(m_reply) + trailerHtml;
    m_replyHtml.clear();
    m_reply.clear();
    m_running = false;
    m_stopRequested = false;
    updateButtons();
    render();
}

void ChatPanel::updateButtons()
{
    m_send->Enable(m_available && !m_running);
    m_stop->Enable(m_running && !m_stopRequested);
    m_retry->Enable(m_available && !m_running);
    const bool retry = !m_retryText.empty() && !m_running && !m_setupNeeded && !m_keyRejected;
    if (m_retry->IsShown() != retry) {
        m_retry->Show(retry);
        m_buttons->Layout();
    }
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
