#include "pdf/SearchWorker.h"

#include "pdf/PdfDocument.h"

#include <memory>
#include <utility>

wxDEFINE_EVENT(EVT_SEARCH_RESULT, wxThreadEvent);

SearchWorker::SearchWorker(wxEvtHandler* sink, std::filesystem::path path, std::string password)
    : m_sink(sink)
    , m_path(std::move(path))
    , m_password(std::move(password))
    , m_thread(&SearchWorker::run, this)
{
}

SearchWorker::~SearchWorker()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_pending.reset();
        ++m_serial;
    }
    m_wake.notify_one();
    m_thread.join();
}

void SearchWorker::start(const std::string& query, bool matchCase, unsigned generation)
{
    {
        std::lock_guard lock(m_mutex);
        const unsigned serial = ++m_serial;
        m_pending = Job{query, matchCase, generation, serial};
    }
    m_wake.notify_one();
}

void SearchWorker::cancel()
{
    std::lock_guard lock(m_mutex);
    m_pending.reset();
    ++m_serial;
}

bool SearchWorker::post(const Job& job, SearchResult result)
{
    // Checked and queued under the lock that start() and cancel() take, so a
    // result never slips out after the call that cancelled its job returned.
    std::lock_guard lock(m_mutex);
    if (m_serial != job.serial)
        return false;
    auto* event = new wxThreadEvent(EVT_SEARCH_RESULT);
    event->SetPayload(std::move(result));
    wxQueueEvent(m_sink, event);
    return true;
}

void SearchWorker::run()
{
    // Opened here so a large file does not stall the UI thread.
    std::string error;
    const std::unique_ptr<PdfDocument> doc = PdfDocument::open(m_path, m_password, error);
    const int pageCount = doc ? doc->pageCount() : 0;

    std::unique_lock lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stop || m_pending.has_value(); });
        if (m_stop)
            return;
        const Job job = std::move(*m_pending);
        m_pending.reset();
        lock.unlock();

        const auto cancelled = [this, &job] { return m_serial != job.serial; };
        bool finished = true;
        if (!job.query.empty()) {
            for (int page = 0; page < pageCount; ++page) {
                std::vector<RectPts> matches = doc->search(page, job.query, job.matchCase, cancelled);
                if (cancelled()) {
                    finished = false;
                    break;
                }
                if (!matches.empty() &&
                    !post(job, {job.generation, page, std::move(matches), false})) {
                    finished = false;
                    break;
                }
            }
        }
        if (finished)
            post(job, {job.generation, -1, {}, true});

        lock.lock();
    }
}

int stepMatch(int current, int count, bool forward)
{
    if (count <= 0)
        return -1;
    if (current < 0 || current >= count)
        return forward ? 0 : count - 1;
    return forward ? (current + 1) % count : (current + count - 1) % count;
}

std::string matchCountLabel(int current, int total)
{
    if (total <= 0)
        return {};
    return std::to_string(current < 0 ? 0 : current + 1) + " of " + std::to_string(total);
}
