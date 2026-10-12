#include "pdf/RenderWorker.h"

#include "pdf/PdfDocument.h"

#include <string>
#include <utility>

wxDEFINE_EVENT(EVT_PAGE_RENDERED, wxThreadEvent);

namespace {

bool sameRequest(const RenderRequest& a, const RenderRequest& b)
{
    return a.page == b.page && a.scale == b.scale && a.generation == b.generation;
}

} // namespace

RenderWorker::RenderWorker(wxEvtHandler* sink, std::filesystem::path path, std::string password)
    : m_sink(sink)
    , m_path(std::move(path))
    , m_password(std::move(password))
    , m_thread(&RenderWorker::run, this)
{
}

RenderWorker::~RenderWorker()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_wake.notify_one();
    m_thread.join();
}

void RenderWorker::setRequests(std::vector<RenderRequest> requests)
{
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        for (const RenderRequest& r : requests) {
            if (!m_current || !sameRequest(*m_current, r))
                m_queue.push_back(r);
        }
    }
    m_wake.notify_one();
}

void RenderWorker::clear()
{
    std::lock_guard lock(m_mutex);
    m_queue.clear();
}

void RenderWorker::run()
{
    // Opened here so a large file does not stall the UI thread.
    std::string error;
    const std::unique_ptr<PdfDocument> doc = PdfDocument::open(m_path, m_password, error);

    std::unique_lock lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_stop)
            return;

        const RenderRequest req = m_queue.front();
        m_queue.pop_front();
        m_current = req;
        lock.unlock();

        auto* event = new wxThreadEvent(EVT_PAGE_RENDERED);
        // The temporary RenderResult dies at the end of this statement, so the
        // event holds the only reference to the image before it crosses threads.
        event->SetPayload(RenderResult{req.page, req.scale, req.generation,
                                       doc ? doc->renderPage(req.page, req.scale) : wxImage()});
        wxQueueEvent(m_sink, event);

        lock.lock();
        m_current.reset();
    }
}
