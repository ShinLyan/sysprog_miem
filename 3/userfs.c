#include "userfs.h"

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

enum
{
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block
{
	/** Block memory. */
	char *memory;

	/** How many bytes are occupied. */
	int occupied;

	/** Next block in the file. */
	struct block *next;

	/** Previous block in the file. */
	struct block *prev;
};

struct file
{
	/** Double-linked list of file blocks. */
	struct block *block_list;

	/** Last block in the list above for fast access to the end of file. */
	struct block *last_block;

	/** How many file descriptors are opened on the file. */
	int refs;

	/** File name. */
	char *name;

	/** Files are stored in a double-linked list. */
	struct file *next;

	/** Files are stored in a double-linked list. */
	struct file *prev;

	/** True if the file is logically deleted but still opened. */
	bool is_deleted;

	/** Current size of the file in bytes. */
	size_t size;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc
{
	/** File this descriptor is bound to. */
	struct file *file;

	/** Current block used by the descriptor. */
	struct block *block;

	/** Current offset in the file. */
	size_t offset;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;

static int file_descriptor_count = 0;

static int file_descriptor_capacity = 0;

enum ufs_error_code ufs_errno()
{
	return ufs_error_code;
}

static struct file *find_file(const char *filename)
{
	for (struct file *file = file_list; file; file = file->next)
		if (!file->is_deleted && strcmp(file->name, filename) == 0)
			return file;

	return NULL;
}

static struct block *allocate_block(void)
{
	struct block *block = malloc(sizeof(*block));
	if (!block)
		return NULL;

	block->memory = malloc(BLOCK_SIZE);
	if (!block->memory)
	{
		free(block);
		return NULL;
	}

	block->occupied = 0;
	block->next = block->prev = NULL;
	return block;
}

static void append_block(struct file *file, struct block *block)
{
	if (file->last_block)
	{
		file->last_block->next = block;
		block->prev = file->last_block;
	}
	else
		file->block_list = block;

	file->last_block = block;
}

static void free_file(struct file *file)
{
	struct block *block = file->block_list;
	while (block)
	{
		struct block *next = block->next;
		free(block->memory);
		free(block);
		block = next;
	}

	free(file->name);
	free(file);
}

static void remove_file_from_list(struct file *file)
{
	if (file->prev)
		file->prev->next = file->next;
	else
		file_list = file->next;

	if (file->next)
		file->next->prev = file->prev;
}

int ufs_open(const char *filename, int flags)
{
	struct file *file = find_file(filename);
	if (!file)
	{
		if (!(flags & UFS_CREATE))
		{
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}

		file = calloc(1, sizeof(*file));
		if (!file)
		{
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}

		file->name = strdup(filename);
		file->next = file_list;

		if (file_list)
			file_list->prev = file;

		file_list = file;
	}

	struct filedesc *descriptor = malloc(sizeof(struct filedesc));
	if (!descriptor)
	{
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	descriptor->file = file;
	descriptor->block = file->block_list;
	descriptor->offset = 0;
	file->refs++;

	if (file_descriptor_count == file_descriptor_capacity)
	{
		int new_capacity = file_descriptor_capacity ? file_descriptor_capacity * 2 : 4;
		struct filedesc **new_array = realloc(file_descriptors, new_capacity * sizeof(*new_array));
		if (!new_array)
		{
			free(descriptor);
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}

		for (int i = file_descriptor_capacity; i < new_capacity; i++)
			new_array[i] = NULL;

		file_descriptors = new_array;
		file_descriptor_capacity = new_capacity;
	}

	for (int i = 0; i < file_descriptor_capacity; i++)
	{
		if (!file_descriptors[i])
		{
			file_descriptors[i] = descriptor;
			file_descriptor_count++;
			return i;
		}
	}

	free(descriptor);
	ufs_error_code = UFS_ERR_NO_MEM;
	return -1;
}

ssize_t ufs_write(int file_descriptor, const char *buffer, size_t size)
{
	if (file_descriptor < 0 || file_descriptor >= file_descriptor_capacity ||
		!file_descriptors[file_descriptor])
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct filedesc *descriptor = file_descriptors[file_descriptor];
	struct file *file = descriptor->file;
	size_t bytes_written = 0;

	while (bytes_written < size)
	{
		if (descriptor->offset >= MAX_FILE_SIZE)
		{
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}

		struct block *block = descriptor->block;
		if (!block)
		{
			block = allocate_block();
			if (!block)
			{
				ufs_error_code = UFS_ERR_NO_MEM;
				return -1;
			}

			append_block(file, block);
			descriptor->block = block;
		}

		size_t block_offset = descriptor->offset % BLOCK_SIZE;
		size_t to_copy = size - bytes_written < BLOCK_SIZE - block_offset
							 ? size - bytes_written
							 : BLOCK_SIZE - block_offset;

		memcpy(block->memory + block_offset, buffer + bytes_written, to_copy);

		int new_occupied = block_offset + to_copy;
		if (new_occupied > block->occupied)
			block->occupied = new_occupied;

		descriptor->offset += to_copy;
		if (descriptor->offset > file->size)
			file->size = descriptor->offset;

		bytes_written += to_copy;

		if (descriptor->offset % BLOCK_SIZE == 0)
		{
			if (!block->next)
			{
				struct block *new_block = allocate_block();
				if (!new_block)
				{
					ufs_error_code = UFS_ERR_NO_MEM;
					return -1;
				}

				block->next = new_block;
				new_block->prev = block;
				file->last_block = new_block;
			}

			descriptor->block = block->next;
		}
	}

	return (ssize_t)bytes_written;
}

ssize_t ufs_read(int file_descriptor, char *buffer, size_t size)
{
	if (file_descriptor < 0 || file_descriptor >= file_descriptor_capacity ||
		!file_descriptors[file_descriptor])
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct filedesc *descriptor = file_descriptors[file_descriptor];
	struct file *file = descriptor->file;

	if (descriptor->offset >= file->size)
		return 0;

	size_t bytes_read = 0;
	size_t current_offset = descriptor->offset;
	struct block *block = descriptor->block ? descriptor->block : file->block_list;

	while (block && bytes_read < size && current_offset < file->size)
	{
		size_t block_offset = current_offset % BLOCK_SIZE;
		size_t available = block->occupied > (int)block_offset ? block->occupied - block_offset : 0;
		size_t remaining_file = file->size - current_offset;
		size_t to_copy = available < remaining_file ? available : remaining_file;
		size_t remaining_buffer = size - bytes_read;

		if (to_copy > remaining_buffer)
			to_copy = remaining_buffer;

		if (to_copy == 0)
			break;

		memcpy(buffer + bytes_read, block->memory + block_offset, to_copy);

		current_offset += to_copy;
		bytes_read += to_copy;

		if (block_offset + to_copy >= BLOCK_SIZE)
			block = block->next;
	}

	descriptor->offset = current_offset;
	descriptor->block = block;

	return (ssize_t)bytes_read;
}

int ufs_close(int file_descriptor)
{
	if (file_descriptor < 0 || file_descriptor >= file_descriptor_capacity ||
		!file_descriptors[file_descriptor])
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct filedesc *descriptor = file_descriptors[file_descriptor];
	struct file *file = descriptor->file;

	free(descriptor);
	file_descriptors[file_descriptor] = NULL;
	file_descriptor_count--;
	file->refs--;

	if (file->refs == 0 && file->is_deleted)
	{
		remove_file_from_list(file);
		free_file(file);
	}

	return 0;
}

int ufs_delete(const char *filename)
{
	struct file *file = file_list;
	while (file && strcmp(file->name, filename) != 0)
		file = file->next;

	if (!file)
	{
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (file->refs > 0)
	{
		file->is_deleted = true;
		return 0;
	}

	remove_file_from_list(file);
	free_file(file);

	return 0;
}

#if NEED_RESIZE

int ufs_resize(int fd, size_t new_size)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)fd;
	(void)new_size;
	ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	return -1;
}

#endif

void ufs_destroy(void)
{
	for (int i = 0; i < file_descriptor_capacity; i++)
	{
		if (file_descriptors && file_descriptors[i])
		{
			free(file_descriptors[i]);
			file_descriptors[i] = NULL;
		}
	}

	free(file_descriptors);
	file_descriptors = NULL;
	file_descriptor_capacity = file_descriptor_count = 0;

	struct file *file = file_list;
	while (file)
	{
		struct file *next_file = file->next;
		free_file(file);
		file = next_file;
	}

	file_list = NULL;
}