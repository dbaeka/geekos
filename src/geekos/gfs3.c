/*
 * GeekOS file system
 * Copyright (c) 2008, David H. Hovemeyer <daveho@cs.umd.edu>, 
 * Neil Spring <nspring@cs.umd.edu>, Aaron Schulman <schulman@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */


#include <limits.h>
#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/bitset.h>
#include <geekos/synch.h>
#include <geekos/bufcache.h>
#include <geekos/gfs3.h>
#include <geekos/pfat.h>
#include <geekos/projects.h>
#include <geekos/vfs.h>
#include <geekos/user.h>

/* ----------------------------------------------------------------------
 * Private data and functions
 * ---------------------------------------------------------------------- */

int debugGFS3 = 0;
#define Debug(args...) if (debugGFS3) Print("GFS3: " args)


typedef struct {
    struct gfs3_superblock superblock; // loaded
    struct gfs3_dirent *rootDir;
    struct gfs3_inode *rootDirEntry;
    struct FS_Buffer_Cache *fscache;
    struct Mutex lock;          /* instance lock guards fileList */ //loaded
    uchar_t *bitmap;
} GFS3_Instance;

typedef struct {
    struct gfs3_inode *inode;
    gfs3_inodenum inodenum;
    char *fileDataCache;
} GFS3_File;

struct gfs3_inode *get_inode(struct FS_Buffer_Cache *bufferCache, gfs3_inodenum inodenum) {
    gfs3_blocknum block_num = INODE_BLOCKNUM(inodenum);
    struct FS_Buffer *buf = 0;

    Get_FS_Buffer(bufferCache, block_num, &buf);


    ulong_t offset = INODE_OFFSET(inodenum);
    ulong_t addr = (ulong_t) buf->data + offset;
    struct gfs3_inode *inode = (struct gfs3_inode *) (addr);
    Release_FS_Buffer(bufferCache, buf);

    return inode;
}

gfs3_blocknum Get_BlocNum_From_Extent(struct gfs3_inode *inode, ulong_t filePos) {

    gfs3_blocknum fileBlock = filePos / GFS3_BLOCK_SIZE;
    int i;
    uint_t total = 0;
    uint_t oldBlockLength = 0;
    for (i = 0; i < GFS3_EXTENTS; i++) {
        struct gfs3_extent *extent = &inode->extents[i];
        uint_t startBlock = extent->start_block;
        uint_t newBlockLength = extent->length_blocks;
        if ((newBlockLength + total) > fileBlock)
            return startBlock + fileBlock - oldBlockLength;
        total += newBlockLength;
        oldBlockLength = newBlockLength;
    }
    return 0;
}


GFS3_File *Create_GFS3_File(struct gfs3_inode *inode, gfs3_inodenum inodenum) {
    GFS3_File *file = 0;
    char *cache = 0;

    if ((file = (GFS3_File *) Malloc(sizeof(*file))) == 0)
        goto memfail;

    if ((cache = (char *) Malloc(sizeof(*cache))) == 0)
        goto memfail;

    file->inode = inode;
    file->inodenum = inodenum;
    file->fileDataCache = cache;
    goto done;

    memfail:
    if (file != 0) {
        Free(file);
    }
    if (cache != 0) {
        Free(cache);
    }
    file = NULL;

    done:
    return file;
}

ulong_t Round_Up_To_Four(ulong_t addr) {
    ulong_t newAddr = (addr + (ulong_t) 3) & ~(ulong_t) 0x03;
    return newAddr;
}

struct gfs3_dirent *Find_Dirent(GFS3_Instance *instance, struct gfs3_inode *inode) {
    if (inode->type != GFS3_DIRECTORY)
        return NULL;

    struct gfs3_extent *extent = &inode->extents[0];

    struct FS_Buffer *buf;

    if (Get_FS_Buffer(instance->fscache, extent->start_block, &buf) != 0)
        return NULL;
    struct gfs3_dirent *dirent = (struct gfs3_dirent *) buf->data;
    Release_FS_Buffer(instance->fscache, buf);

    return dirent;
}

int Find_File_In_Dirent(struct gfs3_dirent *dirent, struct gfs3_inode *inode, char *name) {

    uint_t sizeStart = 0;
    uint_t minEntryLength = 4;

    struct gfs3_dirent *current = dirent;

    while (sizeStart < inode->size) {
        char current_name[251];
        memcpy(current_name, current->name, current->name_length);
        if (strncmp(name, current_name, strlen(name)) == 0)
            return current->inum;
        sizeStart += current->entry_length + minEntryLength;
        current = (struct gfs3_dirent *) ((ulong_t) current + Round_Up_To_Four(4 + 2 + current->name_length));
    }
    return -1;
}


