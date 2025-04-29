/*
blk_io.c - Cross-platform Block & File IO library
Copyright (C) 2022  LekKit <github.com/LekKit>
                    0xCatPKG <0xCatPKG@rvvm.dev>
                    0xCatPKG <github.com/0xCatPKG>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Force 64-bit file offsets
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE

// Needed for pread()/pwrite(), syscall() when not passing -std=gnu..
#define _GNU_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "blk_io.h"

// Maximum buffer size processed per internal IO syscall: 256M
#define RVFILE_MAX_BUFF    0x10000000U

// Valid rvopen() flags
#define RVFILE_LEGAL_FLAGS 0x3F

#if !defined(USE_STDIO) && (defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__))
// Threaded POSIX file implementation using pread() / pwrite()
#include <errno.h>  // For errno
#include <fcntl.h>  // For struct flock, open(), fcntl(), posix_fallocate(), fallocate(), fspacectl(), fdiscard()
#include <unistd.h> // For close(), lseek(), pread(), pwrite(), fdatasync(), ftruncate()

#if defined(__NetBSD__)
#include <sys/param.h> // For __NetBSD_Version__
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

#ifndef O_CREAT
#define O_CREAT 0
#endif

#ifndef O_EXCL
#define O_EXCL 0
#endif

#ifndef O_TRUNC
#define O_TRUNC 0
#endif

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#ifndef O_SYNC
#define O_SYNC 0
#endif

#ifndef O_DSYNC
#define O_DSYNC O_SYNC
#endif

static bool try_lock_fd(int fd)
{
    struct flock flk = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
    };
    return !fcntl(fd, F_SETLK, &flk) || (errno != EACCES && errno != EAGAIN);
}

#define POSIX_FILE_IMPL 1

#elif !defined(USE_STDIO) && defined(_WIN32)
// Threaded Win32 file implementation using OVERLAPPED ReadFile() / WriteFile()
// Synchronizes rvtruncate()/rvfallocate() under a writer lock
#include <windows.h>

#include "spinlock.h"

// Prototypes for older winapi headers
#define DEVIOCTL_SET_SPARSE    0x000900c4
#define DEVIOCTL_SET_ZERO_DATA 0x000980c8

typedef struct {
    LARGE_INTEGER FileOffset;
    LARGE_INTEGER BeyondFinalZero;
} SET_ZERO_DATA_INFO;

#define WIN32_FILE_IMPL        1

#if defined(UNDER_CE) || !defined(HOST_64BIT)
// Windows 9x/CE/NT3.x do not support OVERLAPPED and need a seeking file IO fallback
#define WIN32_LEGACY_FILE_IMPL 1
#endif

#else
// Standard C stdio file implementation using a lock around fseek()+fread() / fseek()+fwrite()
#include <stdio.h> // For fopen(), setvbuf(), fseek(), ftell(), fread(), fwrite(), fflush(), fileno()

#if defined(_WIN32)
#include <io.h>      // For _get_osfhandle()
#include <windows.h> // For GetModuleFileNameW(), WideCharToMultiByte() on WinCE
#endif

#include "spinlock.h"

#define RVFILE_POS_INVALID 0x0
#define RVFILE_POS_READ    0x1
#define RVFILE_POS_WRITE   0x2

static inline bool rvfile_stdio_overflow(uint64_t offset)
{
    return ((uint64_t)(long)offset) != offset;
}

#endif

// RVVM internal headers come after system headers because of safe_free()
#include "utils.h"

struct blk_io_rvfile {
    uint64_t size;
    uint64_t pos;
#if defined(POSIX_FILE_IMPL)
    int fd;
#elif defined(WIN32_FILE_IMPL)
    HANDLE     handle;
    spinlock_t lock;
#else
    uint64_t   pos_real;
    FILE*      fp;
    spinlock_t lock;
    uint8_t    pos_state;
#endif
};

static inline void rvfile_grow_internal(rvfile_t* file, uint64_t length)
{
    uint64_t file_size = 0;
    do {
        file_size = atomic_load_uint64(&file->size);
        if (file_size >= length) {
            // File is already big enough
            break;
        }
    } while (!atomic_cas_uint64(&file->size, file_size, length));
}

#if defined(WIN32_LEGACY_FILE_IMPL)

static bool host_at_least_nt4 = false;

// NOTE: Must be called with file->lock exclusively held!
static uint32_t rvfile_win32_fallback(rvfile_t* file, void* dst, const void* src, size_t size, uint64_t offset)
{
    // Ancient Windows versions do not fill lpNumberOfBytes properly
    DWORD count = size;
    LONG  off_l = (uint32_t)offset;
    LONG  off_h = offset >> 32;
    DWORD ret_l = SetFilePointer(file->handle, off_l, &off_h, FILE_BEGIN);
    if ((((uint32_t)ret_l) != ((uint32_t)off_l)) || (((int32_t)ret_l) == -1 && GetLastError())) {
        return 0;
    }
    if (count && dst && ReadFile(file->handle, dst, count, &count, NULL)) {
        return count;
    } else if (count && src && WriteFile(file->handle, src, count, &count, NULL)) {
        return count;
    }
    return 0;
}

// Returns true on Windows CE, Windows 9x, or NT <4.0
static bool is_legacy_windows(void)
{
#if !defined(UNDER_CE)
    DO_ONCE_SCOPED {
        uint32_t ver      = GetVersion();
        host_at_least_nt4 = !(ver & 0x80000000U) && ((uint8_t)ver >= 0x04);
    }
#endif
    return !host_at_least_nt4;
}

#endif

rvfile_t* rvopen(const char* filepath, uint8_t filemode)
{
    if (filemode & ~RVFILE_LEGAL_FLAGS) {
        return NULL;
    }

#if defined(_WIN32) && defined(UNDER_CE)
    // Windows CE doesn't support relative file paths, nor current working directory
    // Workaround by appending executable directory before each relative path
    char wince_path[256] = {0};
    if (filepath[0] != '\\') {
        wchar_t wpath[256] = {0};
        GetModuleFileNameW(NULL, wpath, STATIC_ARRAY_SIZE(wpath));
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, wince_path, sizeof(wince_path), NULL, NULL);
        size_t len = rvvm_strlen(wince_path);
        while (len && wince_path[len - 1] != '\\') {
            len--;
        }
        rvvm_strlcpy(wince_path + len, filepath, sizeof(wince_path) - len);
        for (char* ptr = wince_path; ptr[0]; ptr++) {
            if (ptr[0] == '/') {
                ptr[0] = '\\';
            }
        }
        filepath = wince_path;
    }
#endif

#if defined(POSIX_FILE_IMPL)
    int open_flags = O_CLOEXEC | O_NOATIME;
    if (filemode & RVFILE_RW) {
        open_flags |= O_RDWR;
        if (filemode & RVFILE_CREAT) {
            open_flags |= O_CREAT;
            if (filemode & RVFILE_EXCL) {
                open_flags |= O_EXCL;
            }
        }
        if (filemode & RVFILE_TRUNC) {
            open_flags |= O_TRUNC;
        }
        if (filemode & RVFILE_SYNC) {
            open_flags |= O_DSYNC;
        }
    } else {
        open_flags |= O_RDONLY;
    }
    if (filemode & RVFILE_DIRECT) {
        open_flags |= O_DIRECT;
    }

    int fd = open(filepath, open_flags, 0600);
    if (fd < 0) {
        return NULL;
    }

    if ((filemode & RVFILE_EXCL) && !try_lock_fd(fd)) {
        rvvm_error("File %s is busy", filepath);
        close(fd);
        return NULL;
    }

    int64_t size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        // Failed to get file size
        close(fd);
        return NULL;
    }

    rvfile_t* file = safe_new_obj(rvfile_t);
    file->size     = (uint64_t)size;
    file->fd       = fd;

#elif defined(WIN32_FILE_IMPL)
    DWORD access = GENERIC_READ | ((filemode & RVFILE_RW) ? GENERIC_WRITE : 0);
    DWORD share  = (filemode & RVFILE_EXCL) ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    DWORD disp   = OPEN_EXISTING;
    DWORD attr   = FILE_ATTRIBUTE_NORMAL;
    if (filemode & RVFILE_RW) {
        if (filemode & (RVFILE_SYNC | RVFILE_DIRECT)) {
            attr = FILE_FLAG_WRITE_THROUGH;
        }
        if (filemode & RVFILE_CREAT) {
            disp = OPEN_ALWAYS;
            if (filemode & RVFILE_EXCL) {
                disp = CREATE_NEW;
            }
        } else {
            if (filemode & RVFILE_TRUNC) {
                disp = TRUNCATE_EXISTING;
            }
        }
    }

    HANDLE handle   = NULL;
    int    path_len = MultiByteToWideChar(CP_UTF8, 0, filepath, -1, NULL, 0);
    if (path_len > 0) {
        wchar_t* filepath_u16 = safe_new_arr(wchar_t, path_len);
        MultiByteToWideChar(CP_UTF8, 0, filepath, -1, filepath_u16, path_len);
        handle = CreateFileW(filepath_u16, access, share, NULL, disp, attr, NULL);
    }

#if defined(WIN32_LEGACY_FILE_IMPL) && !defined(UNDER_CE)
    if ((handle == NULL || handle == INVALID_HANDLE_VALUE) && is_legacy_windows()) {
        // Try to open file via ANSI syscall (Win9x & WinNT 3.51 compat)
        handle = CreateFileA(filepath, access, share, NULL, disp, attr, NULL);
    }
#endif

    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            rvvm_error("File %s is busy", filepath);
        }
        return NULL;
    }

    // Get file size
    LONG size_h = 0;
    LONG size_l = SetFilePointer(handle, 0, &size_h, FILE_END);
    if (((int32_t)size_l) == -1 && GetLastError()) {
        // Failed to get file size
        CloseHandle(handle);
        return NULL;
    }

    // Enable sparse file support whenever possible
    DWORD tmp = 0;
    DeviceIoControl(handle, DEVIOCTL_SET_SPARSE, NULL, 0, NULL, 0, &tmp, NULL);

    rvfile_t* file = safe_new_obj(rvfile_t);
    file->size     = ((uint32_t)size_l) | (((uint64_t)(uint32_t)size_h) << 32);
    file->handle   = handle;

#else
    const char* open_mode = "rb";
    FILE*       fp        = NULL;
    if (filemode & RVFILE_RW) {
        open_mode = "rb+";
        if ((filemode & RVFILE_CREAT) && (filemode & RVFILE_TRUNC) && !(filemode & RVFILE_EXCL)) {
            // Just force non-exclusive file creation + truncation
            open_mode = "wb+";
        } else if ((filemode & RVFILE_CREAT) || (filemode & RVFILE_TRUNC)) {
            fp = fopen(filepath, open_mode);
            if (fp && (filemode & RVFILE_CREAT) && (filemode & RVFILE_EXCL)) {
                // File already exists, but we requested exclusive creation
                fclose(fp);
                return NULL;
            } else if (fp && (filemode & RVFILE_TRUNC)) {
                // Truncate file if it already exists
                fclose(fp);
                fp        = NULL;
                open_mode = "wb+";
            } else if (!fp && (filemode & RVFILE_CREAT)) {
                // Create the file if missing
                open_mode = "wb+";
            }
        }
    }

    if (!fp) {
        fp = fopen(filepath, open_mode);
    }
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END)) {
        fclose(fp);
        return NULL;
    }

    int64_t size = ftell(fp);
    if (size == -1LL) {
        fclose(fp);
        return NULL;
    }

#ifdef _IONBF
    // Disable stdio buffering altogether for coherence sake with mmap() and other opened instances
    setvbuf(fp, NULL, _IONBF, 0);
#endif

    rvfile_t* file = safe_new_obj(rvfile_t);
    file->size     = size;
    file->fp       = fp;
#endif

    if ((filemode & RVFILE_RW) && (filemode & RVFILE_TRUNC) && rvfilesize(file)) {
        // Handle RVFILE_TRUNC if failed natively
        if (!rvtruncate(file, 0)) {
            rvclose(file);
            return NULL;
        }
    }

    return file;
}

void rvclose(rvfile_t* file)
{
    if (file) {
        rvfsync(file);
#if defined(POSIX_FILE_IMPL)
        close(file->fd);
#elif defined(WIN32_FILE_IMPL)
        CloseHandle(file->handle);
#else
        fclose(file->fp);
#endif
        free(file);
    }
}

uint64_t rvfilesize(rvfile_t* file)
{
    if (file) {
        return atomic_load_uint64(&file->size);
    }
    return 0;
}

// Return value of -1 means "Try again", 0 means "IO error / EOF"
static int32_t rvread_chunk(rvfile_t* file, void* dst, size_t size, uint64_t offset)
{
#if defined(POSIX_FILE_IMPL)
    int32_t ret = pread(file->fd, dst, size, offset);
    if (ret < 0 && errno != EINTR) {
        // IO error
        return 0;
    }
#elif defined(WIN32_FILE_IMPL)
#if defined(WIN32_LEGACY_FILE_IMPL)
    if (is_legacy_windows()) {
        return rvfile_win32_fallback(file, dst, NULL, size, offset);
    }
#endif
    // Ancient Windows NT (NT4, etc) do not fill lpNumberOfBytes properly
    DWORD      ret        = size;
    OVERLAPPED overlapped = {
        .Offset     = (uint32_t)offset,
        .OffsetHigh = offset >> 32,
    };
    if (!ReadFile(file->handle, dst, size, &ret, &overlapped)) {
        return 0;
    }
#else
    if (offset != file->pos_real || file->pos_state != RVFILE_POS_READ) {
        if (rvfile_stdio_overflow(offset) || fseek(file->fp, offset, SEEK_SET)) {
            // Seek error, or offset doesn't fit into a long
            return 0;
        }
        file->pos_state = RVFILE_POS_READ;
    }
    uint32_t ret   = fread(dst, 1, size, file->fp);
    file->pos_real = offset + ret;
#endif
    return ret;
}

// Unlocked version of rvread(), requires a valid file offset
static int32_t rvread_unlocked(rvfile_t* file, void* dst, size_t size, uint64_t offset)
{
    uint8_t* buffer = dst;
    size_t   ret    = 0;

    while (file && ret < size) {
        size_t  chunk_size = EVAL_MIN(size - ret, RVFILE_MAX_BUFF);
        int32_t tmp        = rvread_chunk(file, buffer + ret, chunk_size, offset + ret);
        if (likely(tmp > 0)) {
            ret += tmp;
            if (unlikely(ret < size) && offset + ret >= rvfilesize(file)) {
                // End of file reached
                break;
            }
        } else if (unlikely(!tmp)) {
            // IO error
            break;
        }
    }

    return ret;
}

size_t rvread(rvfile_t* file, void* dst, size_t size, uint64_t offset)
{
    uint64_t pos = (offset == RVFILE_POSITION) ? rvtell(file) : offset;
    size_t   ret = 0;

#if defined(POSIX_FILE_IMPL)
    ret = rvread_unlocked(file, dst, size, pos);
#elif defined(WIN32_LEGACY_FILE_IMPL)
    if (is_legacy_windows()) {
        scoped_spin_lock_slow (&file->lock) {
            ret = rvread_unlocked(file, dst, size, pos);
        }
    } else {
        scoped_spin_read_lock_slow (&file->lock) {
            ret = rvread_unlocked(file, dst, size, pos);
        }
    }
#elif defined(WIN32_FILE_IMPL)
    scoped_spin_read_lock_slow (&file->lock) {
        ret = rvread_unlocked(file, dst, size, pos);
    }
#else
    scoped_spin_lock_slow (&file->lock) {
        ret = rvread_unlocked(file, dst, size, pos);
    }
#endif

    if (offset == RVFILE_POSITION && ret) {
        rvseek(file, pos + ret, RVFILE_SEEK_SET);
    }

    return ret;
}

// Return value of -1 means "Try again", 0 means "IO error"
static int32_t rvwrite_chunk(rvfile_t* file, const void* src, size_t size, uint64_t offset)
{
#if defined(POSIX_FILE_IMPL)
    int32_t ret = pwrite(file->fd, src, size, offset);
    if (ret < 0 && errno != EINTR) {
        // IO error
        return 0;
    }
#elif defined(WIN32_FILE_IMPL)
#if defined(WIN32_LEGACY_FILE_IMPL)
    if (is_legacy_windows()) {
        return rvfile_win32_fallback(file, NULL, src, size, offset);
    }
#endif
    // Ancient Windows NT (NT4, etc) do not fill lpNumberOfBytes properly
    DWORD      ret        = size;
    OVERLAPPED overlapped = {
        .Offset     = (uint32_t)offset,
        .OffsetHigh = offset >> 32,
    };
    if (!WriteFile(file->handle, src, size, &ret, &overlapped)) {
        return 0;
    }
#else
    if (offset != file->pos_real || file->pos_state != RVFILE_POS_WRITE) {
        if (rvfile_stdio_overflow(offset) || fseek(file->fp, offset, SEEK_SET)) {
            // Seek error, or offset doesn't fit into a long
            return 0;
        }
        file->pos_state = RVFILE_POS_WRITE;
    }
    uint32_t ret   = fwrite(src, 1, size, file->fp);
    file->pos_real = offset + ret;
#endif
    return ret;
}

// Unlocked version of rvwrite(), requires a valid file offset
static int32_t rvwrite_unlocked(rvfile_t* file, const void* src, size_t size, uint64_t offset)
{
    const uint8_t* buffer = src;
    size_t         ret    = 0;

    while (file && ret < size) {
        size_t  chunk_size = EVAL_MIN(size - ret, RVFILE_MAX_BUFF);
        int32_t tmp        = rvwrite_chunk(file, buffer + ret, chunk_size, offset + ret);
        if (likely(tmp > 0)) {
            ret += tmp;
        } else if (unlikely(!tmp)) {
            // IO error
            break;
        }
    }

    rvfile_grow_internal(file, offset + ret);

    return ret;
}

size_t rvwrite(rvfile_t* file, const void* src, size_t size, uint64_t offset)
{
    uint64_t pos = (offset == RVFILE_POSITION) ? rvtell(file) : offset;
    size_t   ret = 0;

#if defined(POSIX_FILE_IMPL)
    ret = rvwrite_unlocked(file, src, size, offset);
#elif defined(WIN32_LEGACY_FILE_IMPL)
    if (is_legacy_windows()) {
        scoped_spin_lock_slow (&file->lock) {
            ret = rvwrite_unlocked(file, src, size, offset);
        }
    } else {
        scoped_spin_read_lock_slow (&file->lock) {
            ret = rvwrite_unlocked(file, src, size, offset);
        }
    }
#elif defined(WIN32_FILE_IMPL)
    scoped_spin_read_lock_slow (&file->lock) {
        ret = rvwrite_unlocked(file, src, size, offset);
    }
#else
    scoped_spin_lock_slow (&file->lock) {
        ret = rvwrite_unlocked(file, src, size, offset);
    }
#endif

    if (offset == RVFILE_POSITION && ret) {
        rvseek(file, pos + ret, RVFILE_SEEK_SET);
    }

    return ret;
}

bool rvtrim(rvfile_t* file, uint64_t offset, uint64_t size)
{
    if (file) {
#if defined(POSIX_FILE_IMPL) && defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
        // Use fallocate(FALLOC_FL_PUNCH_HOLE) on Linux to punch holes
        return !fallocate(file->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, size);
#elif defined(POSIX_FILE_IMPL) && defined(__FreeBSD__) && __FreeBSD__ >= 14 && defined(SPACECTL_DEALLOC)
        // Use fspacectl(SPACECTL_DEALLOC) added in FreeBSD 14 to punch holes
        struct spacectl_range rqsr = {
            .r_offset = offset,
            .r_len    = size,
        };
        return !fspacectl(file->fd, SPACECTL_DEALLOC, &rqsr, 0, NULL);
#elif defined(POSIX_FILE_IMPL) && defined(__NetBSD_Version__) && __NetBSD_Version__ >= 700000000
        // Use fdiscard() added in NetBSD 7 to punch holes
        return !fdiscard(file->fd, offset, size);
#elif defined(WIN32_FILE_IMPL)
        // Use DeviceIoControl(FSCTL_SET_ZERO_DATA) on Windows to punch holes
        SET_ZERO_DATA_INFO fz       = {0};
        DWORD              tmp      = 0;
        fz.FileOffset.QuadPart      = offset;
        fz.BeyondFinalZero.QuadPart = offset + size;
        return !!DeviceIoControl(file->handle, DEVIOCTL_SET_ZERO_DATA, &fz, sizeof(fz), NULL, 0, &tmp, NULL);
#else
        UNUSED(offset);
        UNUSED(size);
#endif
    }
    return false;
}

bool rvseek(rvfile_t* file, int64_t offset, uint8_t startpos)
{
    if (file) {
        if (startpos == RVFILE_SEEK_CUR) {
            offset = rvtell(file) + offset;
        } else if (startpos == RVFILE_SEEK_END) {
            offset = rvfilesize(file) - offset;
        } else if (startpos != RVFILE_SEEK_SET) {
            // Invalid seek operation
            offset = -1;
        }
        if (offset >= 0) {
            atomic_store_uint64_relax(&file->pos, offset);
            return true;
        }
    }
    return false;
}

uint64_t rvtell(rvfile_t* file)
{
    if (file) {
        return atomic_load_uint64_relax(&file->pos);
    }
    return -1;
}

bool rvfsync(rvfile_t* file)
{
    bool ret = true;
    if (file) {
#if defined(POSIX_FILE_IMPL) && defined(__linux__)
        while (fdatasync(file->fd)) {
            if (errno != EINTR) {
                ret = false;
            }
        }
#elif defined(POSIX_FILE_IMPL) && defined(__APPLE__) && defined(F_BARRIERFSYNC)
        while (fcntl(file->fd, F_BARRIERFSYNC)) {
            if (errno != EINTR) {
                ret = false;
            }
        }
#elif defined(POSIX_FILE_IMPL)
        while (fsync(file->fd)) {
            if (errno != EINTR) {
                ret = false;
            }
        }
#elif defined(WIN32_FILE_IMPL)
        scoped_spin_lock_slow (&file->lock) {
            // Synchronize any previously issued IO
        }
        // Trying to flush a read-only file results in ERROR_ACCESS_DENIED, ignore
        ret = FlushFileBuffers(file->handle) || (GetLastError() == ERROR_ACCESS_DENIED);
#else
        scoped_spin_lock_slow (&file->lock) {
            ret = !fflush(file->fp);
        }
#endif
    }
    return ret;
}

bool rvtruncate(rvfile_t* file, uint64_t length)
{
    bool resized = false;
    if (!file) {
        return false;
    }
    if (length == rvfilesize(file)) {
        return true;
    }
#if defined(POSIX_FILE_IMPL)
    resized = !ftruncate(file->fd, length);
#elif defined(WIN32_FILE_IMPL)
    scoped_spin_lock_slow (&file->lock) {
        // Perform SetFilePointer() + SetEndOfFile() under writer lock
        // to prevent ReadFile()/WriteFile() from moving file pointer
        LONG  len_l = (LONG)(uint32_t)length;
        LONG  len_h = (LONG)(uint32_t)(length >> 32);
        DWORD ret_l = SetFilePointer(file->handle, len_l, &len_h, FILE_BEGIN);
        if ((((uint32_t)ret_l) == ((uint32_t)len_l)) && (((int32_t)ret_l) != -1 || !GetLastError())) {
            // Successfully set file pointer, set end of file here
            resized = !!SetEndOfFile(file->handle);
        }
    }
#endif
    if (resized) {
        // Successfully resized the file natively
        atomic_store_uint64(&file->size, length);
        return true;
    }

    if (length >= rvfilesize(file)) {
        // Try to grow the file via rvfallocate()
        return rvfallocate(file, length);
    }

    // Failed to resize the file
    return false;
}

static bool rvfile_grow_unlocked(rvfile_t* file, uint64_t length)
{
    char tmp = 0;
    if (rvread_unlocked(file, &tmp, 1, length - 1)) {
        // File already big enough
        return true;
    }
    return !!rvwrite_unlocked(file, &tmp, 1, length - 1);
}

static bool rvfile_grow_generic(rvfile_t* file, uint64_t length)
{
    bool ret = true;
    if (length > rvfilesize(file)) {
        // Grow the file by writing one byte at the new end
#if defined(POSIX_FILE_IMPL)
        // NOTE: This is not thread safe whenever there are writers beyond the end of file
        // This only applies to POSIX implementations, where posix_fallocate() is preferable
        ret = rvfile_grow_unlocked(file, length);
#else
        scoped_spin_lock_slow (&file->lock) {
            ret = rvfile_grow_unlocked(file, length);
        }
#endif
    }
    return ret;
}

bool rvfallocate(rvfile_t* file, uint64_t length)
{
    if (!file) {
        return false;
    }
    if (length <= rvfilesize(file)) {
        return true;
    }
#if defined(POSIX_FILE_IMPL)                                                                                           \
    && (defined(__linux__) || (defined(__FreeBSD__) && __FreeBSD__ >= 9)                                               \
        || (defined(__NetBSD_Version__) && __NetBSD_Version__ >= 700000000))
    // Try to grow file via posix_fallocate() (Available on Linux 2.6.23+, FreeBSD 9+, NetBSD 7+)
    if (!posix_fallocate(file->fd, length - 1, 1)) {
        rvfile_grow_internal(file, length);
        return true;
    }
#endif

    // Generic grow file implementation
    return rvfile_grow_generic(file, length);
}

int rvfile_get_posix_fd(rvfile_t* file)
{
#if defined(POSIX_FILE_IMPL)
    return file->fd;
#elif !defined(WIN32_FILE_IMPL) && (defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__))
    return fileno(file->fp);
#else
    UNUSED(file);
    return -1;
#endif
}

void* rvfile_get_win32_handle(rvfile_t* file)
{
#if defined(WIN32_FILE_IMPL)
    return (void*)file->handle;
#elif !defined(POSIX_FILE_IMPL) && defined(_WIN32) && defined(UNDER_CE)
    return (void*)_fileno(file->fp);
#elif !defined(POSIX_FILE_IMPL) && defined(_WIN32) && !defined(UNDER_CE)
    return (void*)_get_osfhandle(_fileno(file->fp));
#else
    UNUSED(file);
    return NULL;
#endif
}

/*
 * Block device layer
 */

