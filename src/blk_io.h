/*
blk_io.h - Cross-platform Block & File IO library
Copyright (C) 2022  0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_BLK_IO_H
#define RVVM_BLK_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * File API
 */

// Flags for rvopen()
#define RVFILE_READ     0x01 // Permit reading the file (Implicit without RVFILE_WRITE)
#define RVFILE_WRITE    0x02 // Permit writing the file
#define RVFILE_RW       0x03 // Open file in read/write mode
#define RVFILE_CREAT    0x04 // Create file if it doesn't exist (for RW only)
#define RVFILE_EXCL     0x08 // Prevent other processes from opening this file
#define RVFILE_TRUNC    0x10 // Truncate file conents upon opening (for RW only)
#define RVFILE_DIRECT   0x20 // Direct read/write DMA with the underlying storage
#define RVFILE_SYNC     0x40 // Disable writeback buffering

// Modes for rvseek()
#define RVFILE_SEEK_SET 0x00 // Set file cursor position relative to file beginning
#define RVFILE_SEEK_CUR 0x01 // Set file cursor position relative to current position
#define RVFILE_SEEK_END 0x02 // Set file cursor position relative to file end

// Pass as offset to rvread()/rvwrite() to use file position, seek via rvseek()/rvtell()
// NOTE: Not thread safe, use with care!
#define RVFILE_POSITION ((uint64_t)-1)

typedef struct blk_io_rvfile rvfile_t;

// Open a binary file, returns NULL on failure
rvfile_t* rvopen(const char* filepath, uint32_t filemode);

// Close a file handle
void rvclose(rvfile_t* file);

// Get file size (Not synced across processes)
uint64_t rvfilesize(rvfile_t* file);

// If offset == RVFILE_POSITION, uses current file position as offset (Not thread safe)
// Otherwise, raw file offset is given, which allows multiple threads to issue IO
size_t rvread(rvfile_t* file, void* dst, size_t size, uint64_t offset);
size_t rvwrite(rvfile_t* file, const void* src, size_t size, uint64_t offset);

// Seek/tell for positioned IO (Not thread safe)
bool     rvseek(rvfile_t* file, int64_t offset, uint32_t startpos);
uint64_t rvtell(rvfile_t* file);

// Trim (punch a hole) in file, leaving zeroes and releasing space on the host
bool rvtrim(rvfile_t* file, uint64_t offset, uint64_t size);

// Set/grow file size
bool rvtruncate(rvfile_t* file, uint64_t length);
bool rvfallocate(rvfile_t* file, uint64_t length);

// Sync buffers to disk, or issue a write barrier (Depending on platform)
// NOTE: If this fails, do NOT perform further actions and GTFO!
bool rvfsync(rvfile_t* file);

// Get native POSIX file descriptor, returns -1 on failure
int rvfile_get_posix_fd(rvfile_t* file);

// Get native Win32 file handle, returns NULL on failure
void* rvfile_get_win32_handle(rvfile_t* file);

#endif
