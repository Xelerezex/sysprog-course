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

/* ------------------------------------------ Helpers ------------------------------------------ */
namespace {

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

int thread_pool_new(const int thread_count, thread_pool **pool) {
    if (thread_count <= 0 || thread_count > TPOOL_MAX_THREADS) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    if (pool == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }
    /* must NOT start all threads immediately */

    return TPOOL_ERR_NOT_IMPLEMENTED;
}

int thread_pool_delete(thread_pool *pool) {
    if (pool == nullptr) {
        return TPOOL_ERR_INVALID_ARGUMENT;
    }

    return TPOOL_ERR_NOT_IMPLEMENTED;
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
