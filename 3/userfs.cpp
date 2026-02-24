#include "userfs.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rlist.h"

namespace {

/* ------------------------------------------- Types ------------------------------------------- */
constexpr int success = 0;
constexpr int failure = -1;

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

// Block memory.
struct block {
    explicit block() { std::memset(memory, 0, BLOCK_SIZE); }

    char memory[BLOCK_SIZE] = {0};
    rlist in_block_list = RLIST_LINK_INITIALIZER;
};

struct file {
    rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
    // File name
    std::string name;
    // How many file descriptors are opened on the file
    int references = 0;
    // Number of allocated blocks
    std::size_t block_count = 0;
    // Size of this file
    std::size_t size = 0;
    // Is this file deleted
    bool is_this_deleted = false;
    // A link in the global file list
    rlist in_file_list = RLIST_LINK_INITIALIZER;
};

struct filedesc {
    // Descriptor of this file
    file *at_file = nullptr;
    // Current cursor position
    std::size_t cursor_position = 0;
    // Current block
    block *current_block = nullptr;
    // Current offset
    std::size_t current_offset = 0;
    // Permissions:
    bool readable = false;
    bool writable = false;
};
/* -------------------------------------------- *** -------------------------------------------- */

/* ----------------------------------------- Variables ----------------------------------------- */
/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
std::vector<std::unique_ptr<filedesc>> file_descriptors_table;

// Global error code. Set from any function on any error.
ufs_error_code ufs_last_error_code = UFS_ERR_NO_ERR;
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------ Helpers ------------------------------------------ */
void set_ufs_errno(const ufs_error_code new_errno) {
    ufs_last_error_code = new_errno;
}

auto findFile(const char *filename) -> file * {
    if (filename == nullptr) {
        return nullptr;
    }

    file *result = nullptr;
    rlist_foreach_entry(result, &file_list, in_file_list) {
        if (result->name == filename) {
            return result;
        }
    }
    return nullptr;
}

auto firstBlock(const file *current_file) -> block * {
    if (current_file == nullptr || rlist_empty(&current_file->blocks)) {
        return nullptr;
    }
    return rlist_first_entry(&current_file->blocks, block, in_block_list);
}

auto nextBlock(const file *current_file, block *current_block) -> block * {
    if (current_file == nullptr || current_block == nullptr) {
        return nullptr;
    }
    // is head check
    if (current_block->in_block_list.next == &current_file->blocks) {
        return nullptr;
    }
    return rlist_next_entry(current_block, in_block_list);
}

bool appendBlock(file *current_file) {
    if (current_file == nullptr) {
        return false;
    }

    const auto new_block = new block();
    rlist_add_tail(&current_file->blocks, &new_block->in_block_list);
    ++current_file->block_count;
    return true;
}

[[maybe_unused]] bool freeAllBlocks(file *current_file) {
    if (current_file == nullptr) {
        return false;
    }

    while (!rlist_empty(&current_file->blocks)) {
        const auto head_block = firstBlock(current_file);
        rlist_del(&head_block->in_block_list);
        delete head_block;
    }
    current_file->block_count = 0;

    return true;
}

bool isValidFileDescriptor(const int file_descriptor) {
    if (file_descriptor <= 0) {
        return false;
    }
    const auto table_index = file_descriptor - 1;
    if (static_cast<std::size_t>(table_index) >= file_descriptors_table.size()) {
        return false;
    }
    if (file_descriptors_table[table_index] == nullptr) {
        return false;
    }

    return true;
}

bool setCursor(const std::unique_ptr<filedesc> &current_file_descriptor, const std::size_t new_cursor_position) {
    if (current_file_descriptor == nullptr) {
        return false;
    }
    const auto current_file = current_file_descriptor->at_file;
    if (current_file == nullptr) {
        return false;
    }

    // set new value
    current_file_descriptor->cursor_position = new_cursor_position;
    // rebuild cached values
    current_file_descriptor->current_block = nullptr;
    current_file_descriptor->current_offset = 0;

    const std::size_t target_block_index = new_cursor_position / BLOCK_SIZE;
    const std::size_t target_block_offset = new_cursor_position % BLOCK_SIZE;

    if (target_block_index < current_file->block_count) {
        // walk until needed block:
        auto block = firstBlock(current_file);
        if (block == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < target_block_index; ++index) {
            block = nextBlock(current_file, block);
            assert(block != nullptr);
        }
        current_file_descriptor->current_block = block;
        current_file_descriptor->current_offset = target_block_offset;
    }
    return true;
}

auto fileDescriptorsFirstNullCell() -> std::vector<std::unique_ptr<filedesc>>::iterator {
    return std::find_if(
            std::begin(file_descriptors_table),
            std::end(file_descriptors_table),
            [](const std::unique_ptr<filedesc> &descriptor_pointer) -> bool { return descriptor_pointer == nullptr; });
}

[[maybe_unused]] bool ensureEnoughCapacity(file *file, const std::size_t end_pos) {
    if (file == nullptr) {
        return false;
    }
    if (end_pos == 0) {
        return true;
    }
    const std::size_t needed_blocks = (end_pos + BLOCK_SIZE - 1) / BLOCK_SIZE;
    while (file->block_count < needed_blocks) {
        // ReSharper disable once CppDFAConstantConditions
        if (!appendBlock(file)) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] bool checkMaxFileSize(const std::size_t end_pos) {
    if (end_pos > MAX_FILE_SIZE) {
        set_ufs_errno(UFS_ERR_NO_MEM);
        return false;
    }
    return true;
}

/* -------------------------------------------- *** -------------------------------------------- */
}    // namespace

ufs_error_code ufs_errno() {
    return ufs_last_error_code;
}

int ufs_open(const char *filename, const int flags) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (filename == nullptr) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return failure;
    }

    // Create new file descriptor:
    auto new_file_descriptor = std::make_unique<filedesc>();
    // Find file or create
    if (const auto found_file = findFile(filename); found_file == nullptr) {
        if ((flags & UFS_CREATE) == 0) {
            set_ufs_errno(UFS_ERR_NO_FILE);
            return failure;
        }

        // Create new file:
        const auto new_file = new file();
        new_file->name = filename;
        new_file->references = 0;
        new_file->size = 0;
        new_file->block_count = 0;
        new_file->is_this_deleted = false;
        rlist_add_tail_entry(&file_list, new_file, in_file_list);

        // Add file to descriptor
        new_file_descriptor->at_file = new_file;
    } else {
        new_file_descriptor->at_file = found_file;
    }
    assert(new_file_descriptor->at_file != nullptr);
    ++new_file_descriptor->at_file->references;

    // Setup descriptor
    setCursor(new_file_descriptor, 0);
    constexpr int read_write_mask = UFS_READ_ONLY | UFS_WRITE_ONLY;
    if (const int read_write = flags & read_write_mask; read_write == 0 || read_write == read_write_mask) {
        // default OR explicit read/write
        new_file_descriptor->readable = true;
        new_file_descriptor->writable = true;
    } else if (read_write == UFS_READ_ONLY) {
        new_file_descriptor->readable = true;
        new_file_descriptor->writable = false;
    } else {    // rw == UFS_WRITE_ONLY
        new_file_descriptor->readable = false;
        new_file_descriptor->writable = true;
    }

    // Calculate descriptor
    int descriptor;
    if (const auto find_iter = fileDescriptorsFirstNullCell(); find_iter == std::end(file_descriptors_table)) {
        file_descriptors_table.push_back(std::move(new_file_descriptor));
        descriptor = static_cast<int>(file_descriptors_table.size());
    } else {
        *find_iter = std::move(new_file_descriptor);
        descriptor = static_cast<int>(std::distance(std::begin(file_descriptors_table), find_iter)) + 1;
    }

    return descriptor;
}

