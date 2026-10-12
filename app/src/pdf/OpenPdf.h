#pragma once

#include "pdf/PdfDocument.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Asked when the document needs a password. `wrongPassword` is false on the
// first ask and true after a rejected password. Returns std::nullopt when the
// user cancels.
using PasswordPrompt = std::function<std::optional<std::string>(bool wrongPassword)>;

struct OpenPdfResult
{
    std::unique_ptr<PdfDocument> doc; // null on failure or cancel
    std::string password;             // the password that opened doc, else empty
    std::string error;                // PdfDocument::open's error on failure
    bool cancelled = false;           // the user cancelled the password prompt
};

// Opens `path`, first with an empty password. While the document reports
// "password required" it asks `askPassword` and retries with the answer, with
// no limit on attempts. Any other error returns at once without asking.
OpenPdfResult openPdf(const std::filesystem::path& path, const PasswordPrompt& askPassword);
