/*
 * ELF executable loading
 * Copyright (c) 2003, Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 * Copyright (c) 2003, David H. Hovemeyer <daveho@cs.umd.edu>
 *
 * All rights reserved.
 *
 * This code may not be resdistributed without the permission of the copyright holders.
 * Any student solutions using any of this code base constitute derviced work and may
 * not be redistributed in any form.  This includes (but is not limited to) posting on
 * public forums or web sites, providing copies to (past, present, or future) students
 * enrolled in similar operating systems courses the University of Maryland's CMSC412 course.
 *
 * $Revision: 1.31 $
 *
 */

#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/ktypes.h>
#include <geekos/screen.h>      /* for debug Print() statements */
#include <geekos/pfat.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/user.h>
#include <geekos/fileio.h>
#include <geekos/elf.h>

#include <geekos/paging.h>

int elfDebug = 0;

/**
 * From the data of an ELF executable, determine how its segments
 * need to be loaded into memory.
 * @param exeFileData buffer containing the executable file
 * @param exeFileLength length of the executable file in bytes
 * @param exeFormat structure describing the executable's segments
 *   and entry address; to be filled in
 * @return 0 if successful, < 0 on error
 */
int Parse_ELF_Executable(char *exeFileData, ulong_t exeFileLength,
                         struct Exe_Format *exeFormat) {
    elfHeader *hdr;
    programHeader *phdr;
    int i;

    hdr = (elfHeader *) exeFileData;

    /*
     * FIXME: when checking offsets, we really ought to be
     * checking overflow cases.  Need to use functions from
     * range.h (which needs to be implemented, too)
     */

    if(exeFileLength < sizeof(elfHeader) ||
       strncmp(exeFileData, "\x7F" "ELF", 4) != 0) {
        if(elfDebug)
            Print("Not an ELF executable\n");
        return ENOEXEC;
    }

    if(hdr->phnum > EXE_MAX_SEGMENTS) {
        if(elfDebug)
            Print("Too many segments (%d) in ELF executable\n",
                  hdr->phnum);
        return ENOEXEC;
    }

    if(exeFileLength < hdr->phoff + (hdr->phnum * sizeof(programHeader))) {
        if(elfDebug)
            Print("Not enough room for program header\n");
        return ENOEXEC;
    }

    exeFormat->numSegments = hdr->phnum;
    exeFormat->entryAddr = hdr->entry;

    phdr = (programHeader *) (exeFileData + hdr->phoff);
    for(i = 0; i < hdr->phnum; ++i) {
        struct Exe_Segment *segment = &exeFormat->segmentList[i];

        /*
         * Fill in segment offset, length, address
         * FIXME: should check that segments are valid
         */
        segment->offsetInFile = phdr[i].offset;
        segment->lengthInFile = phdr[i].fileSize;
        segment->startAddress = phdr[i].vaddr;
        segment->sizeInMemory = phdr[i].memSize;
        segment->protFlags = (phdr[i].flags & PF_W) ? VM_WRITE : 0;

        if(segment->lengthInFile > segment->sizeInMemory) {
            if(elfDebug)
                Print
                    ("Segment %d: length in file (%lu) exceeds size in memory (%lu)\n",
                     i, segment->lengthInFile, segment->sizeInMemory);
            return ENOEXEC;
        }

    }

    /* Groovy */
    return 0;
}
