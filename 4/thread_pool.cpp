#include "thread_pool.h"

#include <pthread.h>

#include <cassert>
#include <cstdint>
#include <ctime>
#include <deque>
#include <unordered_set>
#include <utility>
#include <vector>

/* ----------------------------------------- Variables ----------------------------------------- */
namespace {
constexpr int success = 0;
[[maybe_unused]] constexpr int failure = -1;
}    // namespace
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------- Types ------------------------------------------- */
namespace {
enum class State : std::int32_t {
    New,
    Queued,
    Running,
    Finished,
};

void initializeConditionVariable(pthread_cond_t *condition);

}    // namespace

struct thread_pool {
    explicit thread_pool(const int new_max_threads) : max_threads(new_max_threads) {
        const auto result = pthread_mutex_init(&mutex, nullptr);
        assert(result == success);
        initializeConditionVariable(&has_task_cv);
    }
    ~thread_pool() {
        pthread_cond_destroy(&has_task_cv);
        pthread_mutex_destroy(&mutex);
    }

    std::vector<pthread_t> threads;
    std::deque<thread_task *> tasks_queue;
    std::unordered_set<thread_task *> tasks_in_pool;

    int max_threads = 0;
    pthread_mutex_t mutex {};
    pthread_cond_t has_task_cv {};
    bool stop = false;
    int idle_workers = 0;
};

struct thread_task {
    explicit thread_task(thread_task_f new_function) : function(std::move(new_function)) {
        const auto result = pthread_mutex_init(&mutex, nullptr);
        assert(result == success);
        initializeConditionVariable(&finished_cv);
    }
    ~thread_task() {
        pthread_cond_destroy(&finished_cv);
        pthread_mutex_destroy(&mutex);
    }

    // callable
    thread_task_f function {};
    pthread_mutex_t mutex {};

    pthread_cond_t finished_cv {};
    thread_pool *parent_pool = nullptr;
    State task_state = State::New;
    bool is_joined = false;
    bool is_detached = false;
};
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------ Helpers ------------------------------------------ */
namespace {

/*
 * pthread_cond_t           -> std::condition_variable
 * pthread_cond_signal()    -> .notify_one()
 * pthread_cond_broadcast() -> .notify_all()
 * pthread_cond_wait()      -> .wait()
 */

void initializeConditionVariable(pthread_cond_t *condition) {
    pthread_condattr_t attributes {};
    if (success != pthread_condattr_init(&attributes)) {
        pthread_cond_init(condition, nullptr);
        return;
    }
    if (success != pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC)) {
        pthread_condattr_destroy(&attributes);
        pthread_cond_init(condition, nullptr);
        return;
    }
    if (success != pthread_cond_init(condition, &attributes)) {
        pthread_condattr_destroy(&attributes);
        pthread_cond_init(condition, nullptr);
        return;
    }
    pthread_condattr_destroy(&attributes);
}

void run(thread_task *task) {
    // Running
    pthread_mutex_lock(&task->mutex);
    task->task_state = State::Running;
    pthread_mutex_unlock(&task->mutex);

    // Execution
    try {
        task->function();
    } catch (...) {
        // do nothing
    }

    thread_pool *current_pool = nullptr;

    // Finished
    pthread_mutex_lock(&task->mutex);
    task->task_state = State::Finished;
    const bool do_detach = task->is_detached;
    if (do_detach) {
        task->is_joined = true;
        current_pool = task->parent_pool;    // capture before clearing
        task->parent_pool = nullptr;         // disassociate
    }
    pthread_cond_broadcast(&task->finished_cv);
    pthread_mutex_unlock(&task->mutex);

    if (do_detach) {
        // Remove from pool ownership and delete.
        if (current_pool != nullptr) {
            pthread_mutex_lock(&current_pool->mutex);
            current_pool->tasks_in_pool.erase(task);
            pthread_mutex_unlock(&current_pool->mutex);
        }
        delete task;
    }
}

