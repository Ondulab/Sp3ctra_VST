/**
 * @file VideoParallel.h
 * @brief Chunked parallel-for for the VIDEO MIX render passes.
 *
 * The waterfall passes (warp/age, blur, composite, affine blit) are all
 * INDEPENDENT PER OUTPUT ROW, so they parallelise by simply splitting the row
 * range into contiguous chunks. They used to run single-threaded on the mixer's
 * render thread, which made one core the whole video budget while the other
 * eleven idled — the dominant cause of the "everything lags while recording"
 * report (recording removed the resolution cap, see VideoMixerComponent).
 *
 * Contract:
 *   parallelChunks(begin, end, minRows, fn) calls fn(chunkIndex, y0, y1) on
 *   several threads, once per chunk, with [y0,y1) contiguous, ascending and
 *   disjoint, covering [begin,end). `chunkIndex` < chunkCount() lets a caller
 *   index PER-CHUNK scratch so no two threads share a scratch row buffer.
 *   The call returns only once every chunk has finished (fork/join).
 *
 * `minRows` is the smallest row count worth handing to a thread: a range
 * shorter than one chunk's worth runs INLINE on the calling thread (chunk 0),
 * so a small preview never pays dispatch overhead.
 *
 * Backend: libdispatch (dispatch_apply_f, DISPATCH_APPLY_AUTO) on Apple — it
 * reuses the system's wide concurrent queue and adapts to the QoS of the
 * calling thread. Elsewhere: a persistent worker pool (threads are created
 * once, never per frame). NEVER call this from the RT audio thread: it blocks.
 */
#pragma once

#include <algorithm>
#include <thread>
#include <cstddef>

#if defined(__APPLE__)
 #include <dispatch/dispatch.h>
#else
 #include <atomic>
 #include <condition_variable>
 #include <functional>
 #include <mutex>
 #include <vector>
#endif

namespace videoparallel
{
    /** Upper bound on the number of chunks — i.e. on the concurrency a single
     *  pass can claim. Capped at 8 so a 12-core machine keeps cores for the
     *  audio thread, the message thread and the encoder. */
    inline int chunkCount() noexcept
    {
        static const int n = [] {
            const unsigned hw = std::thread::hardware_concurrency();
            return (int) std::max(1u, std::min(8u, hw == 0 ? 4u : hw - 1u));
        }();
        return n;
    }

#if ! defined(__APPLE__)
    //==========================================================================
    // Persistent worker pool (non-Apple). One fork/join barrier per call; the
    // threads live for the process, so a 60 fps pass never creates a thread.
    class Pool
    {
    public:
        static Pool& get() { static Pool p; return p; }

        void run(const std::function<void(int)>& body, int numChunks)
        {
            {
                std::unique_lock<std::mutex> lk(m_);
                body_      = &body;
                chunks_    = numChunks;
                nextChunk_ = 1;              // chunk 0 is run by the caller itself
                pending_   = numChunks;
                ++generation_;
            }
            cv_.notify_all();

            body(0);                         // the calling thread takes a share
            finishOne();

            std::unique_lock<std::mutex> lk(m_);
            doneCv_.wait(lk, [this] { return pending_ == 0; });
            body_ = nullptr;
        }

    private:
        Pool()
        {
            const int n = chunkCount() - 1;
            for (int i = 0; i < n; ++i)
                workers_.emplace_back([this] { workerLoop(); });
        }

        ~Pool()
        {
            { std::unique_lock<std::mutex> lk(m_); quit_ = true; }
            cv_.notify_all();
            for (auto& t : workers_) if (t.joinable()) t.join();
        }

        void finishOne()
        {
            std::unique_lock<std::mutex> lk(m_);
            if (--pending_ == 0) { lk.unlock(); doneCv_.notify_all(); }
        }

        void workerLoop()
        {
            uint64_t seen = 0;
            for (;;)
            {
                const std::function<void(int)>* body = nullptr;
                int k = -1;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return quit_ || (generation_ != seen && nextChunk_ < chunks_); });
                    if (quit_) return;
                    k    = nextChunk_++;
                    body = body_;
                    if (nextChunk_ >= chunks_) seen = generation_;   // this batch is fully claimed
                }
                (*body)(k);
                finishOne();
            }
        }

        std::vector<std::thread> workers_;
        std::mutex m_;
        std::condition_variable cv_, doneCv_;
        const std::function<void(int)>* body_ { nullptr };
        int chunks_ { 0 }, nextChunk_ { 0 }, pending_ { 0 };
        uint64_t generation_ { 0 };
        bool quit_ { false };
    };
#endif

    /** Split [begin,end) into contiguous chunks and run `fn(chunk, y0, y1)`
     *  concurrently. Returns once every chunk has completed. */
    template <typename Fn>
    void parallelChunks(int begin, int end, int minRows, Fn&& fn)
    {
        const int total = end - begin;
        if (total <= 0) return;

        int chunks = chunkCount();
        if (minRows > 0) chunks = std::min(chunks, std::max(1, total / std::max(1, minRows)));
        if (chunks <= 1) { fn(0, begin, end); return; }

        struct Ctx { Fn* fn; int begin; int total; int chunks; };
        Ctx ctx { &fn, begin, total, chunks };

        auto bounds = [](const Ctx& c, int k, int& y0, int& y1)
        {
            y0 = c.begin + (int) ((long long) c.total * (long long) k       / c.chunks);
            y1 = c.begin + (int) ((long long) c.total * (long long) (k + 1) / c.chunks);
        };

    #if defined(__APPLE__)
        // Captureless lambda → function pointer, so no block runtime is needed.
        dispatch_apply_f((size_t) chunks, DISPATCH_APPLY_AUTO, &ctx,
            [](void* p, size_t k)
            {
                auto* c = static_cast<Ctx*>(p);
                const int y0 = c->begin + (int) ((long long) c->total * (long long) k       / c->chunks);
                const int y1 = c->begin + (int) ((long long) c->total * (long long) (k + 1) / c->chunks);
                if (y1 > y0) (*c->fn)((int) k, y0, y1);
            });
        (void) bounds;
    #else
        std::function<void(int)> body = [&](int k)
        {
            int y0 = 0, y1 = 0;
            bounds(ctx, k, y0, y1);
            if (y1 > y0) fn(k, y0, y1);
        };
        Pool::get().run(body, chunks);
    #endif
    }
}
