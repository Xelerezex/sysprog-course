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

constexpr int success = 0;
constexpr int failure = 1;
constexpr int error_code = -1;

int builtInChangeDirectory(const std::vector<std::string> &arguments) {
    int last_status = success;
    // handling HOME
    if (arguments.empty()) {
        const auto home_path_raw = std::getenv("HOME");
        if (home_path_raw == nullptr) {
            std::cerr << "bash: cd: HOME not set\n";
            return failure;
        }
        const std::string home_path_variable(home_path_raw);
        if (home_path_variable.empty()) {
            std::cerr << "bash: cd: HOME not set\n";
            return failure;
        }
        // change directory of process
        if (chdir(home_path_variable.c_str()) != error_code) {
            last_status = success;
        } else {
            last_status = failure;
            std::cerr << "bash: cd: " << home_path_variable << ": " << strerror(errno) << "\n";
        }
    } else if (arguments.size() == 1) {
        const std::string &path_variable = arguments.at(0);
        if (path_variable.empty()) {
            std::cerr << "bash: cd: No such file or directory\n";
            return failure;
        }
        // change directory of process
        if (chdir(path_variable.c_str()) != error_code) {
            last_status = success;
        } else {
            last_status = failure;
            std::cerr << "bash: cd: " << path_variable << ": " << strerror(errno) << "\n";
        }
    } else {
        last_status = failure;
        std::cerr << "bash: cd: too many arguments\n";
    }

    return last_status;
}

int statusToBash(const int status) {
    int result = success;
    if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result = 128 + WTERMSIG(status);
    } else {
        result = failure;
    }
    return result;
}

int waitForChild(const pid_t child_pid) {
    int status;
    while (true) {
        const pid_t wait_pid = waitpid(child_pid, &status, 0);
        if (wait_pid == error_code && errno == EINTR) {
            errno = success;
            continue;
        }
        if (wait_pid == error_code && errno != EINTR) {
            return failure;
        }
        break;
    }
    return statusToBash(status);
}

int executeRegularNonBackgroundCommand(command &command) {
    int last_status = success;
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

    // if change directory -> do not fork()
    if (command.exe == "cd") {
        return builtInChangeDirectory(command.args);
    }

    // fork process
    const pid_t child_pid = fork();
    if (child_pid <= error_code) {
        std::cerr << "bash: fork: " << strerror(errno) << "\n";
        return failure;
    }
    if (child_pid > 0) {
        // parent:
        return waitForChild(child_pid);
    }

    // child: (child_pid == 0)
    if (const auto execute_result = execvp(arguments.at(0), arguments.data()); execute_result == error_code) {
        if (errno == ENOENT) {
            std::cerr << "bash: " << arguments.at(0) << ": command not found\n";
            _exit(127);
        }
        std::cerr << "bash: " << arguments.at(0) << ": " << strerror(errno) << "\n";
        _exit(126);
    }

    return last_status;
}

int executeCommandLine(command_line *line) {
    assert(line != nullptr);

    int last_status = success;

    // dumb check: execute only one command from now
    if (line->exprs.size() != 1 || line->out_type != OUTPUT_TYPE_STDOUT || line->is_background) {
        return last_status;
    }
    for (const auto &[type, command] : line->exprs) {
        if (type != EXPR_TYPE_COMMAND) {
            return last_status;
        }
    }

    // main logic
    if (line->out_type == OUTPUT_TYPE_STDOUT) {
        if (!line->is_background) {
            // printf("Is background: %d\n", static_cast<int>(line->is_background));
        }
    } else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
    } else {
        assert(false);
    }

    // printf("Expressions:\n");
    for (auto &[type, command] : line->exprs) {
        if (!command.has_value()) {
            continue;
        }

        if (type == EXPR_TYPE_COMMAND) {
            if (!line->is_background) {
                last_status = executeRegularNonBackgroundCommand(command.value());
            }

        } else if (type == EXPR_TYPE_PIPE) {
        } else if (type == EXPR_TYPE_AND) {
        } else if (type == EXPR_TYPE_OR) {
        } else {
            assert(false);
        }
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
        const parser_error parser_error_code = parser_pop_next(parser_object, &current_line_raw);
        if (parser_error_code == PARSER_ERR_NONE && current_line_raw == nullptr) {
            break;
        }
        std::unique_ptr<command_line> current_line_owner(current_line_raw);

        if (parser_error_code != PARSER_ERR_NONE) {
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
