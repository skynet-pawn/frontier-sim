#pragma once
// Binary save and load of the simulation's state (see sim/src/save.cpp).
//
// One function per struct lists its fields once and serves both directions: with a StateWriter it writes them, with a
// StateReader it reads them back into place. Because the two directions share one list, they cannot disagree about order
// or type. When a field is added to a saved struct it must be added to that list, and kStateVersion bumped; the
// save/reload test (tests/save_test.cpp) catches a forgotten field by comparing a straight run with a saved and reloaded one.
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frontier {

struct Headline;

class StateWriter {
public:
    static constexpr bool loading = false;
    explicit StateWriter(std::FILE* f) : f_(f) {}
    void raw(void* p, std::size_t n) {
        if (n && std::fwrite(p, 1, n, f_) != n) throw std::runtime_error("save failed: could not write the file");
    }
    std::unordered_map<const Headline*, std::uint32_t> headline_ids;   // headlines are shared between papers: written once

private:
    std::FILE* f_;
};

class StateReader {
public:
    static constexpr bool loading = true;
    explicit StateReader(std::FILE* f) : f_(f) {}
    void raw(void* p, std::size_t n) {
        if (n && std::fread(p, 1, n, f_) != n) throw std::runtime_error("load failed: the file ends early");
    }
    std::vector<std::shared_ptr<const Headline>> headline_pool;

private:
    std::FILE* f_;
};

// ---- Generic pieces. Struct-specific io() overloads live beside the structs they describe (save.cpp) and are found by
// argument-dependent lookup, so the order of these declarations does not matter. ----

template <class Ar, class T>
std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>> io(Ar& ar, T& v) { ar.raw(&v, sizeof v); }

template <class Ar>
void io(Ar& ar, std::string& s) {
    std::uint64_t n = s.size();
    io(ar, n);
    if constexpr (Ar::loading) s.resize(static_cast<std::size_t>(n));
    ar.raw(s.data(), s.size());
}

template <class Ar, class A, class B>
void io(Ar& ar, std::pair<A, B>& p) { io(ar, p.first); io(ar, p.second); }

template <class Ar, class T>
void io(Ar& ar, std::vector<T>& v) {
    std::uint64_t n = v.size();
    io(ar, n);
    if constexpr (Ar::loading) v.resize(static_cast<std::size_t>(n));
    if constexpr (std::is_arithmetic_v<T>) ar.raw(v.data(), v.size() * sizeof(T));
    else for (T& x : v) io(ar, x);
}

template <class Ar, class T, std::size_t N>
void io(Ar& ar, T (&a)[N]) { for (T& x : a) io(ar, x); }

template <class Ar, class K, class V>
void io(Ar& ar, std::map<K, V>& m) {
    std::uint64_t n = m.size();
    io(ar, n);
    if constexpr (Ar::loading) {
        m.clear();
        for (std::uint64_t i = 0; i < n; ++i) {
            K k{};
            V v{};
            io(ar, k);
            io(ar, v);
            m.emplace(std::move(k), std::move(v));
        }
    } else {
        for (auto& kv : m) {
            K k = kv.first;
            io(ar, k);
            io(ar, kv.second);
        }
    }
}

}  // namespace frontier
