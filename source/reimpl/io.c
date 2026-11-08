/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <fios/fios.h>

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#include "_existing_files.c"

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}


/*
 * Buffered reader for files inside data.psarc.
 *
 * The game issues lots of tiny fread() calls (libpng, CSV parsers, ...).
 * Previously every one of them was a synchronous FIOS request. Now each open
 * psarc file gets a read-ahead buffer: small files are fully read with a single
 * request, bigger ones are read in PSARC_BUF_SIZE windows, and large reads go
 * straight to FIOS without an extra copy.
 *
 * Handles are small encoded integers (PSARC_HANDLE_BASE + slot), so they can
 * never collide with real libc FILE pointers.
 */
#include <pthread.h>

#define PSARC_MAX_FILES    1024
#define PSARC_BUF_SIZE     (256 * 1024)
#define PSARC_HANDLE_BASE  0x10000

typedef struct {
    int      used;
    int32_t  fh;
    int64_t  size;     // file size
    int64_t  pos;      // logical position
    int64_t  phys;     // FIOS handle position (avoids redundant seeks)
    uint8_t *buf;
    int64_t  buf_off;  // file offset of buf[0]
    int32_t  buf_len;  // valid bytes in buf
    int      eof;
    int      unget;    // pushed back char (ungetc), -1 if none
} PsarcFile;

static PsarcFile psarc_files[PSARC_MAX_FILES];
static pthread_mutex_t psarc_lock = PTHREAD_MUTEX_INITIALIZER;

static inline PsarcFile * psarc_get(FILE * f) {
    uintptr_t h = (uintptr_t)f;
    if (h < PSARC_HANDLE_BASE || h >= PSARC_HANDLE_BASE + PSARC_MAX_FILES)
        return NULL;
    PsarcFile * pf = &psarc_files[h - PSARC_HANDLE_BASE];
    return pf->used ? pf : NULL;
}

/*
 * Diagnostics (only when the file DATA_PATH "debug_log" exists): anomalies
 * and I/O statistics are appended to DATA_PATH "psarc_io.log".
 * Creating an (empty) file DATA_PATH "psarc_legacy" switches back to the
 * original unbuffered behaviour at runtime, for A/B testing.
 */
#include <psp2/io/fcntl.h>
#include <stdarg.h>

static int psarc_legacy = -1;

static int psarc_log_enabled = -1;

static void psarc_log(const char * fmt, ...) {
    if (psarc_log_enabled < 0) {
        SceUID f = sceIoOpen(DATA_PATH "debug_log", SCE_O_RDONLY, 0);
        psarc_log_enabled = f >= 0;
        if (f >= 0) sceIoClose(f);
    }
    if (!psarc_log_enabled) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    SceUID fd = sceIoOpen(DATA_PATH "psarc_io.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static int psarc_is_legacy(void) {
    if (psarc_legacy < 0) {
        SceUID fd = sceIoOpen(DATA_PATH "psarc_legacy", SCE_O_RDONLY, 0);
        psarc_legacy = fd >= 0;
        if (fd >= 0) sceIoClose(fd);
        psarc_log("--- session start, mode: %s", psarc_legacy ? "legacy" : "buffered");
    }
    return psarc_legacy;
}

/* I/O statistics, dumped to psarc_io.log by psarc_stats_tick() */
#include <psp2/kernel/processmgr.h>
static volatile uint32_t st_opens, st_freads, st_fios_reads;
static volatile uint64_t st_bytes_req, st_bytes_fios, st_us_fios, st_us_open;

/* Opens per top-level folder (1x.common, 4x.smartphone, audio, game, ...) */
#define ST_DIRS 24
static char st_dir_name[ST_DIRS][24];
static uint32_t st_dir_count[ST_DIRS];
static uint64_t st_dir_bytes[ST_DIRS];

static void st_count_dir(const char * filename, int64_t size) {
    const char * p = filename[0] == '/' ? filename + 1 : filename;
    const char * slash = strchr(p, '/');
    char name[24];
    size_t n = slash ? (size_t)(slash - p) : strlen("(root)");
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, slash ? p : "(root)", n);
    name[n] = 0;
    pthread_mutex_lock(&psarc_lock);
    for (int i = 0; i < ST_DIRS; i++) {
        if (st_dir_name[i][0] == 0) strcpy(st_dir_name[i], name);
        if (strcmp(st_dir_name[i], name) == 0) {
            st_dir_count[i]++;
            st_dir_bytes[i] += size > 0 ? size : 0;
            break;
        }
    }
    pthread_mutex_unlock(&psarc_lock);
}

static int64_t psarc_pread(PsarcFile * pf, void * dst, int64_t len, int64_t off) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int64_t r = sceFiosFHPreadSync(NULL, pf->fh, dst, len, off);
    st_us_fios += sceKernelGetProcessTimeWide() - t0;
    st_fios_reads++;
    if (r > 0) st_bytes_fios += r;
    if (r != len)
        psarc_log("pread short: fh=%d off=%lld len=%lld got=%lld size=%lld",
                  (int)pf->fh, off, len, r, pf->size);
    return r;
}

