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
#include <signal.h>
#include <geekos/errno.h>

int global = 0;                 /* remnant of basic fork testing */
int sigpiped = 0;               /* flag to be set when the handler is invoked */
char bigbuf[2048];              /* spot in which to read data */

/* invoked when sigpipe is delivered. */
void sigpipe_handler(int signal) {
    if(signal == SIGPIPE) {
        Print("GOOD: sigpiped the parent!\n");
        sigpiped = 1;
    } else {
        Print("unexpected signal received!\n");
    }
}

int main() {
    int n = 0;
    int child_pid = 0;
    int i, rc;
    int read_fd, write_fd;
    int read_bytes, written_bytes, pipe_retval;
    char buf[256];

    Print
        ("Parent should handle sigpipe, child should terminate on sigpipe.\n");

    Print("original pid=%d\n", Get_PID());

    pipe_retval = Pipe(&read_fd, &write_fd);
    assert(pipe_retval == 0);

    child_pid = Fork();

    /* each process should increment these independently */
    n++;
    global ++;

    if(child_pid > 0) {
        /* I am the parent, let's read. */
        int total_read_bytes = 0;
        Print("parent n=%d, global=%d, child_pid=%d, my_pid=%d\n",
              n, global, child_pid, Get_PID());

        rc = Signal(sigpipe_handler, SIGPIPE);
        assert(rc == 0);        /* Signal should succeed */

        /* this component (read while > 0) only synchronizes to
           wait for the child when the pipe is blocking, else it
           will likely just terminate quickly, and we'll fall down
           to the explicit wait below. */
        while ((read_bytes = Read(read_fd, bigbuf, 2048)) > 0 &&
               total_read_bytes < 16384) {
            Print("parent read %d bytes\n", read_bytes);
            total_read_bytes += read_bytes;
        }
        Close(read_fd);

        /* Ensure that the child is done, has closed the read
           end of the pipe, and that our write into the pipe
           will yield a sigpipe */
        i = 0;
        do {
            rc = Wait(child_pid);
            Print("expected to reap child_pid %d, Wait returned %d\n",
                  child_pid, rc);
        } while (rc < 0 && i++ < 10);
        assert(rc >= 0);

        written_bytes = Write(write_fd, "nope", 4);
        if(written_bytes != EPIPE) {
            Print("wrote %d bytes into a closed pipe\n", written_bytes);
        }
        assert(written_bytes == EPIPE);
        assert(sigpiped);
        Close(write_fd);

    } else if(child_pid == 0) {
        unsigned long totally_written = 0;
        Close(read_fd);
        Print("child n=%d, global=%d, child_pid=%d, my_pid=%d\n",
              n, global, child_pid, Get_PID());

        for(i = 0; i < 256; i++)
            buf[i] = i;

        /* intended mostly for testing a blocking pipe, but
           should give ample opportunity to sigpipe the child if
           the pipe is not yet blocking */
        for(totally_written = 0; totally_written <= 16384;
            totally_written += sizeof(buf)) {
            written_bytes = Write(write_fd, buf, sizeof(buf));
        }
        Print("Good so far, child could write a lot.");
        for(totally_written = 0; totally_written <= 8192;
            totally_written += sizeof(buf)) {
            written_bytes = Write(write_fd, buf, sizeof(buf));
        }
        written_bytes = Write(write_fd, buf, sizeof(buf));

        /* shouldn't get here with a blocking pipe and the
           default signal handler, since SIGPIPE should have
           killed the child.  However, we should have written 
           so many times anyway, so we have had ample chances to 
           SIGPIPE and terminate. 
         */

        if(written_bytes != EPIPE) {
            Print("FAIL: Didn't even epipe.\n");
            Print("written_bytes on the last call to Write = %d\n",
                  written_bytes);
        } else {
            Print("FAIL: Didn't sigpipe to kill the child.\n");
        }
        Close(write_fd);
    } else {
        Print("FAIL: Error in Fork(): %d\n", child_pid);
        return 1;
    }

    return 0;
}
