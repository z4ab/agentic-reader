// M3.3: spec tests for the find bar's search worker (app/src/pdf/SearchWorker.h).
//
// Test data (tests/data/, reached through the compile definition TEST_DATA_DIR):
//   long.pdf    520 pages written by tests/data/make_long_pdf.py. Page n (1-based)
//               shows "Page n of 520" and 25 lines "Line i of page n. The quick brown
//               fox jumps over the lazy dog."
//   sample.pdf  3 pages. Page index 0: "Page One", "The quick brown fox jumps over the
//               lazy dog. This sample document exists to / test PDF loading, ... Marker /
//               word: zebra." "zebra." is the text box at x 144.348, y 184.633,
//               w 30.567, h 10.616 (top-left origin, points). Page index 1: "Page Two",
//               "Attention is all you need. The second page holds ...". Page index 2:
//               "Page Three", "The third and final page closes the sample document."
//   rotated.pdf sample.pdf with /Rotate 90 on page index 0.
//
// Match semantics: poppler's page::search, which the spec mandates. It matches
// substrings, not whole words ("520" also matches inside "page 520."; "page 7" also
// matches "Page 70 of 520"), across the spaces between words of one line, and
// case_insensitive unless Match case is on. The expected counts are the substring
// counts of pdftotext's output, the automated proxy for "counts match Papers"
// (tasks/VERIFY.md):
//     pdftotext tests/data/long.pdf - | python3 -c 'import sys; t = sys.stdin.read();
//         print(t.lower().count("520"), t.lower().count("page 7"), t.count("Page"))'
//     -> 546 286 520
//     pdftotext tests/data/sample.pdf - | python3 -c 'import sys; t = sys.stdin.read();
//         print(t.lower().count("the"), t.count("the"))'
//     -> 5 2
// The three test words for the task's evidence are "520" (546), "page 7" (286) and
// "Page" with Match case (520), all on long.pdf. poppler-cpp 24.02 gives the same counts
// with a page::search loop. The words are chosen to have few matches per page: each
// page::search call re-extracts the page's text (about 2 ms on long.pdf), so a word with
// 25 matches on every page (e.g. "fox", 13000 in total) takes about 30 s to count.
//
// Interface choices where the spec is silent (global namespace, pdf/SearchWorker.h):
//
//   struct SearchResult
//   {
//       unsigned generation = 0;      // the generation passed to start()
//       int page = -1;                // 0-based; -1 on the final (done) result
//       std::vector<RectPts> matches; // this page's matches in poppler's order
//       bool done = false;            // true only on the final result
//   };
//   wxDECLARE_EVENT(EVT_SEARCH_RESULT, wxThreadEvent);  // payload: SearchResult
//
//   class SearchWorker
//   {
//   public:
//       SearchWorker(wxEvtHandler* sink, std::filesystem::path path,
//                    std::string password = {});
//       ~SearchWorker();
//       void start(const std::string& query, bool matchCase, unsigned generation);
//       void cancel();
//   };
//
//   // Pure helpers for the find bar.
//   int stepMatch(int current, int count, bool forward);
//   std::string matchCountLabel(int current, int total);
//
//   - SearchWorker owns a thread and its own PdfDocument opened from `path` (like
//     RenderWorker), and queues each SearchResult to `sink` from that thread with
//     wxQueueEvent. Non-copyable. The destructor cancels and joins promptly (well under
//     a second), so `sink` must outlive the worker.
//   - start() cancels the running search and searches every page from page 0 upward for
//     `query` (UTF-8): page::search with search_from_top, then search_next_result in a
//     loop until it returns false. matchCase false is poppler::case_insensitive.
//   - One SearchResult per page that has at least one match, in ascending page order,
//     posted as soon as that page is done (streamed). Pages without a match post
//     nothing. After the last page, one result with done = true, page = -1 and no
//     matches. An empty query posts only that done result. When the document cannot be
//     opened, every search posts only the done result.
//   - Rects are in page points with the origin at the top-left of the page as displayed,
//     the convention of TextBox::bbox, so PdfView maps both with PageLayout::pageToView.
//     poppler's search rect already uses it: x = left, y = top, w = right - left,
//     h = bottom - top.
//   - Cancellation is exact: once start() or cancel() returns, no result of an earlier
//     generation is queued any more, done included. (The worker checks the generation
//     and queues the event under its lock, which start() and cancel() also take.) A
//     cancelled search never posts its done result.
//   - stepMatch walks `count` matches in document order with wrap-around: forward from
//     count - 1 gives 0, backward from 0 gives count - 1. current = -1 (no current match)
//     gives 0 forward and count - 1 backward. count 0 gives -1.
//   - matchCountLabel(current, total) is the find bar's count text for a 0-based current
//     match: matchCountLabel(2, 41) == "3 of 41". The text for current = -1 or total = 0
//     is left to the implementation.
//   - The test sink overrides wxEvtHandler::QueueEvent (as in test_text_layer.cpp), so
//     the tests need no wxApp and no event loop.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PageLayout.h"
#include "pdf/PdfDocument.h"
#include "pdf/SearchWorker.h"