static FILE * psarc_open(const char * filename) {
    int32_t fh = 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int open_res = sceFiosFHOpenSync(NULL, &fh, filename, NULL);
    st_us_open += sceKernelGetProcessTimeWide() - t0;
    st_opens++;
    if (open_res < 0)
        return NULL;

    int64_t size = sceFiosFHGetSize(fh);
    if (size < 0) {
        psarc_log("open %s: GetSize failed 0x%llx", filename, size);
        sceFiosFHCloseSync(NULL, fh);
        return NULL;
    }

    st_count_dir(filename, size);
    int32_t cap = psarc_is_legacy() ? 1 : (size < PSARC_BUF_SIZE ? (int32_t)size : PSARC_BUF_SIZE);
    uint8_t * buf = malloc(cap > 0 ? cap : 1);
    if (!buf) {
        sceFiosFHCloseSync(NULL, fh);
        return NULL;
    }

    pthread_mutex_lock(&psarc_lock);
    int slot = -1;
    for (int i = 0; i < PSARC_MAX_FILES; i++) {
        if (!psarc_files[i].used) { slot = i; break; }
    }
    if (slot >= 0) {
        PsarcFile * pf = &psarc_files[slot];
        memset(pf, 0, sizeof(*pf));
        pf->used = 1;
        pf->fh = fh;
        pf->size = size;
        pf->buf = buf;
        pf->unget = -1;
    }
    pthread_mutex_unlock(&psarc_lock);

    if (slot < 0) {
        l_error("psarc_open(%s): too many open files", filename);
        free(buf);
        sceFiosFHCloseSync(NULL, fh);
        return NULL;
    }
    return (FILE *)(uintptr_t)(PSARC_HANDLE_BASE + slot);
}

static void psarc_close(PsarcFile * pf) {
    sceFiosFHCloseSync(NULL, pf->fh);
    free(pf->buf);
    pthread_mutex_lock(&psarc_lock);
    pf->buf = NULL;
    pf->used = 0;
    pthread_mutex_unlock(&psarc_lock);
}

static size_t psarc_read(PsarcFile * pf, void * ptr, size_t total) {
    st_freads++;
    st_bytes_req += total;
    uint8_t * dst = ptr;
    size_t done = 0;

    if (total && pf->unget >= 0) {
        dst[0] = (uint8_t)pf->unget;
        pf->unget = -1;
        pf->pos++;
        done++;
    }

    while (done < total) {
        if (pf->pos >= pf->size) { pf->eof = 1; break; }

        // Serve from the buffer if the position is inside it
        if (pf->pos >= pf->buf_off && pf->pos < pf->buf_off + pf->buf_len) {
            size_t avail = (size_t)(pf->buf_off + pf->buf_len - pf->pos);
            size_t n = total - done < avail ? total - done : avail;
            memcpy(dst + done, pf->buf + (pf->pos - pf->buf_off), n);
            pf->pos += n;
            done += n;
            continue;
        }

        size_t left = total - done;
        int64_t remain_in_file = pf->size - pf->pos;
        if (left >= PSARC_BUF_SIZE) {
            // Big read: go directly to the destination
            int64_t want = left < remain_in_file ? (int64_t)left : remain_in_file;
            int64_t r = psarc_pread(pf, dst + done, want, pf->pos);
            if (r <= 0) { pf->eof = 1; break; }
            pf->pos += r;
            done += r;
        } else {
            // Refill the read-ahead buffer
            int64_t want = remain_in_file < PSARC_BUF_SIZE ? remain_in_file : PSARC_BUF_SIZE;
            int64_t r = psarc_pread(pf, pf->buf, want, pf->pos);
            if (r <= 0) { pf->buf_len = 0; pf->eof = 1; break; }
            pf->buf_off = pf->pos;
            pf->buf_len = (int32_t)r;
        }
    }
    return done;
}

/*
 * Called once per frame from the game thread. Every 5 seconds with I/O
 * activity, logs one line: how much the game asked for, how much was read
 * from FIOS and how long FIOS took. Comparing "fios ms" with the wall time
 * of a loading screen tells whether loading is I/O bound or CPU bound.
 */
