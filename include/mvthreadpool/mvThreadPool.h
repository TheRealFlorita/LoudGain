#ifndef MVTHREADPOOL_H
#define MVTHREADPOOL_H

#include <memory>
#include <mutex>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <future>
#include <functional>
#include <vector>
#include <queue>
#include <deque>
#include <iostream>

#if defined(_OS_WIN32_)
    #include <windows.h>
    #include <winbase.h>
#elif defined(_OS_UNIX_)
    #include <pthread.h>
#endif


namespace Marvel {

    //-----------------------------------------------------------------------------
    // mvThreadJoiner
    //-----------------------------------------------------------------------------
    class mvThreadJoiner
    {

    public:

        explicit mvThreadJoiner(std::vector<std::thread>& threads) : m_threads(threads)
        { }

        ~mvThreadJoiner()
        {
            for (auto& thread : m_threads)
                if (thread.joinable())
                    thread.join();
        }

    private:

        std::vector<std::thread>& m_threads;

    };

    //-----------------------------------------------------------------------------
    // mvFunctionWrapper
    //     - Reguired because packaged_task reguires movable function objects
    //-----------------------------------------------------------------------------
    class mvFunctionWrapper
    {
        struct impl_base 
        {
            virtual void call() = 0;
            virtual ~impl_base() = default;
        };

        template<typename F>
        struct impl_type : impl_base
        {
            F f;
            explicit impl_type(F&& f) : f(std::move(f)) {}
            void call() override { f(); }
        };

    public:

        mvFunctionWrapper() = default;

        template<typename F>
        mvFunctionWrapper(F&& f) : m_impl(new impl_type<F>(std::move(f))) {}

        mvFunctionWrapper(mvFunctionWrapper&& other) noexcept
            : m_impl(std::move(other.m_impl))
        {

        }

        mvFunctionWrapper& operator=(mvFunctionWrapper&& other)
        {
            m_impl = std::move(other.m_impl);
            return *this;
        }

        // delete copy constructor and assignment operator
        mvFunctionWrapper(const mvFunctionWrapper&) = delete;
        mvFunctionWrapper(mvFunctionWrapper&) = delete;
        mvFunctionWrapper& operator=(const mvFunctionWrapper&) = delete;

        void operator()()
        {
            m_impl->call();
        }

    private:

        std::unique_ptr<impl_base> m_impl;

    };

    //-----------------------------------------------------------------------------
    // mvWorkStealingQueue
    //-----------------------------------------------------------------------------
    class mvWorkStealingQueue
    {

    public:

        mvWorkStealingQueue() {}
        ~mvWorkStealingQueue() = default;

        // deleted copy constructor/assignment operator.
        mvWorkStealingQueue(const mvWorkStealingQueue& other) = delete;
        mvWorkStealingQueue& operator=(const mvWorkStealingQueue& other) = delete;

