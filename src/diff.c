#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "diff.h"
#include "file.h"
#include "log.h"
#include "net.h"
#include "util.h"


struct timeval now;

int modified_filter(const struct dirent *dir) {
    struct stat dir_info;
    lstat(dir->d_name, &dir_info);
    /* check it out if it's modified in the last 10 seconds */
    if (dir_info.st_mtimespec.tv_sec > now.tv_sec - 10) {
        return 1;
    }
    return 0;
}


int send_diff_chunk(void *baton, dmp_operation_t op, const void *data, uint32_t len) {
    diff_info_t *di = (diff_info_t*)baton;
    ssize_t bytes_sent;
    off_t offset;
    char *msg;
    int msg_len;

    /* Just so you know, I know this is bad. */
    switch (op) {
        case DMP_DIFF_EQUAL:
            /* Don't care */
            log_debug("equal");
            return 0;
        break;

        case DMP_DIFF_DELETE:
            offset = data - di->mf1->buf;
            log_debug("delete. offset: %i bytes", offset);
            msg_len = asprintf(&msg, "path %s delete offset %lld data_len %u\n", di->path, offset, len);
        break;

        case DMP_DIFF_INSERT:
            offset = data - di->mf2->buf;
            log_debug("delete. offset: %i bytes", offset);
            msg_len = asprintf(&msg, "path %s insert offset %lld data_len %u\n", di->path, offset, len);
        break;

        default:
            die("WTF?!?!");
    }
    log_debug("msg: %s", msg);
    bytes_sent = send_bytes(msg, msg_len);
    fwrite(data, (size_t)len, 1, stdout);

    return 0;
}


void push_changes(const char *path) {
    struct dirent **dir_list = NULL;
    struct dirent *dir = NULL;
    int results;
    int i;
    int rv;
    diff_info_t di;
    ftc_diff_t ftc_diff;
    gettimeofday(&now, NULL);

    results = scandir(path, &dir_list, &modified_filter, &alphasort);
    if (results == 0) {
        log_debug("No results found in directory %s", path);
    }

    char *orig_path;
    char *file_path;
    struct stat dir_info;
    for (i = 0; i < results; i++) {
        dir = dir_list[i];
        asprintf(&file_path, "%s%s", path, dir->d_name);

        /* If a link points to a directory then we need to treat it as a directory. */
        if (dir->d_type == DT_LNK) {
            if (stat(file_path, &dir_info) == -1) {
                log_err("stat() failed on %s", file_path);
                /* If stat fails we may as well carry on and hope for the best. */
            } else if (S_ISDIR(dir_info.st_mode)) {
                dir->d_type = DT_DIR;
            }
        }
        if (dir->d_type == DT_DIR) {
            /* TODO: figure out if we need to recurse */
            continue;
        }

        asprintf(&orig_path, "%s%s", TMP_BASE, file_path);

        diff_files(&ftc_diff, orig_path, file_path);
        if (!ftc_diff.diff) {
            log_err("diff is null. I guess someone wrote the exact same bytes to this file?");
            goto cleanup;
        }

        mmapped_file_t *mf1 = ftc_diff.mf1;
        mmapped_file_t *mf2 = ftc_diff.mf2;
        di.path = file_path;
        di.mf1 = mf1;
        di.mf2 = mf2;
        dmp_diff_print_raw(stderr, ftc_diff.diff);
        dmp_diff_foreach(ftc_diff.diff, send_diff_chunk, &di);

        if (mf1->len != mf2->len) {
            if (ftruncate(mf1->fd, mf2->len) != 0) {
                die("resizing %s failed", ftc_diff.f1);
            }
            log_debug("resized %s to %u bytes", ftc_diff.f1, mf2->len);
        }

        munmap(mf1->buf, mf1->len);
        mf1->buf = mmap(0, mf2->len, PROT_WRITE | PROT_READ, MAP_SHARED, mf1->fd, 0);
        mf1->len = mf2->len;
        memcpy(mf1->buf, mf2->buf, mf1->len);
        rv = msync(mf1->buf, mf1->len, MS_SYNC);

        log_debug("rv %i wrote %i bytes to %s", rv, mf1->len, ftc_diff.f1);

        ftc_diff_cleanup(&ftc_diff);
        cleanup:;
        free(orig_path);
        free(file_path);
    }
}


void diff_files(ftc_diff_t *f, const char *f1, const char *f2) {
    struct stat file_stats;
    int rv;
    off_t f1_size;
    off_t f2_size;
    dmp_options opts;

    memset(&opts, 0, sizeof(opts));
    opts.timeout = 0.5; /* give up diffing after 0.5 seconds of processing */

    f->f1 = f1; /* not sure if this is a good idea*/
    f->f2 = f2;

    rv = lstat(f1, &file_stats);
    if (rv != 0) {
        die("Error lstat()ing file %s.", f1);
    }
    f1_size = file_stats.st_size;
    rv = lstat(f2, &file_stats);
    if (rv != 0) {
        die("Error lstat()ing file %s.", f2);
    }
    f2_size = file_stats.st_size;
    f1_size = f2_size > f1_size ? f2_size : f1_size;

    f->mf2 = mmap_file(f2, f2_size, 0, 0);
    f->mf1 = mmap_file(f1, f1_size, PROT_WRITE | PROT_READ, 0);

    dmp_diff_new(&(f->diff), &opts, f->mf1->buf, f->mf1->len, f->mf2->buf, f->mf2->len);
}


void ftc_diff_cleanup(ftc_diff_t *f) {
    dmp_diff_free(f->diff);
    munmap_file(f->mf1);
    munmap_file(f->mf2);
    free(f->mf1);
    free(f->mf2);
}


/*
int apply_diff_chunk(void *baton, dmp_operation_t op, const void *data, uint32_t len) {
    
}
*/
/*void apply_diff(dmp_diff *diff, void *buf, size_t len) {*/
void apply_diff(void *buf, size_t len) {
    char *path;
    /* parse path & diff from buf */
    ignore_path(path);
}