#include <wx/event.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path dataFile(const char* name)
{
    return std::filesystem::path(TEST_DATA_DIR) / name;
}

double msSince(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

bool near(double a, double b, double tol)
{
    return std::abs(a - b) <= tol + 1e-9;
}

// One entry of the sink's log: a result the worker queued, or a marker the test wrote
// right after start() or cancel() returned.
struct LogEntry
{
    std::optional<SearchResult> result;
    Clock::time_point at;
    std::thread::id thread;
};

// Collects SearchResults that a SearchWorker queues from its thread.
class CaptureSink : public wxEvtHandler
{
public:
    void QueueEvent(wxEvent* event) override
    {
        const std::unique_ptr<wxEvent> owned(event);
        auto* threadEvent = dynamic_cast<wxThreadEvent*>(event);
        if (!threadEvent || threadEvent->GetEventType() != EVT_SEARCH_RESULT)
            return;
        std::lock_guard lock(m_mutex);
        m_log.push_back({threadEvent->GetPayload<SearchResult>(), Clock::now(),
                         std::this_thread::get_id()});
        m_cv.notify_all();
    }

    void mark()
    {
        std::lock_guard lock(m_mutex);
        m_log.push_back({std::nullopt, Clock::now(), std::this_thread::get_id()});
    }

    // Waits until pred(log) holds or `timeout` passes. Returns the log either way.
    std::vector<LogEntry> waitUntil(const std::function<bool(const std::vector<LogEntry>&)>& pred,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(120))
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, timeout, [&] { return pred(m_log); });
        return m_log;
    }

    std::vector<LogEntry> log()
    {
        std::lock_guard lock(m_mutex);
        return m_log;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<LogEntry> m_log;
};

bool isResult(const LogEntry& e, unsigned generation)
{
    return e.result && e.result->generation == generation;
}

bool hasDone(const std::vector<LogEntry>& log, unsigned generation)
{
    return std::any_of(log.begin(), log.end(), [&](const LogEntry& e) {
        return isResult(e, generation) && e.result->done;
    });
}

bool hasPageResult(const std::vector<LogEntry>& log, unsigned generation)
{
    return std::any_of(log.begin(), log.end(), [&](const LogEntry& e) {
        return isResult(e, generation) && !e.result->done;
    });
}

// The results of one generation in arrival order.
std::vector<SearchResult> resultsOf(const std::vector<LogEntry>& log, unsigned generation)
{
    std::vector<SearchResult> out;
    for (const LogEntry& e : log)
        if (isResult(e, generation))
            out.push_back(*e.result);
    return out;
}

int totalMatches(const std::vector<SearchResult>& results)
{
    int n = 0;
    for (const SearchResult& r : results)
        n += static_cast<int>(r.matches.size());
    return n;
}

// Checks the shape of a finished search: page results in strictly ascending page
// order, each with at least one match and done false, then exactly one done result
// (page -1, no matches) as the last entry.
void checkWellFormed(const std::vector<SearchResult>& results, int pageCount)
{
    REQUIRE_FALSE(results.empty());
    const SearchResult& last = results.back();
    CHECK(last.done);
    CHECK(last.page == -1);
    CHECK(last.matches.empty());
    int previous = -1;
    for (std::size_t i = 0; i + 1 < results.size(); ++i) {
        CAPTURE(i);
        const SearchResult& r = results[i];
        CHECK_FALSE(r.done);
        CHECK(r.page > previous);
        CHECK(r.page < pageCount);
        CHECK_FALSE(r.matches.empty());
        previous = r.page;
    }
}