        void push(mvFunctionWrapper data)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(data));
        }

        bool empty() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_queue.empty();
        }

        bool try_pop(mvFunctionWrapper& res)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty())
                return false;

            res = std::move(m_queue.front());
            m_queue.pop_front();
            return true;
        }

        bool try_steal(mvFunctionWrapper& res)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty())
                return false;

            res = std::move(m_queue.back());
            m_queue.pop_back();
            return true;
        }

    private:

        std::deque<mvFunctionWrapper> m_queue;
        mutable std::mutex            m_mutex;

    };

    //-----------------------------------------------------------------------------
    // mvThreadPool
    //-----------------------------------------------------------------------------
    class mvThreadPool
    {

        typedef mvFunctionWrapper task_type;

    public:

        explicit mvThreadPool(unsigned threadcount = 0) : m_wait_for_idle(false), m_done(false), m_joiner(m_threads)
        {
            unsigned thread_count = threadcount;

            if (threadcount == 0)
                thread_count = std::thread::hardware_concurrency();

            try
            {
                for (unsigned i = 0; i < thread_count; ++i)
                    m_queues.push_back(std::make_unique<mvWorkStealingQueue>());

                for (unsigned i = 0; i < thread_count; ++i)
                    m_threads.emplace_back(&mvThreadPool::worker_thread, this, i);

                for (unsigned i = 0; i < thread_count; ++i)
                    m_idling.emplace_back(false);
            }
            catch (...)
            {
                m_done = true;
                throw;
            }
        }

        ~mvThreadPool() { m_done = true; }

        static const char* getVersion() { return "v0.3"; }

        template<typename F, typename ...Args>
        std::future<typename std::invoke_result<F, Args...>::type> submit(F f)
        {
            typedef typename std::invoke_result<F, Args...>::type result_type;
            std::packaged_task<result_type()> task(std::move(f));
            std::future<result_type> res(task.get_future());
            if (m_local_work_queue)
                m_local_work_queue->push(std::move(task));
            else
                m_pool_work_queue.push(std::move(task));

            return res;
        }

        void wait_for_idle()
        {
            m_wait_for_idle = true;
            bool finished = false;
            while (!finished)
            {
                finished = true;
                for (size_t i = 0; i < m_idling.size(); i++)
                    if (!m_idling[i])
                        finished = false;
                std::this_thread::yield();
            }

            m_wait_for_idle = false;
            for (size_t i = 0; i < m_idling.size(); i++)
                m_idling[i] = false;
        }

        void wait_for_finished()
        {
            wait_for_idle();
            m_done = true;
            for (auto& thread : m_threads)
                if (thread.joinable())
                    thread.join();
        }

    private:

        void worker_thread(unsigned index)
        {
            m_index = index;

            #if defined(_OS_WIN32_)
                unsigned max_count = std::thread::hardware_concurrency();
                unsigned core = m_index*2;
                if (core >= max_count)
                    core -= (max_count-1);
                SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
            #elif defined(_OS_UNIX_)
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(m_index, &cpuset);
                pthread_setaffinity_np(m_threads[m_index].native_handle(), sizeof(cpu_set_t), &cpuset);
            #endif

            m_local_work_queue = m_queues[m_index].get();

            while (!m_done)
                run_pending_task();
        }

        void run_pending_task()
        {
            task_type task;
            if (pop_task_from_local_queue(task) || pop_task_from_pool_queue(task) || pop_task_from_other_thread_queue(task))
            {
                task();
            }
            else
            {
                if (m_wait_for_idle && !m_idling[m_index])
                    m_idling[m_index] = true;
                std::this_thread::yield();
            }
        }

        bool pop_task_from_local_queue(task_type& task)
        {
            return m_local_work_queue && m_local_work_queue->try_pop(task);
        }

        bool pop_task_from_pool_queue(task_type& task)
        {
            return m_pool_work_queue.try_pop(task);
        }

        bool pop_task_from_other_thread_queue(task_type& task)
        {
            for (unsigned i = 0; i < m_queues.size(); i++)
            {
                const unsigned index = (m_index + i + 1) % m_queues.size();
                if (m_queues[index]->try_pop(task))
                    return true;
            }

            return false;
        }

    private:

        std::atomic_bool                                   m_wait_for_idle;
        std::atomic_bool                                   m_done;
        mvWorkStealingQueue                                m_pool_work_queue;
        std::vector<std::unique_ptr<mvWorkStealingQueue> > m_queues;
        std::vector<std::thread>                           m_threads;
        std::deque<std::atomic_bool>                       m_idling;
        mvThreadJoiner                                     m_joiner;
        static thread_local mvWorkStealingQueue*           m_local_work_queue;
        static thread_local unsigned                       m_index;

    };

    thread_local mvWorkStealingQueue* mvThreadPool::m_local_work_queue;
    thread_local unsigned mvThreadPool::m_index;
}
#endif
