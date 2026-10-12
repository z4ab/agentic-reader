// M2.5: spec tests for opening files without the GUI.
//
// Test data (tests/data/, reached through the compile definition TEST_DATA_DIR):
//   sample.pdf     3 pages, not encrypted.
//   locked.pdf     sample.pdf encrypted with AES-256, user password "secret",
//                  owner password "ownerpw" (see test_pdf_document.cpp).
//   not-a-pdf.pdf  38 bytes of plain text with a .pdf extension.
//
// Interface choices where the spec is silent:
//   - The password loop of spec item 4 lives in a GUI-free helper so it can be
//     tested here. MainFrame calls it with a prompt that shows
//     wxPasswordEntryDialog. Declared in app/src/pdf/OpenPdf.h:
//
//         // Asked when the document needs a password. `wrongPassword` is false
//         // on the first ask and true after a rejected password. Returns
//         // std::nullopt when the user cancels.
//         using PasswordPrompt =
//             std::function<std::optional<std::string>(bool wrongPassword)>;
//
//         struct OpenPdfResult
//         {
//             std::unique_ptr<PdfDocument> doc; // null on failure or cancel
//             std::string password;  // the password that opened doc, else empty
//             std::string error;     // PdfDocument::open's error on failure
//             bool cancelled = false; // the user cancelled the password prompt
//         };
//
//         OpenPdfResult openPdf(const std::filesystem::path& path,
//                               const PasswordPrompt& askPassword);
//
//     openPdf() first tries an empty password. While PdfDocument::open reports
//     "password required" it asks again and retries with the answer, with no
//     limit on attempts. It asks only for a locked document: a missing file or
//     a non-PDF fails at once with PdfDocument::open's error ("file not found",
//     "not a PDF") and the prompt is never called. An empty answer counts as a
//     wrong password. On cancel, doc is null and cancelled is true; error is
//     not checked then. On success, cancelled is false.
//   - Carried from M2.3: RenderWorker takes the password and opens its own
//     PdfDocument with it. The constructor becomes
//         RenderWorker(wxEvtHandler* sink, std::filesystem::path path,
//                      std::string password = {});
//     With a wrong or missing password the worker keeps today's behaviour:
//     every result carries an invalid image.
//   - The test sink overrides wxEvtHandler::QueueEvent, which wxQueueEvent
//     calls, so the tests need no wxApp and no event loop.
//   - Recent files (spec item 6) are wxFileHistory saved to wxConfig. That is
//     wx code with no app interface to test here, so it is covered by the
//     restart run in tests/acceptance/m2.5.sh instead.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/OpenPdf.h"
#include "pdf/PdfDocument.h"
#include "pdf/RenderWorker.h"

#include <wx/event.h>
#include <wx/image.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

std::filesystem::path dataFile(const char* name)
{
    return std::filesystem::path(TEST_DATA_DIR) / name;
}

// Answers prompts from a script and records every call.
struct ScriptedPrompt
{
    std::vector<std::optional<std::string>> answers;
    std::vector<bool> calls; // the wrongPassword argument of each call

    PasswordPrompt fn()
    {
        return [this](bool wrongPassword) -> std::optional<std::string> {
            calls.push_back(wrongPassword);
            REQUIRE(calls.size() <= answers.size());
            return answers[calls.size() - 1];
        };
    }
};

// Collects RenderResults that a RenderWorker posts from its thread.
class CaptureSink : public wxEvtHandler
{
public:
    void QueueEvent(wxEvent* event) override
    {
        const std::unique_ptr<wxEvent> owned(event);
        auto* threadEvent = dynamic_cast<wxThreadEvent*>(event);
        if (!threadEvent || threadEvent->GetEventType() != EVT_PAGE_RENDERED)
            return;
        std::lock_guard lock(m_mutex);
        m_results.push_back(threadEvent->GetPayload<RenderResult>());
        m_cv.notify_all();
    }

    // Waits until `count` results arrived. Returns them, or fewer on timeout.
    std::vector<RenderResult> waitFor(std::size_t count)
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::seconds(20),
                      [&] { return m_results.size() >= count; });
        return m_results;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<RenderResult> m_results;
};

RenderResult renderFirstPage(const char* name, const std::string& password)
{
    CaptureSink sink;
    std::vector<RenderResult> results;
    {
        RenderWorker worker(&sink, dataFile(name), password);
        worker.setRequests({RenderRequest{0, 0.5, 1}});
        results = sink.waitFor(1);
    }
    REQUIRE(results.size() == 1);
    CHECK(results[0].page == 0);
    CHECK(results[0].generation == 1);
    return results[0];
}

} // namespace

