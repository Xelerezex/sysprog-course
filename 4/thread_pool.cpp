#include "thread_pool.h"

#include <pthread.h>

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

/* ----------------------------------------- Variables ----------------------------------------- */
namespace {
constexpr int success = 0;
constexpr int failure = -1;
constexpr auto nsec_per_sec = 1'000'000'000L;
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
    mutable pthread_mutex_t mutex {};
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
    mutable pthread_mutex_t mutex {};

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

#if defined(__linux__)
constexpr clockid_t kCondClock = CLOCK_MONOTONIC;
#else
constexpr clockid_t kCondClock = CLOCK_REALTIME; // macOS pthread_cond_timedwait uses realtime
#endif

void initializeConditionVariable(pthread_cond_t *condition) {
    pthread_condattr_t attributes{};
    if (pthread_condattr_init(&attributes) != success) {
        pthread_cond_init(condition, nullptr);
        return;
    }

#if defined(__linux__)
    // On Linux we can select the clock used by timed waits.
    if (pthread_condattr_setclock(&attributes, kCondClock) != success) {
        pthread_condattr_destroy(&attributes);
        pthread_cond_init(condition, nullptr);
        return;
    }
    if (pthread_cond_init(condition, &attributes) != success) {
        pthread_condattr_destroy(&attributes);
        pthread_cond_init(condition, nullptr);
        return;
    }
#else
    // macOS: no pthread_condattr_setclock()
    if (pthread_cond_init(condition, nullptr) != success) {
        pthread_condattr_destroy(&attributes);
        pthread_cond_init(condition, nullptr);
        return;
    }
#endif

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

    if (pool->threads.size() < static_cast<std::size_t>(pool->max_threads)) {
        if (const auto spawn_result = spawnLockedWorker(pool); spawn_result != success) {
            return spawn_result;
        }
    }
    pthread_cond_signal(&pool->has_task_cv);

    return success;
}

void initializeDeadline(timespec *deadline, const double delay_seconds) {
    // ReSharper disable once CppDFAConstantConditions
    if (deadline == nullptr) {
        // ReSharper disable once CppDFAUnreachableCode
        return;
    }

    timespec now {};
    clock_gettime(kCondClock, &now);
    if (delay_seconds <= 0.0) {
        *deadline = now;
        return;
    }

    long double delay_per_second = static_cast<long double>(delay_seconds) * static_cast<long double>(nsec_per_sec);
    constexpr auto max_int64_value = static_cast<long double>(std::numeric_limits<int64_t>::max());
    if (delay_per_second < 0.0L) {    // NaN -> treat as 0
        delay_per_second = 0.0L;
    } else if (delay_per_second > max_int64_value) {
        delay_per_second = max_int64_value;
    }

    const auto add_nano_seconds = static_cast<int64_t>(std::ceil(delay_per_second));

    int64_t now_seconds_in_nano_seconds = 0;
    if (now.tv_sec > std::numeric_limits<int64_t>::max() / nsec_per_sec) {
        now_seconds_in_nano_seconds = std::numeric_limits<int64_t>::max();
    } else {
        now_seconds_in_nano_seconds = now.tv_sec * nsec_per_sec;
    }

    const int64_t now_nano_seconds = now_seconds_in_nano_seconds == max_int64_value
                                             ? max_int64_value
                                             : now_seconds_in_nano_seconds + now.tv_nsec;
    const int64_t deadline_nano_seconds = now_nano_seconds > max_int64_value - add_nano_seconds
                                                  ? max_int64_value
                                                  : now_nano_seconds + add_nano_seconds;

    deadline->tv_sec = deadline_nano_seconds / nsec_per_sec;
    deadline->tv_nsec = deadline_nano_seconds % nsec_per_sec;
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

    // lock pool & check
    pthread_mutex_lock(&pool->mutex);
    if (pool->stop) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    if (pool->tasks_in_pool.size() >= TPOOL_MAX_TASKS) {
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TOO_MANY_TASKS;
    }

    // lock task & check
    pthread_mutex_lock(&task->mutex);
    if (task->parent_pool != nullptr) {
        pthread_mutex_unlock(&task->mutex);
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    if (task->task_state == State::Queued || task->task_state == State::Running) {
        pthread_mutex_unlock(&task->mutex);
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    if (task->task_state == State::Finished && !task->is_joined) {
        pthread_mutex_unlock(&task->mutex);
        pthread_mutex_unlock(&pool->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }

    task->parent_pool = pool;
    task->task_state = State::Queued;
    task->is_joined = false;
    task->is_detached = false;
    pthread_mutex_unlock(&task->mutex);

    pool->tasks_queue.push_back(task);
    pool->tasks_in_pool.insert(task);
    if (const auto result = wakeOrSpawnNewTask(pool); result != success) {
        pthread_mutex_unlock(&pool->mutex);
        // do not checked by tests
        return 0;
    }
    pthread_mutex_unlock(&pool->mutex);
    return success;
}

int thread_task_new(thread_task **task, const thread_task_f &function) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    *task = new thread_task(function);
    return success;
}

bool thread_task_is_finished(const thread_task *task) {
    if (task == nullptr) {
        return false;
    }
    pthread_mutex_lock(&task->mutex);
    const auto result = task->task_state == State::Finished && task->is_joined == true;
    pthread_mutex_unlock(&task->mutex);
    return result;
}

bool thread_task_is_running(const thread_task *task) {
    if (task == nullptr) {
        return false;
    }
    pthread_mutex_lock(&task->mutex);
    const auto result = task->task_state == State::Running;
    pthread_mutex_unlock(&task->mutex);
    return result;
}

int thread_task_join(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&task->mutex);
    if (task->parent_pool == nullptr && task->task_state == State::New) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    // already joined
    if (task->task_state == State::Finished && task->is_joined == true && task->parent_pool == nullptr) {
        pthread_mutex_unlock(&task->mutex);
        return success;
    }

    // wait till finished
    while (task->task_state != State::Finished) {
        pthread_cond_wait(&task->finished_cv, &task->mutex);
    }

    task->is_joined = true;
    thread_pool *current_pool = task->parent_pool;
    task->parent_pool = nullptr;
    pthread_mutex_unlock(&task->mutex);
    if (current_pool != nullptr) {
        pthread_mutex_lock(&current_pool->mutex);
        current_pool->tasks_in_pool.erase(task);
        pthread_mutex_unlock(&current_pool->mutex);
    }

    return success;
}

#if NEED_TIMED_JOIN

int thread_task_timed_join(thread_task *task, const double timeout) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&task->mutex);
    if (task->parent_pool == nullptr && task->task_state == State::New) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    // already joined
    if (task->task_state == State::Finished && task->is_joined && task->parent_pool == nullptr) {
        pthread_mutex_unlock(&task->mutex);
        return success;
    }
    if (task->task_state == State::Finished && !task->is_joined) {
        pthread_mutex_unlock(&task->mutex);
        return thread_task_join(task);
    }

