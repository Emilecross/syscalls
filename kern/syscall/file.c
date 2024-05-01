#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <file.h>
#include <syscall.h>
#include <copyinout.h>

/*
    HELPER FUNCTIONS
*/

int create_of(char* filename, int flags, mode_t mode, struct open_file **of) {
    *of = kmalloc(sizeof(struct open_file));
    if (*of == NULL) return ENOMEM;
    struct vnode* vn;
    int result = vfs_open(filename, flags, mode, &vn);
    if (result) return result;
    (*of)->vn = vn;
    (*of)->ref_count = 0;
    (*of)->offset = 0;
    (*of)->mode = mode;
    (*of)->flags = flags;
    return 0;
}

int get_of() {
    int i = 0;
    while (i++ < OPEN_MAX) {
        if (of_table[i] == OF_EMPTY) return i;
    }
    return i;
}

int get_fd() {
    int i = 0;
    while (i++ < FD_MAX) {
        if (curproc->fd_table[i] == FD_EMPTY) return i;
    }
    return i;
}

int check_fd(int fd) {
    // error out if user fd is bad
    if (fd < 0 || fd >= FD_MAX) return EBADF;
    // error out if fd somehow became invalid
    int of = curproc->fd_table[fd];
    if (of < 0 || of >= OPEN_MAX) return EBADF;
    // error out if entry in open file table was empty
    if (of_table[of] == OF_EMPTY) return EBADF;
    return 0;
}

int init_filesystem() {
    // Allocate memory for the open file table
    of_table = kmalloc(sizeof(struct open_file*) * OPEN_MAX);
    if (of_table == NULL) return ENOMEM;
    memset(of_table, OF_EMPTY, sizeof(struct open_file*) * OPEN_MAX);

    // Setup std out
    char conname[5];
    strcpy(conname, "con:");
    int r1 = create_of(conname, O_WRONLY, 0664, &of_table[1]);
    if (r1) return r1;

    // Setup std err
    strcpy(conname, "con:"); 
    int r2 = create_of(conname, O_RDONLY, 0664, &of_table[2]);
    if (r2) return r2;

    // success
    return 0;
}

void cleanup_filesystem() {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (of_table[i] == OF_EMPTY) continue;
        vfs_close(of_table[i]->vn);
        kfree(of_table[i]);
    }
}

/*
    SYSCALLS
*/

int sys_open(userptr_t filename, int flags, mode_t mode, int* rtn) {
    // take in filename safely
    char k_filename[PATH_MAX];
    int err = copyinstr(filename, k_filename, PATH_MAX, NULL);
    if (err) return err;
    if (k_filename == NULL) return EFAULT;

    // recover file access mode and check valid
    int access_mode = flags & O_ACCMODE;
    if (access_mode != O_RDONLY &&
        access_mode != O_WRONLY &&
        access_mode != O_RDWR) return EINVAL;

    // try to open
    struct vnode* vn;
    err = vfs_open(k_filename, flags, mode, &vn);
    if (err) return err;

    // opened so now setup bookkeeping
    int fd = get_fd();
    if (fd == FD_MAX) return EMFILE;
    int of = get_of();
    if (of == OPEN_MAX) return ENFILE;
    err = create_of(k_filename, flags, mode, &of_table[of]);
    if (err) return err;
    curproc->fd_table[fd] = of;

    // if opened in append mode then move offset
    if (flags & O_APPEND) {
        struct stat s;
        err = VOP_STAT(of_table[of]->vn, &s);
        if (err) return err;
        of_table[of]->offset = s.st_size;
    }

    // success so change retval and return 0 signalling sucess
    *rtn = fd;
    return 0;
}

int sys_close(int fd, int* rtn) {
    int err = check_fd(fd);
    if (err) return err;

    // fd is valid so start closing
    int of = curproc->fd_table[fd];
    if (--of_table[of]->ref_count == 0) {
        vfs_close(of_table[of]->vn);
        kfree(of_table[of]);
    }

    // close fd, note that even if vfs_close fails we should invalidiate the fd
    curproc->fd_table[fd] = FD_EMPTY;

    // success so change retval and return 0 signalling sucess
    *rtn = 0;
    return 0;
}

