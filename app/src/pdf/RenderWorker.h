#pragma once

#include "pdf/PdfDocument.h"

#include <wx/event.h>
#include <wx/image.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct RenderRequest
{
    int page;
    double scale;
    unsigned generation;
    // Also extract the page's text boxes (PdfDocument::textBoxes) on the worker.
    bool withText = false;
};

// Payload of EVT_PAGE_RENDERED. `image` is invalid when the render failed.
// `textBoxes` is set only when the request had withText and the document opened.
struct RenderResult
{
    int page;
    double scale;
    unsigned generation;
    wxImage image;
    std::optional<std::vector<TextBox>> textBoxes;
};

// wxThreadEvent whose payload is a RenderResult.
wxDECLARE_EVENT(EVT_PAGE_RENDERED, wxThreadEvent);

// Renders pages on its own thread with its own PdfDocument opened from `path`
// with `password` (empty for an unencrypted file), and posts each result to
// `sink` as EVT_PAGE_RENDERED. It creates wxImage only, never wxBitmap. The
// destructor stops the thread and joins it, so `sink` must outlive this object.
// When the document cannot be opened, every result carries an invalid image.
class RenderWorker
{
public:
    RenderWorker(wxEvtHandler* sink, std::filesystem::path path, std::string password = {});
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    // Replaces the pending queue. Requests are served front to back. A request
    // equal to the one rendering now is dropped, since its result is on the way.
    void setRequests(std::vector<RenderRequest> requests);

    // Drops every pending request. The page rendering now still posts its result.
    void clear();

private:
    void run();

    wxEvtHandler* m_sink;
    std::filesystem::path m_path;
    std::string m_password;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<RenderRequest> m_queue;
    std::optional<RenderRequest> m_current;
    bool m_stop = false;

    // Declared last so it starts after the members above exist.
    std::thread m_thread;
};
