#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Sleep for `dur`, waking immediately if `st` is stop-requested instead of
// only after the full duration elapses. For poll loops spawned via
// TaskRegistry's stop_token overload — replaces std::this_thread::sleep_for
// so request_stop() (e.g. from TaskRegistry's destructor) is responsive
// rather than waiting out whatever interval the loop happened to be in.
template <typename Rep, typename Period>
void stopTokenSleep(std::stop_token st, std::chrono::duration<Rep, Period> dur) {
    std::condition_variable_any cv;
    std::mutex m;
    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, st, dur, [] { return false; });
}

// Shared by kairos, hermes and hephaestus (see shared/README.md). Replaces
// the old std::thread(...).detach() idiom used for background/fire-and-forget
// work: threads are tracked instead of abandoned, reaped lazily as they
// finish, and joined by the destructor if any are still outstanding at
// process teardown rather than silently left running.
class TaskRegistry {
public:
    // One instance per process — hermes/hephaestus/kairos are separate
    // binaries, so this never crosses a service boundary.
    static TaskRegistry& global() {
        static TaskRegistry inst;
        return inst;
    }

    // Fire-and-forget background work, tracked instead of detach()ed. `fn`
    // may optionally take a std::stop_token as its sole argument (the same
    // convention std::jthread itself uses) to observe request_stop() — issued
    // by the destructor — for long-running poll loops; anything else is
    // treated as a one-shot task with no cancellation. Takes `fn` by forwarding
    // reference and hands it straight to jthread rather than going through
    // std::function, so move-only captures (e.g. a std::future) work fine.
    template <typename F>
    void spawn(F&& fn) {
        using Fn = std::decay_t<F>;
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mtx_);
        reapLocked();
        tasks_.push_back({
            std::jthread([fn = std::forward<F>(fn), done](auto&&... args) mutable {
                if constexpr (std::is_invocable_v<Fn, std::stop_token>) {
                    fn(std::forward<decltype(args)>(args)...);
                } else {
                    fn();
                }
                done->store(true, std::memory_order_release);
            }),
            done
        });
    }

    ~TaskRegistry() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& t : tasks_) {
            t.thread.request_stop();
        }
        tasks_.clear(); // ~jthread joins
    }

private:
    TaskRegistry() = default;
    TaskRegistry(const TaskRegistry&) = delete;
    TaskRegistry& operator=(const TaskRegistry&) = delete;

    struct Task {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // Caller must hold mtx_.
    void reapLocked() {
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if (it->done->load(std::memory_order_acquire)) {
                it = tasks_.erase(it); // ~jthread joins (already finished, returns immediately)
            } else {
                ++it;
            }
        }
    }

    std::mutex mtx_;
    std::vector<Task> tasks_;
};
