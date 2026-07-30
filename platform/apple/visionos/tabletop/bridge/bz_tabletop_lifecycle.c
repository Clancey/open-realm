#include "bz_tabletop_lifecycle.h"
#include "common/bz_runtime.h"

#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char text[BZ_TABLETOP_MAP_PATH_MAX + 8];
} bzTabletopCommand_t;

struct bzTabletopLifecycle_s {
    int argc;
    LPCSTR *argv; /* deep-copied strings, owned by this struct */

    pthread_t thread;
    bool thread_started; /* a pthread_create()'d handle exists and has not been joined yet */
    bool joining; /* another thread is currently inside pthread_join(lc->thread) */
    int spawn_count; /* how many times pthread_create() has succeeded for this instance (never >1: single-shot) */
    int running_publish_count; /* how many times state has actually been set to RUNNING (test/diagnostic only) */


    pthread_mutex_t lock;
    pthread_cond_t cond;
    bzTabletopState_t state;
    volatile bool stop_requested;
    LPCSTR last_error; /* points at a BZ_RuntimeInitResultString() literal, or NULL */
    bzTabletopCommand_t commands[BZ_TABLETOP_COMMAND_QUEUE_CAPACITY];
    unsigned command_read, command_count;
};

/* Sys_Quit() runs synchronously on whichever thread calls Com_Quit() —
 * for this host that is always the engine thread bz_tabletop_lifecycle.c
 * itself created, so a thread-local pointer (set/cleared by
 * EngineThreadMain) is enough to find the right instance without a
 * process-wide singleton, and correctly scopes multiple concurrent
 * lifecycle instances to their own engine thread. */
static __thread bzTabletopLifecycle_t *tls_current_lc = NULL;

/* Drains commands under the lifecycle lock, then executes without holding it. */
static void run_queued_commands(bzTabletopLifecycle_t *lc) {
    bzTabletopCommand_t command;

    for (;;) {
        pthread_mutex_lock(&lc->lock);
        if (!lc->command_count) {
            pthread_mutex_unlock(&lc->lock);
            return;
        }
        command = lc->commands[lc->command_read];
        lc->command_read = (lc->command_read + 1) % BZ_TABLETOP_COMMAND_QUEUE_CAPACITY;
        lc->command_count--;
        pthread_mutex_unlock(&lc->lock);
        if (!BZ_RuntimeExecuteCommand(command.text))
            fprintf(stderr, "BZ_Tabletop: engine rejected command '%s'\n", command.text);
    }
}

void Sys_Quit(void) {
    bzTabletopLifecycle_t *lc = tls_current_lc;
    if (!lc) {
        /* Sys_Quit() is only ever expected to run on an engine thread this
         * module created (via Com_Quit(), from BZ_RuntimeFrame's frame-limit
         * check or a "quit" command); log rather than silently doing
         * nothing so a misuse (e.g. calling it from an unrelated thread) is
         * visible instead of hidden. */
        fprintf(stderr, "Sys_Quit: no tabletop lifecycle bound to this thread\n");
        return;
    }
    /* Re-entrant call from inside the engine thread's own BZ_RuntimeFrame():
     * BZ_TabletopStop() detects this and only marks the stop request rather
     * than trying to join the calling thread against itself. */
    BZ_TabletopStop(lc);
}

