/*
 * Declarations for file handle and file table management.
 */

#ifndef _FILE_H_
#define _FILE_H_

/*
 * Contains some file-related maximum length constants
 */
#include <limits.h>

/* maximum amount of files */
#define FD_MAX 128
#define FD_EMPTY -1
#define OF_EMPTY 0
/*
 * Put your function declarations and data types here ...
 */
struct open_file {
    struct vnode*   vn;         // pointer to corresponding vnode
    int             ref_count;  // reference count
    off_t           offset;     // offset in the file
    int             mode;       // mode to open in
    int             flags;     // flags to start with
};

struct open_file** of_table;    // pointer to array of open_file structures

// Creates an entry in open file table
int create_of(char* filename, int flags, mode_t mode, struct open_file **of);

// Finds first free of entry and returns its index
int get_of(void);

// Finds first free fd entry and returns its index
int get_fd(void);

// Checks if fd is bad and returns 0 if valid, else error code
int check_fd(int fd);

// Initialises the global open file table, this is only done once
int init_filesystem(void);

// Cleans up the memory, used in cur_killthread and shutdown
void cleanup_filesystem(void);

#endif /* _FILE_H_ */
