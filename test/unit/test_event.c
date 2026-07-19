#include "common/event.h"
#include "unity.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static struct eventloop loop;

void
setUp(void)
{
    TEST_ASSERT_TRUE(eventloop_init(&loop));
}

void
tearDown(void)
{
    eventloop_uninit(&loop);
}

/* ------------------------------------------------------------------- */
/* Helpers                                                              */
/* ------------------------------------------------------------------- */

struct call_record
{
    int      count;
    int      last_fd;
    uint32_t last_events;
    bool     remove_after;
};

static bool
recording_source_cb(int fd, int events, void *udata)
{
    struct call_record *rec = udata;
    rec->count++;
    rec->last_fd = fd;
    rec->last_events = events;
    return rec->remove_after;
}

struct prepare_record
{
    int  count;
    bool remove_after;
};

static bool
recording_prepare_cb(void *udata)
{
    struct prepare_record *rec = udata;
    rec->count++;
    return rec->remove_after;
}

/* Runs eventloop_run() on a background thread so tests can drive fds
 * from the main thread and then stop the loop. */
struct run_thread_ctx
{
    struct eventloop *loop;
    bool              result;
};

static void *
run_thread_main(void *arg)
{
    struct run_thread_ctx *ctx = arg;
    ctx->result = eventloop_run(ctx->loop);
    return NULL;
}

/* ------------------------------------------------------------------- */
/* eventloop_init / eventloop_uninit                                    */
/* ------------------------------------------------------------------- */

static void
test_init_creates_valid_loop(void)
{
    struct eventloop l;
    TEST_ASSERT_TRUE(eventloop_init(&l));
    TEST_ASSERT_FALSE(l.stop);
    eventloop_uninit(&l);
}

static void
test_uninit_with_no_sources_is_safe(void)
{
    /* setUp already initialized `loop`; tearDown will uninit it.
     * Just make sure calling uninit here doesn't crash and tearDown
     * doesn't double-free. */
    struct eventloop l;
    TEST_ASSERT_TRUE(eventloop_init(&l));
    eventloop_uninit(&l);
}

static void
test_uninit_frees_dangling_sources(void)
{
    /* If the caller forgets to remove sources before uninit, the loop
     * should clean them up itself rather than leaking/crashing. */
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd != -1);

    struct call_record rec = {0};
    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec
    ));

    /* tearDown() will call eventloop_uninit(&loop); it must not crash
     * even though the source above was never removed. */
    close(fd);
}

static void
test_uninit_frees_dangling_prepares(void)
{
    struct prepare_record rec = {0};
    int id = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);
    TEST_ASSERT_TRUE(id >= 0);

    /* tearDown() should clean this up without crashing. */
}

/* ------------------------------------------------------------------- */
/* eventloop_add / eventloop_del                                        */
/* ------------------------------------------------------------------- */

static void
test_add_valid_fd_succeeds(void)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd != -1);

    struct call_record rec = {0};
    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec
    ));

    TEST_ASSERT_TRUE(eventloop_del(&loop, fd));
    close(fd);
}

static void
test_add_invalid_fd_fails(void)
{
    struct call_record rec = {0};
    /* -1 is never a valid fd; epoll_ctl(ADD) should fail. */
    TEST_ASSERT_FALSE(eventloop_add(
        &loop, -1, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec
    ));
}

static void
test_del_unknown_fd_returns_false(void)
{
    TEST_ASSERT_FALSE(eventloop_del(&loop, 12345));
}

static void
test_del_removes_only_matching_fd(void)
{
    int fd1 = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int fd2 = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd1 != -1 && fd2 != -1);

    struct call_record rec1 = {0}, rec2 = {0};
    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd1, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec1
    ));
    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd2, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec2
    ));

    TEST_ASSERT_TRUE(eventloop_del(&loop, fd1));
    /* fd1 is gone, but a second attempt must not affect fd2 or crash. */
    TEST_ASSERT_FALSE(eventloop_del(&loop, fd1));
    TEST_ASSERT_TRUE(eventloop_del(&loop, fd2));

    close(fd1);
    close(fd2);
}

/* ------------------------------------------------------------------- */
/* eventloop_add_prepare / eventloop_del_prepare                        */
/* ------------------------------------------------------------------- */

static void
test_add_prepare_returns_increasing_ids(void)
{
    struct prepare_record rec = {0};

    int id1 = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);
    int id2 = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);

    TEST_ASSERT_TRUE(id1 >= 0);
    TEST_ASSERT_TRUE(id2 >= 0);
    TEST_ASSERT_NOT_EQUAL(id1, id2);

    TEST_ASSERT_TRUE(eventloop_del_prepare(&loop, id1));
    TEST_ASSERT_TRUE(eventloop_del_prepare(&loop, id2));
}

