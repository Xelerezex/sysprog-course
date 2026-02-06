#include "corobus.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>

#include "libcoro.h"
#include "rlist.h"

struct wakeup_entry {
    rlist base;
    coro *coroutine;
};

struct wakeup_queue {
    rlist coroutines;
};

static void wakeup_queue_init(wakeup_queue *queue) {
    rlist_create(&queue->coroutines);
}

static void wakeup_queue_suspend_this(wakeup_queue *queue) {
    wakeup_entry entry {};
    rlist_create(&entry.base);
    entry.coroutine = coro_this();

    rlist_add_tail_entry(&queue->coroutines, &entry, base);
    coro_suspend();

    // Always unlink. Safe even if already popped by waker.
    rlist_del_entry(&entry, base);
}

static void wakeup_queue_wakeup_first(wakeup_queue *queue) {
    if (rlist_empty(&queue->coroutines)) {
        return;
    }

    auto *entry = rlist_first_entry(&queue->coroutines, wakeup_entry, base);
    rlist_del_entry(entry, base);    // pop first, then wake
    coro_wakeup(entry->coroutine);
}

static void wakeup_queue_wakeup_all(wakeup_queue *queue) {
    while (!rlist_empty(&queue->coroutines)) {
        auto *e = rlist_first_entry(&queue->coroutines, wakeup_entry, base);
        rlist_del_entry(e, base);
        coro_wakeup(e->coroutine);
    }
}

struct coro_bus_channel {
    std::size_t size_limit = 0;
    wakeup_queue send_queue {};
    wakeup_queue recv_queue {};
    std::deque<unsigned> message_queue {};
};

struct coro_bus {
    coro_bus_channel **channels = nullptr;    // descriptor table with holes
    int channel_count = 0;                    // capacity of descriptor table
};

static coro_bus_error_code global_errno = CORO_BUS_ERR_NONE;

coro_bus_error_code coro_bus_errno() {
    return global_errno;
}

void coro_bus_errno_set(const coro_bus_error_code error_code) {
    global_errno = error_code;
}

static coro_bus_channel *get_bus_channel(const coro_bus *coroutines_bus, const int index) {
    if (coroutines_bus == nullptr || coroutines_bus->channels == nullptr) {
        return nullptr;
    }
    if (index < 0 || index >= coroutines_bus->channel_count) {
        return nullptr;
    }
    return coroutines_bus->channels[index];
}

static bool is_bus_has_any_channels(const coro_bus *coroutines_bus) {
    if (coroutines_bus == nullptr || coroutines_bus->channels == nullptr) {
        return false;
    }
    for (int index = 0; index < coroutines_bus->channel_count; ++index) {
        if (coroutines_bus->channels[index] != nullptr) {
            return true;
        }
    }
    return false;
}

coro_bus *coro_bus_new() {
    auto *coroutines_bus = new coro_bus {};
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return coroutines_bus;
}

void coro_bus_delete(coro_bus *coroutines_bus) {
    if (coroutines_bus == nullptr) {
        return;
    }

    if (coroutines_bus->channels != nullptr) {
        for (int index = 0; index < coroutines_bus->channel_count; ++index) {
            if (coroutines_bus->channels[index] != nullptr) {
                coro_bus_channel_close(coroutines_bus, index);
            }
        }
        delete[] coroutines_bus->channels;
        coroutines_bus->channels = nullptr;
        coroutines_bus->channel_count = 0;
    }

    delete coroutines_bus;
}

int coro_bus_channel_open(coro_bus *coroutines_bus, std::size_t size_limit) {
    if (coroutines_bus == nullptr) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    if (size_limit == 0) {
        size_limit = 1;
    }

    // Reuse holes first
    if (coroutines_bus->channels != nullptr) {
        for (int index = 0; index < coroutines_bus->channel_count; ++index) {
            if (coroutines_bus->channels[index] == nullptr) {
                auto *ch = new coro_bus_channel {};
                ch->size_limit = size_limit;
                wakeup_queue_init(&ch->send_queue);
                wakeup_queue_init(&ch->recv_queue);
                coroutines_bus->channels[index] = ch;
                coro_bus_errno_set(CORO_BUS_ERR_NONE);
                return index;
            }
        }
    }

    // Grow descriptor table
    const int old_capacity = coroutines_bus->channel_count;
    const int new_doubled_cap = old_capacity == 0 ? 2 : old_capacity * 2;

    auto **new_arr = new coro_bus_channel *[static_cast<std::size_t>(new_doubled_cap)];
    std::memset(new_arr, 0, sizeof(*new_arr) * static_cast<std::size_t>(new_doubled_cap));

    if (coroutines_bus->channels != nullptr) {
        std::memcpy(new_arr, coroutines_bus->channels, sizeof(*new_arr) * static_cast<std::size_t>(old_capacity));
        delete[] coroutines_bus->channels;
    }

    coroutines_bus->channels = new_arr;
    coroutines_bus->channel_count = new_doubled_cap;

    // First free slot is old_cap (no holes existed).
    auto *new_channel = new coro_bus_channel {};
    new_channel->size_limit = size_limit;
    wakeup_queue_init(&new_channel->send_queue);
    wakeup_queue_init(&new_channel->recv_queue);
    coroutines_bus->channels[old_capacity] = new_channel;

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return old_capacity;
}

void coro_bus_channel_close(coro_bus *coroutines_bus, const int channel) {
    auto *current_channel = get_bus_channel(coroutines_bus, channel);
    if (current_channel == nullptr) {
        return;
    }

    // Mark closed first so woken coroutines observe NO_CHANNEL on retry
    coroutines_bus->channels[channel] = nullptr;

    // Wake all waiters, then delete channel safely
    wakeup_queue_wakeup_all(&current_channel->send_queue);
    wakeup_queue_wakeup_all(&current_channel->recv_queue);

    delete current_channel;
}

int coro_bus_try_send(coro_bus *bus, const int channel, const unsigned data) {
    auto *current_channel = get_bus_channel(bus, channel);
    if (current_channel == nullptr) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

    if (current_channel->message_queue.size() >= current_channel->size_limit) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }

    current_channel->message_queue.push_back(data);
    wakeup_queue_wakeup_first(&current_channel->recv_queue);

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int coro_bus_send(coro_bus *coroutines_bus, const int channel, const unsigned data) {
    while (true) {
        if (coro_bus_try_send(coroutines_bus, channel, data) == 0) {
            return 0;
        }
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK) {
            return -1;
        }

        // Channel might get closed while we sleep: re-check on loop top
        auto *current_channel = get_bus_channel(coroutines_bus, channel);
        if (current_channel == nullptr) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
        wakeup_queue_suspend_this(&current_channel->send_queue);
    }
}

int coro_bus_try_recv(coro_bus *coroutines_bus, const int channel, unsigned *data) {
    assert(data != nullptr);

    auto *current_channel = get_bus_channel(coroutines_bus, channel);
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
    wakeup_queue_wakeup_first(&current_channel->send_queue);

    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int coro_bus_recv(coro_bus *coroutines_bus, const int channel, unsigned *data) {
    assert(data != nullptr);

    while (true) {
        if (coro_bus_try_recv(coroutines_bus, channel, data) == 0) {
            return 0;
        }
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK) {
            return -1;
        }

        auto *current_channel = get_bus_channel(coroutines_bus, channel);
        if (current_channel == nullptr) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
        wakeup_queue_suspend_this(&current_channel->recv_queue);
    }
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
