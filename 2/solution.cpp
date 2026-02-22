#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>

#include "parser.h"

namespace utils {

namespace {

/* ------------------------------------------- codes ------------------------------------------- */
constexpr int success = 0;
constexpr int failure = 1;
constexpr int error_code = -1;
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------- types ------------------------------------------- */
enum PipeDescriptors : std::int32_t {
    Read = 0,    // read end
    Write = 1    // write end
};

struct Exit {
    int last_status = 0;
    bool terminate_shell = false;
};
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------ helpers ------------------------------------------ */

void closeOpened(int &pipe_descriptor) {
    if (pipe_descriptor != error_code) {
        close(pipe_descriptor);
        pipe_descriptor = error_code;
    }
}

// if outputFileOpen == error_code -> last_status = 1
int outputFileOpen(const output_type current_type, const std::string &current_file) {
    if (current_type == OUTPUT_TYPE_STDOUT) {
        return error_code;
    }

    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    if (current_type == OUTPUT_TYPE_FILE_NEW) {
        flags |= O_TRUNC;
    } else if (current_type == OUTPUT_TYPE_FILE_APPEND) {
        flags |= O_APPEND;
    }

    const int file_descriptor = open(current_file.c_str(), flags, 0666);
    if (file_descriptor == error_code) {
        std::cerr << "bash: " << current_file << ": " << strerror(errno) << "\n";
        return error_code;
    }

    return file_descriptor;
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

int executeBuiltInChangeDirectory(const std::vector<std::string> &arguments,
                                  const output_type current_type = OUTPUT_TYPE_STDOUT,
                                  const std::string &current_file = "") {
    int last_status = success;

    // redirect cs if needed (bash do this)
    int redirect_file_descriptors = error_code;
    if (current_type != OUTPUT_TYPE_STDOUT) {
        // open or create file
        redirect_file_descriptors = outputFileOpen(current_type, current_file);
        if (redirect_file_descriptors <= error_code) {
            return failure;
        }
        closeOpened(redirect_file_descriptors);
    }

    // handling HOME
    if (arguments.empty()) {
        const auto home_path_raw = std::getenv("HOME");
        if (home_path_raw == nullptr) {
            std::cerr << "bash: cd: HOME not set\n";
            closeOpened(redirect_file_descriptors);
            return failure;
        }
        const std::string home_path_variable(home_path_raw);
        if (home_path_variable.empty()) {
            std::cerr << "bash: cd: HOME not set\n";
            closeOpened(redirect_file_descriptors);
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
            closeOpened(redirect_file_descriptors);
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

    closeOpened(redirect_file_descriptors);
    return last_status;
}

Exit executeParentBuiltInExit(const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        return Exit {success, true};
    }

    errno = success;
    char *end_of_string = nullptr;
    const long status = std::strtol(arguments.at(0).c_str(), &end_of_string, 10);

    if (errno != success || end_of_string == arguments.at(0).c_str() || *end_of_string != '\0') {
        std::cerr << "bash: exit: " << arguments.at(0) << ": numeric argument required\n";
        return Exit {2, true};
    }
    if (arguments.size() > 1) {
        std::cerr << "bash: exit: too many arguments\n";
        return Exit {failure, false};
    }

    return Exit {static_cast<int>(static_cast<unsigned char>(status)), true};
}

Exit builtinRunInParent(const command &command, const output_type current_type, const std::string &current_file) {
    if (command.exe == "cd") {
        return Exit {executeBuiltInChangeDirectory(command.args, current_type, current_file), false};
    }
    if (command.exe == "exit" && current_type == OUTPUT_TYPE_STDOUT) {
        return executeParentBuiltInExit(command.args);
    }
    // ReSharper disable once CppDFAConstantConditions
    if (command.exe == "exit" && current_type != OUTPUT_TYPE_STDOUT) {
        // redirect cs if needed (bash do this)
        // ReSharper disable once CppDFAUnreachableCode
        // open or create file
        int redirect_file_descriptors = outputFileOpen(current_type, current_file);
        if (redirect_file_descriptors <= error_code) {
            return Exit {failure, false};
        }
        closeOpened(redirect_file_descriptors);
        return Exit {executeParentBuiltInExit(command.args).last_status, false};
    }
    return Exit {error_code, false};
}

int executeChildBuiltInExit(const std::vector<std::string> &arguments) {
    if (arguments.empty()) {
        return success;
    }

    errno = success;
    char *end_of_string = nullptr;
    const long status = std::strtol(arguments.at(0).c_str(), &end_of_string, 10);

    if (errno != success || end_of_string == arguments.at(0).c_str() || *end_of_string != '\0') {
        std::cerr << "bash: exit: " << arguments.at(0) << ": numeric argument required\n";
        return 2;
    }
    if (arguments.size() > 1) {
        std::cerr << "bash: exit: too many arguments\n";
        return failure;
    }

    return static_cast<unsigned char>(status);
}

int builtinRunInChild(const command &command, const output_type current_type, const std::string &current_file) {
    if (command.exe == "cd") {
        return executeBuiltInChangeDirectory(command.args, current_type, current_file);
    }
    // ReSharper disable once CppDFAConstantConditions
    if (command.exe == "exit" && current_type == OUTPUT_TYPE_STDOUT) {
        return executeChildBuiltInExit(command.args);
    }
    // ReSharper disable once CppDFAConstantConditions
    if (command.exe == "exit" && current_type != OUTPUT_TYPE_STDOUT) {
        // redirect cs if needed (bash do this)
        // ReSharper disable once CppDFAUnreachableCode
        // open or create file
        int redirect_file_descriptors = outputFileOpen(current_type, current_file);
        if (redirect_file_descriptors <= error_code) {
            return failure;
        }
        closeOpened(redirect_file_descriptors);
        return executeChildBuiltInExit(command.args);
    }
    return error_code;
}

void reapAll(const std::vector<pid_t> &pids) {
    for (const pid_t child_pid : pids) {
        [[maybe_unused]] const auto result = waitForChild(child_pid);
    }
}

std::vector<char *> generateArguments(command &command) {
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
    return arguments;
}

void execute(const std::vector<char *> &arguments) {
    if (const auto execute_result = execvp(arguments.at(0), arguments.data()); execute_result == error_code) {
        if (errno == ENOENT) {
            std::cerr << "bash: " << arguments.at(0) << ": command not found\n";
            _exit(127);
        }
        std::cerr << "bash: " << arguments.at(0) << ": " << strerror(errno) << "\n";
        _exit(126);
    }
}

/* -------------------------------------------- *** -------------------------------------------- */
}    // namespace

Exit executeNonBackgroundCommand(command &command, const output_type current_type, const std::string &current_file) {
    const std::vector<char *> arguments = generateArguments(command);

    // Run built ins
    if (const auto status = builtinRunInParent(command, current_type, current_file); status.last_status >= 0) {
        return status;
    }

    int redirect_file_descriptors = error_code;
    if (current_type != OUTPUT_TYPE_STDOUT) {
        // open or create file
        redirect_file_descriptors = outputFileOpen(current_type, current_file);
        if (redirect_file_descriptors <= error_code) {
            return {failure, false};
        }
    }

    // fork process
    const pid_t child_pid = fork();
    if (child_pid <= error_code) {
        std::cerr << "bash: fork: " << strerror(errno) << "\n";
        closeOpened(redirect_file_descriptors);
        return {failure, false};
    }
    // parent:
    if (child_pid > 0) {
        closeOpened(redirect_file_descriptors);
        const auto result = waitForChild(child_pid);
        return {result, false};
    }

    // child: (child_pid == 0)
    if (current_type != OUTPUT_TYPE_STDOUT) {
        if (dup2(redirect_file_descriptors, STDOUT_FILENO) == error_code) {
            std::cerr << "bash: dup2: " << strerror(errno) << "\n";
            closeOpened(redirect_file_descriptors);
            _exit(failure);
        }
        closeOpened(redirect_file_descriptors);
    }

    // execute command
    execute(arguments);
    return {success, false};
}

int executePipelineNonBackgroundCommands(std::vector<command> &commands, const output_type current_type,
                                         const std::string &current_file) {
    if (commands.empty()) {
        return success;
    }

    int previous_pipe_read_file_descriptor = error_code;
    int redirect_file_descriptors = error_code;
    std::vector<pid_t> pids;

    for (std::size_t index = 0; index < commands.size(); ++index) {
        const bool is_not_last = index != commands.size() - 1;
        const bool is_last = !is_not_last;
        auto &current_command = commands.at(index);

        // create new pipe
        int pipes_file_descriptors[2];
        pipes_file_descriptors[Read] = error_code;
        pipes_file_descriptors[Write] = error_code;
        if (is_not_last) {
            if (pipe(pipes_file_descriptors) == error_code) {
                std::cerr << "bash: pipe: " << strerror(errno) << "\n";
                closeOpened(redirect_file_descriptors);
                closeOpened(previous_pipe_read_file_descriptor);
                closeOpened(pipes_file_descriptors[Read]);
                closeOpened(pipes_file_descriptors[Write]);
                reapAll(pids);
                return failure;
            }
        } else if (is_last && current_type != OUTPUT_TYPE_STDOUT) {
            // open or create file
            redirect_file_descriptors = outputFileOpen(current_type, current_file);
            if (redirect_file_descriptors <= error_code) {
                closeOpened(redirect_file_descriptors);
                closeOpened(pipes_file_descriptors[Read]);
                closeOpened(pipes_file_descriptors[Write]);
                closeOpened(previous_pipe_read_file_descriptor);
                reapAll(pids);
                return failure;
            }
        }

        // fork process:
        const pid_t child_pid = fork();
        if (child_pid <= error_code) {
            std::cerr << "bash: fork: " << strerror(errno) << "\n";
            closeOpened(redirect_file_descriptors);
            closeOpened(previous_pipe_read_file_descriptor);
            closeOpened(pipes_file_descriptors[Read]);
            closeOpened(pipes_file_descriptors[Write]);
            reapAll(pids);
            return failure;
        }

        // in child:
        if (child_pid == 0) {
            if (previous_pipe_read_file_descriptor != error_code) {
                if (dup2(previous_pipe_read_file_descriptor, STDIN_FILENO) == error_code) {
                    std::cerr << "bash: dup2: " << strerror(errno) << "\n";
                    _exit(failure);
                }
            }

            if (is_not_last) {
                if (dup2(pipes_file_descriptors[Write], STDOUT_FILENO) == error_code) {
                    std::cerr << "bash: dup2: " << strerror(errno) << "\n";
                    _exit(failure);
                }
            } else if (is_last && current_type != OUTPUT_TYPE_STDOUT) {
                if (dup2(redirect_file_descriptors, STDOUT_FILENO) == error_code) {
                    std::cerr << "bash: dup2: " << strerror(errno) << "\n";
                    closeOpened(redirect_file_descriptors);
                    _exit(failure);
                }
            }

            closeOpened(redirect_file_descriptors);
            closeOpened(pipes_file_descriptors[Read]);
            closeOpened(pipes_file_descriptors[Write]);
            closeOpened(previous_pipe_read_file_descriptor);

            // Run built ins as child
            if (const auto last_status = builtinRunInChild(current_command, OUTPUT_TYPE_STDOUT, ""); last_status >= 0) {
                _exit(last_status);
            }

            // execute
            const std::vector<char *> arguments = generateArguments(current_command);
            execute(arguments);
            _exit(success);
        }

        // in parent:
        pids.push_back(child_pid);

        closeOpened(previous_pipe_read_file_descriptor);
        closeOpened(redirect_file_descriptors);

        if (is_not_last) {
            // in not last:
            closeOpened(pipes_file_descriptors[Write]);
            previous_pipe_read_file_descriptor = pipes_file_descriptors[Read];
        } else {
            // in last:
            closeOpened(pipes_file_descriptors[Read]);
            closeOpened(pipes_file_descriptors[Write]);
        }
    }

    closeOpened(redirect_file_descriptors);
    closeOpened(previous_pipe_read_file_descriptor);
    int last_status = error_code;
    for (std::size_t index = 0; index < pids.size(); ++index) {
        const auto pid = pids.at(index);
        const int result_wait = waitForChild(pid);
        if (index == pids.size() - 1) {
            last_status = result_wait;
        }
    }

    return last_status;
}

Exit executeCommandLine(command_line *line, const Exit last_exit_status) {
    assert(line != nullptr);
    Exit exit_status = last_exit_status;
    // dumb check: execute only one command from now
    if (line->exprs.empty() || line->is_background) {
        return exit_status;
    }

    // filter commands
    std::vector<command> commands;
    std::size_t pipes_count = 0;
    for (const auto &[type, command] : line->exprs) {
        if (type == EXPR_TYPE_COMMAND) {
            commands.push_back(command.value());
        } else if (type == EXPR_TYPE_PIPE) {
            ++pipes_count;
        } else {
            return Exit {failure, false};
        }
    }
    if (commands.size() != pipes_count + 1) {
        return Exit {failure, false};
    }

    // main logic:
    if (commands.size() == 1 && pipes_count == 0) {    // <- run only single command:
        if (!line->is_background) {
            exit_status = executeNonBackgroundCommand(commands.at(0), line->out_type, line->out_file);
        }
    } else if (commands.size() > 1 && pipes_count > 0) {    // <- run pipe:
        if (!line->is_background) {
            exit_status.last_status = executePipelineNonBackgroundCommands(commands, line->out_type, line->out_file);
        }
    }

    return exit_status;
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

Exit createAndExecuteCommand(parser *parser_object, const Exit current_last_exit_status) {
    Exit last_exit_status = current_last_exit_status;

    while (!last_exit_status.terminate_shell) {
        command_line *current_line_raw = nullptr;
        const parser_error parser_error_code = parser_pop_next(parser_object, &current_line_raw);
        if (parser_error_code == PARSER_ERR_NONE && current_line_raw == nullptr) {
            break;
        }
        std::unique_ptr<command_line> current_line_owner(current_line_raw);

        if (parser_error_code != PARSER_ERR_NONE) {
            continue;
        }
        last_exit_status = executeCommandLine(current_line_raw, last_exit_status);
    }
    return last_exit_status;
}

}    // namespace utils

int main() {
    parser *parser_object = parser_new();
    utils::Exit exit_status = {utils::success, false};

    constexpr std::size_t chunk_size = 16 * 1024;
    char chunk_buffer[chunk_size];

    while (!exit_status.terminate_shell) {
        const ssize_t bytes_to_read = read(STDIN_FILENO, chunk_buffer, chunk_size);
        if (bytes_to_read == 0) {
            // EOF:
            break;
        }

        if (bytes_to_read < 0) {
            // retry:
            if (errno == EINTR) {
                errno = 0;
                continue;
            }
            exit_status = {utils::failure, false};
            break;
        }

        // Command parse
        parser_feed(parser_object, chunk_buffer, static_cast<uint32_t>(bytes_to_read));
        exit_status = utils::createAndExecuteCommand(parser_object, exit_status);
    }

    // optional final drain
    exit_status = utils::createAndExecuteCommand(parser_object, exit_status);

    parser_delete(parser_object);
    return exit_status.last_status;
}
