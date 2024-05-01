_______________________________________________________ General overview

First we should know that open should return a file descriptor for use with other system calls.

A file descriptor should be a way to access an array of open files kept by the kernel. This array of open files
then corresponds to inodes which are representative of the actual data on the disk.

File descriptor table -> Open files table -> inode table -> datablocks on disk

File descriptor table: (Should be of size FD_MAX = 128, because thats the bare minimum that we need to implenent)
Multiple FDs can reference the same file in the open file table.
Open + write to a single FD shouldn't move the offset of file referenced by other FDs, they should not share an entry in the open file table.
Open + dup2 + write should move the offset of both FDs, so they should share an entry in the file table.

Open files table: (Should be of size OPEN_MAX, since the system can only have that many open files)
In the open files table each entry should store information about the inode index, number of times opened (ref count), current
position of the file (lseek).

Since we are only taking care of the bookkeeping, we dont need to worry about anything else underneath, we will leave it to the vfs and VOP.

_______________________________________________________ Data structures

struct open_file {
    struct vnode*   vn;         // pointer to corresponding vnode
    int             ref_count;  // reference count
    off_t           offset;     // offset in the file
    int             mode;       // mode to open in
    int             flags;     // flags to start with
};

struct open_file** of_table;    // pointer to array of open_file structures

int fd_table[FD_MAX = 128]      // each entry stores open file index

In earlier iterations, and fd struct would have stored a pointer to an open file and the flags. I soon realised that this was a redundant idea and it could
just store the index of the open file inside the open file table. Then at this point I realised that it would be much simpler if I just changed fd to an int
and moved the flags into the open file struct that the fd referenced.

TLDR; fd is an int equal to the index of the open file in the open file table that fd references.

_______________________________________________________ Issues and considerations

Some considerations were made in determinig which should be malloc'd and which to be statically sized arrays. I chose the of_table to be dynamically sized
because it was larger and I wanted to check if there was enough memory for the large array. I allowed for the fd_table to be statically sized as FD_MAX = 128
is quite small.

_______________________________________________________ Global and per process datastructures

The fd_table should be per process and the of_table should be global (across the whole system). The of_table is initialized once in runprogram.c, this is 
enforced by checking that the of_table is NULL and only then is the of_table created. 

_______________________________________________________ Implementation of fork

Currently biggest issues are with l_seek and dup2. With dup2, if there are 2 fd's referencing the same open file and we perform 2 simultaneous writes,
we cant control access to the offset. This would result in unintended results when writing (unexpected behaviour). Another problem may be with sys_close. 
We are not controlling access and modification of the ref_count. So we may end up with cases where an open file with 0 fd's referencing it isnt closed, 
hence data is lost.

When forking since parent and children have the same fd_table which holds ints for indexes of the of_table which is global and shared by all processes in
the system. When lseeking, which moves the value of the offset in the open file, we cannot know the state (where the offset ends up) due to unknown
execution order of the calls. If there is a subsequent read/write, then we may end up with unexpected behaviour/results.

Another race condition that is independent of our implementation of the syscalls but still relevant to the given implementation of the system as a whole 
is because children also inherit the trapframe from the parent. A race condition may occur when the trapframe is modified by the parent before the child 
has a chance to copy it.

These are some of the issues that may be encountered if fork was to be implemented in our current system without any modifications.