gfs3_blocknum Find_Inode_From_Path(GFS3_Instance *instance, const char *path, struct gfs3_inode **node) {
    struct gfs3_inode *root = instance->rootDirEntry;

    if (strcmp(path, "/") == 0) {
        *node = root;
        return instance->superblock.block_with_inode_zero;
    }

    char mutable_path[GFS3_MAX_PATH_LEN + 1], prefix[GFS3_MAX_PATH_LEN + 1];
    const char *suffix = 0;
    Unpack_Path(path, prefix, &suffix);

    struct gfs3_inode *currentInode = root;
    struct gfs3_dirent *currentDirent = 0;
    int target;

    while (suffix[0] == '/' && strlen(suffix) > 1) {
        currentDirent = Find_Dirent(instance, currentInode);
        if ((target = Find_File_In_Dirent(currentDirent, currentInode, prefix)) < 0)
            return 0;
        currentInode = get_inode(instance->fscache, target);
        strncpy(mutable_path, suffix, GFS3_MAX_PATH_LEN + 1);
        Unpack_Path(mutable_path, prefix, &suffix);
    }

    currentDirent = Find_Dirent(instance, currentInode);
    if ((target = Find_File_In_Dirent(currentDirent, currentInode, prefix)) < 0)
        return 0;
    *node = get_inode(instance->fscache, target);
    return (gfs3_blocknum) target;
}

/*
 * Get metadata for given file.
 */
static int GFS3_FStat(struct File *file, struct VFS_File_Stat *stat) {
    int rc = 0;
    GFS3_File *gfs3File = (GFS3_File *) file->fsData;
    struct gfs3_inode *fileInode = gfs3File->inode;

    struct VFS_File_Stat *stats = (struct VFS_File_Stat *) Malloc(sizeof(struct VFS_File_Stat));
    memset(stats, '\0', sizeof(*stat));
    stats->isDirectory = (unsigned int) (fileInode->type == GFS3_DIRECTORY);
    stats->size = fileInode->size;
    memcpy(stat, stats, sizeof(struct VFS_File_Stat));
    Free(stats);

    return rc;
}

/*
 * Read data from current position in file.
 */
static int GFS3_Read(struct File *file, void *buf, ulong_t numBytes) {

    GFS3_File *gfs3File = (GFS3_File *) file->fsData;
    GFS3_Instance *instance = (GFS3_Instance *) file->mountPoint->fsData;

    ulong_t startPos = file->filePos, i;
    ulong_t endPos = file->filePos + numBytes;
    ulong_t bytesRead = 0;
    int rc = 0;

    if (gfs3File->inode->type == GFS3_DIRECTORY) {
        rc = ENOTFOUND;
        goto exit;
    }

    if (startPos >= endPos) {
        rc = EINVALID;
        goto exit;
    }

    gfs3_blocknum start_block = Get_BlocNum_From_Extent(gfs3File->inode, startPos);
    gfs3_blocknum end_block = Get_BlocNum_From_Extent(gfs3File->inode, endPos);

    gfs3_blocknum first_block = gfs3File->inode->extents[0].start_block;

    ulong_t start_offset = startPos - (start_block - first_block) * GFS3_BLOCK_SIZE;
//    gfs3_blocknum current_block = start_block;

    instance->fscache = Create_FS_Buffer_Cache(file->mountPoint->dev, GFS3_BLOCK_SIZE);
    struct FS_Buffer *cacheBuf = 0;

    for (i = start_block; i <= end_block; i++) {
        if ((rc = Get_FS_Buffer(instance->fscache, i, &cacheBuf)) != 0)
            goto memfail;

        if (i == start_block) {
            if (numBytes < GFS3_BLOCK_SIZE - start_offset) {
                memcpy(buf, cacheBuf->data + start_offset, numBytes);
                bytesRead += numBytes;
            } else {
                memcpy(buf, cacheBuf->data + start_offset, GFS3_BLOCK_SIZE - start_offset);
                bytesRead += GFS3_BLOCK_SIZE - start_offset;
            }
        } else if (i == end_block - 1) {
            ulong_t bytesLeft = numBytes - bytesRead;
            memcpy(buf + bytesRead, cacheBuf->data, bytesLeft);
            bytesRead += bytesLeft;
        } else {
            memcpy(buf + bytesRead, cacheBuf->data, GFS3_BLOCK_SIZE);
            bytesRead += GFS3_BLOCK_SIZE;
        }
        Release_FS_Buffer(instance->fscache, cacheBuf);
    }

    file->filePos += bytesRead;
    return (int) bytesRead;

    memfail:
    Release_FS_Buffer(instance->fscache, cacheBuf);

    exit:
    return rc;
}

