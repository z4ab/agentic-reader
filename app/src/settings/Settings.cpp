#include "settings/Settings.h"

#include <wx/confbase.h>
#include <wx/secretstore.h>

namespace
{

constexpr const char* kModelEntry = "/Agent/Model";
constexpr const char* kLocalLoginEntry = "/Agent/UseLocalLogin";

#if wxUSE_SECRETSTORE

class SystemKeyStore : public KeyStore
{
public:
    bool isOk(wxString* why) const override
    {
        return m_store.IsOk(why);
    }

    bool save(const wxString& service, const wxString& username,
              const wxString& secret) override
    {
        return m_store.Save(service, username, wxSecretValue(secret));
    }

    bool load(const wxString& service, wxString& username, wxString& secret) const override
    {
        wxSecretValue value;
        if (!m_store.Load(service, username, value))
            return false;
        secret = value.GetAsString();
        return true;
    }

    bool erase(const wxString& service) override
    {
        return m_store.Delete(service);
    }

private:
    wxSecretStore m_store = wxSecretStore::GetDefault();
};

#else

// wx was built without wxSecretStore. Nothing can be stored.
class SystemKeyStore : public KeyStore
{
public:
    bool isOk(wxString* why) const override
    {
        if (why)
            *why = "this build has no secure key storage (wxSecretStore is disabled)";
        return false;
    }

    bool save(const wxString&, const wxString&, const wxString&) override
    {
        return false;
    }

    bool load(const wxString&, wxString&, wxString&) const override
    {
        return false;
    }

    bool erase(const wxString&) override
    {
        return false;
    }
};

#endif

} // namespace

std::unique_ptr<KeyStore> makeSystemKeyStore()
{
    return std::make_unique<SystemKeyStore>();
}

Settings::Settings(wxConfigBase& config, KeyStore& keys) : m_config(config), m_keys(keys) {}

wxString Settings::model() const
{
    return m_config.Read(kModelEntry, wxString());
}

void Settings::setModel(const wxString& model)
{
    m_config.Write(kModelEntry, model);
    m_config.Flush();
}

bool Settings::useLocalLogin() const
{
    return m_config.ReadBool(kLocalLoginEntry, false);
}

void Settings::setUseLocalLogin(bool on)
{
    m_config.Write(kLocalLoginEntry, on);
    m_config.Flush();
}

bool Settings::keyStoreAvailable(wxString* why) const
{
    return m_keys.isOk(why);
}

wxString Settings::apiKey() const
{
    if (!m_keys.isOk(nullptr))
        return wxString();
    wxString username;
    wxString secret;
    if (!m_keys.load(kKeyService, username, secret))
        return wxString();
    return secret;
}

bool Settings::setApiKey(const wxString& key, wxString* error)
{
    wxString why;
    if (!m_keys.isOk(&why)) {
        if (error)
            *error = "The API key cannot be stored securely: " + why;
        return false;
    }

    if (key.empty()) {
        if (apiKey().empty())
            return true;
        if (m_keys.erase(kKeyService))
            return true;
        if (error)
            *error = "The stored API key could not be removed from the system key store.";
        return false;
    }

    if (m_keys.save(kKeyService, kKeyUsername, key))
        return true;
    if (error)
        *error = "The API key could not be saved to the system key store.";
    return false;
}