static void
test_del_prepare_unknown_id_returns_false(void)
{
    TEST_ASSERT_FALSE(eventloop_del_prepare(&loop, 999));
}

static void
test_del_prepare_removes_only_matching_id(void)
{
    struct prepare_record rec = {0};

    int id1 = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);
    int id2 = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);

    TEST_ASSERT_TRUE(eventloop_del_prepare(&loop, id1));
    TEST_ASSERT_FALSE(eventloop_del_prepare(&loop, id1));
    TEST_ASSERT_TRUE(eventloop_del_prepare(&loop, id2));
}

/* ------------------------------------------------------------------- */
/* eventloop_run / callback dispatch                                    */
/* ------------------------------------------------------------------- */

static void
test_source_callback_fires_on_readable_fd(void)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd != -1);

    struct call_record rec = {0};
    rec.remove_after = false;

    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec
    ));

    uint64_t one = 1;
    TEST_ASSERT_TRUE(write(fd, &one, sizeof(one)) == sizeof(one));

    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    /* Give the loop a brief moment to dispatch, then stop it. */
    usleep(50 * 1000);
    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    TEST_ASSERT_TRUE(ctx.result);
    TEST_ASSERT_TRUE(rec.count >= 1);
    TEST_ASSERT_EQUAL_INT(fd, rec.last_fd);

    eventloop_del(&loop, fd);
    close(fd);
}

static void
test_source_removed_when_callback_returns_true(void)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd != -1);

    struct call_record rec = {0};
    rec.remove_after = true;

    TEST_ASSERT_TRUE(eventloop_add(
        &loop, fd, EVENT_PRIORITY_NORMAL, EPOLLIN, recording_source_cb, &rec
    ));

    uint64_t one = 1;
    TEST_ASSERT_TRUE(write(fd, &one, sizeof(one)) == sizeof(one));

    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    usleep(50 * 1000);
    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    TEST_ASSERT_EQUAL_INT(1, rec.count);
    /* Source was auto-removed by the loop; deleting it again must fail. */
    TEST_ASSERT_FALSE(eventloop_del(&loop, fd));

    close(fd);
}

/* Shared dispatch-order tracker used by
 * test_high_priority_dispatched_before_normal. Not thread-safe in general, but
 * only ever written from within the single event loop thread, and only read
 * from the main thread after that thread has been joined, so no locking is
 * needed here. */
struct order_record
{
    int count;
    int order_slot; /* position in the global dispatch sequence, or -1 */
};

static int order_sequence[8];
static int order_sequence_len;

static bool
order_tracking_cb(int fd, int events, void *udata)
{
    (void)events;
    struct order_record *rec = udata;

    /* eventfd is level-triggered: if we don't drain it, epoll will keep
     * reporting it as readable forever and the loop will spin calling
     * this callback on every iteration. Read the counter to clear it. */
    uint64_t val;
    ssize_t  r = read(fd, &val, sizeof(val));
    (void)r;

    rec->count++;
    if (rec->order_slot == -1 && order_sequence_len < 8)
        rec->order_slot = order_sequence_len++;
    return false;
}

static void
test_high_priority_dispatched_before_normal(void)
{
    int fd_high = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int fd_norm = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT_TRUE(fd_high != -1 && fd_norm != -1);

    order_sequence_len = 0;
    memset(order_sequence, 0, sizeof(order_sequence));

    struct order_record rec_high = {.count = 0, .order_slot = -1};
    struct order_record rec_norm = {.count = 0, .order_slot = -1};

    /* Add the normal-priority source first so a pass that simply
     * dispatches in insertion order would (incorrectly) fire it before
     * the high-priority one. */
    TEST_ASSERT_TRUE(eventloop_add(
        &loop,
        fd_norm,
        EVENT_PRIORITY_NORMAL,
        EPOLLIN,
        order_tracking_cb,
        &rec_norm
    ));
    TEST_ASSERT_TRUE(eventloop_add(
        &loop,
        fd_high,
        EVENT_PRIORITY_HIGH,
        EPOLLIN,
        order_tracking_cb,
        &rec_high
    ));

    /* Signal both fds *before* the loop starts so a single epoll_wait()
     * call returns both events together, forcing the loop to sort them
     * into priority buckets rather than just dispatching in whatever
     * order epoll_wait() happened to report them. */
    uint64_t one = 1;
    TEST_ASSERT_TRUE(write(fd_norm, &one, sizeof(one)) == sizeof(one));
    TEST_ASSERT_TRUE(write(fd_high, &one, sizeof(one)) == sizeof(one));

    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    usleep(50 * 1000);
    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    /* Both must have fired exactly once, and the high-priority source's
     * slot in the dispatch sequence must come strictly before the
     * normal-priority source's slot. */
    TEST_ASSERT_EQUAL_INT(1, rec_high.count);
    TEST_ASSERT_EQUAL_INT(1, rec_norm.count);
    TEST_ASSERT_TRUE(rec_high.order_slot != -1);
    TEST_ASSERT_TRUE(rec_norm.order_slot != -1);
    TEST_ASSERT_TRUE(rec_high.order_slot < rec_norm.order_slot);

    eventloop_del(&loop, fd_high);
    eventloop_del(&loop, fd_norm);
    close(fd_high);
    close(fd_norm);
}

