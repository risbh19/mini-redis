#include "net/thread_pool.hpp"

#include <pthread.h>

#include <csignal>
#include <exception>
#include <stdexcept>

#include "util/logger.hpp"
namespace mr {
ThreadPool::ThreadPool(std::size_t workers) {
  // Workers inherit blocked termination signals; the main thread handles them.
  sigset_t blocked{}, previous{};
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0)
    throw std::runtime_error("cannot set worker signal mask");
  try {
    for (std::size_t i = 0; i < workers; ++i)
      workers_.emplace_back([this] {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          try {
            task();
          } catch (const std::exception& e) {
            log("error", e.what());
          }
        }
      });
  } catch (...) {
    stop();
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    throw;
  }
  ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
ThreadPool::~ThreadPool() { stop(); }
void ThreadPool::submit(std::function<void()> task) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw std::runtime_error("thread pool stopped");
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}
void ThreadPool::stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
}
}  // namespace mr
