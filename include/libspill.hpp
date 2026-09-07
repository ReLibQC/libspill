// SPDX-License-Identifier: BSD-3-Clause
// libspill -- header-only C++ layer.
//
// DESIGN.md §4a: "The C layer exists for reach -- Fortran, Python, and a stable
// ABI -- not because anyone should enjoy writing against it." This is where the
// ergonomics live: RAII, spans, exceptions, and a typed accumulate that supplies
// the reduction from T so a C++ caller never writes a callback.
//
// Header-only and templated on purpose, and equally on purpose it sits ABOVE
// the C core rather than inside it -- §4 is explicit that templating the core
// would forfeit the Fortran and Python reach that makes the component worth
// building. Nothing here is in the ABI; all of it compiles away.
//
// Requires C++20 for std::span. The C entry points remain public and supported.
#ifndef LIBSPILL_HPP
#define LIBSPILL_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include "libspill.h"
}

/* The primary namespace is `libspill`. `ls` is a two-character name and this is
 * a header a project vendors, so squatting on it would be rude at best; the
 * short alias below is provided for convenience and can be turned off with
 * -DLIBSPILL_NO_SHORT_NAMESPACE if the consuming project already has one. */
namespace libspill {

// ---------------------------------------------------------------- errors

/// Thrown by every operation that fails. Carries the C code unchanged, so a
/// caller that wants to branch on -ENOSPC still can -- §4b keeps that code
/// meaningful precisely because a full scratch filesystem is routine.
class error : public std::runtime_error {
public:
    error(int code, std::string_view what)
        : std::runtime_error(build(code, what)), code_(code) {}

    /// The libspill or negated-errno code (§4b).
    int code() const noexcept { return code_; }
    /// True when the failure came from the OS rather than from libspill.
    bool is_errno() const noexcept { return code_ < 0 && code_ > -1000; }

private:
    static std::string build(int code, std::string_view what) {
        char buf[LS_ERRBUF_MIN];
        ls_strerror(code, buf, sizeof buf);
        return std::string(what) + ": " + buf;
    }
    int code_;
};

inline void check(int rc, std::string_view what) {
    if (rc != LS_OK) throw error(rc, what);
}

// ---------------------------------------------------------------- options

/// Designated-initialiser friendly, and correct by default: the version field
/// §4b requires is filled in here so a caller never has to know it exists.
struct options {
    ls_backend  backend       = LS_POSIX;
    ls_mode     mode          = LS_EXPLICIT;
    ls_parallel parallel      = LS_LOCAL;
    int         rank          = -1;
    std::size_t memory_budget = 0;
    std::string dir           = {};
    bool        direct_io     = false;
    /* Write to <dir>/<name> verbatim, with no .libspill suffix. For callers
     * whose own code inspects the filesystem; see the note in libspill.h. */
    bool        exact_name    = false;
    /* fsync a kept store on close. Off by default; see the note in
     * libspill.h. Scratch does not need to outlive the process. */
    bool        durable_close = false;

    ls_opts to_c() const {
        ls_opts o;
        ls_opts_default(&o);
        o.backend       = backend;
        o.mode          = mode;
        o.parallel      = parallel;
        o.rank          = rank;
        o.memory_budget = memory_budget;
        o.dir           = dir.empty() ? nullptr : dir.c_str();
        o.direct_io     = direct_io ? 1 : 0;
        o.exact_name    = exact_name ? 1 : 0;
        o.durable_close = durable_close ? 1 : 0;
        return o;
    }
};

// ---------------------------------------------------------------- requests

class store;

/// Move-only, and waits in the destructor. §4b makes ls_wait the call that both
/// reports status and releases the handle, so a request that goes out of scope
/// unwaited would otherwise leak until the store closed.
class request {
public:
    request() = default;
    request(const request &) = delete;
    request &operator=(const request &) = delete;

    request(request &&o) noexcept : req_(std::exchange(o.req_, nullptr)) {}
    request &operator=(request &&o) noexcept {
        if (this != &o) { drain(); req_ = std::exchange(o.req_, nullptr); }
        return *this;
    }

