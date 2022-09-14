#include "fs.h"
#include "disk.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// DISK_BLOCKS
//#define BLOCK_SIZE   4096

#define NUM_BLOCKS 4096
#define MAX_FILE_SIZE NUM_BLOCKS * BLOCK_SIZE
#define MAX_NUM_FILES 64
#define MAX_SIZE_FILENAME 15
#define MAX_NUM_FD 32
#define UNASSIGNED -1
#define SUPER_BLOCK_INDEX 0

//helper declarations
int get_dir_index(char *name);
int get_free_block();

// struct declarations
/*
The directory holds the names of the files. 
When using a FAT-based design, the directory also stores, for each file, its file size and the head of the list of corresponding data blocks. 
When you use inodes, the directory only stores the mapping from file names to inodes. 
*/
typedef struct {
        char file_name[MAX_SIZE_FILENAME];
        int file_size;
        int first_block_num;
        int total_fds;
	bool busy;
} directory_t;

/*
The file allocation table (FAT) is convenient because it can be used to keep track of empty blocks and the mapping between files and their data blocks. 
When you use an inode-based design, you will need: 
a bitmap to mark disk blocks as used 
and an inode array to hold file information (including the file size and pointers to data blocks). 
*/
typedef struct {
        bool busy;// = false;
        int next;// = UNASSIGNED;
} fat_t;

/*
The super block is typically the first block of the disk, and it stores information about the location of the other data structures. 
For example, you can store in the super block the whereabouts of the file allocation table, the directory, and the start of the data blocks. 
*/
typedef struct {
        int dir_start_block;
        int dir_size;
        int fat_start_block;
        int fat_total_blocks;
        //int data_start_block;
        int total_data_storage;
        int avail_data_blocks;

	directory_t *my_directory;
	fat_t* my_fat;
} super_block_t;

// file descriptor type
/*
A file descriptor is associated with a file, and it also contains a file offset (seek pointer). 
This offset indicates the point in the file where read and write operations start. It is implicitly updated (incremented) whenever you perform a fs_read or fs_write operation, and it can be explicitly moved within the file by calling fs_lseek. 
Note that file descriptors are not stored on disk.
*/
typedef struct {
        char file_name[MAX_SIZE_FILENAME];
        int dir_index;
        off_t offset;
} fd_t;

// global struct
static super_block_t *my_sb;
fd_t *my_fds[MAX_NUM_FD];

// implementations
int make_fs(char *disk_name) {
	if (make_disk(disk_name) == -1) return -1;
	if (open_disk(disk_name) == -1) return -1;

	// super_block_t my_sb
	my_sb = (super_block_t*)malloc(sizeof(super_block_t));
	if (my_sb == NULL) return -1;

	fat_t* my_fat = (fat_t*)malloc(DISK_BLOCKS * sizeof(fat_t));
	if (my_fat == NULL) return -1;

	// the first block will hold our super block
	my_fat[0].busy = true;

	char buf[BLOCK_SIZE];
	int num_full_block_copies = (DISK_BLOCKS * sizeof(fat_t)) / BLOCK_SIZE;
	int remainder_bytes = (DISK_BLOCKS * sizeof(fat_t)) % BLOCK_SIZE;
	char *start_copy_from = (char*)my_fat;
	int write_block = 1;
	int num_fat_blocks = 0;
	for (int i = 0; i < num_full_block_copies; ++i) {
		my_fat[write_block].busy = true;
		if ((i == (num_full_block_copies - 1)) && (remainder_bytes == 0)) break;
		else my_fat[write_block].next = ++write_block;
	}
	if (remainder_bytes != 0) my_fat[write_block].busy = true;
	my_sb->my_fat = my_fat;
	write_block = 1;
	for (int i = 0; i < num_full_block_copies; ++i) {
		memset(buf, 0, BLOCK_SIZE);
		memcpy(buf, start_copy_from, BLOCK_SIZE);
		block_write(write_block++, buf);
		num_fat_blocks++;
		start_copy_from += BLOCK_SIZE;
	}
	if (remainder_bytes != 0) {
		memset(buf, 0, BLOCK_SIZE);
		memcpy(buf, start_copy_from, remainder_bytes);
		block_write(write_block++, buf);
		num_fat_blocks++;
	}

	my_sb->dir_start_block = write_block;
	my_sb->dir_size = 0;
	my_sb->fat_start_block = 1;
	my_sb->fat_total_blocks = num_fat_blocks;
	//my_sb->data_start_block = 4096;
	my_sb->total_data_storage = 0;
	my_sb->avail_data_blocks = 4096;
	my_sb->my_directory = (directory_t*) malloc(MAX_NUM_FILES * sizeof(directory_t));
	for (int i = 0; i < MAX_NUM_FILES; i++) {
		my_sb->my_directory[i].busy = false;
	}

	memset(buf, 0, BLOCK_SIZE);
	memcpy(buf, my_sb, sizeof(super_block_t));
	block_write(0, buf);
	free(my_sb);
	free(my_fat);

	if(close_disk() == -1) return -1;
	return 0;
}