static void *EngineThreadMain(void *arg) {
    bzTabletopLifecycle_t *lc = (bzTabletopLifecycle_t *)arg;
    tls_current_lc = lc;

    bzRuntimeArgs_t rt_args;
    rt_args.argc = lc->argc;
    rt_args.argv = lc->argv;
    bzRuntimeInitResult_t result = BZ_RuntimeInit(&rt_args);

    pthread_mutex_lock(&lc->lock);
    if (result != BZ_RUNTIME_INIT_OK) {
        lc->last_error = BZ_RuntimeInitResultString(result);
        lc->state = BZ_TABLETOP_STATE_FAILED;
        pthread_cond_broadcast(&lc->cond);
        pthread_mutex_unlock(&lc->lock);
        tls_current_lc = NULL;
        return NULL;
    }
    /* An external BZ_TabletopStop() may have arrived while we were still
     * inside BZ_RuntimeInit() above (state STARTING) — reap_engine_thread()
     * is already blocked in pthread_join() waiting for us in that case.
     * This background init-completion path must never publish RUNNING
     * once a stop has been requested: doing so would let a background
     * startup path transition the visible state away from the shutdown
     * the caller already asked for, even if only for a brief window before
     * the loop below immediately breaks. Skip straight to shutdown/STOPPED
     * instead — BZ_RuntimeInit() already ran (NET_Init() etc. may have
     * allocated resources), so BZ_RuntimeShutdown() below still must run. */
    bool stop_before_running = lc->stop_requested;
    if (!stop_before_running) {
        lc->state = BZ_TABLETOP_STATE_RUNNING;
        lc->running_publish_count++;
    }
    pthread_cond_broadcast(&lc->cond);
    pthread_mutex_unlock(&lc->lock);

    if (stop_before_running) {
        BZ_RuntimeShutdown();
        pthread_mutex_lock(&lc->lock);
        lc->state = BZ_TABLETOP_STATE_STOPPED;
        pthread_cond_broadcast(&lc->cond);
        pthread_mutex_unlock(&lc->lock);
        tls_current_lc = NULL;
        return NULL;
    }

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    for (;;) {
        pthread_mutex_lock(&lc->lock);
        while (lc->state == BZ_TABLETOP_STATE_SUSPENDED && !lc->stop_requested) {
            pthread_cond_wait(&lc->cond, &lc->lock);
        }
        bool stop = lc->stop_requested;
        pthread_mutex_unlock(&lc->lock);
        if (stop) {
            break;
        }

        run_queued_commands(lc);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - prev.tv_sec) * 1000L + (now.tv_nsec - prev.tv_nsec) / 1000000L;
        if (elapsed_ms < 0) {
            elapsed_ms = 0; /* CLOCK_MONOTONIC never goes backwards, but guard anyway */
        }
        prev = now;

        /* false means the engine already shut itself down from inside this
         * call (com_frame_limit reached, or a menu/console "quit" — both
         * paths run Com_Quit(), which calls Sys_Quit() above and sets
         * lc->stop_requested via the re-entrant BZ_TabletopStop() call). */
        if (!BZ_RuntimeFrame((DWORD)elapsed_ms)) {
            break;
        }

        struct timespec tick = { 0, 16L * 1000L * 1000L }; /* ~60Hz: no external display link drives this layer yet */
        nanosleep(&tick, NULL);
    }

    BZ_RuntimeShutdown(); /* idempotent: no-op if Com_Quit() already ran it */

    pthread_mutex_lock(&lc->lock);
    lc->state = BZ_TABLETOP_STATE_STOPPED;
    pthread_cond_broadcast(&lc->cond);
    pthread_mutex_unlock(&lc->lock);

    tls_current_lc = NULL;
    return NULL;
}

/* Joins lc->thread if the engine thread has ended but not yet been reaped
 * (thread_started still true) — used by BZ_TabletopStop()'s external-stop
 * path so a frame-limit/console-"quit" self-stop that already exited
 * doesn't leak its thread handle. Must NEVER be called from the engine
 * thread itself (callers check pthread_equal() first; see
 * BZ_TabletopStop()).
 *
 * Only one caller may actually be inside pthread_join(lc->thread) at a
 * time: calling pthread_join() on the same target thread from two threads
 * simultaneously is undefined behavior per POSIX ("the behavior is
 * undefined if... the value specified by thread does not refer to a
 * joinable thread" / multiple concurrent joiners of one thread), and was
 * observed to abort under ThreadSanitizer with the concurrent-stop test.
 * The `joining` flag, checked and set under `lock`, serializes this: a
 * second concurrent caller instead waits on `cond` until the first
 * finishes and flips thread_started to false, rather than racing its own
 * pthread_join() call against the first. */
static void reap_engine_thread(bzTabletopLifecycle_t *lc) {
    pthread_mutex_lock(&lc->lock);
    if (!lc->thread_started) {
        pthread_mutex_unlock(&lc->lock);
        return;
    }
    if (lc->joining) {
        while (lc->thread_started) {
            pthread_cond_wait(&lc->cond, &lc->lock);
        }
        pthread_mutex_unlock(&lc->lock);
        return;
    }
    lc->joining = true;
    pthread_t t = lc->thread;
    pthread_mutex_unlock(&lc->lock);

    pthread_join(t, NULL);

    pthread_mutex_lock(&lc->lock);
    lc->thread_started = false;
    lc->joining = false;
    pthread_cond_broadcast(&lc->cond); /* wake any callers waiting in the branch above */
    pthread_mutex_unlock(&lc->lock);
}