static void blk_raw_close(void* dev)
{
    rvfile_t* file = dev;
    rvclose(file);
}

static size_t blk_raw_read(void* dev, void* dst, size_t size, uint64_t offset)
{
    rvfile_t* file = dev;
    return rvread(file, dst, size, offset);
}

static size_t blk_raw_write(void* dev, const void* src, size_t size, uint64_t offset)
{
    rvfile_t* file = dev;
    return rvwrite(file, src, size, offset);
}

static bool blk_raw_trim(void* dev, uint64_t offset, uint64_t size)
{
    rvfile_t* file = dev;
    return rvtrim(file, offset, size);
}

// Raw block device implementation
// Be careful with function prototypes
static const blkdev_type_t blkdev_type_raw = {
    .name  = "blk-raw",
    .close = blk_raw_close,
    .read  = blk_raw_read,
    .write = blk_raw_write,
    .trim  = blk_raw_trim,
};

static blkdev_t* blk_raw_open(const char* filename, uint8_t filemode)
{
    rvfile_t* file = rvopen(filename, filemode);
    if (!file) {
        return NULL;
    }
    blkdev_t* dev = safe_new_obj(blkdev_t);
    dev->type     = &blkdev_type_raw;
    dev->size     = rvfilesize(file);
    dev->data     = file;
    return dev;
}

blkdev_t* blk_dedup_open(const char* filename, uint8_t filemode);

static bool check_file_ext(const char* filename, const char* ext)
{
    const char* occurence = NULL;
    while (true) {
        const char* tmp = rvvm_strfind(filename, ext);
        if (tmp == NULL) {
            break;
        }
        occurence = tmp;
        filename  = occurence + 1;
    }
    return occurence && rvvm_strcmp(occurence, ext);
}

blkdev_t* blk_open(const char* filename, uint8_t opts)
{
    uint8_t filemode = (opts & BLKDEV_RW) ? (RVFILE_RW | RVFILE_EXCL) : 0;
    if (check_file_ext(filename, ".bdv")) {
        return NULL;
    }
    if (check_file_ext(filename, ".qcow2")) {
        rvvm_error("QCOW2 images aren't supported yet");
        return NULL;
    }
    return blk_raw_open(filename, filemode);
}

void blk_close(blkdev_t* dev)
{
    if (dev) {
        dev->type->close(dev->data);
        free(dev);
    }
}
