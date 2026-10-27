#include "serve/prompt_cache.hpp"
#include "common/log.hpp"
#include "formats/strixw.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace strix;
namespace fs = std::filesystem;
std::atomic<int> checks{0};
void check(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
std::vector<int32_t> ids(int64_t n) {
    std::vector<int32_t> v(n);
    std::iota(v.begin(), v.end(), 100);
    return v;
}
struct Payload { int64_t n, base; uint64_t hash; };
struct Fixture {
    uint64_t available = 128ull << 30;
    bool ram_only;
    PromptCache::Options opt;
    std::unique_ptr<PromptCache> cache;
    Fixture(const fs::path &dir, int turns = 4, bool ram = true, bool delta = false) : ram_only(ram) {
        opt.turns_per_chain = turns;
        opt.checkpoints_per_chain = 2;
        opt.mem_available = [this] { return available; };
        opt.disk_free = [] { return 1024ull << 30; };
        opt.delta_max_tokens = delta ? 65536 : 0;
        opt.prefault = false;
        opt.max_spares = 0;
        opt.shutdown_seconds = ram ? 0 : 5;
        opt.write_gib_per_hour = 0;
        opt.room_wait_seconds = 0.1;
        cache = std::make_unique<PromptCache>(dir.string(), 32ull << 30, "turn-cache-test-v1", opt);
    }
    bool save(const std::vector<int32_t> &v, PromptCache::Kind kind = PromptCache::Kind::Turn, int64_t conversation = 0) {
        const int64_t n = v.size(), base = cache->delta_base(v.data(), n, kind);
        HostBuffer b = kind == PromptCache::Kind::System ? HostBuffer()
            : cache->take_replaced_buffer(v.data(), n, base, kind);
        bool reused = b.capacity() > 0;
        if (b.capacity() < 4096) check(cache->make_room_for(4096), "no room for fixture");
        b.resize(4096);
        std::memset(b.data(), 0, b.size());
        const Payload payload{n, base, strix_hash64(v.data(), v.size() * sizeof(int32_t))};
        std::memcpy(b.data(), &payload, sizeof(payload));
        check(cache->put(v, kind, std::move(b), ram_only, base, 0, conversation), "put rejected");
        return reused;
    }
    void restore(const std::vector<int32_t> &prompt, int64_t n, bool *ram = nullptr) {
        int64_t end = 0;
        check(cache->with_state(prompt, n, [&](const HostBuffer &b, int64_t from, int64_t to) {
            Payload payload;
            std::memcpy(&payload, b.data(), sizeof(payload));
            check(payload.n == to && payload.base == from && end == from, "wrong state/delta endpoint");
            check(payload.hash == strix_hash64(prompt.data(), to * sizeof(int32_t)), "wrong state prefix");
            end = to;
        }, ram), "restore failed");
        check(end == n, "incomplete state chain");
    }
};

int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        set_log_level(LogLevel::Error);
        fs::path root(argv[1]);
        {
            Fixture f(root / "window");
            for (int k = 0; k < 10; ++k) {
                const bool reused = f.save(ids(2000 + k * 400));
                check(reused == (k >= 4), "a retained turn was consumed for its buffer");
                for (int j = 0; j <= k; ++j) {
                    auto old = ids(2000 + j * 400);
                    check(f.cache->contains(old.data(), old.size()) == (j >= k - 3), "not the newest four turns");
                }
                check(f.cache->stats().entries == std::min(k + 1, 4), "turn window grew without bound");
            }
            auto v = ids(5600);
            check(f.cache->take_replaced_buffer(v.data(), v.size()).empty(), "duplicate stole an ancestor");
            f.save(v);
            check(f.cache->stats().entries == 4, "duplicate changed retained count");
            f.restore(ids(6000), 5600);
        }
        {
            Fixture f(root / "single", 1);
            check(!f.save(ids(2000)), "first turn reused nonexistent state");
            check(f.save(ids(2400)), "single-turn policy did not reuse predecessor");
            auto old = ids(2000);
            check(!f.cache->contains(old.data(), old.size()), "single-turn policy retained predecessor");
            check(f.cache->stats().entries == 1, "single-turn count wrong");
        }
        {
            Fixture f(root / "checkpoints");
            for (int k = 0; k < 4; ++k) f.save(ids(2000 + k * 400));
            check(!f.save(ids(3500), PromptCache::Kind::Checkpoint), "checkpoint stole retained turn buffer");
            for (int k = 0; k < 4; ++k) {
                auto v = ids(2000 + k * 400);
                check(f.cache->contains(v.data(), v.size()), "checkpoint consumed turn retention budget");
            }
            f.save(ids(3700), PromptCache::Kind::Checkpoint);
            f.save(ids(3900), PromptCache::Kind::Checkpoint);
            auto first = ids(3500);
            check(!f.cache->contains(first.data(), first.size()), "user checkpoint retention broken");
            f.restore(ids(4200), 3900);
        }
        {
            Fixture f(root / "deltas", 4, true, true);
            for (int k = 0; k < 24; ++k) {
                const int n = 10000 + k * 500;
                f.save(ids(n));
                f.restore(ids(n + 100), n);
                check(f.cache->stats().entries <= 5, "obsolete delta bases leaked beyond window plus base");
                for (int j = std::max(0, k - 3); j <= k; ++j) f.restore(ids(n + 100), 10000 + j * 500);
            }
            check(f.cache->stats().delta_puts > 0, "delta path not exercised");
            check(f.cache->stats().deltas_dropped == 0, "retention dropped a needed delta base");
        }
        {
            Fixture f(root / "branches");
            f.save(ids(1000), PromptCache::Kind::System);
            f.save(ids(2500));
            auto branch = ids(5000);
            branch[3000] = -1234;
            f.save(branch);
            auto revised = branch;
            revised[3000] = -9999;
            check(f.cache->best(revised, revised.size() - 1) == 2500, "wrong image branch reused");
            f.restore(revised, 2500);
            revised[1500] = 88;
            check(f.cache->best(revised, revised.size() - 1) == 1000, "state reused beyond deep edit");
            f.restore(revised, 1000);
        }
        {
            // A rewritten message (a hidden image at 2200) leaves its conversation's later turns behind: they are
            // superseded - in RAM while it fits, never written, first to leave - while the branch's prefix stays
            // ordinary, and a conversation sharing only the system prompt (it differs at 1200, inside the first message
            // that ends at 1500) is untouched. A client returning to the old branch makes it ordinary again.
            Fixture f(root / "superseded", 4, false);
            const int64_t conv = 1500;
            auto other = ids(2600);
            other[1200] = -77;
            f.save(other, PromptCache::Kind::Turn, conv);
            f.save(ids(2000), PromptCache::Kind::Turn, conv);
            f.save(ids(2400), PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 0, "a straight conversation superseded its own turns");
            auto hidden = ids(2800);
            hidden[2200] = -5;
            f.save(hidden, PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 1, "the turn behind the rewritten message was not superseded");
            auto old = ids(2400), prefix = ids(2000);
            check(f.cache->contains(old.data(), old.size()), "a superseded turn left RAM while it fits");
            check(f.cache->best(hidden, hidden.size() - 1) == 2000, "the branch lost its prefix turn");
            auto next = hidden;
            next.push_back(42);
            f.save(next, PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 1, "a superseded turn was counted twice");
            other.push_back(43);
            f.save(other, PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 1, "another conversation with the same system prompt was superseded");
            f.available = 0;
            (void)f.cache->make_room_for(4096);
            f.cache->flush();
            f.available = 128ull << 30;
            check(f.cache->stats().superseded_dropped == 1, "the superseded turn was not dropped first");
            check(!f.cache->contains(old.data(), old.size()), "a superseded turn reached the disk");
            check(f.cache->contains(prefix.data(), prefix.size()), "the branch's prefix was not written");
            auto shared = other;
            shared.pop_back();
            check(f.cache->contains(shared.data(), shared.size()), "the other conversation's turn was not written");
            // Back to the old branch: a hit clears the mark, so the turn is written like any other.
            auto back = ids(2600);
            f.save(back, PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 3, "returning to the old branch did not supersede the rewritten one");
            auto fork = ids(3000);
            fork[2550] = -6;
            f.save(fork, PromptCache::Kind::Turn, conv);
            check(f.cache->stats().superseded == 4, "the turn behind the fork was not superseded");
            f.restore(ids(2700), 2600);  // the client comes back to it: a hit
            f.available = 0;
            (void)f.cache->make_room_for(4096);
            f.cache->flush();
            f.available = 128ull << 30;
            check(f.cache->contains(back.data(), back.size()), "a used turn stayed superseded");
            bool refused = false;
            try {
                HostBuffer b;
                b.resize(64);
                f.cache->put(ids(3100), PromptCache::Kind::Checkpoint, std::move(b), false, 0, 0, 10);
            }
            catch (const std::exception &) { refused = true; }
            check(refused, "a checkpoint named a conversation");
        }
        {
            fs::path dir = root / "restart";
            {
                Fixture f(dir, 4, false, true);
                for (int k = 0; k < 7; ++k) f.save(ids(10000 + k * 500));
            }
            Fixture f(dir, 4, false, true);
            check(f.cache->stats().invalid == 0, "persisted entries invalid");
            auto v = ids(14000);
            check(f.cache->best(v, v.size() - 1) == 13000, "restart lost latest turn");
            bool ram = true;
            f.restore(v, 13000, &ram);
            check(!ram, "restart did not exercise disk restore");
            for (int n : {11500, 12000, 12500}) f.restore(v, n);
        }
        {
            Fixture f(root / "pressure", 4, false);
            for (int n : {2000, 2500, 3000, 3500}) f.save(ids(n));
            f.available = 0;
            (void)f.cache->make_room_for(4096);
            f.cache->flush();
            check(f.cache->stats().ram_entries == 0, "retained points were pinned against RAM eviction");
            f.available = 128ull << 30;
            bool ram = true;
            f.restore(ids(4000), 2500, &ram);
            check(!ram, "pressure did not spill retained state to disk");
        }
        {
            Fixture f(root / "concurrent");
            std::exception_ptr error;
            std::mutex errors;
            auto work = [&](int branch) {
                try {
                    for (int k = 0; k < 12; ++k) {
                        auto v = ids(2000 + 400 * k);
                        v[1000] = -100 - branch;
                        f.save(v);
                        auto next = v;
                        next.push_back(42);
                        f.restore(next, v.size());
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errors);
                    error = std::current_exception();
                }
            };
            std::thread a(work, 0), b(work, 1);
            a.join(); b.join();
            if (error) std::rethrow_exception(error);
            check(f.cache->stats().entries == 8, "concurrent chains did not retain four each");
            for (int branch : {0, 1}) {
                auto v = ids(7000);
                v[1000] = -100 - branch;
                check(f.cache->best(v, v.size() - 1) == 6400, "concurrent state leaked across branches");
                f.restore(v, 6400);
            }
        }
        for (int bad : {0, 65}) {
            bool refused = false;
            try { Fixture f(root / ("invalid-" + std::to_string(bad)), bad); }
            catch (const std::exception &) { refused = true; }
            check(refused, "invalid turn count accepted");
        }
        std::cout << "PASS: " << checks << " assertions; window/buffer reuse/duplicates/checkpoints/deltas/branches/superseded/restart/pressure/concurrency/options\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " assertions: " << e.what() << '\n';
        return 1;
    }
}
