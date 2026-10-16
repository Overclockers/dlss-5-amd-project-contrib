#pragma once
#include <cstddef>
#include <vector>

namespace DlssNr::Backend
{
// Indices of idle pool entries that are neither the current geometry nor the single
// previous one. In-flight recordings are not pool entries and are never listed here.
template <class Key, class At, class Eq>
std::vector<size_t> IdlePoolDropIndices(size_t count, const Key& current, bool havePrevious,
                                        const Key& previous, At at, Eq eq)
{
    std::vector<size_t> drop;
    drop.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const Key& key = at(i);
        if (eq(key, current)) continue;
        if (havePrevious && eq(key, previous)) continue;
        drop.push_back(i);
    }
    return drop;
}
} // namespace DlssNr::Backend