// whatever the heck we do to my_fat we need to do to my_directory too
int mount_fs(char *disk_name) {
	if (open_disk(disk_name) == -1) return -1;

	my_sb = (super_block_t*)malloc(sizeof(super_block_t));
	if (my_sb == NULL) return -1;

	char buf[BLOCK_SIZE];
	memset(buf, 0, BLOCK_SIZE);
	block_read(0, buf);
	memcpy(my_sb, buf, sizeof(my_sb));

	fat_t *my_fat = my_sb->my_fat;
	my_fat = (fat_t*)malloc(DISK_BLOCKS * sizeof(fat_t));
	if (my_fat == NULL) return -1;

	char *start_copy_from = (char*)my_fat;
	int fat_start = my_sb->fat_start_block;
	for (int i = 0; i < my_sb->fat_total_blocks; ++i) {
		memset(buf, 0, BLOCK_SIZE);
		block_read(fat_start++, buf);
		memcpy(start_copy_from, buf, BLOCK_SIZE);
		start_copy_from += BLOCK_SIZE;
	}
	directory_t *my_directory = my_sb->my_directory;
	if (my_directory == NULL) return -1;


	int num_full_block_copies = (my_sb->dir_size * sizeof(directory_t)) / BLOCK_SIZE;
	int remainder_bytes = (my_sb->dir_size * sizeof(directory_t)) % BLOCK_SIZE;
	start_copy_from = (char*)my_directory;
	int dir_block_copy = my_sb->dir_start_block;

	for (int i = 0; i < num_full_block_copies; ++i) {
		memset(buf, 0, BLOCK_SIZE);
		block_read(dir_block_copy, buf);
		memcpy(start_copy_from, buf, BLOCK_SIZE);
		dir_block_copy = my_fat[dir_block_copy].next;
		if ((dir_block_copy == UNASSIGNED) && (remainder_bytes == 0)) break;
		start_copy_from += BLOCK_SIZE;
	}
	if ((dir_block_copy != UNASSIGNED) && (remainder_bytes != 0)) {
		memset(buf, 0, BLOCK_SIZE);
		block_read(dir_block_copy, buf);
		memcpy(start_copy_from, buf, remainder_bytes);
	}


	for (int i = 0; i < MAX_NUM_FD; ++i) {
		my_fds[i] = NULL;
	}

	return 0;
}