ssize_t sys_read(int fd, userptr_t buf, size_t buflen, int* rtn) {
    // check fd is valid
    int err = check_fd(fd);
    if (err) return err;
    int of_idx = curproc->fd_table[fd];
    struct open_file* of = of_table[of_idx];

    // check if file was opened for write only
    if (of->flags & O_WRONLY) return EBADF;

    // file is now valid so start reading
    // create and init iovec and uio
    struct iovec* iovec = kmalloc(sizeof(struct iovec));
    if (iovec == NULL) return ENOMEM;
    struct uio* uio = kmalloc(sizeof(struct uio));
    if (uio == NULL) return ENOMEM;
    uio_uinit(iovec, uio, buf, buflen, of->offset, UIO_READ);

    // do the read
    err = VOP_READ(of->vn, uio);
    if (err) return err;

    // track new offset and data read (to_read - remaining)
    of->offset = uio->uio_offset;

    // success so change retval
    *rtn = buflen - uio->uio_resid;

    // clean up
    kfree(iovec);
    kfree(uio);

    // return 0 signalling sucess
    return 0;
}

ssize_t sys_write(int fd, userptr_t buf, size_t nbytes, int* rtn) {
    // check fd is valid
    int err = check_fd(fd);
    if (err) return err;
    int of_idx = curproc->fd_table[fd];
    struct open_file* of = of_table[of_idx];

    // check if file was opened for read only
    // have to & O_ACCMODE because & O_RDONLY is doing & 0 which always is false
    if ((of->flags & O_ACCMODE) == O_RDONLY) return EBADF;

    // file is now valid so start writing
    // create and init iovec and uio
    struct iovec* iovec = kmalloc(sizeof(struct iovec));
    if (iovec == NULL) return ENOMEM;
    struct uio* uio = kmalloc(sizeof(struct uio));
    if (uio == NULL) return ENOMEM;
    uio_uinit(iovec, uio, buf, nbytes, of->offset, UIO_WRITE);

    // do the write
    err = VOP_WRITE(of->vn, uio);
    if (err) return err;

    // track new offset and data read (to_write - remaining)
    of->offset = uio->uio_offset;

    // success so change retval
    *rtn = nbytes - uio->uio_resid;

    // clean up
    kfree(iovec);
    kfree(uio);

    // return 0 signalling sucess
    return 0;
}

off_t sys_lseek(int fd, off_t pos, int whence, off_t* rtn) {
    // whence invalid
    if (!(whence == SEEK_SET || whence == SEEK_CUR || whence == SEEK_END)) {
        return EINVAL;
    }

    // check fd is valid
    int err = check_fd(fd);
    if (err) return err;

    // fd valid, get open file
    int of_idx = curproc->fd_table[fd];
    struct open_file* of = of_table[of_idx];

    // check seekable
    if(!VOP_ISSEEKABLE(of->vn)) return ESPIPE;

    // get new offset
    off_t new_offset;
    struct stat s;
    err = VOP_STAT(of->vn, &s);
    if (err) return err;
    if (whence == SEEK_END) {
        new_offset = s.st_size + pos;
    } else if (whence == SEEK_SET) {
        new_offset = pos;
    } else {
        new_offset = pos + of->offset;
    }
    // check offset after seek is valid
    if (new_offset > s.st_size || new_offset < 0) return EINVAL;

    // success so change retval and return 0 signalling sucess
    *rtn = of->offset = new_offset;
    return 0;
}

int sys_dup2(int oldfd, int newfd, int* rtn) {
    // check oldfd is valid
    int err = check_fd(oldfd);
    if (err) return err;
    // check newfd would be valid
    if (newfd < 0 || newfd >= FD_MAX) return EBADF;

    // old and newfd checked
    int old_of_idx = curproc->fd_table[oldfd];
    struct open_file* old_of = of_table[old_of_idx];

    // cloning onto yourself should have no effect
    if (oldfd == newfd) {
        *rtn = newfd;
        return 0;
    }

    // close file at new fd if it contained a file
    if (curproc->fd_table[newfd] != FD_EMPTY) {
        err = sys_close(newfd, rtn);
        if (err) return err;
    }

    // make newfd refer to same entry in of_table as oldfd increment ref_count
    curproc->fd_table[newfd] = curproc->fd_table[oldfd];
    old_of->ref_count++;

    // success so change retval and return 0 signalling sucess
    *rtn = newfd;
    return 0;
}