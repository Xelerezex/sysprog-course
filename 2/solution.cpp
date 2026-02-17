#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <ostream>

#include "parser.h"

namespace utils {

int statusToBash(const int status) {
    int result = 0;
    if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result = 128 + WTERMSIG(status);
    } else {
        result = 1;
    }
    return result;
}

int executeRegularNonBackgroundCommand(command &command) {
    int last_status = 0;
    std::vector<char *> arguments;
    arguments.reserve(command.args.size() + 2);

    // First value:
    arguments.push_back(command.exe.data());
    // All arguments:
    std::transform(std::begin(command.args),
                   std::end(command.args),
                   std::back_inserter(arguments),
                   [](std::string &argument) -> char * { return argument.data(); });
    // End for arguments:
    arguments.push_back(nullptr);

    if (const pid_t child_pid = fork(); child_pid == -1) {
        std::cerr << "bash: " << arguments.at(0) << ": child fork error\n";
        last_status = 1;
    } else if (child_pid > 0) {
        int status;
        do {
            const pid_t wait_pid = waitpid(child_pid, &status, 0);
            if (wait_pid == -1 && errno == EINTR) {
                errno = 0;
                continue;
            }
            if (wait_pid == -1 && errno != EINTR) {
                last_status = 1;
                return last_status;
            }
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        last_status = statusToBash(status);
        // parent
    } else if (child_pid == 0) {
        const auto execute_result = execvp(arguments.at(0), arguments.data());
        if (execute_result == -1) {
            if (errno == ENOENT) {
                std::cerr << "bash: " << arguments.at(0) << ": command not found\n";
                _exit(127);
            }
            std::cerr << "bash: " << arguments.at(0) << ": " << strerror(errno) << "\n";
            _exit(126);
        }
    }

    return last_status;
}

int executeCommandLine(command_line *line) {
    assert(line != nullptr);

    if (line->out_type == OUTPUT_TYPE_STDOUT) {
        if (!line->is_background) {
            // printf("Is background: %d\n", static_cast<int>(line->is_background));
        }

    } else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        // printf("new file - \"%s\"\n", line->out_file.c_str());
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        // printf("append file - \"%s\"\n", line->out_file.c_str());
    } else {
        assert(false);
    }

    int last_status = 0;

    // printf("Expressions:\n");
    for (auto &[type, command] : line->exprs) {
        if (!command.has_value()) {
            continue;
        }

        if (type == EXPR_TYPE_COMMAND) {
            if (!line->is_background) {
                last_status = executeRegularNonBackgroundCommand(command.value());
            }

        } /*else if (type == EXPR_TYPE_PIPE) {
        } else if (type == EXPR_TYPE_AND) {
        } else if (type == EXPR_TYPE_OR) {
        } else { assert(false); } */
    }
    return last_status;
}

std::optional<std::string> readLineStdin() {
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

int createAndExecuteCommand(parser *parser_object, const std::string &line) {
    int last_status = 0;
    parser_feed(parser_object, line.data(), static_cast<uint32_t>(line.size()));
    command_line *current_line_raw = nullptr;
    while (true) {
        const parser_error error_code = parser_pop_next(parser_object, &current_line_raw);
        if (error_code == PARSER_ERR_NONE && current_line_raw == nullptr) {
            break;
        }
        std::unique_ptr<command_line> current_line_owner(current_line_raw);

        if (error_code != PARSER_ERR_NONE) {
            continue;
        }
        last_status = executeCommandLine(current_line_raw);
    }
    return last_status;
}

}    // namespace utils

int main() {
    parser *parser_object = parser_new();
    int execution_code = 0;

    while (true) {
        const auto line_optional = utils::readLineStdin();
        if (!line_optional.has_value()) {
            execution_code = 1;
            break;
        }
        const auto &line = line_optional.value();
        if (line.empty()) {
            break;
        }

        // Command parse
        execution_code = utils::createAndExecuteCommand(parser_object, line);
    }

    parser_delete(parser_object);
    return execution_code;
}
