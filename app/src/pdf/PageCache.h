#pragma once

#include <wx/bitmap.h>

#include <cstddef>
#include <list>
#include <map>
#include <utility>

// LRU cache of rendered page bitmaps keyed by (page, scale) with a byte budget.
// The caller passes each entry's size to put(). The cache never measures bitmaps.
// UI thread only, like every wxBitmap.
class PageCache
{
public:
    static constexpr std::size_t kDefaultBudgetBytes = std::size_t{256} * 1024 * 1024;

    explicit PageCache(std::size_t budgetBytes = kDefaultBudgetBytes);

    // Inserts or replaces the entry and marks it most recently used, then evicts
    // least recently used entries while bytes() > budget(). Never evicts this entry.
    void put(int page, double scale, const wxBitmap& bitmap, std::size_t bytes);

    // nullptr on a miss. A hit marks the entry most recently used. The pointer
    // stays valid until the next put() or clear().
    const wxBitmap* get(int page, double scale);

    // Does not change recency.
    bool contains(int page, double scale) const;

    void clear();

    std::size_t count() const { return m_index.size(); }
    std::size_t bytes() const { return m_bytes; }
    std::size_t budget() const { return m_budget; }

private:
    using Key = std::pair<int, double>;

    struct Entry
    {
        Key key;
        wxBitmap bitmap;
        std::size_t bytes;
    };

    // Front is most recently used.
    std::list<Entry> m_entries;
    std::map<Key, std::list<Entry>::iterator> m_index;
    std::size_t m_bytes = 0;
    std::size_t m_budget;
};