ssize_t ufs_write(const int file_descriptor, const char *buffer, const std::size_t size) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return failure;
    }
    const auto descriptor_index = file_descriptor - 1;
    const auto &descriptor_pointer = file_descriptors_table[descriptor_index];
    if (!descriptor_pointer->writable) {
        set_ufs_errno(UFS_ERR_NO_PERMISSION);
        return failure;
    }
    if (size == 0) {
        return static_cast<ssize_t>(success);
    }

    if (size > MAX_FILE_SIZE - descriptor_pointer->cursor_position) {
        set_ufs_errno(UFS_ERR_NO_MEM);
        return failure;
    }

    const auto end_position = descriptor_pointer->cursor_position + size;
    if (end_position > MAX_FILE_SIZE) {
        set_ufs_errno(UFS_ERR_NO_MEM);
        return failure;
    }

    assert(descriptor_pointer->at_file != nullptr);
    if (!ensureEnoughCapacity(descriptor_pointer->at_file, end_position)) {
        set_ufs_errno(UFS_ERR_NO_MEM);
        return failure;
    }
    if (descriptor_pointer->current_block == nullptr) {
        setCursor(descriptor_pointer, descriptor_pointer->cursor_position);
    }

    std::size_t bytes_written = 0;
    while (bytes_written < size) {
        const std::size_t available = BLOCK_SIZE - descriptor_pointer->current_offset;
        const std::size_t chunk = std::min(available, size - bytes_written);
        std::memcpy(descriptor_pointer->current_block->memory + descriptor_pointer->current_offset,
                    buffer + bytes_written,
                    chunk);
        bytes_written += chunk;
        descriptor_pointer->cursor_position += chunk;
        descriptor_pointer->current_offset += chunk;
        if (descriptor_pointer->current_offset == BLOCK_SIZE) {
            descriptor_pointer->current_block =
                    nextBlock(descriptor_pointer->at_file, descriptor_pointer->current_block);
            descriptor_pointer->current_offset = 0;
            // cursor to next block
            if (descriptor_pointer->current_block == nullptr) {
                setCursor(descriptor_pointer, descriptor_pointer->cursor_position);
            }
        }
    }
    descriptor_pointer->at_file->size = std::max(descriptor_pointer->at_file->size, end_position);

    return static_cast<ssize_t>(bytes_written);
}

