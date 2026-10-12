#pragma once

#include <wx/string.h>

#include <memory>

class wxConfigBase;

// The system secret store (wxSecretStore), behind an interface so tests can
// use a fake. Methods mirror wxSecretStore.
class KeyStore
{
public:
    virtual ~KeyStore() = default;

    // False when secrets cannot be stored, for example with no secret service
    // on Linux. Sets *why (when not null) to the reason.
    virtual bool isOk(wxString* why) const = 0;
    virtual bool save(const wxString& service, const wxString& username,
                      const wxString& secret) = 0;
    virtual bool load(const wxString& service, wxString& username, wxString& secret) const = 0;
    virtual bool erase(const wxString& service) = 0;
};

// The key store over wxSecretStore::GetDefault(). When wx was built without
// wxSecretStore, its isOk() is false and says so.
std::unique_ptr<KeyStore> makeSystemKeyStore();

// The app's settings: the API key in the key store, the model and the dev
// login toggle in wxConfig. The key never goes to wxConfig, and there is no
// plain-text fallback when the key store is unavailable.
class Settings
{
public:
    static constexpr const char* kKeyService = "agentic-reader";
    static constexpr const char* kKeyUsername = "anthropic-api-key";

    // Keeps both references. Owns neither.
    Settings(wxConfigBase& config, KeyStore& keys);

    // Empty means the SDK default.
    wxString model() const;
    void setModel(const wxString& model);

    // "Use my local Claude Code login (developer)". Off by default.
    bool useLocalLogin() const;
    void setUseLocalLogin(bool on);

    bool keyStoreAvailable(wxString* why = nullptr) const;

    // The stored key, or empty when none is stored or the store is unavailable.
    wxString apiKey() const;

    // Stores `key` in the key store. An empty key erases the stored key.
    // Returns false and sets *error (when not null) when the store is
    // unavailable or the save fails. Nothing is written anywhere else.
    bool setApiKey(const wxString& key, wxString* error = nullptr);

private:
    wxConfigBase& m_config;
    KeyStore& m_keys;
};
