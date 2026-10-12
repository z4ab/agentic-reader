#pragma once

#include <wx/window.h>

class wxMouseEvent;
class wxPaintEvent;

// One row above the chat input naming what the next prompt carries, such as
// "attention.pdf · p. 12 · 214 chars selected". With removable() an x at its
// right end sends wxEVT_BUTTON when clicked. The text is shortened in the
// middle when the row is too narrow for it.
class ContextChip : public wxWindow
{
public:
    explicit ContextChip(wxWindow* parent);

    // `label` is UTF-8. Repaints only when something changed.
    void setContent(const wxString& label, bool removable);
    const wxString& label() const { return m_label; }
    bool removable() const { return m_removable; }

protected:
    wxSize DoGetBestClientSize() const override;

private:
    wxRect closeRect() const;
    void setHover(bool hover);

    void onPaint(wxPaintEvent& event);
    void onMotion(wxMouseEvent& event);
    void onLeave(wxMouseEvent& event);
    void onLeftDown(wxMouseEvent& event);
    void onLeftUp(wxMouseEvent& event);

    wxString m_label;
    bool m_removable = false;
    bool m_hover = false;
    bool m_pressed = false;
};