/*
 * Write data to current position in file.
 */
static int GFS3_Write(struct File *file, void *buf, ulong_t numBytes) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem write operation");
    return EUNSUPPORTED;
}


/*
 * Seek to a position in file; returns 0 on success.
 */
static int GFS3_Seek(struct File *file, ulong_t pos) {
    int rc = 0;
    if (!(pos >= file->filePos && pos <= file->endPos)) {
        rc = EINVALID;
        goto exit;
    }

    file->filePos = pos;

    exit:
    return rc;
}

/*
 * Close a file.
 */
static int GFS3_Close(struct File *file) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem close operation");
    return EUNSUPPORTED;
}

/*static*/ struct File_Ops s_gfs3FileOps = {
        &GFS3_FStat,
        &GFS3_Read,
        &GFS3_Write,
        &GFS3_Seek,
        &GFS3_Close,
        0,                          /* Read_Entry */
};

/*
 * Stat operation for an already open directory.
 */
static int GFS3_FStat_Directory(struct File *dir,
                                struct VFS_File_Stat *stat) {
    int rc = 0;
    GFS3_File *gfs3File = (GFS3_File *) dir->fsData;
    struct gfs3_inode *fileInode = gfs3File->inode;

    if (fileInode->type != GFS3_DIRECTORY) {
        rc = ENOTDIR;
        goto exit;
    }

    struct VFS_File_Stat *stats = (struct VFS_File_Stat *) Malloc(sizeof(struct VFS_File_Stat));
    memset(stats, '\0', sizeof(*stat));
    stats->isDirectory = (unsigned int) (fileInode->type == GFS3_DIRECTORY);
    stats->size = fileInode->size;
    memcpy(stat, stats, sizeof(struct VFS_File_Stat));
    Free(stats);

    exit:
    return rc;
}

/*
 * Directory Close operation.
 */
static int GFS3_Close_Directory(struct File *dir) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem Close directory operation");
    return EUNSUPPORTED;
}

/*
 * Read a directory entry from an open directory.
 */
static int GFS3_Read_Entry(struct File *dir, struct VFS_Dir_Entry *entry) {
    GFS3_File *gfs3File = (GFS3_File *) dir->fsData;
    GFS3_Instance *instance = (GFS3_Instance *) dir->mountPoint->fsData;

    struct gfs3_dirent *dirent = Find_Dirent(instance, gfs3File->inode);

    ulong_t i;
    int rc = 0;
    for (i = 0; i < dir->filePos; i++) {
        dirent = (struct gfs3_dirent *) ((ulong_t) dirent + Round_Up_To_Four(4 + 2 + dirent->name_length));
    }

    if (dirent == NULL) {
        rc = VFS_NO_MORE_DIR_ENTRIES;
        goto exit;
    }

    struct gfs3_inode *inode = get_inode(instance->fscache, dirent->inum);
    dir->filePos++;

    char *dirname = (char *) Malloc(dirent->name_length);
    strcpy(dirname, dirent->name);
    dirname[dirent->name_length] = '\0';
    strcpy(entry->name, dirname);

    struct VFS_File_Stat *stat = (struct VFS_File_Stat *) Malloc(sizeof(struct VFS_File_Stat));
    memset(stat, '\0', sizeof(*stat));
    stat->isDirectory = (unsigned int) (inode->type == GFS3_DIRECTORY);
    stat->size = inode->size;
    memcpy(&entry->stats, stat, sizeof(struct VFS_File_Stat));
    Free(stat);

    exit:
    return rc;
}

/*static*/ struct File_Ops s_gfs3DirOps = {
        &GFS3_FStat_Directory,
        0,                          /* Read */
        0,                          /* Write */
        0,                          /* Seek */
        &GFS3_Close_Directory,
        &GFS3_Read_Entry,
};


/*
 * Open a file named by given path.
 */
