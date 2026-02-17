#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>

#include "parser.h"

static void execute_command_line(const command_line *line) {
    assert(line != nullptr);
    // printf("================================\n");
    // printf("Command line:\n");
    // printf("Is background: %d\n", static_cast<int>(line->is_background));
    // printf("Output: ");

    if (line->out_type == OUTPUT_TYPE_STDOUT) {
        // printf("stdout\n");
    } else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        // printf("new file - \"%s\"\n", line->out_file.c_str());
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        // printf("append file - \"%s\"\n", line->out_file.c_str());
    } else {
        assert(false);
    }

    // printf("Expressions:\n");
    for (const auto &[type, command] : line->exprs) {
        if (type == EXPR_TYPE_COMMAND) {
            // printf("\tCommand: %s", command->exe.c_str());
            // for (const std::string &argument : command->args) {
            //     printf(" %s", argument.c_str());
            // }
            // printf("\n");
        } else if (type == EXPR_TYPE_PIPE) {
            // printf("\tPIPE\n");
        } else if (type == EXPR_TYPE_AND) {
            // printf("\tAND\n");
        } else if (type == EXPR_TYPE_OR) {
            // printf("\tOR\n");
        } else {
            assert(false);
        }
    }
}

std::optional<std::string> read_line_stdin() {
    std::string buffer;
    char character;

    while (true) {
        constexpr std::size_t one_byte = 1;
        const ssize_t bytes_to_read = read(STDIN_FILENO, &character, one_byte);
        if (bytes_to_read == 0) {
            break;
        }
        if (bytes_to_read < 0) {
            // retry:
            if (errno == EINTR) {
                errno = 0;
                continue;
            }
            // error:
            return std::nullopt;
        }

        // append values on success
        buffer.push_back(character);
        if (character == '\n') {
            break;
        }
    }

    return buffer;
}

int main() {
    parser *parser_object = parser_new();
    int execution_code = 0;

    while (true) {
        const auto line_optional = read_line_stdin();
        if (!line_optional.has_value()) {
            execution_code = 1;
            break;
        }
        const auto &line = line_optional.value();
        if (line.empty()) {
            break;
        }

        // Command parse
        parser_feed(parser_object, line.data(), static_cast<uint32_t>(line.size()));
        command_line *current_line = nullptr;
        while (true) {
            const parser_error error_code = parser_pop_next(parser_object, &current_line);
            if (error_code == PARSER_ERR_NONE && current_line == nullptr) {
                break;
            }
            if (error_code != PARSER_ERR_NONE) {
                // printf("Error: %d\n", static_cast<int>(error_code));
                if (current_line != nullptr) {
                    delete current_line;
                    current_line = nullptr;
                }
                continue;
            }
            execute_command_line(current_line);
            delete current_line;
        }
    }

    parser_delete(parser_object);
    return execution_code;
}