// i think here we hould only write super block from memory to disk???
int umount_fs(char *disk_name) {
	if (!disk_name) return -1;

	char buf[BLOCK_SIZE];
	int num_full_block_copies = (my_sb->dir_size * sizeof(directory_t)) / BLOCK_SIZE;

	int remainder_bytes = (my_sb->dir_size * sizeof(directory_t)) % BLOCK_SIZE;
	directory_t *my_directory = my_sb->my_directory;
	char **start_copy_from = (char**)my_directory;
	int dir_block_copy = my_sb->dir_start_block;

	for (int i = 0; i < num_full_block_copies; ++i) {
		memcpy(buf, start_copy_from, BLOCK_SIZE);
		block_write(dir_block_copy, buf);
		dir_block_copy++;
		start_copy_from += BLOCK_SIZE;
	}

	if ((dir_block_copy != UNASSIGNED) && (remainder_bytes != 0)) {
		block_read(dir_block_copy, buf);
		memcpy(buf, start_copy_from, remainder_bytes);
		block_write(dir_block_copy, buf);
	}


	fat_t *my_fat = my_sb->my_fat;
	my_fat = (fat_t*)malloc(DISK_BLOCKS * sizeof(fat_t));
	if (my_fat == NULL) return -1;

	char *start_copy_from2 = (char*)my_fat;
	int fat_start = my_sb->fat_start_block;
	for (int i = 0; i < my_sb->fat_total_blocks; ++i) {
		memcpy(buf, start_copy_from2, BLOCK_SIZE);
		block_write(fat_start++, buf);
		start_copy_from2 += sizeof(directory_t);
	}

	memset(buf, 0, BLOCK_SIZE);
	memcpy(buf, my_sb, sizeof(my_sb));
	block_write(0, buf);

	for (int i = 0; i < MAX_NUM_FD; ++i) {
		if (my_fds[i] != NULL) {
			free(my_fds[i]);
			my_fds[i] = NULL;
		}
	}
	free(my_sb->my_fat);
	free(my_sb);
	close_disk();
	return 0;
}

int fs_open(char *name) {
	if (!name) return -1;

	int dir_index = get_dir_index(name);
	if (dir_index == -1) return -1;

	for (int i = 0; i < MAX_NUM_FD; i++) {
		if (my_fds[i] == NULL) {
			fd_t *new_fd = malloc(sizeof(fd_t));
			strcpy(new_fd->file_name, name);
			new_fd->dir_index = dir_index;
			new_fd->offset = 0;
			my_sb->my_directory[dir_index].total_fds++;
			my_fds[i] = new_fd;
			return i;
		}
	}
	return -1;
}

