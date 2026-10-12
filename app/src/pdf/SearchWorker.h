#pragma once

#include "pdf/PageLayout.h"

#include <wx/event.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Payload of EVT_SEARCH_RESULT: the matches of one page, or the end of a search.
struct SearchResult
{
    // The generation passed to SearchWorker::start().
    unsigned generation = 0;
    // 0-based page, -1 on the final (done) result.
    int page = -1;
    // This page's matches in poppler's order, in page points from the top-left
    // of the page as displayed (the convention of TextBox::bbox).
    std::vector<RectPts> matches;
    // True only on the final result, which carries no matches.
    bool done = false;
};

// wxThreadEvent whose payload is a SearchResult.
wxDECLARE_EVENT(EVT_SEARCH_RESULT, wxThreadEvent);

// Searches a document on its own thread with its own PdfDocument opened from
// `path` with `password` (empty for an unencrypted file), and queues the
// results to `sink` as EVT_SEARCH_RESULT: one per page with a match, in page
// order, as soon as that page is searched, then one done result. When the
// document cannot be opened, or the query is empty, a search posts only the
// done result. The destructor stops the thread and joins it, so `sink` must
// outlive this object.
class SearchWorker
{
public:
    SearchWorker(wxEvtHandler* sink, std::filesystem::path path, std::string password = {});
    ~SearchWorker();

    SearchWorker(const SearchWorker&) = delete;
    SearchWorker& operator=(const SearchWorker&) = delete;

    // Cancels the running search and searches every page for `query` (UTF-8),
    // case-insensitive unless `matchCase`. Once it returns, no result of an
    // earlier search is queued any more.
    void start(const std::string& query, bool matchCase, unsigned generation);
    // Cancels the running search. Once it returns, no result of it is queued
    // any more, its done result included.
    void cancel();

private:
    struct Job
    {
        std::string query;
        bool matchCase = false;
        unsigned generation = 0;
        // Value of m_serial when the job was started.
        unsigned serial = 0;
    };

    void run();
    // Queues `result` unless the job was cancelled or replaced. False when it was.
    bool post(const Job& job, SearchResult result);

    wxEvtHandler* m_sink;
    std::filesystem::path m_path;
    std::string m_password;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Job> m_pending;
    // Bumped under m_mutex by every start(), cancel() and the destructor. A job
    // whose serial differs is cancelled. Atomic so the search loop can poll it
    // without the lock.
    std::atomic<unsigned> m_serial{0};
    bool m_stop = false;

    // Declared last so it starts after the members above exist.
    std::thread m_thread;
};

// The match after (`forward`) or before `current` among `count` matches in
// document order, wrapping at both ends. With no current match (-1), the first
// or the last one. -1 when there are no matches.
int stepMatch(int current, int count, bool forward);

// The find bar's count text for the 0-based `current` of `total` matches:
// matchCountLabel(2, 41) == "3 of 41". "" when there are no matches, and
// "0 of <total>" with no current match.
std::string matchCountLabel(int current, int total);