TEST_CASE("open_files opens an unencrypted PDF without asking for a password")
{
    ScriptedPrompt prompt;
    const OpenPdfResult r = openPdf(dataFile("sample.pdf"), prompt.fn());
    REQUIRE(r.doc != nullptr);
    CHECK(r.doc->pageCount() == 3);
    CHECK(r.password.empty());
    CHECK_FALSE(r.cancelled);
    CHECK(prompt.calls.empty());
}

TEST_CASE("open_files reports a missing file without asking for a password")
{
    ScriptedPrompt prompt;
    const OpenPdfResult r = openPdf(dataFile("no-such-file.pdf"), prompt.fn());
    CHECK(r.doc == nullptr);
    CHECK(r.error == "file not found");
    CHECK_FALSE(r.cancelled);
    CHECK(prompt.calls.empty());
}

TEST_CASE("open_files reports a renamed text file as not a PDF")
{
    ScriptedPrompt prompt;
    const OpenPdfResult r = openPdf(dataFile("not-a-pdf.pdf"), prompt.fn());
    CHECK(r.doc == nullptr);
    CHECK(r.error == "not a PDF");
    CHECK_FALSE(r.cancelled);
    CHECK(prompt.calls.empty());
}

TEST_CASE("open_files opens an encrypted PDF with the right password")
{
    ScriptedPrompt prompt{{std::string("secret")}, {}};
    const OpenPdfResult r = openPdf(dataFile("locked.pdf"), prompt.fn());
    REQUIRE(r.doc != nullptr);
    CHECK(r.doc->pageCount() == 3);
    CHECK(r.password == "secret");
    CHECK_FALSE(r.cancelled);
    CHECK(prompt.calls == std::vector<bool>{false});
}

TEST_CASE("open_files accepts the owner password")
{
    ScriptedPrompt prompt{{std::string("ownerpw")}, {}};
    const OpenPdfResult r = openPdf(dataFile("locked.pdf"), prompt.fn());
    REQUIRE(r.doc != nullptr);
    CHECK(r.password == "ownerpw");
}

TEST_CASE("open_files asks again after a wrong password")
{
    ScriptedPrompt prompt{{std::string("wrong"), std::string(""), std::string("secret")}, {}};
    const OpenPdfResult r = openPdf(dataFile("locked.pdf"), prompt.fn());
    REQUIRE(r.doc != nullptr);
    CHECK(r.password == "secret");
    CHECK_FALSE(r.cancelled);
    CHECK(prompt.calls == std::vector<bool>{false, true, true});
}

TEST_CASE("open_files stops when the password prompt is cancelled")
{
    ScriptedPrompt prompt{{std::nullopt}, {}};
    const OpenPdfResult r = openPdf(dataFile("locked.pdf"), prompt.fn());
    CHECK(r.doc == nullptr);
    CHECK(r.cancelled);
    CHECK(prompt.calls == std::vector<bool>{false});
}

TEST_CASE("open_files stops when the prompt is cancelled after a wrong password")
{
    ScriptedPrompt prompt{{std::string("wrong"), std::nullopt}, {}};
    const OpenPdfResult r = openPdf(dataFile("locked.pdf"), prompt.fn());
    CHECK(r.doc == nullptr);
    CHECK(r.cancelled);
    CHECK(prompt.calls == std::vector<bool>{false, true});
}

TEST_CASE("open_files RenderWorker renders an encrypted PDF with the password")
{
    const RenderResult r = renderFirstPage("locked.pdf", "secret");
    REQUIRE(r.image.IsOk());
    // Page 0 is 612 x 792 pt, so ceil(306) x ceil(396) at scale 0.5.
    CHECK(r.image.GetWidth() == 306);
    CHECK(r.image.GetHeight() == 396);
}

TEST_CASE("open_files RenderWorker matches an unencrypted render of the same page")
{
    const RenderResult locked = renderFirstPage("locked.pdf", "secret");
    const RenderResult plain = renderFirstPage("sample.pdf", "");
    REQUIRE(locked.image.IsOk());
    REQUIRE(plain.image.IsOk());
    REQUIRE(locked.image.GetWidth() == plain.image.GetWidth());
    REQUIRE(locked.image.GetHeight() == plain.image.GetHeight());
    const std::size_t bytes =
        static_cast<std::size_t>(plain.image.GetWidth()) * plain.image.GetHeight() * 3;
    CHECK(std::equal(plain.image.GetData(), plain.image.GetData() + bytes,
                     locked.image.GetData()));
}

TEST_CASE("open_files RenderWorker without the password posts invalid images")
{
    CHECK_FALSE(renderFirstPage("locked.pdf", "").image.IsOk());
    CHECK_FALSE(renderFirstPage("locked.pdf", "wrong").image.IsOk());
}