    /// Completes the operation. Throws on failure; safe to call once.
    void wait() {
        if (!req_) return;
        int rc = ls_wait(std::exchange(req_, nullptr));
        check(rc, "ls_wait");
    }

    /// Has it finished? A request that tests done must still be waited on.
    bool done() const {
        if (!req_) return true;
        int d = 0;
        check(ls_test(req_, &d), "ls_test");
        return d != 0;
    }

    /// The destructor waits but never throws: a failure there would run during
    /// stack unwinding. Call wait() explicitly to see the error.
    ~request() { drain(); }

private:
    friend class store;
    explicit request(ls_req *r) : req_(r) {}
    void drain() noexcept { if (req_) ls_wait(std::exchange(req_, nullptr)); }

    ls_req *req_ = nullptr;
};

// ------------------------------------------------------------------ store

namespace detail {
// Only trivially copyable element types may cross into a byte store: anything
// with a pointer, a vtable or a destructor would be restored as garbage. The
// C core cannot say this -- it moves opaque bytes and must -- but a typed layer
// can, and this is most of what the typed layer is for.
template <class T>
inline constexpr bool storable_v =
    std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

template <class T> using bytes_of = std::span<const std::byte>;
}  // namespace detail

class store {
public:
    /// Throws on failure rather than returning null (§4a).
    explicit store(std::string_view name, const options &opt = {}) {
        ls_opts o = opt.to_c();
        int err = 0;
        s_ = ls_open(std::string(name).c_str(), &o, &err);
        if (!s_) throw error(err, "ls_open(" + std::string(name) + ")");
    }

    store(const store &) = delete;
    store &operator=(const store &) = delete;
    store(store &&o) noexcept : s_(std::exchange(o.s_, nullptr)), keep_(o.keep_) {}
    store &operator=(store &&o) noexcept {
        if (this != &o) { shut(); s_ = std::exchange(o.s_, nullptr); keep_ = o.keep_; }
        return *this;
    }

    /// Scratch is unlinked at close unless keep(true) was asked for.
    ~store() { shut(); }

    void keep(bool k) noexcept { keep_ = k; }

    /// Closes explicitly, so that a failure is visible instead of swallowed by
    /// the destructor.
    void close() {
        if (!s_) return;
        int rc = ls_close(std::exchange(s_, nullptr), keep_ ? 1 : 0);
        check(rc, "ls_close");
    }

    // ---- typed data path; sizes are deduced, never passed ----

    template <class T>
    void write(std::string_view key, std::uint64_t off, std::span<const T> v) {
        static_assert(detail::storable_v<T>, "element type is not trivially copyable");
        check(ls_write(s_, k(key), off, v.size_bytes(), v.data()), "ls_write");
    }

    template <class T>
    void read(std::string_view key, std::uint64_t off, std::span<T> v) {
        static_assert(detail::storable_v<T>, "element type is not trivially copyable");
        check(ls_read(s_, k(key), off, v.size_bytes(), v.data()), "ls_read");
    }

    /// Appends and reports where it landed. Atomic against other appenders.
    template <class T>
    std::uint64_t append(std::string_view key, std::span<const T> v) {
        static_assert(detail::storable_v<T>, "element type is not trivially copyable");
        std::uint64_t off = 0;
        check(ls_append(s_, k(key), v.size_bytes(), v.data(), &off), "ls_append");
        return off;
    }

    /// The point of the exercise (§4a): the reduction is chosen by T, so a C++
    /// caller never writes an ls_reduce callback. alpha scales the source.
    template <class T>
    void accumulate(std::string_view key, std::uint64_t off, std::span<const T> v,
                    T alpha = T{1}) {
        static_assert(std::is_same_v<T, double> || std::is_same_v<T, float>,
                      "accumulate<T> supplies a reduction only for double and float; "
                      "for anything else call ls_accumulate with your own ls_reduce");
        T a = alpha;
        auto op = std::is_same_v<T, double> ? &ls_add_f64 : &ls_add_f32;
        check(ls_accumulate(s_, k(key), off, v.size_bytes(), v.data(), op, &a),
              "ls_accumulate");
    }

