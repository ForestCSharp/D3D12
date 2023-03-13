//	The MIT License (MIT)
//	
//	Copyright (c) 2015 Kingsley Chen
//	
//	Permission is hereby granted, free of charge, to any person obtaining a copy
//	of this software and associated documentation files (the "Software"), to deal
//	in the Software without restriction, including without limitation the rights
//	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//	copies of the Software, and to permit persons to whom the Software is
//	furnished to do so, subject to the following conditions:
//	
//	The above copyright notice and this permission notice shall be included in all
//	copies or substantial portions of the Software.
//	
//	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//	SOFTWARE.

#pragma once

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "Common.h"

template<typename C>
class ThreadsJoiner {
public:
    explicit ThreadsJoiner(C& threads) noexcept
        : threads_(threads)
    {}

    ~ThreadsJoiner()
    {
        for (auto& th : threads_) {
            th.join();
        }
    }

    DISALLOW_COPY(ThreadsJoiner);

    DEFAULT_MOVE(ThreadsJoiner);

private:
    C& threads_;
};

enum class TaskShutdownBehavior {
    BlockShutdown,
    SkipOnShutdown
};

class ThreadPool {
private:
    using Task = std::pair<std::function<void()>, TaskShutdownBehavior>;

public:
	explicit ThreadPool(size_t num)
	: running_(true)
	, thread_joiner_(threads_)
	{
		threads_.reserve(num);
		for (size_t i = 0; i < num; ++i) {
			threads_.emplace_back(std::bind(&ThreadPool::RunInThread, this));
		}
	}

	~ThreadPool()
	{
		{
			std::lock_guard<std::mutex> lock(pool_mutex_);
			running_ = false;
		}
	
		not_empty_.notify_all();
	}

    DISALLOW_COPY(ThreadPool);

    template<typename F, typename... Args>
	std::future<std::invoke_result_t<F,Args...>> PostTask(F&& fn, Args&&... args)
    {
        return PostTaskWithShutdownBehavior(TaskShutdownBehavior::BlockShutdown,
                                            std::forward<F>(fn),
                                            std::forward<Args>(args)...);
    }

    template<typename F, typename... Args>
	std::future<std::invoke_result_t<F,Args...>> PostTaskWithShutdownBehavior(
        TaskShutdownBehavior behavior, F&& fn, Args&&... args)
    {
		using R = std::invoke_result_t<F,Args...>;

        // We have to manage the packaged_task with shared_ptr, because std::function<>
        // requires being copy-constructible and copy-assignable.
        auto task_fn = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(fn), std::forward<Args>(args)...));

        auto future = task_fn->get_future();

        Task task([task_fn=std::move(task_fn)] { (*task_fn)(); }, behavior);

        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
			assert(running_);
            task_queue_.push_back(std::move(task));
        }

        not_empty_.notify_one();

        return future;
    }

private:
    void RunInThread()
	{
		while (true) {
			Task task(RetrieveTask());
	
			// The pool is going to shutdown.
			if (!task.first) 
			{
				return;
			}
	
			task.first();
		}
	}

    Task RetrieveTask()
	{
		Task task;
	
		std::unique_lock<std::mutex> lock(pool_mutex_);
		not_empty_.wait(lock, [this] { return !running_ || !task_queue_.empty(); });
	
		while (!task_queue_.empty()) {
			if (!running_ && task_queue_.front().second == TaskShutdownBehavior::SkipOnShutdown) {
				task_queue_.pop_front();
				continue;
			}
	
			task = std::move(task_queue_.front());
			task_queue_.pop_front();
			break;
		}
	
		return task;
	}

private:
    std::mutex pool_mutex_;
    std::condition_variable not_empty_;
    std::deque<Task> task_queue_;
    bool running_;
    std::vector<std::thread> threads_;
    ThreadsJoiner<decltype(threads_)> thread_joiner_;
};
