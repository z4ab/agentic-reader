#pragma once

#include <wx/panel.h>
#include <wx/timer.h>

class PdfView;
class wxButton;
class wxCheckBox;
class wxCommandEvent;
class wxKeyEvent;
class wxStaticText;
class wxTextCtrl;
class wxTimerEvent;

// The find bar above the PDF view: a text field, previous and next buttons, a
// "Match case" checkbox, the match count and a close button. Starts hidden. It
// searches as the text changes and drives PdfView's find. Enter goes to the next
// match, Shift+Enter to the previous one, Esc closes the bar.
//
// Lives in a sizer: showing or hiding it lays out its parent.
class FindBar : public wxPanel
{
public:
    // `view` must outlive the bar.
    FindBar(wxWindow* parent, PdfView* view);

    // Shows the bar, focuses the text field and selects its text. A bar that
    // was hidden searches its text again.
    void activate();
    // Hides the bar, ends the search and removes the highlights, and gives the
    // focus back to the view. Does nothing while hidden.
    void dismiss();

private:
    // Runs the query in the field now, unless the view already shows it.
    void search();
    void step(bool forward);
    void showCount();

    void onCharHook(wxKeyEvent& event);
    void onText(wxCommandEvent& event);
    void onDebounce(wxTimerEvent& event);
    void onFindChanged(wxCommandEvent& event);

    PdfView* m_view;
    wxTextCtrl* m_field = nullptr;
    wxButton* m_prev = nullptr;
    wxButton* m_next = nullptr;
    wxCheckBox* m_matchCase = nullptr;
    wxStaticText* m_count = nullptr;
    wxButton* m_close = nullptr;
    // Delays the search while typing.
    wxTimer m_debounce;
};