// ReSharper disable once CppDFAConstantFunctionResult
void *worker(void *arg) {
    auto *pool = static_cast<thread_pool *>(arg);
    if (pool == nullptr) {
        return nullptr;
    }

    // Wait loop:
    while (true) {
        pthread_mutex_lock(&pool->mutex);

        // wait for task
        while (!pool->stop && pool->tasks_queue.empty()) {
            ++pool->idle_workers;
            pthread_cond_wait(&pool->has_task_cv, &pool->mutex);
            --pool->idle_workers;
        }
        // exit wait loop
        if (pool->stop && pool->tasks_queue.empty()) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        const auto task = pool->tasks_queue.front();
        pool->tasks_queue.pop_front();
        pthread_mutex_unlock(&pool->mutex);
        run(task);
    }
    return nullptr;
}

/*
* this is dangerous:
bool isLockHeld(pthread_mutex_t *mutex) {
    // we should hold it
    if (const auto result = pthread_mutex_trylock(mutex); result == success) {
        pthread_mutex_unlock(mutex);
        return false;
    }
    return true;
}
*/

[[maybe_unused]] int spawnLockedWorker(thread_pool *pool) {
    // mutex should be locked
    if (pool->stop) {
        return failure;
    }
    if (pool->threads.size() >= static_cast<std::size_t>(pool->max_threads)) {
        return failure;
    }

    pthread_t new_thread;
    if (const int result = pthread_create(&new_thread, nullptr, worker, pool); result != success) {
        return result;
    }
    pool->threads.push_back(new_thread);
    return success;
}

[[maybe_unused]] int wakeOrSpawnNewTask(thread_pool *pool) {
    // mutex should be locked
    if (pool->idle_workers > 0) {
        pthread_cond_signal(&pool->has_task_cv);
    } else if (pool->threads.size() < static_cast<std::size_t>(pool->max_threads)) {
        if (const auto spawn_result = spawnLockedWorker(pool); spawn_result != success) {
            return spawn_result;
        }
        pthread_cond_signal(&pool->has_task_cv);
    } else {
        pthread_cond_signal(&pool->has_task_cv);
    }

    return success;
}

}    // namespace
/* -------------------------------------------- *** -------------------------------------------- */

int thread_pool_new(const int thread_count, thread_pool **pool) {
    if (pool == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    if (thread_count <= 0 || thread_count > TPOOL_MAX_THREADS) {
        *pool = nullptr;
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    /* must NOT start all threads immediately */
    *pool = new thread_pool(thread_count);
    return success;
}

int thread_pool_delete(thread_pool *pool) {
    if (pool == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&pool->mutex);
    if (!pool->tasks_in_pool.empty()) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_HAS_TASKS;
    }

    pool->stop = true;
    pthread_cond_broadcast(&pool->has_task_cv);
    pthread_mutex_unlock(&pool->mutex);

    for (const auto &thread : pool->threads) {
        pthread_join(thread, nullptr);
    }
    delete pool;
    return success;
}

int thread_pool_push_task(thread_pool *pool, thread_task *task) {
    if (pool == nullptr || task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

int thread_task_new(thread_task **task, const thread_task_f &function) {
    // assert(assert(task != nullptr) != nullptr);
    // assert(assert(task != nullptr) != nullptr);

    (void)task;
    (void)function;
    return TPOOL_ERR_NOT_IMPLEMENTED;
}

bool thread_task_is_finished(const thread_task *task) {
    if (task == nullptr) {
        return false;
    }

    return false;
}

bool thread_task_is_running(const thread_task *task) {
    if (task == nullptr) {
        return false;
    }

    return false;
}

int thread_task_join(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#if NEED_TIMED_JOIN

int thread_task_timed_join(thread_task *task, const double timeout) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    if (timeout < 0) {
        return TPOOL_ERR_TIMEOUT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif

int thread_task_delete(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#if NEED_DETACH

int thread_task_detach(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

#endif
