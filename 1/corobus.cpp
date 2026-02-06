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
    rlist base;
    // ReSharper disable once CppRedundantElaboratedTypeSpecifier
    struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
    rlist coros;
};

/** Suspend the current coroutine until it is woken up. */
static void wakeup_queue_suspend_this(wakeup_queue *queue) {
    wakeup_entry entry {};
    entry.coro = coro_this();
    rlist_add_tail_entry(&queue->coros, &entry, base);
    coro_suspend();
    rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void wakeup_queue_wakeup_first(const wakeup_queue *queue) {
    if (rlist_empty(&queue->coros)) {
        return;
    }
    const wakeup_entry *entry = rlist_first_entry(&queue->coros, struct wakeup_entry, base);
    coro_wakeup(entry->coro);
}

/**
 * Wake all waiters and yield until each woken coroutine unlinks itself.
 * Must be called from a coroutine context.
 */
static void wakeup_queue_wakeup_all_and_drain(const wakeup_queue *queue) {
    while (!rlist_empty(&queue->coros)) {
        const wakeup_entry *head_entry = rlist_first_entry(&queue->coros, struct wakeup_entry, base);
        coro *coroutine = head_entry->coro;
        coro_wakeup(coroutine);

        // Yield until the head changes (the woken coro unlinked itself) or the queue becomes empty.
        do {
            coro_yield();
        } while (!rlist_empty(&queue->coros) &&
                 rlist_first_entry(&queue->coros, struct wakeup_entry, base)->coro == coroutine);
    }
}

struct coro_bus_channel {
    /** Channel max capacity. */
    size_t size_limit = 0;
    /** Coroutines waiting until the channel is not full. */
    wakeup_queue send_queue {};
    /** Coroutines waiting until the channel is not empty. */
    wakeup_queue recv_queue {};
    /** Message queue. */
    std::deque<unsigned> message_queue {};
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

static coro_bus_channel *get_bus_channel(const coro_bus *current_coroutine_bus, const int channel) {
    if (current_coroutine_bus == nullptr || current_coroutine_bus->channels == nullptr) {
        return nullptr;
    }
    if (channel < 0 || channel >= current_coroutine_bus->channel_count) {
        return nullptr;
    }
    return current_coroutine_bus->channels[channel];
}

coro_bus *coro_bus_new() {
    auto *current_coroutine_bus = new coro_bus {};
    current_coroutine_bus->channel_count = 0;
    current_coroutine_bus->channels = nullptr;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return current_coroutine_bus;
}

void coro_bus_delete(coro_bus *current_coroutine_bus) {
    if (current_coroutine_bus == nullptr) {
        return;
    }

    if (current_coroutine_bus->channels == nullptr) {
        delete current_coroutine_bus;
        return;
    }

    for (int index = 0; index < current_coroutine_bus->channel_count; ++index) {
        const coro_bus_channel *current_channel = current_coroutine_bus->channels[index];
        if (current_channel == nullptr) {
            continue;
        }

        // Per task: no suspended coroutines are allowed here.
        assert(rlist_empty(&current_channel->send_queue.coros));
        assert(rlist_empty(&current_channel->recv_queue.coros));

        delete current_channel;
        current_coroutine_bus->channels[index] = nullptr;
    }
    delete[] current_coroutine_bus->channels;
    current_coroutine_bus->channels = nullptr;
    current_coroutine_bus->channel_count = 0;

    delete current_coroutine_bus;
}

int coro_bus_channel_open(coro_bus *current_coroutine_bus, size_t size_limit) {
    if (current_coroutine_bus == nullptr) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    if (size_limit == 0) {
        size_limit = 1;
    }

    // Reuse holes first.
    if (current_coroutine_bus->channels != nullptr) {
        for (int index = 0; index < current_coroutine_bus->channel_count; ++index) {
            if (current_coroutine_bus->channels[index] == nullptr) {
                auto *new_channel = new coro_bus_channel;
                new_channel->size_limit = size_limit;
                rlist_create(&new_channel->send_queue.coros);
                rlist_create(&new_channel->recv_queue.coros);
                current_coroutine_bus->channels[index] = new_channel;
                coro_bus_errno_set(CORO_BUS_ERR_NONE);
                return index;
            }
        }
    }

    // Need to grow the descriptor table.
    const int old_capacity = current_coroutine_bus->channel_count;
    const int new_double_capacity = (old_capacity == 0) ? 2 : (old_capacity * 2);

    auto **new_bus_channels = new coro_bus_channel *[new_double_capacity];
    memset(new_bus_channels, 0, sizeof(*new_bus_channels) * static_cast<size_t>(new_double_capacity));

    if (current_coroutine_bus->channels != nullptr) {
        memcpy(new_bus_channels,
               current_coroutine_bus->channels,
               sizeof(*new_bus_channels) * static_cast<size_t>(old_capacity));
        delete[] current_coroutine_bus->channels;
    }

    current_coroutine_bus->channels = new_bus_channels;
    current_coroutine_bus->channel_count = new_double_capacity;

    // First free slot is old_cap (because there were no holes).
    const auto new_bus_channel = new coro_bus_channel;
    new_bus_channel->size_limit = size_limit;
    rlist_create(&new_bus_channel->send_queue.coros);
    rlist_create(&new_bus_channel->recv_queue.coros);
    current_coroutine_bus->channels[old_capacity] = new_bus_channel;

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return old_capacity;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
void coro_bus_channel_close(coro_bus *current_coroutine_bus, const int channel) {
    const coro_bus_channel *current_channel = get_bus_channel(current_coroutine_bus, channel);
    if (current_channel == nullptr) {
        return;
    }

    // Mark as gone first, so woken coroutines observe NO_CHANNEL.
    current_coroutine_bus->channels[channel] = nullptr;

    // Wake all waiters safely (they must unlink themselves), then delete.
    wakeup_queue_wakeup_all_and_drain(&current_channel->send_queue);
    wakeup_queue_wakeup_all_and_drain(&current_channel->recv_queue);

    delete current_channel;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int coro_bus_send(coro_bus *current_coroutine_bus, const int channel, const unsigned data) {
    while (true) {
        coro_bus_channel *current_channel = get_bus_channel(current_coroutine_bus, channel);
        if (current_channel == nullptr) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        if (current_channel->message_queue.size() < current_channel->size_limit) {
            current_channel->message_queue.push_back(data);

            // Wake a receiver (there is data now).
            wakeup_queue_wakeup_first(&current_channel->recv_queue);

            // If there is still space chain-wakeup more senders.
            if (current_channel->message_queue.size() < current_channel->size_limit) {
                wakeup_queue_wakeup_first(&current_channel->send_queue);
            }

            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            break;
        }

        // Full -> wait.
        wakeup_queue_suspend_this(&current_channel->send_queue);
        // retry
    }
    return 0;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int coro_bus_try_send(coro_bus *current_coroutine_bus, const int channel, const unsigned data) {
    coro_bus_channel *current_channels = get_bus_channel(current_coroutine_bus, channel);
    if (current_channels == nullptr) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (current_channels->message_queue.size() >= current_channels->size_limit) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    current_channels->message_queue.push_back(data);

    // Let a waiting receiver know there is data.
    wakeup_queue_wakeup_first(&current_channels->recv_queue);

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int coro_bus_recv(coro_bus *current_coroutine_bus, int channel, unsigned *data) {
    assert(data != nullptr);

    while (true) {
        coro_bus_channel *current_channel = get_bus_channel(current_coroutine_bus, channel);
        if (current_channel == nullptr) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }

        if (!current_channel->message_queue.empty()) {
            *data = current_channel->message_queue.front();
            current_channel->message_queue.pop_front();

            // Space appeared -> wake one sender.
            wakeup_queue_wakeup_first(&current_channel->send_queue);

            // If still has data (e.g., after bulk send), chain-wakeup receivers.
            if (!current_channel->message_queue.empty()) {
                wakeup_queue_wakeup_first(&current_channel->recv_queue);
            }

            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return 0;
        }

        // Empty -> wait.
        wakeup_queue_suspend_this(&current_channel->recv_queue);
        // retry
    }
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int coro_bus_try_recv(coro_bus *current_coroutine_bus, int channel, unsigned *data) {
    assert(data != nullptr);

    coro_bus_channel *current_channel = get_bus_channel(current_coroutine_bus, channel);
    if (current_channel == nullptr) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (current_channel->message_queue.empty()) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    *data = current_channel->message_queue.front();
    current_channel->message_queue.pop_front();

    // Space appeared -> wake one sender.
    wakeup_queue_wakeup_first(&current_channel->send_queue);

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
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