bzTabletopLifecycle_t *BZ_TabletopCreate(int argc, LPCSTR *argv) {
    bzTabletopLifecycle_t *lc = calloc(1, sizeof(*lc));
    if (!lc) {
        fprintf(stderr, "BZ_TabletopCreate: out of memory\n");
        return NULL;
    }

    lc->argc = argc;
    lc->argv = calloc((size_t)argc, sizeof(LPCSTR));
    if (!lc->argv) {
        fprintf(stderr, "BZ_TabletopCreate: out of memory copying argv\n");
        free(lc);
        return NULL;
    }
    for (int i = 0; i < argc; i++) {
        lc->argv[i] = argv[i] ? strdup(argv[i]) : strdup("");
    }

    pthread_mutex_init(&lc->lock, NULL);
    pthread_cond_init(&lc->cond, NULL);
    lc->state = BZ_TABLETOP_STATE_IDLE;
    lc->last_error = NULL;
    return lc;
}

void BZ_TabletopStart(bzTabletopLifecycle_t *lc) {
    if (!lc) {
        return;
    }

    /* The engine thread is single-shot per lifecycle instance: STOPPED and
     * FAILED are terminal and must never regress back into
     * STARTING/RUNNING. Checking state and (if IDLE) claiming STARTING
     * happen atomically under one lock hold so two threads racing
     * BZ_TabletopStart() concurrently on a fresh IDLE instance cannot both
     * pass the check and both pthread_create() a thread for the same
     * lc. Callers that need to run again must BZ_TabletopCreate() a new
     * instance. */
    pthread_mutex_lock(&lc->lock);
    if (lc->state != BZ_TABLETOP_STATE_IDLE) {
        if (lc->state == BZ_TABLETOP_STATE_FAILED || lc->state == BZ_TABLETOP_STATE_STOPPED) {
            fprintf(stderr, "BZ_TabletopStart: instance already reached a terminal state "
                             "(engine thread is single-shot); create a new lifecycle to run again\n");
        }
        pthread_mutex_unlock(&lc->lock);
        return;
    }
    lc->state = BZ_TABLETOP_STATE_STARTING;
    lc->stop_requested = false;
    lc->last_error = NULL;
    pthread_mutex_unlock(&lc->lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, EngineThreadMain, lc) != 0) {
        fprintf(stderr, "BZ_TabletopStart: pthread_create failed\n");
        pthread_mutex_lock(&lc->lock);
        lc->state = BZ_TABLETOP_STATE_FAILED;
        lc->last_error = "failed to create engine thread";
        pthread_cond_broadcast(&lc->cond);
        pthread_mutex_unlock(&lc->lock);
        return;
    }

    pthread_mutex_lock(&lc->lock);
    lc->thread = tid;
    lc->thread_started = true;
    lc->spawn_count++;
    while (lc->state == BZ_TABLETOP_STATE_STARTING) {
        pthread_cond_wait(&lc->cond, &lc->lock);
    }
    pthread_mutex_unlock(&lc->lock);
}

void BZ_TabletopSuspend(bzTabletopLifecycle_t *lc) {
    if (!lc) {
        return;
    }
    pthread_mutex_lock(&lc->lock);
    if (lc->state == BZ_TABLETOP_STATE_RUNNING) {
        lc->state = BZ_TABLETOP_STATE_SUSPENDED;
        pthread_cond_broadcast(&lc->cond);
    }
    pthread_mutex_unlock(&lc->lock);
}

void BZ_TabletopResume(bzTabletopLifecycle_t *lc) {
    if (!lc) {
        return;
    }
    pthread_mutex_lock(&lc->lock);
    if (lc->state == BZ_TABLETOP_STATE_SUSPENDED) {
        lc->state = BZ_TABLETOP_STATE_RUNNING;
        pthread_cond_broadcast(&lc->cond);
    }
    pthread_mutex_unlock(&lc->lock);
}

