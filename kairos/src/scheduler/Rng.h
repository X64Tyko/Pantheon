#pragma once
#include <cstdint>
#include <sstream>
#include <string>

// xoshiro256** — 4×uint64 state, ~80 bytes serialized.
// Satisfies UniformRandomBitGenerator so it works directly with
// std::uniform_int_distribution and std::shuffle.
struct Xoshiro256 {
    using result_type = uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return UINT64_MAX; }

    uint64_t s[4];

    explicit Xoshiro256(uint64_t seed) {
        // Splitmix64 expansion: safe to seed from a single integer.
        for (int i = 0; i < 4; ++i) {
            seed += 0x9e3779b97f4a7c15ULL;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            s[i] = z ^ (z >> 31);
        }
    }

    result_type operator()() {
        const uint64_t r = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;    s[3]  = rotl(s[3], 45);
        return r;
    }

    std::string serialize() const {
        std::ostringstream ss;
        ss << s[0] << ' ' << s[1] << ' ' << s[2] << ' ' << s[3];
        return ss.str();
    }

    static Xoshiro256 deserialize(const std::string& str) {
        Xoshiro256 r(0);
        std::istringstream ss(str);
        ss >> r.s[0] >> r.s[1] >> r.s[2] >> r.s[3];
        return r;
    }

private:
    static uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};