// Runs one search to its done result and returns its results.
std::vector<SearchResult> searchAll(const char* file, const std::string& query, bool matchCase)
{
    CaptureSink sink;
    std::vector<LogEntry> log;
    {
        SearchWorker worker(&sink, dataFile(file));
        worker.start(query, matchCase, 1);
        log = sink.waitUntil([](const auto& l) { return hasDone(l, 1); });
    }
    REQUIRE_MESSAGE(hasDone(log, 1), "search for '" << query << "' did not finish");
    return resultsOf(log, 1);
}

} // namespace

// ---------------------------------------------------------------------------
// Spec 2: search on a worker thread, streamed per page

TEST_CASE("find results arrive per page in order from the worker thread")
{
    CaptureSink sink;
    std::vector<LogEntry> log;
    Clock::time_point started;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        started = Clock::now();
        worker.start("520", false, 7);
        log = sink.waitUntil([](const auto& l) { return hasDone(l, 7); });
    }
    const std::vector<SearchResult> results = resultsOf(log, 7);
    REQUIRE(hasDone(log, 7));

    // Every page shows "Page n of 520"; page 520 (index 519) adds "Page 520" and
    // "of page 520." on each of its 25 lines.
    checkWellFormed(results, 520);
    REQUIRE(results.size() == 521);
    for (int page = 0; page < 520; ++page) {
        CAPTURE(page);
        CHECK(results[page].page == page);
        CHECK(results[page].generation == 7);
        CHECK(results[page].matches.size() == (page == 519 ? 27u : 1u));
    }
    CHECK(totalMatches(results) == 546);

    // Queued from the worker thread, never from the test thread.
    for (const LogEntry& e : log)
        CHECK(e.thread != std::this_thread::get_id());

    // Streamed: the first page arrived long before the whole document was searched,
    // not in one batch at the end.
    Clock::time_point firstAt;
    Clock::time_point doneAt;
    bool sawFirst = false;
    for (const LogEntry& e : log) {
        if (!isResult(e, 7))
            continue;
        if (!sawFirst) {
            firstAt = e.at;
            sawFirst = true;
        }
        if (e.result->done)
            doneAt = e.at;
    }
    CHECK(msSince(started, firstAt) < msSince(started, doneAt) / 2);
}

TEST_CASE("find match rects are in page points from the top-left like text boxes")
{
    SUBCASE("sample.pdf zebra")
    {
        const auto results = searchAll("sample.pdf", "zebra", false);
        checkWellFormed(results, 3);
        REQUIRE(results.size() == 2);
        REQUIRE(results[0].page == 0);
        REQUIRE(results[0].matches.size() == 1);
        const RectPts& r = results[0].matches[0];
        // Starts where the box "zebra." starts, same line, a little narrower (no ".").
        CHECK(near(r.x, 144.348, 0.5));
        CHECK(near(r.y, 184.633, 0.5));
        CHECK(near(r.h, 10.616, 0.5));
        CHECK(r.w > 20.0);
        CHECK(r.w < 30.567);
    }
    SUBCASE("rotated.pdf zebra lies inside the rotated text box")
    {
        std::string error;
        auto doc = PdfDocument::open(dataFile("rotated.pdf"), {}, error);
        REQUIRE_MESSAGE(doc != nullptr, "open failed: " << error);
        std::optional<RectPts> box;
        for (const TextBox& b : doc->textBoxes(0))
            if (b.text == "zebra.")
                box = b.bbox;
        REQUIRE(box.has_value());

        const auto results = searchAll("rotated.pdf", "zebra", false);
        REQUIRE(results.size() == 2);
        REQUIRE(results[0].page == 0);
        REQUIRE(results[0].matches.size() == 1);
        const RectPts& r = results[0].matches[0];
        const double cx = r.x + r.w / 2;
        const double cy = r.y + r.h / 2;
        CHECK(cx > box->x);
        CHECK(cx < box->x + box->w);
        CHECK(cy > box->y);
        CHECK(cy < box->y + box->h);
    }
}

TEST_CASE("find matches within a page come top to bottom")
{
    CaptureSink sink;
    std::vector<LogEntry> log;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        worker.start("fox", false, 1);
        log = sink.waitUntil([](const auto& l) { return hasPageResult(l, 1); });
    }
    const auto results = resultsOf(log, 1);
    REQUIRE_FALSE(results.empty());
    const SearchResult& first = results.front();
    CHECK(first.page == 0);
    REQUIRE(first.matches.size() == 25);
    for (std::size_t i = 1; i < first.matches.size(); ++i) {
        CAPTURE(i);
        CHECK(first.matches[i].y > first.matches[i - 1].y);
    }
}

