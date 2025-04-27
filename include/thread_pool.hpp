#ifndef THREAD_POOL
#define THREAD_POOL

#include "spsc_queue.hpp"

#include <functional>
#include <future>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <memory>

class ThreadPool
{
  using Job = std::function<void()>;

  std::atomic<bool> m_enabled{ false };
  std::atomic<bool> m_terminating{ false };
  std::size_t m_nthreads;

  std::vector<std::thread> m_pool;
  SPSCQueue<Job, 1000> m_job;

  struct TaskWrapper {
    std::shared_ptr<std::packaged_task<void()>> task;
    std::shared_ptr<std::atomic<bool>> executed;

    TaskWrapper(std::shared_ptr<std::packaged_task<void()>> t)
      : task(std::move(t)), executed(std::make_shared<std::atomic<bool>>(false)) {}

    TaskWrapper(const TaskWrapper& other) = default;
    TaskWrapper& operator=(const TaskWrapper& other) = default;

    void operator()() {
      if (!executed->exchange(true)) {
        (*task)();
      }
    }
  };

  void init()
  {
    for (std::size_t i = 0; i < m_nthreads; ++i)
    {
      try
      {
        m_pool.emplace_back(
          std::thread{
            [this]()
            {
              while (m_enabled && !m_terminating)
              {
                Job job;
                if (!m_job.wait_pop_timeout(job, std::chrono::milliseconds(100)))
                {
                  continue;
                }
                if (job) {
                  try {
                    job();
                  } catch (const std::exception& e) {
                    std::cerr << "Exception in thread " << std::this_thread::get_id() << ": " << e.what() << "\n";
                  }
                }
              }
            }
          }
        );
      }
      catch (const std::exception& e)
      {
        std::cerr << "Failed to create thread: " << e.what() << "\n";
        m_enabled.store(false);
        m_terminating.store(true);
      }
    }
  }

public:
  explicit ThreadPool(std::size_t nthreads = std::thread::hardware_concurrency(), bool enabled = true)
    : m_nthreads{ nthreads }, m_enabled{ enabled }, m_terminating{ false }, m_job{}
  {
    m_pool.reserve(nthreads);
    init();
  }

  ~ThreadPool() noexcept
  {
    stop();
  }

  ThreadPool(ThreadPool const&) = delete;
  ThreadPool& operator=(const ThreadPool& rhs) = delete;

  void start()
  {
    m_enabled.store(true);
    m_terminating.store(false);
    init();
  }

  void stop() noexcept
  {
    m_enabled.store(false);
    m_terminating.store(true);
    join();
  }

  void join() noexcept
  {
    m_terminating.store(true);
    // Clear queue first
    Job job;
    while (m_job.pop(job)) {}
    // Push one dummy job per thread
    for (std::size_t i = 0; i < m_nthreads; ++i)
    {
      while (!m_job.push([](){})) {
        std::this_thread::yield();
      }
    }
    // Join all threads
    for (auto& t : m_pool)
    {
      if (t.joinable())
      {
        t.join();
      }
    }
    m_pool.clear();
    m_terminating.store(false);
  }

  template<typename Callable, typename... Args>
  decltype(auto) push(Callable&& f, Args&&... args)
  {
    using ResultType = std::invoke_result_t<Callable, Args...>;
    using Packed = std::packaged_task<ResultType()>;
    auto task = std::make_shared<Packed>([f = std::forward<Callable>(f), args_copy = std::make_tuple(std::forward<Args>(args)...)]() mutable {
      return std::apply(f, std::move(args_copy));
    });
    auto wrapper = TaskWrapper(task);
    auto future = task->get_future();
    while (!m_job.push(wrapper)) {
      std::cerr << "ThreadPool: Queue full, size=" << m_job.size() << "\n";
      std::this_thread::yield();
    }
    return future;
  }
};

#endif