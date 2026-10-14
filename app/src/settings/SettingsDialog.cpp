#include "settings/SettingsDialog.h"

#include "settings/KeyTester.h"
#include "settings/Settings.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace
{
constexpr int kKeyFieldWidth = 360;
constexpr int kSectionGap = 8;
} // namespace

SettingsDialog::SettingsDialog(wxWindow* parent, Settings& settings)
    : wxDialog(parent, wxID_ANY, "Preferences"), m_settings(settings),
      m_tester(std::make_unique<KeyTester>())
{
    wxString why;
    m_keyStoreOk = m_settings.keyStoreAvailable(&why);
    if (m_keyStoreOk)
        m_savedKey = m_settings.apiKey();

    auto* top = new wxBoxSizer(wxVERTICAL);
    const wxSizerFlags label = wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP);
    const wxSizerFlags row = wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP);

    // API key. Children are created in tab order.
    top->Add(new wxStaticText(this, wxID_ANY, "Anthropic API key"), label);
    auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
    m_keyMasked = new wxTextCtrl(this, wxID_ANY, m_savedKey, wxDefaultPosition,
                                 wxSize(kKeyFieldWidth, -1), wxTE_PASSWORD);
    m_keyPlain = new wxTextCtrl(this, wxID_ANY, wxString(), wxDefaultPosition,
                                wxSize(kKeyFieldWidth, -1));
    m_keyPlain->Hide();
    m_showKey = new wxCheckBox(this, wxID_ANY, "Show");
    auto* testKey = new wxButton(this, wxID_ANY, "Test key");
    keyRow->Add(m_keyMasked, wxSizerFlags(1).CenterVertical());
    keyRow->Add(m_keyPlain, wxSizerFlags(1).CenterVertical());
    keyRow->Add(m_showKey, wxSizerFlags().CenterVertical().Border(wxLEFT));
    keyRow->Add(testKey, wxSizerFlags().CenterVertical().Border(wxLEFT));
    top->Add(keyRow, row);

    m_testStatus = new wxStaticText(this, wxID_ANY, wxString());
    top->Add(m_testStatus, row);

    if (!m_keyStoreOk) {
        auto* warning = new wxStaticText(
            this, wxID_ANY,
            "The API key cannot be stored securely, so it will not be saved.\nReason: " + why);
        warning->SetForegroundColour(*wxRED);
        top->Add(warning, row);
    }

    // Model.
    top->AddSpacer(kSectionGap);
    top->Add(new wxStaticText(this, wxID_ANY, "Model"), label);
    m_model = new wxTextCtrl(this, wxID_ANY, m_settings.model());
    top->Add(m_model, row);
    top->Add(new wxStaticText(this, wxID_ANY, "Leave empty to use the default model."), row);

    // Developer login.
    m_localLogin = new wxCheckBox(this, wxID_ANY, "Use my local Claude Code login (developer)");
    m_localLogin->SetValue(m_settings.useLocalLogin());
    top->AddSpacer(kSectionGap);
    top->Add(m_localLogin, row);
    top->Add(new wxStaticText(this, wxID_ANY,
                              "For personal use only: it uses your own login on this computer "
                              "instead of the API key."),
             row);

    // claude.ai connectors. Enabled only with the local login, since the CLI
    // loads them only then.
    m_connectors =
        new wxCheckBox(this, wxID_ANY, "Use my claude.ai connectors (Gmail, Drive, ...)");
    m_connectors->SetValue(m_settings.useClaudeAiConnectors());
    m_connectors->Enable(m_localLogin->GetValue());
    top->Add(m_connectors, row);
    top->Add(new wxStaticText(this, wxID_ANY,
                              "Lets the assistant use the connectors on your claude.ai account. "
                              "It asks before each use."),
             row);

    top->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Expand().Border());
    SetSizerAndFit(top);
    CentreOnParent();

    m_showKey->Bind(wxEVT_CHECKBOX, &SettingsDialog::onShowKey, this);
    m_localLogin->Bind(wxEVT_CHECKBOX, &SettingsDialog::onLocalLoginChanged, this);
    testKey->Bind(wxEVT_BUTTON, &SettingsDialog::onTestKey, this);
    Bind(wxEVT_BUTTON, &SettingsDialog::onOk, this, wxID_OK);

    m_keyMasked->SetFocus();
    // Take keyboard focus once shown. Without a window manager that focuses
    // new windows (Xvfb, some X11 setups) GTK never activates the dialog, so
    // Tab stops moving focus.
    CallAfter([this] { Raise(); });
}

SettingsDialog::~SettingsDialog() = default;

wxString SettingsDialog::enteredKey() const
{
    wxString key = (m_showKey->GetValue() ? m_keyPlain : m_keyMasked)->GetValue();
    return key.Trim(true).Trim(false);
}

void SettingsDialog::onShowKey(wxCommandEvent& /*event*/)
{
    const bool show = m_showKey->GetValue();
    wxTextCtrl* from = show ? m_keyMasked : m_keyPlain;
    wxTextCtrl* to = show ? m_keyPlain : m_keyMasked;
    to->ChangeValue(from->GetValue());
    from->ChangeValue(wxString());
    from->Hide();
    to->Show();
    Layout();
}

void SettingsDialog::onTestKey(wxCommandEvent& /*event*/)
{
    const wxString key = enteredKey();
    if (key.empty()) {
        m_tester->cancel();
        m_testStatus->SetLabel("Enter a key to test.");
        return;
    }
    m_testStatus->SetLabel("Testing the key...");
    m_tester->start(key, [this](bool /*ok*/, const wxString& message) {
        m_testStatus->SetLabel(message);
        Layout();
    });
}

void SettingsDialog::onLocalLoginChanged(wxCommandEvent& /*event*/)
{
    m_connectors->Enable(m_localLogin->GetValue());
}

void SettingsDialog::onOk(wxCommandEvent& /*event*/)
{
    const wxString key = enteredKey();
    if (m_keyStoreOk && key != m_savedKey) {
        wxString error;
        if (!m_settings.setApiKey(key, &error)) {
            wxMessageBox(error, "Preferences", wxOK | wxICON_ERROR, this);
            return;
        }
    }
    m_settings.setModel(m_model->GetValue().Trim(true).Trim(false));
    m_settings.setUseLocalLogin(m_localLogin->GetValue());
    m_settings.setUseClaudeAiConnectors(m_connectors->GetValue());
    m_tester->cancel();
    EndModal(wxID_OK);
}