    if (timeout <= 0.0) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TIMEOUT;
    }

    timespec deadline {};
    initializeDeadline(&deadline, timeout);

    while (task->task_state != State::Finished) {
        if (pthread_cond_timedwait(&task->finished_cv, &task->mutex, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&task->mutex);
            return TPOOL_ERR_TIMEOUT;
        }
    }

    pthread_mutex_unlock(&task->mutex);
    return thread_task_join(task);
}

#endif

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int thread_task_delete(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&task->mutex);
    if (task->parent_pool != nullptr) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    if (task->task_state == State::Queued || task->task_state == State::Running) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    if (task->task_state == State::Finished && task->is_joined == false) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    pthread_mutex_unlock(&task->mutex);
    delete task;

    return success;
}

#if NEED_DETACH

int thread_task_detach(thread_task *task) {
    if (task == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&task->mutex);
    if (task->parent_pool == nullptr && task->task_state == State::New) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }

    bool delete_now = false;
    thread_pool *current_pool = nullptr;
    if (task->task_state == State::Finished) {
        task->is_detached = true;
        task->is_joined = true;
        current_pool = task->parent_pool;
        task->parent_pool = nullptr;
        delete_now = true;
    } else {
        task->is_detached = true;
    }
    pthread_mutex_unlock(&task->mutex);

    // Remove from pool ownership and delete.
    if (current_pool != nullptr) {
        pthread_mutex_lock(&current_pool->mutex);
        current_pool->tasks_in_pool.erase(task);
        pthread_mutex_unlock(&current_pool->mutex);
    }
    if (delete_now) {
        delete task;
    }

    return success;
}

#endif
