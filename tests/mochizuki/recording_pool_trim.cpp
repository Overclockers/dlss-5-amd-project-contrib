#include "dlssnr/backend/mochizuki_runtime/RecordingPoolTrim.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
int fails = 0;

void Expect(bool cond, const char* what)
{
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++fails;
}

std::vector<size_t> Drop(const std::vector<int>& pool, int current, bool havePrevious, int previous)
{
    return DlssNr::Backend::IdlePoolDropIndices(
        pool.size(), current, havePrevious, previous,
        [&](size_t i) -> const int& { return pool[i]; },
        [](int a, int b) { return a == b; });
}
} // namespace

int main()
{
    Expect(Drop({}, 1, false, 0).empty(), "empty pool");
    Expect(Drop({1, 1, 1}, 1, false, 0).empty(), "only current");
    Expect(Drop({1, 2, 1}, 1, true, 2).empty(), "current and previous stay");
    {
        const auto drop = Drop({1, 2, 3, 2}, 1, true, 2);
        Expect(drop.size() == 1 && drop[0] == 2, "third geometry drops");
    }
    {
        const auto drop = Drop({4, 1, 4}, 1, false, 0);
        Expect(drop.size() == 2 && drop[0] == 0 && drop[1] == 2, "no previous drops the rest");
    }
    {
        const auto drop = Drop({9}, 1, true, 9);
        Expect(drop.empty(), "previous alone is the hot cache");
    }
    if (fails)
    {
        std::fprintf(stderr, "recording_pool_trim: %d failed\n", fails);
        return 1;
    }
    std::printf("recording_pool_trim: PASS\n");
    return 0;
}
