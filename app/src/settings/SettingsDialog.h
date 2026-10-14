#pragma once

#include <wx/dialog.h>

#include <memory>

class KeyTester;
class Settings;
class wxCheckBox;
class wxCommandEvent;
class wxStaticText;
class wxTextCtrl;

// Edit > Preferences: the API key (masked, with Show and Test key), the model,
// the developer local-login toggle, and the claude.ai connectors toggle. OK
// saves through Settings.
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent, Settings& settings);
    ~SettingsDialog() override;

private:
    wxString enteredKey() const;
    void onShowKey(wxCommandEvent& event);
    void onTestKey(wxCommandEvent& event);
    void onLocalLoginChanged(wxCommandEvent& event);
    void onOk(wxCommandEvent& event);

    Settings& m_settings;
    bool m_keyStoreOk = false;
    wxString m_savedKey;

    // Two fields, one shown at a time: GTK cannot switch a field's masking.
    wxTextCtrl* m_keyMasked = nullptr;
    wxTextCtrl* m_keyPlain = nullptr;
    wxCheckBox* m_showKey = nullptr;
    wxStaticText* m_testStatus = nullptr;
    wxTextCtrl* m_model = nullptr;
    wxCheckBox* m_localLogin = nullptr;
    wxCheckBox* m_connectors = nullptr;

    std::unique_ptr<KeyTester> m_tester;
};
