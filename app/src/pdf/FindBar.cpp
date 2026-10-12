#include "pdf/FindBar.h"

#include "pdf/PdfEvents.h"
#include "pdf/PdfView.h"
#include "pdf/SearchWorker.h"

#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <string>

namespace {
constexpr int kFieldWidth = 260;
// Typing pause before the search starts.
constexpr int kDebounceMs = 100;
constexpr int kGap = 4;

// An icon button when the theme has the icon, else a small text button.
wxButton* makeButton(wxWindow* parent, const wxArtID& art, const wxString& fallback,
                     const wxString& tip)
{
    const wxBitmapBundle icon = wxArtProvider::GetBitmapBundle(art, wxART_BUTTON);
    wxButton* button = icon.IsOk() ? new wxBitmapButton(parent, wxID_ANY, icon)
                                   : new wxButton(parent, wxID_ANY, fallback, wxDefaultPosition,
                                                  wxDefaultSize, wxBU_EXACTFIT);
    button->SetToolTip(tip);
    button->SetName(fallback);
    return button;
}
} // namespace

FindBar::FindBar(wxWindow* parent, PdfView* view)
    : wxPanel(parent, wxID_ANY)
    , m_view(view)
    , m_debounce(this)
{
    m_field = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition,
                             FromDIP(wxSize(kFieldWidth, -1)), wxTE_PROCESS_ENTER);
    m_field->SetHint("Find in document");
    m_prev = makeButton(this, wxART_GO_UP, "Previous", "Previous match (Shift+Enter)");
    m_next = makeButton(this, wxART_GO_DOWN, "Next", "Next match (Enter)");
    m_matchCase = new wxCheckBox(this, wxID_ANY, "Match case");
    // Sized for a large count so the bar does not shift while the count grows.
    m_count = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxST_NO_AUTORESIZE);
    m_count->SetMinSize(wxSize(m_count->GetTextExtent("Searching... 88888 of 88888").x, -1));
    m_close = wxBitmapButton::NewCloseButton(this, wxID_ANY);
    m_close->SetToolTip("Close (Esc)");
    m_close->SetName("Close");

    const int gap = FromDIP(kGap);
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_field, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
    sizer->Add(m_prev, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, gap);
    sizer->Add(m_next, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, gap);
    sizer->Add(m_matchCase, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 2 * gap);
    sizer->Add(m_count, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 2 * gap);
    sizer->AddStretchSpacer();
    sizer->Add(m_close, 0, wxALIGN_CENTER_VERTICAL | wxALL, gap);
    SetSizer(sizer);
    Hide();

    Bind(wxEVT_CHAR_HOOK, &FindBar::onCharHook, this);
    Bind(wxEVT_TIMER, &FindBar::onDebounce, this);
    m_field->Bind(wxEVT_TEXT, &FindBar::onText, this);
    // Enter is handled in onCharHook. This keeps it from reaching a default button.
    m_field->Bind(wxEVT_TEXT_ENTER, [](wxCommandEvent&) {});
    m_prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { step(false); });
    m_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { step(true); });
    m_matchCase->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { search(); });
    m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { dismiss(); });
    m_view->Bind(EVT_PDF_FIND_CHANGED, &FindBar::onFindChanged, this);
}

void FindBar::activate()
{
    if (!IsShown()) {
        Show();
        GetParent()->Layout();
        search();
    }
    m_field->SetFocus();
    m_field->SelectAll();
}

void FindBar::dismiss()
{
    if (!IsShown())
        return;
    m_debounce.Stop();
    m_view->endFind();
    Hide();
    GetParent()->Layout();
    m_view->SetFocus();
}

void FindBar::search()
{
    m_debounce.Stop();
    const std::string query = m_field->GetValue().utf8_string();
    const bool matchCase = m_matchCase->GetValue();
    if (query == m_view->findQuery() && matchCase == m_view->findMatchCase())
        return;
    m_view->find(query, matchCase);
    showCount();
}

void FindBar::step(bool forward)
{
    // A query still waiting for the debounce runs first. Its first match becomes
    // current, which is where a step from no match lands anyway.
    if (m_debounce.IsRunning() || m_view->findQuery() != m_field->GetValue().utf8_string()) {
        search();
        return;
    }
    m_view->findStep(forward);
}

void FindBar::showCount()
{
    wxString text;
    if (!m_view->findQuery().empty()) {
        if (m_view->findTotal() > 0)
            text = matchCountLabel(m_view->findCurrent(), m_view->findTotal());
        else
            text = m_view->findDone() ? "No matches" : "Searching...";
    }
    if (m_count->GetLabel() != text)
        m_count->SetLabel(text);
}

void FindBar::onCharHook(wxKeyEvent& event)
{
    const int key = event.GetKeyCode();
    if (key == WXK_ESCAPE && !event.HasAnyModifiers()) {
        dismiss();
        return;
    }
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && FindFocus() == m_field &&
        (event.GetModifiers() & ~wxMOD_SHIFT) == 0) {
        step(!event.ShiftDown());
        return;
    }
    event.Skip();
}

void FindBar::onText(wxCommandEvent& /*event*/)
{
    m_debounce.StartOnce(kDebounceMs);
}

void FindBar::onDebounce(wxTimerEvent& /*event*/)
{
    search();
}

void FindBar::onFindChanged(wxCommandEvent& event)
{
    event.Skip();
    showCount();
}
