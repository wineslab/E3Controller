#ifndef E3_JBPF_DISPATCHER_H
#define E3_JBPF_DISPATCHER_H

/*
 * JbpfDispatcher: central routing of jbpf output buffers to service models.
 *
 * Instead of each service model running its own polling thread,
 * the dispatcher provides a single poll() method driven by the main loop.
 * Service models register a stream_id + callback; the dispatcher routes
 * incoming buffers to the right SM and always releases them.
 */

#include <functional>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <mutex>

extern "C" {
#include "jbpf_io.h"
#include "jbpf_io_channel.h"
#include "jbpf_common.h"
}

class JbpfDispatcher {
public:
    // Callback signature: the SM receives (stream_id, bufs, num_bufs).
    // By default the SM must NOT release buffers — the dispatcher frees them
    // after the handler returns. A stream registered with defer_release=true
    // instead takes OWNERSHIP of its buffers: the dispatcher will NOT free them,
    // and the handler (or whatever it hands them to) must call
    // jbpf_io_channel_release_buf() on each exactly once. SlotIqPipeline uses
    // this to queue the buffer POINTER to its worker (zero-copy) and release it
    // after the consumer fan-out, instead of memcpy'ing the whole ~733 KB slot
    // on the poll thread.
    using BufferHandler = std::function<void(struct jbpf_io_stream_id*, void**, int)>;

    explicit JbpfDispatcher(jbpf_io_ctx* io_ctx) : io_ctx_(io_ctx) {}

    // Register a handler for a specific stream_id. With defer_release=true the
    // handler takes ownership of the buffers (the dispatcher will not free them).
    void register_stream(const struct jbpf_io_stream_id& stream_id, BufferHandler handler,
                         bool defer_release = false) {
        std::lock_guard<std::mutex> lock(mu_);
        handlers_[StreamKey(stream_id)] = StreamEntry{std::move(handler), defer_release};
    }

    // Unregister a handler.
    void unregister_stream(const struct jbpf_io_stream_id& stream_id) {
        std::lock_guard<std::mutex> lock(mu_);
        handlers_.erase(StreamKey(stream_id));
    }

    // Poll jbpf for output buffers — call this from the main loop.
    void poll() {
        jbpf_io_channel_handle_out_bufs(io_ctx_, dispatch_callback, this);
    }

private:
    // Hashable wrapper around jbpf_io_stream_id
    struct StreamKey {
        struct jbpf_io_stream_id id;

        StreamKey() { std::memset(&id, 0, sizeof(id)); }
        explicit StreamKey(const struct jbpf_io_stream_id& sid) { std::memcpy(&id, &sid, sizeof(id)); }

        bool operator==(const StreamKey& other) const {
            return std::memcmp(&id, &other.id, sizeof(id)) == 0;
        }
    };

    struct StreamKeyHash {
        std::size_t operator()(const StreamKey& key) const {
            // FNV-1a hash over the 16 stream_id bytes
            std::size_t h = 14695981039346656037ULL;
            for (int i = 0; i < JBPF_IO_STREAM_ID_LEN; i++) {
                h ^= key.id.id[i];
                h *= 1099511628211ULL;
            }
            return h;
        }
    };

    // Per-stream handler + whether the handler owns (defers) buffer release.
    struct StreamEntry {
        BufferHandler handler;
        bool          defer_release{false};
    };

    jbpf_io_ctx* io_ctx_;
    std::mutex mu_;
    std::unordered_map<StreamKey, StreamEntry, StreamKeyHash> handlers_;

    // Static C callback — thin trampoline to the dispatcher instance
    static void dispatch_callback(
        struct jbpf_io_channel* /*io_channel*/,
        struct jbpf_io_stream_id* stream_id,
        void** bufs,
        int num_bufs,
        void* ctx)
    {
        auto* self = static_cast<JbpfDispatcher*>(ctx);
        self->dispatch(stream_id, bufs, num_bufs);
    }

    void dispatch(struct jbpf_io_stream_id* stream_id, void** bufs, int num_bufs) {
        if (!stream_id || num_bufs <= 0) return;

        BufferHandler handler;
        bool          defer_release = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = handlers_.find(StreamKey(*stream_id));
            if (it != handlers_.end()) {
                handler       = it->second.handler;
                defer_release = it->second.defer_release;
            }
        }

        if (handler) {
            // Route to the registered SM
            handler(stream_id, bufs, num_bufs);
        } else {
            // No handler — log and drop
            std::printf("[Dispatcher] %d buf(s) from unregistered stream: ", num_bufs);
            for (int i = 0; i < JBPF_IO_STREAM_ID_LEN; i++) {
                std::printf("%02x", stream_id->id[i]);
            }
            std::printf("\n");
        }

        // Release all buffers — unless a registered handler took ownership
        // (defer_release=true), in which case it releases each exactly once
        // itself. An unregistered stream is always released here.
        if (!handler || !defer_release) {
            for (int i = 0; i < num_bufs; i++) {
                jbpf_io_channel_release_buf(bufs[i]);
            }
        }
    }
};

#endif /* E3_JBPF_DISPATCHER_H */
