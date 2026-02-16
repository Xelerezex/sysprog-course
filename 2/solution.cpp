#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "parser.h"

static void execute_command_line(const command_line *line) {
    assert(line != nullptr);
    printf("================================\n");
    printf("Command line:\n");
    printf("Is background: %d\n", static_cast<int>(line->is_background));
    printf("Output: ");

    if (line->out_type == OUTPUT_TYPE_STDOUT) {
        printf("stdout\n");
    } else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        printf("new file - \"%s\"\n", line->out_file.c_str());
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        printf("append file - \"%s\"\n", line->out_file.c_str());
    } else {
        assert(false);
    }

    printf("Expressions:\n");
    for (const auto &[type, command] : line->exprs) {
        if (type == EXPR_TYPE_COMMAND) {
            printf("\tCommand: %s", command->exe.c_str());
            for (const std::string &argument : command->args) {
                printf(" %s", argument.c_str());
            }
            printf("\n");
        } else if (type == EXPR_TYPE_PIPE) {
            printf("\tPIPE\n");
        } else if (type == EXPR_TYPE_AND) {
            printf("\tAND\n");
        } else if (type == EXPR_TYPE_OR) {
            printf("\tOR\n");
        } else {
            assert(false);
        }
    }
}

int main() {
    constexpr size_t current_buffer_size = 1024;
    char buffer[current_buffer_size];
    long read_result;
    parser *parser_object = parser_new();
    while ((read_result = read(STDIN_FILENO, buffer, current_buffer_size)) > 0) {
        parser_feed(parser_object, buffer, read_result);
        command_line *current_line = nullptr;
        while (true) {
            const parser_error error_code = parser_pop_next(parser_object, &current_line);
            if (error_code == PARSER_ERR_NONE && current_line == nullptr) {
                break;
            }
            if (error_code != PARSER_ERR_NONE) {
                printf("Error: %d\n", static_cast<int>(error_code));
                continue;
            }
            execute_command_line(current_line);
            delete current_line;
        }
    }
    parser_delete(parser_object);
    return 0;
}
