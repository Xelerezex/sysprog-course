#pragma once

/**
 * Here you should specify which features do you want to implement via macros:
 * If you want to enable author name support, do:
 *
 *     #define NEED_AUTHOR 1
 *
 * To enable server-feed from admin do:
 *
 *     #define NEED_SERVER_FEED 1
 *
 * It is important to define these macros here, in the header, because it is
 * used by tests.
 */
#define NEED_AUTHOR 1
#define NEED_SERVER_FEED 1

#include <cstdint>
#include <string>

enum chat_errcode {
    CHAT_ERR_INVALID_ARGUMENT = 1,
    CHAT_ERR_TIMEOUT,
    CHAT_ERR_PORT_BUSY,
    CHAT_ERR_NO_ADDR,
    CHAT_ERR_ALREADY_STARTED,
    CHAT_ERR_NOT_IMPLEMENTED,
    CHAT_ERR_NOT_STARTED,
    CHAT_ERR_SYS,
};

enum chat_events {
    CHAT_EVENT_INPUT = 1,
    CHAT_EVENT_OUTPUT = 2,
};

struct chat_message {
    /** Author's name. */
    std::string author;
    /** 0-terminate text. */
    std::string data;
};

int setNonBlocking(int file_descriptor);

bool isSpace(char character);

std::string trimCopy(std::string_view string);

void appendU32(std::string &buffer, uint32_t value);

void readU32(const char *pointer_to_data, uint32_t &out);

void enqueueFrame(std::string &out, std::string_view author, std::string_view data);

struct frame_parser {
    std::string buffer;
    size_t offset = 0;

    bool try_pop(std::string &author, std::string &data);
};

int parseAddress(std::string_view address, std::string &host, std::string &port);

int chat_events_to_poll_events(int mask);
