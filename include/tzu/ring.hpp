// Lock-free SPSC ring: producer = your colo decoder, consumer = dashboard.
// Lives in heap memory (demo / same process) or POSIX shared memory (separate processes).
#pragma once
#include "events.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#if defined(__unix__) || defined(__APPLE__)
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace tzu {

struct RingHeader {
    uint64_t magic;
    uint64_t capacity;                         // power of two
    uint64_t evSize;
    alignas(64) std::atomic<uint64_t> wr;      // producer index
    alignas(64) std::atomic<uint64_t> rd;      // consumer index
    alignas(64) std::atomic<uint64_t> dropped; // producer drops when full (never blocks the hot path)
};
constexpr uint64_t kRingMagic = 0x545A555249474E31ull;

class Ring {
public:
    Ring() = default;
    static Ring heap(uint64_t capPow2) {
        void* m = alignedAlloc(bytes(capPow2));
        if (!m) throw std::bad_alloc();
        std::memset(m, 0, bytes(capPow2));
        Ring r; r.init(m, capPow2, true); r.heapOwned_ = true; return r;
    }
#if defined(__unix__) || defined(__APPLE__)
    // Producer side: create (or recreate) /name
    static Ring createShm(const std::string& name, uint64_t capPow2) {
        shm_unlink(name.c_str());
        int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd < 0) throw std::runtime_error("shm_open create failed");
        if (ftruncate(fd, off_t(bytes(capPow2))) != 0) throw std::runtime_error("ftruncate failed");
        void* m = mmap(nullptr, bytes(capPow2), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) throw std::runtime_error("mmap failed");
        Ring r; r.init(m, capPow2, true); r.mapLen_ = bytes(capPow2); return r;
    }
    // Consumer side: attach to existing /name
    static Ring openShm(const std::string& name) {
        int fd = shm_open(name.c_str(), O_RDWR, 0666);
        if (fd < 0) throw std::runtime_error("shm_open attach failed (is the producer running?)");
        struct stat st{}; fstat(fd, &st);
        void* m = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) throw std::runtime_error("mmap failed");
        auto* h = static_cast<RingHeader*>(m);
        if (h->magic != kRingMagic || h->evSize != sizeof(Event)) throw std::runtime_error("ring header mismatch");
        Ring r; r.init(m, h->capacity, false); r.mapLen_ = size_t(st.st_size); return r;
    }
#elif defined(_WIN32)
    // Producer side: create (or recreate) a named, page-file-backed mapping
    static Ring createShm(const std::string& name, uint64_t capPow2) {
        const uint64_t sz = bytes(capPow2);
        HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                          DWORD(sz >> 32), DWORD(sz & 0xFFFFFFFFull), name.c_str());
        if (!hMap) throw std::runtime_error("CreateFileMapping failed");
        void* m = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size_t(sz));
        if (!m) { CloseHandle(hMap); throw std::runtime_error("MapViewOfFile failed"); }
        std::memset(m, 0, size_t(sz));
        Ring r; r.init(m, capPow2, true); r.hMap_ = hMap; return r;
    }
    // Consumer side: attach to an existing named mapping
    static Ring openShm(const std::string& name) {
        HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!hMap) throw std::runtime_error("OpenFileMapping failed (is the producer running?)");
        void* m = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);   // 0 => map the whole mapping
        if (!m) { CloseHandle(hMap); throw std::runtime_error("MapViewOfFile failed"); }
        auto* h = static_cast<RingHeader*>(m);
        if (h->magic != kRingMagic || h->evSize != sizeof(Event)) {
            UnmapViewOfFile(m); CloseHandle(hMap); throw std::runtime_error("ring header mismatch");
        }
        Ring r; r.init(m, h->capacity, false); r.hMap_ = hMap; return r;
    }
#endif
    Ring(Ring&& o) noexcept { *this = std::move(o); }
    Ring& operator=(Ring&& o) noexcept {
        release(); h_ = o.h_; buf_ = o.buf_; mask_ = o.mask_; heapOwned_ = o.heapOwned_; mapLen_ = o.mapLen_;
        o.h_ = nullptr; o.buf_ = nullptr; o.heapOwned_ = false; o.mapLen_ = 0;
#if defined(_WIN32)
        hMap_ = o.hMap_; o.hMap_ = nullptr;
#endif
        return *this;
    }
    Ring(const Ring&) = delete; Ring& operator=(const Ring&) = delete;
    ~Ring() { release(); }

    bool tryPush(const Event& e) {                       // producer only
        uint64_t w = h_->wr.load(std::memory_order_relaxed);
        uint64_t r = h_->rd.load(std::memory_order_acquire);
        if (w - r > mask_) { h_->dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        buf_[w & mask_] = e;
        h_->wr.store(w + 1, std::memory_order_release);
        return true;
    }
    bool tryPop(Event& e) {                              // consumer only
        uint64_t r = h_->rd.load(std::memory_order_relaxed);
        uint64_t w = h_->wr.load(std::memory_order_acquire);
        if (r == w) return false;
        e = buf_[r & mask_];
        h_->rd.store(r + 1, std::memory_order_release);
        return true;
    }
    template <class F> size_t drain(F&& fn, size_t maxN) {
        Event e; size_t n = 0;
        while (n < maxN && tryPop(e)) { fn(e); ++n; }
        return n;
    }
    uint64_t dropped() const { return h_->dropped.load(std::memory_order_relaxed); }
    uint64_t backlog() const { return h_->wr.load(std::memory_order_acquire) - h_->rd.load(std::memory_order_acquire); }
    bool valid() const { return h_ != nullptr; }

private:
    static size_t bytes(uint64_t cap) { return sizeof(RingHeader) + cap * sizeof(Event); }
    static void* alignedAlloc(size_t n) {
#if defined(_WIN32)
        return _aligned_malloc(n, 64);
#else
        return std::aligned_alloc(64, n);
#endif
    }
    static void alignedFree(void* p) {
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
    void init(void* m, uint64_t cap, bool fresh) {
        h_ = static_cast<RingHeader*>(m);
        if (fresh) {
            h_->magic = kRingMagic; h_->capacity = cap; h_->evSize = sizeof(Event);
            h_->wr.store(0); h_->rd.store(0); h_->dropped.store(0);
        }
        buf_ = reinterpret_cast<Event*>(static_cast<char*>(m) + sizeof(RingHeader));
        mask_ = cap - 1;
    }
    void release() {
        if (!h_) return;
        if (heapOwned_) alignedFree(h_);
#if defined(__unix__) || defined(__APPLE__)
        else if (mapLen_) munmap(h_, mapLen_);
#elif defined(_WIN32)
        else if (hMap_) { UnmapViewOfFile(h_); CloseHandle(hMap_); hMap_ = nullptr; }
#endif
        h_ = nullptr;
    }
    RingHeader* h_ = nullptr; Event* buf_ = nullptr; uint64_t mask_ = 0; bool heapOwned_ = false; size_t mapLen_ = 0;
#if defined(_WIN32)
    HANDLE hMap_ = nullptr;
#endif
};

}  // namespace tzu
