#include "chat/ContextChip.h"

#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include <algorithm>

namespace
{

constexpr int kPadX = 8;      // DIPs between the edge and the text
constexpr int kPadY = 4;      // DIPs above and below the text
constexpr int kCloseWidth = 22;
constexpr int kRadius = 6;

// `a` with `percent` of `b` mixed in.
wxColour mix(const wxColour& a, const wxColour& b, int percent)
{
    auto channel = [percent](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>((x * (100 - percent) + y * percent) / 100);
    };
    return wxColour(channel(a.Red(), b.Red()), channel(a.Green(), b.Green()),
                    channel(a.Blue(), b.Blue()));
}

} // namespace

ContextChip::ContextChip(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ContextChip::onPaint, this);
    Bind(wxEVT_MOTION, &ContextChip::onMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &ContextChip::onLeave, this);
    Bind(wxEVT_LEFT_DOWN, &ContextChip::onLeftDown, this);
    Bind(wxEVT_LEFT_DCLICK, &ContextChip::onLeftDown, this);
    Bind(wxEVT_LEFT_UP, &ContextChip::onLeftUp, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Refresh();
        event.Skip();
    });
}

void ContextChip::setContent(const wxString& label, bool removable)
{
    if (label == m_label && removable == m_removable)
        return;
    m_label = label;
    m_removable = removable;
    if (!removable) {
        m_hover = false;
        m_pressed = false;
        SetCursor(wxNullCursor);
    }
    SetToolTip(removable ? wxString("Leave the selection out of the next message") : wxString());
    Refresh();
}

wxSize ContextChip::DoGetBestClientSize() const
{
    return wxSize(FromDIP(2 * kPadX + kCloseWidth), GetCharHeight() + FromDIP(2 * kPadY));
}

wxRect ContextChip::closeRect() const
{
    const wxSize size = GetClientSize();
    const int width = FromDIP(kCloseWidth);
    return wxRect(size.x - width, 0, width, size.y);
}

void ContextChip::setHover(bool hover)
{
    if (hover == m_hover)
        return;
    m_hover = hover;
    SetCursor(hover ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    RefreshRect(closeRect());
}

void ContextChip::onPaint(wxPaintEvent& /*event*/)
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour background = GetParent()->GetBackgroundColour();
    const wxColour window = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    dc.SetBackground(wxBrush(background));
    dc.Clear();
    if (m_label.empty())
        return;

    const wxSize size = GetClientSize();
    dc.SetPen(wxPen(mix(window, text, 25)));
    dc.SetBrush(wxBrush(mix(window, text, 8)));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(kRadius));

    dc.SetFont(GetFont());
    const int padX = FromDIP(kPadX);
    const int right = m_removable ? closeRect().x : size.x - padX;
    const int room = std::max(0, right - padX);
    const wxString shown = wxControl::Ellipsize(m_label, dc, wxELLIPSIZE_MIDDLE, room);
    dc.SetTextForeground(text);
    const int textY = (size.y - dc.GetCharHeight()) / 2;
    dc.DrawText(shown, padX, textY);

    if (!m_removable)
        return;
    const wxRect close = closeRect();
    if (m_hover) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(mix(window, text, m_pressed ? 30 : 18)));
        const int d = std::min(close.width, close.height) - FromDIP(6);
        dc.DrawRoundedRectangle(close.x + (close.width - d) / 2, close.y + (close.height - d) / 2,
                                d, d, FromDIP(kRadius / 2));
    }
    // The x as two strokes, crisp at any font.
    const int arm = FromDIP(4);
    const wxPoint c(close.x + close.width / 2, close.y + close.height / 2);
    dc.SetPen(wxPen(m_hover ? text : mix(text, window, 35), FromDIP(2)));
    dc.DrawLine(c.x - arm, c.y - arm, c.x + arm + 1, c.y + arm + 1);
    dc.DrawLine(c.x - arm, c.y + arm, c.x + arm + 1, c.y - arm - 1);
}

void ContextChip::onMotion(wxMouseEvent& event)
{
    setHover(m_removable && closeRect().Contains(event.GetPosition()));
    event.Skip();
}

void ContextChip::onLeave(wxMouseEvent& event)
{
    m_pressed = false;
    setHover(false);
    event.Skip();
}

void ContextChip::onLeftDown(wxMouseEvent& event)
{
    m_pressed = m_removable && closeRect().Contains(event.GetPosition());
    if (m_pressed)
        RefreshRect(closeRect());
    event.Skip();
}

void ContextChip::onLeftUp(wxMouseEvent& event)
{
    const bool click = m_pressed && m_removable && closeRect().Contains(event.GetPosition());
    m_pressed = false;
    event.Skip();
    if (!click)
        return;
    wxCommandEvent button(wxEVT_BUTTON, GetId());
    button.SetEventObject(this);
    ProcessWindowEvent(button);
}