int fs_close(int fildes) {
	if ((fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;
	my_sb->my_directory[my_fds[fildes]->dir_index].total_fds--;
	free(my_fds[fildes]);
	my_fds[fildes] = NULL;
	return 0;
}

int fs_create(char *name) {
	// if we already have MAX_NUM_FILES files
	if ((my_sb->dir_size) >= MAX_NUM_FILES) return -1;
	// if name is too long
	if (strlen(name) > MAX_SIZE_FILENAME) return -1;
	// if filename already exists
	if (get_dir_index(name) != -1) return -1;


	directory_t *my_directory = my_sb->my_directory;
	for (int i = 0; i < MAX_NUM_FILES; i++) {
		if (my_directory[i].busy == false) {
			my_sb->dir_size++;
			directory_t* new_file = &my_directory[i];

			strcpy(new_file->file_name, name);
			new_file->file_size = 0;
			new_file->first_block_num = UNASSIGNED;
			new_file->total_fds = 0;
			new_file->busy = true;

			return 0;
		}
	}

	return -1;
}

int fs_delete(char *name) {
	int dir_index = get_dir_index(name);
	if (dir_index == -1) {
		return -1;
	}

	directory_t *my_directory = my_sb->my_directory;

	directory_t* current_file = &my_directory[dir_index];

	if (current_file->total_fds != 0) {
		return -1;
	}

	int block_num = current_file->first_block_num;
	fat_t *my_fat = my_sb->my_fat;
	while (block_num != UNASSIGNED) {
		my_sb->avail_data_blocks--;
		my_fat[block_num].busy = false;
		int next = my_fat[block_num].next;
		my_fat[block_num].next = UNASSIGNED;
		block_num = next;
	}

	my_sb->total_data_storage -= current_file->file_size;

	my_sb->dir_size--;
	my_directory[dir_index].busy = false;

	return 0;
}

int fs_read(int fildes, void *buf, size_t nbyte) {
	if ((nbyte <= 0) || (nbyte > MAX_FILE_SIZE) || (fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;

	fd_t *current_fd = my_fds[fildes];
	directory_t *current_file = &my_sb->my_directory[current_fd->dir_index];
	fat_t *my_fat = my_sb->my_fat;

	size_t bytes_to_read = current_file->file_size - current_fd->offset;
	if (bytes_to_read == 0) {
		return 0;
	} else if (bytes_to_read > nbyte) {
		bytes_to_read = nbyte;
	}
	
	char read_block[BLOCK_SIZE];
	size_t bytes_read = 0;
	size_t bytes_to_read_this_block = 0;

	int current_block = current_file->first_block_num;
	if (current_block == -1) return -1;
	int offset = current_fd->offset;
	while (offset >= BLOCK_SIZE) {
		current_block = my_fat[current_block].next;
		if (current_block == -1) return -1;
		offset -= BLOCK_SIZE;
	}
	if (offset > 0) {
		if (current_block == -1) return -1;
		block_read(current_block, read_block);
		
		if ((bytes_to_read + offset) < BLOCK_SIZE) {
			bytes_to_read_this_block = bytes_to_read;
		} else {
			bytes_to_read_this_block = BLOCK_SIZE - (size_t) offset;
		}

		memcpy(buf, read_block + offset, bytes_to_read_this_block);
		buf += bytes_to_read_this_block;
		bytes_to_read -= bytes_to_read_this_block;
		bytes_read += bytes_to_read_this_block;
		current_block = my_fat[current_block].next;
	}

	while (bytes_to_read > 0) {
		if (current_block == -1) return -1;
		block_read(current_block, read_block);

		if (bytes_to_read < BLOCK_SIZE) {
			bytes_to_read_this_block = bytes_to_read;
		} else {
			bytes_to_read_this_block = BLOCK_SIZE;
		}

		memcpy(buf, read_block, bytes_to_read_this_block);
		buf += bytes_to_read_this_block;
		bytes_to_read -= bytes_to_read_this_block;
		bytes_read += bytes_to_read_this_block;
		current_block = my_fat[current_block].next;
	}
	current_fd->offset += bytes_read;
	return bytes_read;
}

int fs_write(int fildes, void *buf, size_t nbyte) {
	if ((nbyte <= 0) || (fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;

	fd_t *current_fd = my_fds[fildes];
	// try to get rid of this one
	int dir_index = current_fd->dir_index;
	directory_t *current_file = &my_sb->my_directory[dir_index];
	fat_t *my_fat = my_sb->my_fat;

	size_t bytes_written = 0;
	size_t bytes_to_write = MAX_FILE_SIZE - my_sb->total_data_storage;
	if (bytes_to_write == 0) {
		return 0;
	} else if (bytes_to_write > nbyte) {
		bytes_to_write = nbyte;
	}
	size_t bytes_to_write_this_block = 0;
	char *buffer = (char*) buf;
	char read_block[BLOCK_SIZE];

	int current_block = current_file->first_block_num;
	int offset = current_fd->offset;
	while (offset >= BLOCK_SIZE) {
		if (my_fat[current_block].next == -1) {
			my_fat[current_block].next = get_free_block();
			my_fat[my_fat[current_block].next].busy = true;
			my_fat[my_fat[current_block].next].next = UNASSIGNED;
		}
		current_block = my_fat[current_block].next;
		if (current_block == -1) {
			return 0;
		}
		offset -= BLOCK_SIZE;
	}
	
	if (offset > 0) {
		if (current_block == -1) return -1;
		block_read(current_block, read_block);
		if ((bytes_to_write + offset) < BLOCK_SIZE) {
			bytes_to_write_this_block = bytes_to_write;
		} else {
			bytes_to_write_this_block = BLOCK_SIZE - (size_t) offset;
		}

		for (int i = offset; i < bytes_to_write_this_block + offset; i++) {
			read_block[i] = buffer[bytes_written];
			bytes_written++;
			bytes_to_write--;
		}
	} else {
		if (current_block == -1) {
			current_block = get_free_block();
			if (current_block == -1) {
				// out of blocks
				return 0;
			}
			current_file->first_block_num = current_block;
			my_fat[current_block].busy = true;
			my_fat[current_block].next = UNASSIGNED;
		}
		block_read(current_block, read_block);
		
		if ((bytes_to_write) < BLOCK_SIZE) {
			bytes_to_write_this_block = bytes_to_write;
		} else {
			bytes_to_write_this_block = BLOCK_SIZE;
		}

		for (int i = 0; i < bytes_to_write_this_block; i++) {
			read_block[i] = buffer[bytes_written];
			bytes_written++;
			bytes_to_write--;
		}
	}
	block_write(current_block, read_block);

	// move to end
	if (bytes_written == nbyte) {
		current_fd->offset += bytes_written;
		if (current_file->file_size < current_fd->offset) {
			my_sb->total_data_storage += (current_fd->offset - current_file->file_size);
			current_file->file_size = current_fd->offset;
		}
		return bytes_written;
	}

	while (bytes_to_write > 0) {
		if (my_fat[current_block].next == -1) {
			my_fat[current_block].next = get_free_block();
			my_fat[my_fat[current_block].next].busy = true;
			my_fat[my_fat[current_block].next].next = UNASSIGNED;
			my_sb->avail_data_blocks--;
		}
		current_block = my_fat[current_block].next;
		if (current_block == -1) {
			// out of blocks return
			// move to end
			bytes_to_write = 0;
			current_fd->offset += bytes_written;
			if (current_file->file_size < current_fd->offset) {
				my_sb->total_data_storage += (current_fd->offset - current_file->file_size);
				current_file->file_size = current_fd->offset;
			}
			return bytes_written;
		}

		block_read(current_block, read_block);

		if ((bytes_to_write) < BLOCK_SIZE) {
			bytes_to_write_this_block = bytes_to_write;
		} else {
			bytes_to_write_this_block = BLOCK_SIZE;
		}

		for (int i = 0; i < bytes_to_write_this_block; i++) {
			read_block[i] = buffer[bytes_written];
			bytes_written++;
			bytes_to_write--;
		}

		if (bytes_to_write == 0) {
			block_write(current_block, read_block);
			current_fd->offset += bytes_written;
			if (current_file->file_size < current_fd->offset) {
				my_sb->total_data_storage += (current_fd->offset - current_file->file_size);
				current_file->file_size = current_fd->offset;
			}
			return bytes_written;
		}

		block_write(current_block, read_block);
	}
	return bytes_written;
}

int fs_get_filesize(int fildes) {
	if ((fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;
	return my_sb->my_directory[my_fds[fildes]->dir_index].file_size;
}

int fs_lseek(int fildes, off_t offset) {
	if ((fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;
	fd_t *current_fd = my_fds[fildes];
	if ((offset < 0) || (offset > my_sb->my_directory[current_fd->dir_index].file_size)) return -1;
	current_fd->offset = offset;
	return 0;
}

int fs_truncate(int fildes, off_t length) {
	if ((fildes < 0) || (fildes >= MAX_NUM_FD) || (my_fds[fildes] == NULL)) return -1;

	fd_t *current_fd = my_fds[fildes];

	directory_t* current_file = &my_sb->my_directory[current_fd->dir_index];

	if ((length < 0) || (current_file->file_size < (int) length)) return -1;

	int current_block = current_file->first_block_num;

	int counter = (int) length;

	int old_file_size = current_file->file_size;

	while (counter > BLOCK_SIZE) {
		current_block = my_sb->my_fat[current_block].next;
		if (current_block == -1) return -1;

		counter -= BLOCK_SIZE;
	}

	current_block = my_sb->my_fat[current_block].next;

	fat_t * my_fat = my_sb->my_fat;
	while (current_block != UNASSIGNED) {
		int next = my_fat[current_block].next;
		my_fat[current_block].busy = false;
		my_fat[current_block].next = UNASSIGNED;
		my_sb->avail_data_blocks++;
		current_block = next;
	}

	my_sb->total_data_storage = my_sb->total_data_storage - old_file_size + (int) length;
	current_file->file_size = (int)length;
	if (current_fd->offset > current_file->file_size) current_fd->offset = current_file->file_size;

	return 0;
}

//helpers
int get_dir_index(char *name) {
	directory_t *my_directory = my_sb->my_directory;
	for (int i = 0; i < MAX_NUM_FILES; i++) {
		if ((my_directory[i].busy == true) && (strcmp(name, my_directory[i].file_name) == 0)) return i;
	}
	return -1;
}

int get_free_block() {
	fat_t * my_fat = my_sb->my_fat;
	for (int i = 4096; i < DISK_BLOCKS; ++i) {
		if (my_fat[i].busy == false) {
			return i;
		}
	}
	return -1;
}
