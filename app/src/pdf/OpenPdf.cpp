#include "pdf/OpenPdf.h"

#include <utility>

namespace {
// PdfDocument::open's error for a locked document or a wrong password.
constexpr const char* kPasswordRequired = "password required";
} // namespace

OpenPdfResult openPdf(const std::filesystem::path& path, const PasswordPrompt& askPassword)
{
    OpenPdfResult result;
    std::string password;
    bool asked = false;
    for (;;) {
        result.doc = PdfDocument::open(path, password, result.error);
        if (result.doc) {
            result.password = std::move(password);
            result.error.clear();
            return result;
        }
        if (result.error != kPasswordRequired)
            return result;

        std::optional<std::string> answer = askPassword(asked);
        asked = true;
        if (!answer) {
            result.cancelled = true;
            return result;
        }
        password = std::move(*answer);
    }
}