    // ---- asynchronous; move-only requests that wait in their destructor ----

    template <class T>
    [[nodiscard]] request awrite(std::string_view key, std::uint64_t off,
                                 std::span<const T> v) {
        static_assert(detail::storable_v<T>, "element type is not trivially copyable");
        ls_req *r = nullptr;
        check(ls_awrite(s_, k(key), off, v.size_bytes(), v.data(), &r), "ls_awrite");
        return request(r);
    }

    template <class T>
    [[nodiscard]] request aread(std::string_view key, std::uint64_t off, std::span<T> v) {
        static_assert(detail::storable_v<T>, "element type is not trivially copyable");
        ls_req *r = nullptr;
        check(ls_aread(s_, k(key), off, v.size_bytes(), v.data(), &r), "ls_aread");
        return request(r);
    }

    // ---- vectored: a segment list, which subsumes strided access ----

    template <class T>
    void writev(std::string_view key, std::span<const ls_seg> segs) {
        check(ls_writev(s_, k(key), segs.data(), segs.size()), "ls_writev");
    }
    void readv(std::string_view key, std::span<const ls_seg> segs) {
        check(ls_readv(s_, k(key), segs.data(), segs.size()), "ls_readv");
    }

    // ---- table of contents ----

    bool exists(std::string_view key) const {
        int f = 0;
        check(ls_exists(s_, k(key), &f), "ls_exists");
        return f != 0;
    }

    std::uint64_t size(std::string_view key) const {
        std::uint64_t n = 0;
        check(ls_size(s_, k(key), &n), "ls_size");
        return n;
    }

    void reserve(std::string_view key, std::uint64_t nbytes) {
        check(ls_reserve(s_, k(key), nbytes), "ls_reserve");
    }

    void erase(std::string_view key) { check(ls_erase(s_, k(key)), "ls_erase"); }

    /// A snapshot, as §4b requires; the vector owns its strings.
    std::vector<std::string> keys() const {
        char **kk = nullptr;
        std::size_t n = 0;
        check(ls_keys(s_, &kk, &n), "ls_keys");
        std::vector<std::string> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; i++) out.emplace_back(kk[i]);
        ls_keys_free(kk, n);
        return out;
    }

    // ---- attributes: bounded, opaque, never interpreted (§3a(3)) ----

    void set_attr(std::string_view key, std::span<const std::byte> blob) {
        check(ls_set_attr(s_, k(key), blob.data(), blob.size()), "ls_set_attr");
    }

    std::vector<std::byte> get_attr(std::string_view key) const {
        std::size_t n = 0;
        check(ls_get_attr(s_, k(key), nullptr, &n), "ls_get_attr");
        std::vector<std::byte> out(n);
        if (n) check(ls_get_attr(s_, k(key), out.data(), &n), "ls_get_attr");
        out.resize(n);
        return out;
    }

    ls_store *c_handle() noexcept { return s_; }

private:
    // std::string_view is not NUL-terminated; every key crosses here.
    const char *k(std::string_view key) const {
        kbuf_.assign(key);
        return kbuf_.c_str();
    }
    void shut() noexcept { if (s_) ls_close(std::exchange(s_, nullptr), keep_ ? 1 : 0); }

    ls_store *s_ = nullptr;
    bool keep_ = false;
    mutable std::string kbuf_;
};

/// Does a store of this name already exist, without creating one?
inline bool store_exists(std::string_view name, const options &opt = {}) {
    ls_opts o = opt.to_c();
    int found = 0;
    check(ls_store_exists(std::string(name).c_str(), &o, &found), "ls_store_exists");
    return found != 0;
}

/// Convenience for the common case of a whole contiguous container.
template <class C>
inline auto as_span(const C &c) { return std::span<const typename C::value_type>(c); }
template <class C>
inline auto as_mutable_span(C &c) { return std::span<typename C::value_type>(c); }

}  // namespace libspill

#ifndef LIBSPILL_NO_SHORT_NAMESPACE
namespace ls = libspill;
#endif

#endif  // LIBSPILL_HPP
