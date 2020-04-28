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
#include <conio.h>
#include <process.h>
#include <sched.h>
#include <sema.h>
#include <string.h>
#include <fileio.h>
#include <geekos/errno.h>       /* ensure not /usr/include/errno.h */

/* test read with no writers. */

int main(int argc, char **argv) {
    int i, j;
    int read_fd, write_fd;
    int read_bytes, written_bytes, pipe_retval;
    char buf[1024];

    for(i = 0; i < 1024; i++) {
        buf[i] = i;
    }

    /* Print("calling pipe"); */
    pipe_retval = Pipe(&read_fd, &write_fd);
    assert(pipe_retval == 0);

    for(i = 0; i < 10; i++) {
        written_bytes = Write(write_fd, buf, 1024);
        assert(written_bytes == 1024);
    }

    Close(write_fd);

    for(i = 0; i < 10; i++) {
        read_bytes = Read(read_fd, buf, 1024);
        if(read_bytes != 1024) {
            Print("read unexpected number of bytes iteration %d rc %d\n",
                  i, read_bytes);
            assert(read_bytes == 1024);
        }
        for(j = 0; j < 1024; j++) {
            if(buf[j] != (char)j) {
                Print("mismatch at iter %d index %d, buf[j] = %d\n", i, j,
                      buf[j]);
            }
            assert(buf[j] == (char)j);
        }
    }

    read_bytes = Read(read_fd, buf, 256);
    if(read_bytes != 0) {
        Print("FAIL: expected 0, got %d reading at the end\n",
              read_bytes);
        assert(read_bytes == EWOULDBLOCK);
    }

    Close(read_fd);

    return 0;
}
