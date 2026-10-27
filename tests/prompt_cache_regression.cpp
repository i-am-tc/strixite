#include "serve/prompt_cache.hpp"
#include "common/log.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace strix;
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        set_log_level(LogLevel::Error);
        PromptCache::Options opt;
        opt.mem_available = [] { return 128ull << 30; };
        opt.disk_free = [] { return 1024ull << 30; };
        opt.delta_max_tokens = 0;
        opt.prefault = false;
        opt.max_spares = 0;
        opt.shutdown_seconds = 0;
        PromptCache cache(argv[1], 32ull << 30, "turn-cache-regression", opt);
        std::vector<int32_t> history(95475);
        std::iota(history.begin(), history.end(), 100);
        // Content-derived negative vision keys: the next request hides this image.
        std::fill(history.begin() + 72000, history.begin() + 75000, -1234567);
        auto save = [&](int64_t n, PromptCache::Kind kind) {
            const int64_t base = cache.delta_base(history.data(), n, kind);
            HostBuffer bytes = kind == PromptCache::Kind::System ? HostBuffer()
                : cache.take_replaced_buffer(history.data(), n, base);
            bytes.resize(4096);
            std::memset(bytes.data(), 0, bytes.size());
            std::memcpy(bytes.data(), &n, sizeof(n));
            if (!cache.put({history.begin(), history.begin() + n}, kind, std::move(bytes), true, base))
                throw std::runtime_error("save refused");
        };
        save(11591, PromptCache::Kind::System);
        save(68866, PromptCache::Kind::Turn); // request before the image arrives
        save(85000, PromptCache::Kind::Turn); // request seeing the image
        auto next = history;
        next[72000] = 77; // same old history except the hidden image
        const int64_t best = cache.best(next, next.size() - 1);
        if (best != 68866) {
            std::cerr << "FAIL: hiding the previous image resumed " << best
                      << " instead of 68866 shared tokens\n";
            return 1;
        }
        bool checked = false;
        if (!cache.with_state(next, best, [&](const HostBuffer &bytes) {
            int64_t stored = -1;
            std::memcpy(&stored, bytes.data(), sizeof(stored));
            if (stored != best) throw std::runtime_error("wrong saved state selected");
            checked = true;
        }) || !checked) throw std::runtime_error("state unavailable");
        std::cout << "PASS: image hidden after one turn; reused 68866 instead of 11591; saved state matches endpoint\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