ssize_t ufs_read(const int file_descriptor, char *buffer, const std::size_t size) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }
    const auto descriptor_index = file_descriptor - 1;
    const auto &descriptor_pointer = file_descriptors_table[descriptor_index];
    if (!descriptor_pointer->readable) {
        set_ufs_errno(UFS_ERR_NO_PERMISSION);
        return failure;
    }
    if (size == 0) {
        return static_cast<ssize_t>(success);
    }
    const auto file = descriptor_pointer->at_file;
    assert(file != nullptr);

    if (descriptor_pointer->cursor_position >= file->size) {
        return static_cast<ssize_t>(success);
    }

    const auto remaining_bytes = file->size - descriptor_pointer->cursor_position;
    const auto bytes_to_read = std::min(size, remaining_bytes);
    if (descriptor_pointer->current_block == nullptr && bytes_to_read > 0) {
        setCursor(descriptor_pointer, descriptor_pointer->cursor_position);
    }

    std::size_t bytes_read_total = 0;
    while (bytes_read_total < bytes_to_read) {
        const auto available_bytes = BLOCK_SIZE - descriptor_pointer->current_offset;
        const std::size_t chunk = std::min(available_bytes, bytes_to_read - bytes_read_total);
        std::memcpy(buffer + bytes_read_total,
                    descriptor_pointer->current_block->memory + descriptor_pointer->current_offset,
                    chunk);
        bytes_read_total += chunk;
        descriptor_pointer->cursor_position += chunk;
        descriptor_pointer->current_offset += chunk;
        if (descriptor_pointer->current_offset == BLOCK_SIZE) {
            descriptor_pointer->current_block =
                    nextBlock(descriptor_pointer->at_file, descriptor_pointer->current_block);
            descriptor_pointer->current_offset = 0;
            // cursor to next block
            if (descriptor_pointer->current_block == nullptr) {
                setCursor(descriptor_pointer, descriptor_pointer->cursor_position);
            }
        }
    }

    return bytes_read_total;
}

int ufs_close(const int file_descriptor) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return failure;
    }

    const auto descriptor_index = file_descriptor - 1;
    auto &descriptor_pointer = file_descriptors_table[descriptor_index];
    const auto file = descriptor_pointer->at_file;
    assert(file != nullptr);
    // remove from table
    descriptor_pointer.reset(nullptr);
    --file->references;
    if (file->references == 0 && file->is_this_deleted == true) {
        freeAllBlocks(file);
        delete file;
    }

    return success;
}

int ufs_delete(const char *filename) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (filename == nullptr) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
}

#if NEED_RESIZE

int ufs_resize(const int file_descriptor, const std::size_t new_size) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }
    if (new_size > MAX_FILE_SIZE) {
        set_ufs_errno(UFS_ERR_NO_MEM);
        return -1;
    }

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
}

#endif

void ufs_destroy() {
    /*
     * The file_descriptors array is likely to leak even if
     * you resize it to zero or call clear(). This is because
     * the vector keeps memory reserved in case more elements
     * would be added.
     *
     * The recommended way of freeing the memory is to swap()
     * the vector with a temporary empty vector.
     */

    /*
    while (!rlist_empty(&file_list)) {

    }
    */

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
}