TEST_CASE("find Match case off and on")
{
    SUBCASE("sample.pdf the")
    {
        const auto insensitive = searchAll("sample.pdf", "the", false);
        checkWellFormed(insensitive, 3);
        // "The quick", "the lazy" | "The second" | "The third", "the sample".
        REQUIRE(insensitive.size() == 4);
        CHECK(insensitive[0].page == 0);
        CHECK(insensitive[0].matches.size() == 2);
        CHECK(insensitive[1].page == 1);
        CHECK(insensitive[1].matches.size() == 1);
        CHECK(insensitive[2].page == 2);
        CHECK(insensitive[2].matches.size() == 2);
        CHECK(totalMatches(insensitive) == 5);

        const auto sensitive = searchAll("sample.pdf", "the", true);
        checkWellFormed(sensitive, 3);
        // Only "the lazy" and "the sample".
        REQUIRE(sensitive.size() == 3);
        CHECK(sensitive[0].page == 0);
        CHECK(sensitive[0].matches.size() == 1);
        CHECK(sensitive[1].page == 2);
        CHECK(sensitive[1].matches.size() == 1);
        CHECK(totalMatches(sensitive) == 2);
    }
    SUBCASE("query case does not matter when Match case is off")
    {
        CHECK(totalMatches(searchAll("sample.pdf", "THE", false)) == 5);
        CHECK(totalMatches(searchAll("sample.pdf", "THE", true)) == 0);
    }
}

TEST_CASE("find posts only done for no match, an empty query or a missing file")
{
    SUBCASE("no match")
    {
        const auto results = searchAll("sample.pdf", "xylophone", false);
        REQUIRE(results.size() == 1);
        CHECK(results[0].done);
        CHECK(results[0].page == -1);
    }
    SUBCASE("empty query")
    {
        const auto results = searchAll("sample.pdf", "", false);
        REQUIRE(results.size() == 1);
        CHECK(results[0].done);
    }
    SUBCASE("document cannot be opened")
    {
        const auto results = searchAll("no-such-file.pdf", "the", false);
        REQUIRE(results.size() == 1);
        CHECK(results[0].done);
    }
}

// ---------------------------------------------------------------------------
// Done when 2: counts match Papers (pdftotext as the proxy) for three test words

TEST_CASE("find counts on long.pdf equal pdftotext counts for three words")
{
    SUBCASE("520")
    {
        CHECK(totalMatches(searchAll("long.pdf", "520", false)) == 546);
    }
    SUBCASE("page 7")
    {
        // 26 on page 7 ("Page 7 of 520" and "of page 7." on its 25 lines) and 26 on
        // each of pages 70..79 ("Page 70 of 520", "of page 70."), as substrings.
        const auto results = searchAll("long.pdf", "page 7", false);
        checkWellFormed(results, 520);
        CHECK(totalMatches(results) == 286);
        CHECK(results.front().page == 6);
    }
    SUBCASE("Page with Match case")
    {
        // Only the heading "Page n of 520" of each page, not "of page n." in the lines.
        const auto results = searchAll("long.pdf", "Page", true);
        checkWellFormed(results, 520);
        CHECK(results.size() == 521);
        CHECK(totalMatches(results) == 520);
    }
}

// ---------------------------------------------------------------------------
// Spec 2: a new query cancels the running search

TEST_CASE("find a new query cancels the running one")
{
    CaptureSink sink;
    std::vector<LogEntry> log;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        // "fox" has 13000 matches and takes many seconds to finish.
        worker.start("fox", false, 1);
        sink.waitUntil([](const auto& l) { return hasPageResult(l, 1); });
        worker.start("Page", true, 2);
        sink.mark();
        log = sink.waitUntil([](const auto& l) { return hasDone(l, 2); });
        // Give a leaked old result time to show up.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        log = sink.log();
    }

    const auto marker = std::find_if(log.begin(), log.end(),
                                     [](const LogEntry& e) { return !e.result; });
    REQUIRE(marker != log.end());
    for (auto it = marker + 1; it != log.end(); ++it) {
        REQUIRE(it->result);
        CHECK(it->result->generation == 2);
    }

    // The old search never finished, and the new one is complete and correct.
    CHECK_FALSE(hasDone(log, 1));
    CHECK(totalMatches(resultsOf(log, 1)) < 13000);
    const auto fresh = resultsOf(log, 2);
    checkWellFormed(fresh, 520);
    CHECK(fresh.size() == 521);
    CHECK(totalMatches(fresh) == 520);
}

