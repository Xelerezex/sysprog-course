#include "userfs.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "rlist.h"

namespace {

/* ------------------------------------------- Types ------------------------------------------- */
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
std::vector<filedesc *> file_descriptors_table;

// Global error code. Set from any function on any error.
ufs_error_code ufs_last_error_code = UFS_ERR_NO_ERR;
/* -------------------------------------------- *** -------------------------------------------- */

/* ------------------------------------------ Helpers ------------------------------------------ */
void set_ufs_errno(const ufs_error_code new_errno) {
    ufs_last_error_code = new_errno;
}

[[maybe_unused]] auto findFile(const char *filename) -> file * {
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

[[maybe_unused]] auto nextBlock(const file *current_file, block *current_block) -> block * {
    if (current_file == nullptr || current_block == nullptr) {
        return nullptr;
    }
    // is head check
    if (current_block->in_block_list.next == &current_file->blocks) {
        return nullptr;
    }
    return rlist_next_entry(current_block, in_block_list);
}

[[maybe_unused]] bool appendBlock(file *current_file) {
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

[[maybe_unused]] bool isValidFileDescriptor(const int file_descriptor) {
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

[[maybe_unused]] bool setCursor(filedesc *current_file_descriptor, const std::size_t new_cursor_position) {
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

/* -------------------------------------------- *** -------------------------------------------- */
}    // namespace

ufs_error_code ufs_errno() {
    return ufs_last_error_code;
}

int ufs_open(const char *filename, const int flags) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (filename == nullptr) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }
    /*
    rlist_create(&file_list);

    auto *a = new file;
    a->name = "A";
    rlist_create(&a->in_file_list);

    auto *b = new file;
    b->name = "B";
    rlist_create(&b->in_file_list);

    rlist_add_tail_entry(&file_list, a, in_file_list);
    rlist_add_tail_entry(&file_list, b, in_file_list);

    [[maybe_unused]] file *f = findFile("B");
    */

    (void)flags;

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
}

ssize_t ufs_write(const int file_descriptor, const char *buffer, const std::size_t size) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }

    (void)buffer;
    (void)size;
    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
}

ssize_t ufs_read(const int file_descriptor, char *buffer, const std::size_t size) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }

    (void)buffer;
    (void)size;

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
}

int ufs_close(const int file_descriptor) {
    set_ufs_errno(UFS_ERR_NO_ERR);
    if (!isValidFileDescriptor(file_descriptor)) {
        set_ufs_errno(UFS_ERR_NO_FILE);
        return -1;
    }

    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
    return -1;
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
    set_ufs_errno(UFS_ERR_NO_ERR);
    /*
     * The file_descriptors array is likely to leak even if
     * you resize it to zero or call clear(). This is because
     * the vector keeps memory reserved in case more elements
     * would be added.
     *
     * The recommended way of freeing the memory is to swap()
     * the vector with a temporary empty vector.
     */
    set_ufs_errno(UFS_ERR_NOT_IMPLEMENTED);
}
