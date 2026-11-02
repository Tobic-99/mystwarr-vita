// Save states: one stream type for both directions, so each module lists its state once.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct State {
    std::vector<uint8_t>* out = nullptr;   // saving
    const uint8_t* in = nullptr;           // loading
    size_t left = 0;
    bool ok = true;
    void raw(void* p, size_t n) {
        if (out) { auto* b = static_cast<uint8_t*>(p); out->insert(out->end(), b, b + n); return; }
        if (n > left) { ok = false; return; }
        std::memcpy(p, in, n); in += n; left -= n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
};
