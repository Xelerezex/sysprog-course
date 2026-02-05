#include "corobus.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <queue>

#include "libcoro.h"
#include "rlist.h"

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
    struct rlist base;
    struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
    struct rlist coros;
};

#if 0 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
    /** Channel max capacity. */
    size_t size_limit = 0;
    /** Coroutines waiting until the channel is not full. */
    wakeup_queue send_queue {};
    /** Coroutines waiting until the channel is not empty. */
    wakeup_queue recv_queue {};
    /** Message queue. */
    std::queue<unsigned> message_queue {};
};

struct coro_bus {
    coro_bus_channel **channels;
    int channel_count;
};

static coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

coro_bus_error_code coro_bus_errno() {
    return global_error;
}

void coro_bus_errno_set(const coro_bus_error_code error_code) {
    global_error = error_code;
}

coro_bus *coro_bus_new() {
    auto *current_coroutine_bus = new coro_bus {};
    current_coroutine_bus->channel_count = 0;
    current_coroutine_bus->channels = nullptr;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return current_coroutine_bus;
}

void coro_bus_delete(struct coro_bus *bus) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
}

coro_bus_channel *get_channel(const coro_bus *current_coroutine_bus, const std::size_t index) {
    if (current_coroutine_bus == nullptr || current_coroutine_bus->channels == nullptr) {
        return nullptr;
    }
    if (current_coroutine_bus->channel_count < 0) {
        return nullptr;
    }
    if (const auto count = static_cast<std::size_t>(current_coroutine_bus->channel_count); index >= count) {
        return nullptr;
    }

    return current_coroutine_bus->channels[index];
}

int coro_bus_channel_open(coro_bus *current_coroutine_bus, const size_t size_limit) {
    const int old_count = current_coroutine_bus->channel_count;
    // Reuse old descriptors
    for (int index = 0; index < old_count; index++) {
        if (const auto *current_channel = current_coroutine_bus->channels[index]; current_channel == nullptr) {
            auto *new_channel = new coro_bus_channel;
            rlist_create(&new_channel->send_queue.coros);
            rlist_create(&new_channel->recv_queue.coros);
            new_channel->size_limit = size_limit;
            current_coroutine_bus->channels[index] = new_channel;
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return index;
        }
    }

    // Create new allocation
    const int capacity = std::max(1, old_count);
    const int doubled_capacity = capacity * 2;

    auto **newly_allocated_channels = new coro_bus_channel *[doubled_capacity];
    const auto new_channel_size = sizeof(*newly_allocated_channels) * doubled_capacity;
    memset(newly_allocated_channels, 0, new_channel_size);

    // Copy & remove old data
    if (current_coroutine_bus->channels != nullptr) {
        memcpy(newly_allocated_channels,
               current_coroutine_bus->channels,
               sizeof(*newly_allocated_channels) * old_count);
        delete[] current_coroutine_bus->channels;
    }

    current_coroutine_bus->channels = newly_allocated_channels;
    current_coroutine_bus->channel_count = doubled_capacity;

    auto *new_channel = new coro_bus_channel;
    rlist_create(&new_channel->send_queue.coros);
    rlist_create(&new_channel->recv_queue.coros);
    new_channel->size_limit = size_limit;
    current_coroutine_bus->channels[old_count] = new_channel;

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return old_count;
}

void coro_bus_channel_close(struct coro_bus *bus, int channel) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    /*
     * Be very attentive here. What happens, if the channel is
     * closed while there are coroutines waiting on it? For
     * example, the channel was empty, and some coros were
     * waiting on its recv_queue.
     *
     * If you wakeup those coroutines and just delete the
     * channel right away, then those waiting coroutines might
     * on wakeup try to reference invalid memory.
     *
     * Can happen, for example, if you use an intrusive list
     * (rlist), delete the list itself (by deleting the
     * channel), and then the coroutines on wakeup would try
     * to remove themselves from the already destroyed list.
     *
     * Think how you could address that. Remove all the
     * waiters from the list before freeing it? Yield this
     * coroutine after waking up the waiters but before
     * freeing the channel, so the waiters could safely leave?
     */
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    /*
     * Try sending in a loop, until success. If error, then
     * check which one is that. If 'wouldblock', then suspend
     * this coroutine and try again when woken up.
     *
     * If see the channel has space, then wakeup the first
     * coro in the send-queue. That is needed so when there is
     * enough space for many messages, and many coroutines are
     * waiting, they would then wake each other up one by one
     * as lone as there is still space.
     */
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    /*
     * Append data if has space. Otherwise 'wouldblock' error.
     * Wakeup the first coro in the recv-queue! To let it know
     * there is data.
     */
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

#if NEED_BROADCAST

int coro_bus_broadcast(struct coro_bus *bus, unsigned data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)data;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)data;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

#endif

#if NEED_BATCH

int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    (void)count;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    (void)count;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    (void)capacity;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity) {
    /* IMPLEMENT THIS FUNCTION */
    (void)bus;
    (void)channel;
    (void)data;
    (void)capacity;
    coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
    return -1;
}

#endif