static int GFS3_Open(struct Mount_Point *mountPoint, const char *path,
                     int mode, struct File **pFile) {
    int rc = 0;
    struct Kernel_Thread *current = CURRENT_THREAD;
    if (current->userContext != 0)
        if (current->userContext->fileCount == USER_MAX_FILES) {
            rc = EMFILE;
            goto exit;
        }

    if (strlen(path) > GFS3_MAX_PATH_LEN) {
        rc = ENAMETOOLONG;
        goto exit;
    }

    // Retrieve instance
    GFS3_Instance *instance = (GFS3_Instance *) mountPoint->fsData;

    // Find file inode
    struct gfs3_inode *fileInode;
    if ((rc = Find_Inode_From_Path(instance, path, &fileInode)) == 0) {
        rc = ENOTFOUND;
        goto exit;
    }

    if (fileInode->type == GFS3_DIRECTORY) {
        rc = ENOTFOUND;
        goto exit;
    }

    gfs3_inodenum node_num = rc;

    GFS3_File *gfs3_file = Create_GFS3_File(fileInode, node_num);
    if (gfs3_file == 0) {
        rc = ENOMEM;
        goto exit;
    }

    struct File *file = Allocate_File(&s_gfs3FileOps, 0, fileInode->size, gfs3_file, mode, mountPoint);
    if (file == 0) {
        rc = ENOMEM;
        goto exit;
    }

    *pFile = file;
    return rc;

    exit:
    return rc;
}

/*
 * Create a directory named by given path.
 */
static int GFS3_Create_Directory(struct Mount_Point *mountPoint,
                                 const char *path) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem create directory operation");
    return EUNSUPPORTED;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Open_Directory(struct Mount_Point *mountPoint,
                               const char *path, struct File **pDir) {
    int rc = 0;

    if (strlen(path) > GFS3_MAX_PATH_LEN) {
        rc = ENAMETOOLONG;
        goto exit;
    }

    // Retrieve instance
    GFS3_Instance *instance = (GFS3_Instance *) mountPoint->fsData;

    // Find file inode
    struct gfs3_inode *fileInode = 0;
    if ((rc = Find_Inode_From_Path(instance, path, &fileInode)) == 0) {
        rc = ENOTFOUND;
        goto exit;
    }

    if (fileInode->type != GFS3_DIRECTORY) {
        rc = ENOTFOUND;
        goto exit;
    }

    gfs3_inodenum node_num = rc;

    GFS3_File *gfs3_file = Create_GFS3_File(fileInode, node_num);
    if (gfs3_file == 0) {
        rc = ENOMEM;
        goto exit;
    }

    struct File *fileDir = Allocate_File(&s_gfs3DirOps, 0, fileInode->size, gfs3_file, O_READ, mountPoint);
    if (fileDir == 0) {
        rc = ENOMEM;
        goto exit;
    }

    *pDir = fileDir;
    return rc;

    exit:
    return rc;
}

/*
 * Open a directory named by given path.
 */
static int GFS3_Delete(struct Mount_Point *mountPoint, const char *path,
                       bool recursive) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem delete operation");
    return EUNSUPPORTED;
}

/*
 * Get metadata (size, permissions, etc.) of file named by given path.
 */
static int GFS3_Stat(struct Mount_Point *mountPoint, const char *path,
                     struct VFS_File_Stat *stat) {
    GFS3_Instance *instance = (GFS3_Instance *) mountPoint->fsData;
    int rc = 0;

    // Find file inode
    struct gfs3_inode *fileInode = 0;
    if ((rc = Find_Inode_From_Path(instance, path, &fileInode)) == 0) {
        rc = ENOTFOUND;
        goto exit;
    }

    struct VFS_File_Stat *stats = (struct VFS_File_Stat *) Malloc(sizeof(struct VFS_File_Stat));
    memset(stats, '\0', sizeof(*stat));
    stats->isDirectory = (unsigned int) (fileInode->type == GFS3_DIRECTORY);
    stats->size = fileInode->size;
    memcpy(stat, stats, sizeof(struct VFS_File_Stat));
    Free(stats);

    exit:
    return rc;
}

/*
 * Synchronize the filesystem data with the disk
 * (i.e., flush out all buffered filesystem data).
 */
static int GFS3_Sync(struct Mount_Point *mountPoint) {
    TODO_P(PROJECT_GFS3, "GeekOS filesystem sync operation");
    return EUNSUPPORTED;
}

