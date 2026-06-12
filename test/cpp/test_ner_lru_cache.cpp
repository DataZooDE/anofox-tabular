//===----------------------------------------------------------------------===//
// Catch2 unit tests for the NER LRUCache (issue #50).
//
// Built as a standalone test binary (anofox_tabular_cpp_tests) using the
// Catch2 header bundled with DuckDB (duckdb/third_party/catch/catch.hpp).
//
// Run: ./build/release/extension/anofox_tabular/anofox_tabular_cpp_tests
//===----------------------------------------------------------------------===//

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "anofox_ner.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using duckdb::anofox::LRUCache;

TEST_CASE("LRUCache basic put/get and eviction order", "[ner][lru]") {
    LRUCache<std::string, int> cache(2);

    REQUIRE(cache.Size() == 0);
    REQUIRE(cache.Capacity() == 2);
    REQUIRE(!cache.Get("a").has_value());

    cache.Put("a", 1);
    cache.Put("b", 2);
    REQUIRE(cache.Size() == 2);

    // Touch "a" so that "b" becomes least recently used
    REQUIRE(cache.Get("a").value() == 1);

    // Inserting "c" must evict the least recently used entry "b"
    cache.Put("c", 3);
    REQUIRE(cache.Size() == 2);
    REQUIRE(!cache.Get("b").has_value());
    REQUIRE(cache.Get("a").value() == 1);
    REQUIRE(cache.Get("c").value() == 3);

    // Updating an existing key must not grow the cache
    cache.Put("a", 10);
    REQUIRE(cache.Size() == 2);
    REQUIRE(cache.Get("a").value() == 10);
}

TEST_CASE("LRUCache clear empties the cache", "[ner][lru]") {
    LRUCache<std::string, int> cache(4);
    cache.Put("a", 1);
    cache.Put("b", 2);
    REQUIRE(cache.Size() == 2);

    cache.Clear();
    REQUIRE(cache.Size() == 0);
    REQUIRE(!cache.Get("a").has_value());
    REQUIRE(!cache.Get("b").has_value());
}

TEST_CASE("LRUCache Put with zero capacity is a safe no-op", "[ner][lru]") {
    // Regression test for issue #50: with capacity 0, Put() used to evaluate
    // cache_.size() >= capacity_ as true and call lru_list_.back() on an
    // empty list (undefined behavior, observed as a crash).
    LRUCache<std::string, int> cache(0);

    cache.Put("a", 1);

    REQUIRE(cache.Size() == 0);
    REQUIRE(!cache.Get("a").has_value());
}

TEST_CASE("LRUCache SetCapacity(0) then Put is a safe no-op", "[ner][lru]") {
    // Mirrors SET anofox_ner_cache_size = 0 happening while a query is using
    // the cache: subsequent Put() calls must not touch an empty LRU list.
    LRUCache<std::string, int> cache(4);
    cache.Put("a", 1);
    cache.Put("b", 2);

    cache.SetCapacity(0);
    REQUIRE(cache.Capacity() == 0);
    REQUIRE(cache.Size() == 0); // all entries evicted

    cache.Put("c", 3); // must not crash
    REQUIRE(cache.Size() == 0);
    REQUIRE(!cache.Get("c").has_value());

    // Growing the capacity again must restore normal behavior
    cache.SetCapacity(2);
    cache.Put("d", 4);
    REQUIRE(cache.Get("d").value() == 4);
}

TEST_CASE("LRUCache concurrent Get/Put/SetCapacity stress", "[ner][lru][threads]") {
    // Issue #50: Capacity() read capacity_ without the mutex while
    // SetCapacity() wrote it, and call sites checked Capacity() > 0 outside
    // the lock before Put(). Toggling the capacity to 0 while workers insert
    // reproduces the race (best-effort; run under TSan for a deterministic
    // data-race report).
    LRUCache<std::string, int> cache(64);
    std::atomic<bool> stop {false};

    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&cache, &stop, t]() {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::string key = "k" + std::to_string((t * 31 + i) % 97);
                // Same pattern the NER call sites used: check capacity, then Put
                if (cache.Capacity() > 0) {
                    cache.Put(key, i);
                }
                cache.Get(key);
                ++i;
            }
        });
    }

    for (int round = 0; round < 20000; ++round) {
        cache.SetCapacity(round % 2 == 0 ? 0 : 32);
    }
    stop.store(true);
    for (auto &worker : workers) {
        worker.join();
    }

    cache.SetCapacity(8);
    cache.Put("final", 42);
    REQUIRE(cache.Get("final").value() == 42);
}
