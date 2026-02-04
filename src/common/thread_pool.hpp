#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace xdp {

// High-performance thread pool optimized for PCAP processing workloads
class ThreadPool {
public:
  explicit ThreadPool(size_t num_threads = 0) : stop_(false) {
    if (num_threads == 0) {
      num_threads = std::thread::hardware_concurrency();
      if (num_threads == 0) num_threads = 4;
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
      workers_.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] {
              return stop_ || !tasks_.empty();
            });
            if (stop_ && tasks_.empty()) {
              return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          task();
          active_tasks_--;
        }
      });
    }
  }

  ~ThreadPool() {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      stop_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
      worker.join();
    }
  }

  // Non-copyable, non-movable
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  // Enqueue a task and return a future
  template <typename F, typename... Args>
  auto enqueue(F&& f, Args&&... args)
      -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> result = task->get_future();
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stop_) {
        throw std::runtime_error("enqueue on stopped ThreadPool");
      }
      active_tasks_++;
      tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return result;
  }

  // Wait for all tasks to complete
  void wait_all() {
    while (active_tasks_ > 0 || !tasks_.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  [[nodiscard]] size_t thread_count() const noexcept {
    return workers_.size();
  }

  [[nodiscard]] size_t pending_tasks() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
  }

  [[nodiscard]] size_t active_tasks() const noexcept {
    return active_tasks_.load();
  }

private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  mutable std::mutex queue_mutex_;
  std::condition_variable condition_;
  std::atomic<bool> stop_;
  std::atomic<size_t> active_tasks_{0};
};

// RAII wrapper for parallel for loops
template <typename Iterator, typename Func>
void parallel_for(ThreadPool& pool, Iterator begin, Iterator end, Func&& func) {
  std::vector<std::future<void>> futures;
  futures.reserve(std::distance(begin, end));

  for (Iterator it = begin; it != end; ++it) {
    futures.push_back(pool.enqueue(std::forward<Func>(func), *it));
  }

  for (auto& f : futures) {
    f.wait();
  }
}

// Partition work into chunks for better cache locality
template <typename Func>
void parallel_for_range(ThreadPool& pool, size_t begin, size_t end,
                        size_t chunk_size, Func&& func) {
  std::vector<std::future<void>> futures;

  for (size_t i = begin; i < end; i += chunk_size) {
    size_t chunk_end = std::min(i + chunk_size, end);
    futures.push_back(pool.enqueue([&func, i, chunk_end] {
      for (size_t j = i; j < chunk_end; ++j) {
        func(j);
      }
    }));
  }

  for (auto& f : futures) {
    f.wait();
  }
}

} // namespace xdp
