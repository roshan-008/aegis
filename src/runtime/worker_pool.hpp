#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace aegis::runtime {

class WorkerPool {
public:
    explicit WorkerPool(size_t threads = std::thread::hardware_concurrency(),
                        bool pin_linux = true) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this, i, pin_linux] { worker(i, pin_linux); });
    }
    ~WorkerPool() {
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    template <typename Fn>
    std::future<void> submit(Fn&& fn) {
        auto task = std::make_shared<std::packaged_task<void()>>(
            std::forward<Fn>(fn));
        auto future = task->get_future();
        {
            std::lock_guard lock(mu_);
            if (stop_) throw std::runtime_error("submit on stopped worker pool");
            queue_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }
    size_t size() const { return workers_.size(); }

private:
    void worker(size_t index, bool pin_linux) {
#if defined(__linux__)
        if (pin_linux) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(index % std::max(1u, std::thread::hardware_concurrency()), &set);
            (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        }
#else
        (void)index;
        (void)pin_linux;
#endif
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace aegis::runtime
