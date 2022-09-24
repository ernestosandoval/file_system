# file_system

In this project, we implement a simple file system on top of a virtual disk:

    int make_fs(char *disk_name);
    int mount_fs(char *disk_name);
    int umount_fs(char *disk_name);
    int fs_open(char *name);
    int fs_close(int fildes);
    int fs_create(char *name);
    int fs_delete(char *name); 
    int fs_read(int fildes, void *buf, size_t nbyte);
    int fs_write(int fildes, void *buf, size_t nbyte);
    int fs_get_filesize(int fildes);
    int fs_lseek(int fildes, off_t offset);
    int fs_truncate(int fildes, off_t length);
    
To compile, run:

    gcc -c fs.c
	gcc -c disk.c
    gcc application [list of application object files] fs.o disk.o
