// Savestates: the complete emulated machine in one file. Each subsystem serializes its state with
// a Stream (the same function saves and loads); threads resume at the MIPS level (kernel.cpp), so a
// savestate does not depend on the program build.
#pragma once
#include "common.h"
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace ss {

class Stream {
public:
    // out == nullptr: counting pass (records the layout of raw structures, stores nothing)
    explicit Stream(std::vector<u8>* out) : loading(false), out_(out) {}
    Stream(const u8* in, size_t n) : loading(true), in_(in), n_(n) {}
    const bool loading;
    bool ok = true;
    std::string error;

    bool counting() const { return !loading && !out_; }
    const std::map<std::string, u32>& layout() const { return layout_; }

    void raw(void* p, size_t n) {
        if (counting()) return;
        if (!loading) {
            const u8* b = (const u8*)p;
            out_->insert(out_->end(), b, b + n);
        } else if (pos_ + n > n_) {
            fail("section too short");
            memset(p, 0, n);
        } else {
            memcpy(p, in_ + pos_, n);
            pos_ += n;
        }
    }
    template <class T> void v(T& x) {
        static_assert(std::is_trivially_copyable_v<T>, "POD only");
        raw(&x, sizeof(T));
    }
    template <class T, size_t N> void arr(T (&a)[N]) {
        static_assert(std::is_trivially_copyable_v<T>, "POD only");
        raw(a, sizeof(a));
    }
    // a structure stored as raw bytes: its size is recorded, a build with another layout refuses it
    template <class T> void pod(T& x, const char* what) {
        if (!loading) layout_[what] = (u32)sizeof(T);
        u32 n = sizeof(T);
        v(n);
        if (loading && n != sizeof(T)) { fail(std::string("layout of ") + what + " changed"); return; }
        v(x);
    }
    template <class T> void vec(std::vector<T>& x) {
        static_assert(std::is_trivially_copyable_v<T>, "POD only");
        u32 n = (u32)x.size();
        v(n);
        if (loading) {
            if ((size_t)n * sizeof(T) > n_ - pos_) { fail("bad vector size"); return; }
            x.resize(n);
        }
        if (n) raw(x.data(), (size_t)n * sizeof(T));
    }
    template <class T> void deq(std::deque<T>& x) {
        std::vector<T> t(x.begin(), x.end());
        vec(t);
        if (loading) x.assign(t.begin(), t.end());
    }
    void str(std::string& s) {
        u32 n = (u32)s.size();
        v(n);
        if (loading) {
            if (n > n_ - pos_) { fail("bad string size"); return; }
            s.assign((const char*)in_ + pos_, n);
            pos_ += n;
        } else {
            raw(s.data(), n);
        }
    }
    template <class K, class V> void map(std::map<K, V>& m) {
        static_assert(std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>, "POD only");
        u32 n = (u32)m.size();
        v(n);
        if (loading) {
            m.clear();
            for (u32 i = 0; i < n && ok; i++) {
                K k{};
                V val{};
                v(k);
                v(val);
                m[k] = val;
            }
        } else {
            for (auto& kv : m) {
                K k = kv.first;
                v(k);
                v(kv.second);
            }
        }
    }
    void fail(const std::string& why) {
        if (ok) error = why;
        ok = false;
    }

private:
    std::vector<u8>* out_ = nullptr;
    std::map<std::string, u32> layout_;
    const u8* in_ = nullptr;
    size_t n_ = 0, pos_ = 0;
};

// Requests from the user interface; carried out by the emulation thread at the next safe point.
void request_save(int slot);
void request_load(int slot);
std::string slot_path(int slot);
bool slot_exists(int slot, std::string* when = nullptr);  // when: "20.10.2026 16:05"

// Kernel side (emulation thread): at a safe point the kernel saves (every thread at a resume point)
// or, from the scheduler (no thread running), loads.
bool pending();
bool save_pending();
bool load_pending();
bool can_save(std::string* why);  // the other subsystems allow a savestate now
void do_save();
void do_load();

}  // namespace ss
