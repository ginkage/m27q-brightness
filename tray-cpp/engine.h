#pragma once

#include "ddc.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace bright {

// How long input must be quiet before the authoritative, verified write happens.
constexpr int kQuietMs = 200;

// Serialises DDC/CI traffic onto one background thread and makes the UI feel
// instant despite a hard ~35-60 ms floor per transaction.
//
// While input is arriving, each display keeps a single latest-desired slot rather
// than a queue, and the worker fires unverified writes. Each SetVCPFeature call
// blocks long enough to pace itself, so the panel tracks the slider and stale
// intermediate values are overwritten instead of backing up.
//
// Once input has been quiet for kQuietMs, the worker does one authoritative write
// with read-back verification. That is the only place correctness is enforced.
class Engine {
public:
    using Callback = std::function<void(int, DWORD)>;

    ~Engine() {
        stop_ = true;
        {
            std::lock_guard<std::mutex> lock(gate_);
            wake_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        set_.Close();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static std::unique_ptr<Engine> Open() {
        ddc::DisplaySet set = ddc::DisplaySet::Open();

        std::vector<Channel> channels;
        auto& displays = set.displays();
        for (size_t i = 0; i < displays.size(); ++i) {
            DWORD current = 0;
            if (!displays[i].TryRead(&current, nullptr))
                continue;  // present but not talking DDC/CI
            Channel ch;
            ch.index = static_cast<int>(i);
            ch.desired = current;
            ch.last_sent = current;
            ch.confirmed = current;
            channels.push_back(ch);
        }
        if (channels.empty()) return nullptr;

        return std::unique_ptr<Engine>(new Engine(std::move(set), std::move(channels)));
    }

    int count() const { return static_cast<int>(channels_.size()); }

    const std::wstring& NameOf(int i) const {
        return set_.displays()[channels_[i].index].name();
    }

    DWORD MaxOf(int i) const {
        return set_.displays()[channels_[i].index].max();
    }

    // The optimistic value: what the user asked for, not necessarily on the panel yet.
    DWORD ValueOf(int i) const {
        std::lock_guard<std::mutex> lock(gate_);
        return channels_[i].desired;
    }

    void Set(int i, DWORD value) { Store(i, std::min(value, MaxOf(i)), false); }

    // Like Set, but always reaches the panel with a verified write, even when the
    // cached value already matches. The panel can change behind the app's back -
    // it may wake from deep sleep at full brightness - so an explicit command must
    // not be skipped on the strength of a cache that might be stale.
    void Force(int i, DWORD value) { Store(i, std::min(value, MaxOf(i)), true); }

    void Nudge(int i, int delta) {
        DWORD clamped;
        {
            std::lock_guard<std::mutex> lock(gate_);
            long long next = static_cast<long long>(channels_[i].desired) + delta;
            long long limit = static_cast<long long>(set_.displays()[channels_[i].index].max());
            clamped = static_cast<DWORD>(std::clamp<long long>(next, 0, limit));
            if (channels_[i].desired == clamped && !channels_[i].dirty) return;
            channels_[i].desired = clamped;
            channels_[i].dirty = true;
            wake_ = true;
        }
        cv_.notify_one();
        if (on_target_changed_) on_target_changed_(i, clamped);
    }

    // Invoked on the worker thread. The UI implementation posts a message.
    void set_on_confirmed(Callback cb) { on_confirmed_ = std::move(cb); }

    // Invoked synchronously on the calling thread.
    void set_on_target_changed(Callback cb) { on_target_changed_ = std::move(cb); }

private:
    // Never a real reading, so VerifySettled always treats the channel as unconfirmed.
    static constexpr DWORD kUnknown = ~DWORD{0};

    struct Channel {
        int index = 0;
        DWORD desired = 0;    // guarded by gate_
        bool dirty = false;   // guarded by gate_
        bool forced = false;  // guarded by gate_
        DWORD last_sent = 0;  // worker thread only
        DWORD confirmed = 0;  // worker thread only
    };

    void Store(int i, DWORD clamped, bool force) {
        {
            std::lock_guard<std::mutex> lock(gate_);
            if (!force && channels_[i].desired == clamped && !channels_[i].dirty) return;
            channels_[i].desired = clamped;
            channels_[i].dirty = true;
            if (force) channels_[i].forced = true;
            wake_ = true;
        }
        cv_.notify_one();
        if (on_target_changed_) on_target_changed_(i, clamped);
    }

    Engine(ddc::DisplaySet set, std::vector<Channel> channels)
        : set_(std::move(set)), channels_(std::move(channels)) {
        worker_ = std::thread(&Engine::Run, this);
    }

    void Run() {
        while (!stop_) {
            bool wrote = false;

            // Drain: keep firing the newest value for whichever display is dirty.
            for (;;) {
                int idx = -1;
                DWORD value = 0;
                bool forced = false;
                {
                    std::lock_guard<std::mutex> lock(gate_);
                    for (size_t i = 0; i < channels_.size(); ++i) {
                        if (!channels_[i].dirty) continue;
                        channels_[i].dirty = false;
                        forced = channels_[i].forced;
                        channels_[i].forced = false;
                        value = channels_[i].desired;
                        idx = static_cast<int>(i);
                        break;
                    }
                }
                if (idx < 0 || stop_) break;

                set_.displays()[channels_[idx].index].WriteUnverified(value);
                channels_[idx].last_sent = value;
                if (forced) channels_[idx].confirmed = kUnknown;
                wrote = true;
            }

            if (wrote) {
                std::unique_lock<std::mutex> lock(gate_);
                // New input inside the quiet window means the user is still moving:
                // go back to draining and postpone verification.
                bool signaled = cv_.wait_for(lock, std::chrono::milliseconds(kQuietMs),
                                             [this] { return wake_ || stop_.load(); });
                wake_ = false;
                lock.unlock();
                if (!signaled && !stop_) VerifySettled();
                continue;
            }

            std::unique_lock<std::mutex> lock(gate_);
            cv_.wait(lock, [this] { return wake_ || stop_.load(); });
            wake_ = false;
        }
    }

    void VerifySettled() {
        for (size_t i = 0; i < channels_.size() && !stop_; ++i) {
            Channel& ch = channels_[i];
            if (ch.last_sent == ch.confirmed) continue;

            // Panel refused: leave confirmed alone so the next settle retries.
            if (!set_.displays()[ch.index].WriteVerified(ch.last_sent)) continue;

            ch.confirmed = ch.last_sent;
            if (on_confirmed_) on_confirmed_(static_cast<int>(i), ch.confirmed);
        }
    }

    ddc::DisplaySet set_;
    std::vector<Channel> channels_;
    mutable std::mutex gate_;
    std::condition_variable cv_;
    bool wake_ = false;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    Callback on_confirmed_;
    Callback on_target_changed_;
};

}  // namespace bright