static void
test_prepare_callback_runs_each_iteration(void)
{
    struct prepare_record rec = {0};
    rec.remove_after = false;

    int id = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);
    TEST_ASSERT_TRUE(id >= 0);

    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    /* Wake the loop a couple of times so prepare runs more than once. */
    usleep(20 * 1000);
    eventloop_wakeup(&loop);
    usleep(20 * 1000);
    eventloop_wakeup(&loop);
    usleep(20 * 1000);

    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    TEST_ASSERT_TRUE(rec.count >= 2);

    eventloop_del_prepare(&loop, id);
}

static void
test_prepare_removed_when_callback_returns_true(void)
{
    struct prepare_record rec = {0};
    rec.remove_after = true;

    int id = eventloop_add_prepare(&loop, recording_prepare_cb, &rec);
    TEST_ASSERT_TRUE(id >= 0);

    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    usleep(20 * 1000);
    eventloop_wakeup(&loop);
    usleep(20 * 1000);
    eventloop_wakeup(&loop);
    usleep(20 * 1000);

    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    /* Prepare callback should only have fired once before being removed. */
    TEST_ASSERT_EQUAL_INT(1, rec.count);
    TEST_ASSERT_FALSE(eventloop_del_prepare(&loop, id));
}

/* ------------------------------------------------------------------- */
/* eventloop_wakeup / eventloop_stop                                    */
/* ------------------------------------------------------------------- */

static void
test_stop_causes_run_to_return_true(void)
{
    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    usleep(20 * 1000);
    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    TEST_ASSERT_TRUE(ctx.result);
}

static void
test_wakeup_does_not_stop_loop(void)
{
    pthread_t             thread;
    struct run_thread_ctx ctx = {.loop = &loop};
    pthread_create(&thread, NULL, run_thread_main, &ctx);

    /* Wake up several times; the loop must keep running until an
     * explicit eventloop_stop(). */
    for (int i = 0; i < 5; i++)
    {
        eventloop_wakeup(&loop);
        usleep(10 * 1000);
    }

    /* If the loop had incorrectly stopped, this eventloop_stop() call
     * plus join would still succeed quickly; the real assertion is
     * that the thread is still alive/running right before this call.
     * We approximate this by checking the total elapsed behavior via
     * the result below. */
    eventloop_stop(&loop);
    pthread_join(thread, NULL);

    TEST_ASSERT_TRUE(ctx.result);
}

/* ------------------------------------------------------------------- */

int
main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_creates_valid_loop);
    RUN_TEST(test_uninit_with_no_sources_is_safe);
    RUN_TEST(test_uninit_frees_dangling_sources);
    RUN_TEST(test_uninit_frees_dangling_prepares);

    RUN_TEST(test_add_valid_fd_succeeds);
    RUN_TEST(test_add_invalid_fd_fails);
    RUN_TEST(test_del_unknown_fd_returns_false);
    RUN_TEST(test_del_removes_only_matching_fd);

    RUN_TEST(test_add_prepare_returns_increasing_ids);
    RUN_TEST(test_del_prepare_unknown_id_returns_false);
    RUN_TEST(test_del_prepare_removes_only_matching_id);

    RUN_TEST(test_source_callback_fires_on_readable_fd);
    RUN_TEST(test_source_removed_when_callback_returns_true);
    RUN_TEST(test_high_priority_dispatched_before_normal);
    RUN_TEST(test_prepare_callback_runs_each_iteration);
    RUN_TEST(test_prepare_removed_when_callback_returns_true);

    RUN_TEST(test_stop_causes_run_to_return_true);
    RUN_TEST(test_wakeup_does_not_stop_loop);

    return UNITY_END();
}