static int GFS3_Disk_Properties(struct Mount_Point *mountPoint,
                                unsigned int *block_size,
                                unsigned int *blocks_in_disk) {
    GFS3_Instance *instance = (GFS3_Instance *) mountPoint->fsData;
    *blocks_in_disk = instance->superblock.blocks_per_disk;
    *block_size = GFS3_BLOCK_SIZE;

    return 0;
}

/*static*/ struct Mount_Point_Ops s_gfs3MountPointOps = {
        &GFS3_Open,
        &GFS3_Create_Directory,
        &GFS3_Open_Directory,
        &GFS3_Stat,
        &GFS3_Sync,
        &GFS3_Delete,
        0,                          /* Rename  */
        0,                          /* Link  */
        0,                          /* SymLink  */
        0,                          /* setuid  */
        0,                          /* acl  */
        &GFS3_Disk_Properties,
};

static int GFS3_Format(struct Block_Device *blockDev
__attribute__ ((unused))) {
    TODO_P(PROJECT_GFS3,
           "DO NOT IMPLEMENT: There is no format operation for GFS3");
    return EUNSUPPORTED;
}

static int GFS3_Mount(struct Mount_Point *mountPoint) {

    GFS3_Instance *instance = 0;
    struct gfs3_superblock *superblock = 0;
    void *bootSect = 0;
    int rc;
    //   int i;

    if (mountPoint->fsData != NULL) {
        goto done;
    }

    /* Allocate instance. */
    instance = (GFS3_Instance *) Malloc(sizeof(*instance));
    if (instance == 0)
        goto memfail;
    memset(instance, '\0', sizeof(*instance));
    superblock = &instance->superblock;
    Debug("Created instance object\n");

    /*
     * Allocate buffer to read bootsector,
     * which contains metainformation about the PFAT filesystem.
     */
    bootSect = Malloc(SECTOR_SIZE);
    if (bootSect == 0)
        goto memfail;

    /* Read boot sector */
    if ((rc = Block_Read(mountPoint->dev, 0, bootSect)) < 0)
        goto fail;
    Debug("Read boot sector\n");

    /* Copy filesystem parameters from boot sector */
    memcpy(superblock,
           ((char *) bootSect) + PFAT_BOOT_RECORD_OFFSET,
           sizeof(bootSector));
    Debug("Copied boot record\n");

    /* Does magic number match? */
    if (superblock->gfs3_magic != GFS3_MAGIC) {
        Print("Bad magic number (%x) for GFS3 filesystem\n",
              superblock->gfs3_magic);
        goto invalidfs;
    }
    Debug("Magic number is good!\n");

    instance->fscache = Create_FS_Buffer_Cache(mountPoint->dev, GFS3_BLOCK_SIZE);
    struct FS_Buffer *buf = 0;

    // root directory is inode 1
    instance->rootDirEntry = get_inode(instance->fscache, GFS3_INUM_ROOT);

    /* Initialize instance lock and PFAT_File list. */
    Mutex_Init(&instance->lock);

    // inode 2 contains bitmap
    struct gfs3_inode *inode = get_inode(instance->fscache, 2);

    instance->bitmap = (uchar_t *) Malloc(GFS3_BLOCK_SIZE); // must apparently allocate before...
    instance->bitmap = Create_Bit_Set(8 * GFS3_BLOCK_SIZE);

    if ((rc = Get_FS_Buffer(instance->fscache, inode->extents->start_block, &buf)) != 0) {
        Print("\n failed to read block\n");
        goto fail;
    }

    memcpy(instance->bitmap, buf->data, GFS3_BLOCK_SIZE);
    Release_FS_Buffer(instance->fscache, buf);

    /*
     * Success!
     * This mount point is now ready
     * to handle file accesses.
     */
    mountPoint->ops = &s_gfs3MountPointOps;
    mountPoint->fsData = instance;
    done:
    return 0;

    memfail:
    rc = ENOMEM;
    goto fail;
    invalidfs:
    rc = EINVALIDFS;
    goto fail;
    fail:
    if (instance != 0) {
        if (instance->rootDir != 0)
            Free(instance->rootDir);
        Free(instance);
    }
    if (bootSect != 0)
        Free(bootSect);
    Release_FS_Buffer(instance->fscache, buf);
    return rc;
}


static struct Filesystem_Ops s_gfs3FilesystemOps = {
        &GFS3_Format,
        &GFS3_Mount,
};

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

void Init_GFS3(void) {
    Register_Filesystem("gfs3", &s_gfs3FilesystemOps);
}