void psarc_stats_tick(void) {
    if (psarc_log_enabled == 0)
        return;
    static uint64_t last_t = 0;
    static uint32_t last_opens = 0, last_freads = 0, last_fr = 0;
    static uint64_t last_req = 0, last_fios = 0, last_us = 0, last_uso = 0;
    static uint64_t last_frame = 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_frame && now - last_frame > 1000000)
        psarc_log("[t=%llus] game blocked for %llu ms in one frame", now / 1000000, (now - last_frame) / 1000);
    last_frame = now;
    if (last_t == 0) { last_t = now; return; }
    if (now - last_t < 5000000) return;
    if (st_freads != last_freads || st_opens != last_opens) {
        psarc_log("[t=%llus] last %llus: opens=%u freads=%u fios_reads=%u req=%lluKB fios=%lluKB fios_ms=%llu open_ms=%llu",
                  now / 1000000, (now - last_t) / 1000000,
                  st_opens - last_opens, st_freads - last_freads, st_fios_reads - last_fr,
                  (st_bytes_req - last_req) / 1024, (st_bytes_fios - last_fios) / 1024,
                  (st_us_fios - last_us) / 1000, (st_us_open - last_uso) / 1000);
    }
    if (st_opens != last_opens) {
        char line[400]; int off = 0;
        pthread_mutex_lock(&psarc_lock);
        for (int i = 0; i < ST_DIRS && st_dir_name[i][0]; i++) {
            if (!st_dir_count[i]) continue;
            off += snprintf(line + off, sizeof(line) - off, " %s=%u/%lluKB", st_dir_name[i],
                            st_dir_count[i], st_dir_bytes[i] / 1024);
            st_dir_count[i] = 0; st_dir_bytes[i] = 0;
            if (off >= (int)sizeof(line) - 1) break;
        }
        pthread_mutex_unlock(&psarc_lock);
        psarc_log("        dirs:%s", line);
    }
    last_t = now; last_opens = st_opens; last_freads = st_freads; last_fr = st_fios_reads;
    last_req = st_bytes_req; last_fios = st_bytes_fios; last_us = st_us_fios; last_uso = st_us_open;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strchr(filename, '/') == filename) {
        char **existing_file = bsearch(&filename, existing_files, existing_files_len, sizeof(existing_files[0]), compare_strings);
        if (existing_file) {
            FILE * f = psarc_open(filename);
            if (!f)
                l_warn("fopen<psarc>(%s, %s): not found", filename, mode);
            return f;
        }
        return NULL;
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    //l_debug("fclose(%p)", f);

    PsarcFile * pf = psarc_get(f);
    if (!pf) {
#ifdef USE_SCELIBC_IO
        int ret = sceLibcBridge_fclose(f);
#else
        int ret = fclose(f);
#endif

        return ret;
    }

    psarc_close(pf);
    return 0;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

int statfs_soloader(const char *path, struct statfs *buf) {
    l_debug("statfs(\"%s\", %p)");

    if (!buf)
        return -1;

    buf->f_type = EXT4_SUPER_MAGIC;
    //TODO: can do actual values here at some point
    buf->f_blocks = 10000000;
    buf->f_bfree = 10000000;
    buf->f_bavail = 10000000;
    buf->f_files = 10000000;
    buf->f_ffree = 10000000;
    buf->f_bsize = 1024;
    buf->f_frsize = 1024;

    return 0;
}

#ifdef USE_SCELIBC_IO
#define LIBC(fn) sceLibcBridge_##fn
#else
#define LIBC(fn) fn
#endif

int fseek_soloader(FILE *stream, long int offset, int origin) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(fseek)(stream, offset, origin);

    int64_t base;
    switch (origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = pf->pos; break;
        case SEEK_END: base = pf->size; break;
        default: return -1;
    }
    int64_t np = base + offset;
    if (np < 0)
        return -1;
    pf->pos = np;
    pf->eof = 0;
    pf->unget = -1;
    return 0;
}

size_t fread_soloader(void *ptr, size_t size, size_t count, FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(fread)(ptr, size, count, stream);

    if (psarc_legacy) {
        // Original behaviour: direct FIOS read, always report full count
        int64_t r = sceFiosFHPreadSync(NULL, pf->fh, ptr, (int64_t)size * count, pf->pos);
        if (r > 0) pf->pos += r;
        return count;
    }
    if (size == 0 || count == 0)
        return 0;
    return psarc_read(pf, ptr, size * count) / size;
}

long int ftell_soloader(FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(ftell)(stream);
    return (long int)pf->pos;
}

int feof_soloader(FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(feof)(stream);
    return psarc_legacy ? 0 : pf->eof;
}

int ferror_soloader(FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(ferror)(stream);
    return 0;
}

int fflush_soloader(FILE *stream) {
    if (!psarc_get(stream))
        return LIBC(fflush)(stream);
    return 0;
}

int fgetc_soloader(FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(fgetc)(stream);
    if (psarc_legacy)
        return 0;

    uint8_t c;
    return psarc_read(pf, &c, 1) == 1 ? c : EOF;
}

int ungetc_soloader(int c, FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(ungetc)(c, stream);

    if (c == EOF || pf->unget >= 0 || pf->pos == 0)
        return EOF;
    pf->unget = (uint8_t)c;
    pf->pos--;
    pf->eof = 0;
    return (uint8_t)c;
}

char * fgets_soloader(char *str, int num, FILE *stream) {
    PsarcFile * pf = psarc_get(stream);
    if (!pf)
        return LIBC(fgets)(str, num, stream);

    if (num <= 0)
        return NULL;
    int i = 0;
    while (i < num - 1) {
        int c = fgetc_soloader(stream);
        if (c == EOF)
            break;
        str[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    str[i] = '\0';
    return str;
}
