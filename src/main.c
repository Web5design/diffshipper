#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#ifdef INOTIFY
#include <inotifytools/inotifytools.h>
#include <inotifytools/inotify.h>
#endif

#ifdef FSEVENTS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#endif

#include "api.h"
#include "buf.h"
#include "conf.h"
#include "dmp_lua.h"
#include "fs_event_handlers.h"
#include "ignore.h"
#include "log.h"
#include "net.h"
#include "options.h"
#include "proto_handlers.h"
#include "util.h"

lua_State *l_mp;


#ifdef FSEVENTS
void event_cb(ConstFSEventStreamRef streamRef, void *cb_data, size_t count, void *paths,
              const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    size_t i;
    const char *path;
    char *base_path = cb_data;

    for (i = 0; i < count; i++) {
        path = ((char**)paths)[i];
        /* flags are unsigned long, IDs are uint64_t */
        log_debug("Change %llu in %s, flags %lu", ids[i], path, (long)flags[i]);
        push_changes(l_mp, base_path, path);
    }
}
#endif


void init() {
    pthread_t remote_changes;
    ignored_changes = NULL;
    ignored_changes_len = 0;

    set_log_level(LOG_LEVEL_DEBUG);

    if (pthread_cond_init(&server_conn_ready, NULL))
        die("pthread_cond_init failed!");
    if (pthread_cond_init(&server_conn_done, NULL))
        die("pthread_cond_init failed!");
    if (pthread_mutex_init(&server_conn_mtx, NULL))
        die("pthread_mutex_init failed!");
    if (pthread_mutex_init(&ignore_changes_mtx, NULL))
        die("pthread_mutex_init failed!");

    init_bufs();
    root_ignores = init_ignore(NULL);
    pthread_create(&remote_changes, NULL, &remote_change_worker, NULL);
    l_mp = init_lua_state();
}


void cleanup() {
    /* TODO: free() ignored_changes */
    cleanup_bufs();
    cleanup_ignore(root_ignores); /* TODO: free the children! */
    free(opts.path);
    pthread_cond_destroy(&server_conn_ready);
    pthread_cond_destroy(&server_conn_done);
    pthread_mutex_destroy(&server_conn_mtx);
    pthread_mutex_destroy(&ignore_changes_mtx);
    net_cleanup();
    lua_close(l_mp);
}


int main(int argc, char **argv) {
    int rv;
    char *path;

    init();
    init_opts();
    rv = parse_conf();
    if (rv) {
        die("Couldn't parse config file.");
    }
    parse_opts(argc, argv);
    path = opts.path;

    if (opts.delete_room) {
        rv = api_delete_room();
        if (rv && !opts.create_room)
            die("Couldn't delete room");
        if (!opts.create_room) {
            log_msg("Deleted room %s owned by %s", opts.room, opts.owner);
            return 0;
        }
    }
    if (opts.create_room) {
        rv = api_create_room();
        if (rv)
            die("Couldn't create room");
    }

    log_msg("Watching %s", path);

    rv = server_connect(opts.host, opts.port);
    if (rv)
        die("Couldn't connect to server");

    if (opts.read_only) {
        pthread_cond_wait(&server_conn_done, &server_conn_mtx);
        goto main_cleanup;
    }

#ifdef INOTIFY
    log_debug("Using Inotify to watch for changes.");
    rv = inotifytools_initialize();
    if (rv == 0)
        die("inotifytools_initialize() failed: %s", strerror(inotifytools_error()));

    rv = inotifytools_watch_recursively(path, IN_MODIFY);
    if (rv == 0)
        die("inotifytools_watch_recursively() failed: %s", strerror(inotifytools_error()));

    inotifytools_set_printf_timefmt("%T");

    struct inotify_event *event;
    char *full_path;
    do {
        event = inotifytools_next_event(-1);
        inotifytools_printf(event, "%T %w%f %e\n");
        full_path = inotifytools_filename_from_wd(event->wd);
        log_debug("Change in %s", full_path);
        push_changes(l_mp, path, full_path);
    } while (event);
#elif FSEVENTS
    log_debug("Using FSEvents to watch for changes.");
    CFStringRef cfs_path = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8); /* pretty sure I'm leaking this */
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&cfs_path, 1, NULL); /* ditto */
    FSEventStreamContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.info = path;
    FSEventStreamRef stream;
    /* TODO: make this an option */
    CFAbsoluteTime latency = 0.1;

    stream = FSEventStreamCreate(NULL, &event_cb, &ctx, paths, kFSEventStreamEventIdSinceNow, latency, kFSEventStreamCreateFlagNone);
    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(stream);
    CFRunLoopRun();
#else
#error "Need FSEvents or Inotify to build this"
#endif

    main_cleanup:;
    cleanup();
    return(0);
}
