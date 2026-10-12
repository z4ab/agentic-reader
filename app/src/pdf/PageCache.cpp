#include "pdf/PageCache.h"

PageCache::PageCache(std::size_t budgetBytes)
    : m_budget(budgetBytes)
{
}

void PageCache::put(int page, double scale, const wxBitmap& bitmap, std::size_t bytes)
{
    const Key key{page, scale};
    if (auto it = m_index.find(key); it != m_index.end()) {
        m_bytes -= it->second->bytes;
        m_entries.erase(it->second);
        m_index.erase(it);
    }

    m_entries.push_front({key, bitmap, bytes});
    m_index.emplace(key, m_entries.begin());
    m_bytes += bytes;

    while (m_bytes > m_budget && m_entries.size() > 1) {
        const Entry& oldest = m_entries.back();
        m_bytes -= oldest.bytes;
        m_index.erase(oldest.key);
        m_entries.pop_back();
    }
}

const wxBitmap* PageCache::get(int page, double scale)
{
    const auto it = m_index.find({page, scale});
    if (it == m_index.end())
        return nullptr;
    m_entries.splice(m_entries.begin(), m_entries, it->second);
    return &it->second->bitmap;
}

bool PageCache::contains(int page, double scale) const
{
    return m_index.contains({page, scale});
}

void PageCache::clear()
{
    m_entries.clear();
    m_index.clear();
    m_bytes = 0;
}