bool BZ_TabletopSubmitMap(bzTabletopLifecycle_t *lc, LPCSTR map) {
    size_t length;

    if (!lc || !map || !(length = strlen(map)) || length >= BZ_TABLETOP_MAP_PATH_MAX ||
        strpbrk(map, "\"\r\n;")) {
        fprintf(stderr, "BZ_TabletopSubmitMap: invalid map path\n");
        return false;
    }
    pthread_mutex_lock(&lc->lock);
    if ((lc->state != BZ_TABLETOP_STATE_RUNNING && lc->state != BZ_TABLETOP_STATE_SUSPENDED) ||
        lc->command_count == BZ_TABLETOP_COMMAND_QUEUE_CAPACITY) {
        fprintf(stderr, "BZ_TabletopSubmitMap: lifecycle unavailable or command queue full\n");
        pthread_mutex_unlock(&lc->lock);
        return false;
    }
    unsigned write = (lc->command_read + lc->command_count) % BZ_TABLETOP_COMMAND_QUEUE_CAPACITY;
    snprintf(lc->commands[write].text, sizeof(lc->commands[write].text), "map \"%s\"", map);
    lc->command_count++;
    pthread_cond_broadcast(&lc->cond);
    pthread_mutex_unlock(&lc->lock);
    return true;
}

void BZ_TabletopStop(bzTabletopLifecycle_t *lc) {
    if (!lc) {
        return;
    }

    pthread_mutex_lock(&lc->lock);
    if (lc->state == BZ_TABLETOP_STATE_IDLE) {
        lc->state = BZ_TABLETOP_STATE_STOPPED; /* engine thread never ran */
    }
    lc->stop_requested = true;
    /* Snapshot the self-thread check under the same lock that guards
     * lc->thread/thread_started, so it cannot race BZ_TabletopStart()'s
     * own pthread_create()-success write of those two fields (which also
     * happens under this lock, immediately after spawning — see
     * BZ_TabletopStart()). Since Start() never re-spawns once a thread
     * exists (single-shot/terminal design), there is no restart path here
     * to race against; this snapshot only needs to be consistent with the
     * one-time IDLE->STARTING transition. */
    bool is_self = lc->thread_started && pthread_equal(pthread_self(), lc->thread);
    pthread_cond_broadcast(&lc->cond); /* wake a SUSPENDED engine thread so it observes the stop request */
    pthread_mutex_unlock(&lc->lock);

    if (is_self) {
        /* Called from inside the engine thread (Sys_Quit()): a thread
         * cannot join itself. Leave thread_started set — the thread will
         * finish exiting on its own, and a later BZ_TabletopStart()/
         * BZ_TabletopStop()/BZ_TabletopDestroy() call from another thread
         * performs the actual join via reap_engine_thread(). */
        return;
    }

    reap_engine_thread(lc);
}

void BZ_TabletopDestroy(bzTabletopLifecycle_t *lc) {
    if (!lc) {
        return;
    }
    /* BZ_TabletopStop() reaps the engine thread when called from any
     * thread other than the engine thread itself. Destroy() being called
     * FROM the engine thread is not a supported usage (a running engine
     * thread cannot be tearing down its own lifecycle object), so no
     * further join attempt is made here beyond what Stop() already did. */
    BZ_TabletopStop(lc);
    pthread_mutex_destroy(&lc->lock);
    pthread_cond_destroy(&lc->cond);
    for (int i = 0; i < lc->argc; i++) {
        free((void *)lc->argv[i]);
    }
    free(lc->argv);
    free(lc);
}

bzTabletopState_t BZ_TabletopGetState(bzTabletopLifecycle_t const *lc) {
    if (!lc) {
        return BZ_TABLETOP_STATE_IDLE;
    }
    pthread_mutex_lock((pthread_mutex_t *)&lc->lock);
    bzTabletopState_t state = lc->state;
    pthread_mutex_unlock((pthread_mutex_t *)&lc->lock);
    return state;
}

LPCSTR BZ_TabletopLastError(bzTabletopLifecycle_t const *lc) {
    if (!lc) {
        return NULL;
    }
    return lc->last_error;
}

int BZ_TabletopEngineThreadSpawnCount(bzTabletopLifecycle_t const *lc) {
    if (!lc) {
        return 0;
    }
    pthread_mutex_lock((pthread_mutex_t *)&lc->lock);
    int count = lc->spawn_count;
    pthread_mutex_unlock((pthread_mutex_t *)&lc->lock);
    return count;
}

int BZ_TabletopRunningPublishCount(bzTabletopLifecycle_t const *lc) {
    if (!lc) {
        return 0;
    }
    pthread_mutex_lock((pthread_mutex_t *)&lc->lock);
    int count = lc->running_publish_count;
    pthread_mutex_unlock((pthread_mutex_t *)&lc->lock);
    return count;
}
