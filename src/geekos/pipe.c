/*
 * Copyright (c) 2001,2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * Copyright (c) 2003,2013,2014 Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 */
#include <geekos/pipe.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/errno.h>
#include <geekos/projects.h>
#include <geekos/int.h>


const struct File_Ops Pipe_Read_Ops =
        {NULL, Pipe_Read, NULL, NULL, Pipe_Close, NULL};
const struct File_Ops Pipe_Write_Ops =
        {NULL, NULL, Pipe_Write, NULL, Pipe_Close, NULL};

int Pipe_Create(struct File **read_file, struct File **write_file) {
    struct Pipe *pipe = Malloc(sizeof(struct Pipe));
    if (pipe) {
        pipe->data_buffer = Malloc(MAX_PIPE_BUFFER);
        if (pipe->data_buffer) {
            pipe->rPos = pipe->wPos = pipe->bufferLength = 0;
            pipe->num_readers = pipe->num_writers = 1;
            *read_file = Allocate_File(&Pipe_Read_Ops, 0, 0, (void *) pipe, 0, 0);
            if (*read_file) {
                *write_file = Allocate_File(&Pipe_Write_Ops, 0, 0, (void *) pipe, 0, 0);
                if (*write_file)
                    return 0;
                Free(*read_file);
            }
        }
        Free(pipe);
    }
    return ENOMEM;
}

int Pipe_Read(struct File *f, void *buf, ulong_t numBytes) {
    struct Pipe *pipe = (struct Pipe *) (f->fsData);

    if (!pipe->data_buffer || !buf)
        return EINVALID;

    int bytesRead = 0;
    ulong_t i;

    char *sourceBuffer = (char *) pipe->data_buffer;
    char *dstBuffer = (char *) buf;

    if (pipe->num_writers > 0)
        if (!pipe->bufferLength)
            return EWOULDBLOCK;

    for (i = 0; i < numBytes; i++) {
        if (!pipe->bufferLength)
            break;
        dstBuffer[i] = sourceBuffer[pipe->rPos];
        pipe->bufferLength--;
        bytesRead++;
        pipe->rPos = (pipe->rPos + 1) % MAX_PIPE_BUFFER;
    }

    return bytesRead;
}

int Pipe_Write(struct File *f, void *buf, ulong_t numBytes) {
    struct Pipe *pipe = (struct Pipe *) (f->fsData);
    if (!pipe->num_readers)
        return EPIPE;

    if (!pipe->data_buffer || !buf)
        return EINVALID;

    int bytesWritten = 0;
    ulong_t i;

    char *dstBuffer = (char *) pipe->data_buffer;
    char *sourceBuffer = (char *) buf;

    for (i = 0; i < numBytes; i++) {
        if (pipe->bufferLength == MAX_PIPE_BUFFER)
            break;
        dstBuffer[pipe->wPos] = sourceBuffer[i];
        pipe->bufferLength++;
        bytesWritten++;
        pipe->wPos = (pipe->wPos + 1) % MAX_PIPE_BUFFER;
    }

    return bytesWritten;
}

int Pipe_Close(struct File *f) {
    struct Pipe *pipe = (struct Pipe *) (f->fsData);
    if (!pipe->data_buffer)
        return EINVALID;

    if (f->ops == &Pipe_Read_Ops && pipe->num_readers > 0)
        pipe->num_readers--;
    else if (f->ops == &Pipe_Write_Ops && pipe->num_writers > 0)
        pipe->num_writers--;

    if (pipe->bufferLength && pipe->num_readers)
        return 0;

    if (!pipe->data_buffer)
        Free(pipe->data_buffer);
    if (!pipe->num_readers && !pipe->num_writers)
        Free(pipe);

    return 0;
}