TEST_CASE("find cancel stops the search and posts nothing more")
{
    CaptureSink sink;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        worker.start("fox", false, 3);
        sink.waitUntil([](const auto& l) { return hasPageResult(l, 3); });
        worker.cancel();
        sink.mark();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const auto log = sink.log();
    REQUIRE_FALSE(log.empty());
    CHECK_FALSE(log.back().result.has_value()); // the marker is still the last entry
    CHECK_FALSE(hasDone(log, 3));
}

TEST_CASE("find worker can search again after a search finished")
{
    CaptureSink sink;
    std::vector<LogEntry> log;
    {
        SearchWorker worker(&sink, dataFile("sample.pdf"));
        worker.start("zebra", false, 1);
        sink.waitUntil([](const auto& l) { return hasDone(l, 1); });
        worker.start("the", true, 2);
        log = sink.waitUntil([](const auto& l) { return hasDone(l, 2); });
    }
    CHECK(totalMatches(resultsOf(log, 1)) == 1);
    CHECK(totalMatches(resultsOf(log, 2)) == 2);
}

TEST_CASE("find destructor stops a running search promptly")
{
    CaptureSink sink;
    Clock::time_point destroying;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        worker.start("the", false, 1);
        sink.waitUntil([](const auto& l) { return hasPageResult(l, 1); });
        destroying = Clock::now();
    }
    CHECK(msSince(destroying, Clock::now()) < 1000.0);
    CHECK_FALSE(hasDone(sink.log(), 1));
}

// ---------------------------------------------------------------------------
// Done when 1: the first result of a common word in a 500-page PDF in under 300 ms

TEST_CASE("find first result for a common word on long.pdf in under 300 ms")
{
    // Measured from start() to the arrival of the first page result (page 0, its 25
    // "fox" matches, about 70 ms on an idle 4-core box). The worker is created first, so
    // opening the document on its thread may count against the first attempt. The best
    // of three attempts must meet the spec's bound, which absorbs a busy CI box without
    // loosening the bound itself.
    CaptureSink sink;
    double best = 1e9;
    {
        SearchWorker worker(&sink, dataFile("long.pdf"));
        for (unsigned generation = 1; generation <= 3; ++generation) {
            const Clock::time_point started = Clock::now();
            worker.start("fox", false, generation);
            const auto log = sink.waitUntil(
                [generation](const auto& l) { return hasPageResult(l, generation); },
                std::chrono::seconds(10));
            for (const LogEntry& e : log) {
                if (isResult(e, generation) && !e.result->done) {
                    CHECK(e.result->page == 0);
                    best = std::min(best, msSince(started, e.at));
                    break;
                }
            }
        }
    }
    MESSAGE("best time to first result: " << best << " ms");
    CHECK(best < 300.0);
}

// ---------------------------------------------------------------------------
// Spec 4 and 5: wrap-around stepping and the count text

TEST_CASE("find stepMatch wraps at both ends")
{
    CHECK(stepMatch(0, 5, true) == 1);
    CHECK(stepMatch(3, 5, true) == 4);
    CHECK(stepMatch(4, 5, true) == 0);
    CHECK(stepMatch(4, 5, false) == 3);
    CHECK(stepMatch(1, 5, false) == 0);
    CHECK(stepMatch(0, 5, false) == 4);

    // A single match stays current.
    CHECK(stepMatch(0, 1, true) == 0);
    CHECK(stepMatch(0, 1, false) == 0);

    // No current match yet.
    CHECK(stepMatch(-1, 5, true) == 0);
    CHECK(stepMatch(-1, 5, false) == 4);

    // No matches.
    CHECK(stepMatch(-1, 0, true) == -1);
    CHECK(stepMatch(-1, 0, false) == -1);
}

TEST_CASE("find stepMatch visits every match once per round in document order")
{
    const int count = 7;
    int current = -1;
    for (int i = 0; i < 2 * count; ++i) {
        current = stepMatch(current, count, true);
        CHECK(current == i % count);
    }
    current = -1;
    for (int i = 0; i < 2 * count; ++i) {
        current = stepMatch(current, count, false);
        CHECK(current == count - 1 - i % count);
    }
}

TEST_CASE("find matchCountLabel reads '3 of 41'")
{
    CHECK(matchCountLabel(2, 41) == "3 of 41");
    CHECK(matchCountLabel(0, 1) == "1 of 1");
    CHECK(matchCountLabel(545, 546) == "546 of 546");
    CHECK(matchCountLabel(0, 13000) == "1 of 13000");
}
