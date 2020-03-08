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

/* Single-thread pipe test, writes 10K, 1K at a time; then
 *  reads out the bytes, confirming that they're correct.
 *  Reads more, ensures EWOULDBLOCK.  Closes the write end,
 *  ensures that the read end sees EOF.  
 *
 *  THIS TEST SHOULD STALL WITH BLOCKING PIPE */

int main() {
    int i;
    int read_fd, write_fd;
    int pipe_retval;

    /* Print("calling pipe"); */
    for (i = 0; i < 10000; i++) {
        pipe_retval = Pipe(&read_fd, &write_fd);
//        if (i >= 4) {
        Close(read_fd);
        Close(write_fd);
//        }
        if (pipe_retval != 0){
            Print("iteration %d Error: %d\n", i, pipe_retval);
            assert(pipe_retval == 0);
        }

    }


    return 0;
}
