// Handling of timers in simulator environment (using libevent)
//
// Copyright (C) 2017-2021  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <time.h> // struct timespec
#include <signal.h> // signal handling
#include <unistd.h> // usleep
#include <event2/event.h> // libevent
#include <event2/event_struct.h> // libevent structures
#include <sys/time.h> // gettimeofday
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_from_us
#include "board/timer_irq.h" // timer_dispatch_many
#include "command.h" // DECL_CONSTANT
#include "compiler.h" // unlikely
#include "sched.h" // DECL_INIT

// Global storage for timer handling
static uint32_t last_read_time;
static time_t start_sec;
static struct event_base *event_base;
static struct event *timer_event;
static uint32_t must_wake_timers;
static uint32_t next_wake_counter;
static struct timespec next_wake;

#define NSECS 1000000000LL
#define NSECS_PER_TICK (NSECS / CONFIG_CLOCK_FREQ)


/****************************************************************
 * Timespec helpers
 ****************************************************************/

// Convert a 'struct timespec' to a counter value
static inline uint32_t
timespec_to_time(struct timespec ts)
{
    return ((ts.tv_sec - start_sec) * CONFIG_CLOCK_FREQ
            + ts.tv_nsec / NSECS_PER_TICK);
}

// Convert an internal time counter to a 'struct timespec'
static inline struct timespec
timespec_from_time(uint32_t time)
{
    int32_t counter_diff = time - next_wake_counter;
    struct timespec ts;
    ts.tv_sec = next_wake.tv_sec;
    ts.tv_nsec = next_wake.tv_nsec + counter_diff * NSECS_PER_TICK;
    if ((unsigned long)ts.tv_nsec >= NSECS) {
        if (ts.tv_nsec < 0) {
            ts.tv_sec--;
            ts.tv_nsec += NSECS;
        } else {
            ts.tv_sec++;
            ts.tv_nsec -= NSECS;
        }
    }
    return ts;
}

// Return the current time
static struct timespec
timespec_read(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

// Convert timespec to timeval for libevent
static inline struct timeval
timespec_to_timeval(struct timespec ts)
{
    struct timeval tv;
    tv.tv_sec = ts.tv_sec;
    tv.tv_usec = ts.tv_nsec / 1000;
    return tv;
}

/****************************************************************
 * Timers
 ****************************************************************/

DECL_CONSTANT("CLOCK_FREQ", CONFIG_CLOCK_FREQ);

// Check if a given time has past
int
timer_check_periodic(uint32_t *ts)
{
    uint32_t lrt = last_read_time;
    if (timer_is_before(lrt, *ts))
        return 0;
    *ts = lrt + timer_from_us(2000000);
    return 1;
}

// Return the current time (in clock ticks)
uint32_t
timer_read_time(void)
{
    uint32_t t = timespec_to_time(timespec_read());
    last_read_time = t;
    return t;
}


#define TIMER_IDLE_REPEAT_COUNT 100
#define TIMER_REPEAT_COUNT 20
#define TIMER_MIN_TRY_TICKS timer_from_us(2)

// Invoke timers
static void
timer_dispatch(void)
{
    uint32_t repeat_count = TIMER_REPEAT_COUNT, next;
    for (;;) {
        // Run the next software timer
        next = timer_dispatch_many();

        repeat_count--;
        uint32_t lrt = last_read_time;
        if (!timer_is_before(lrt, next) && repeat_count)
            // Can run next timer without overhead of calling timer_read_time()
            continue;

        uint32_t now = timer_read_time();
        int32_t diff = next - now;
        if (diff > (int32_t)TIMER_MIN_TRY_TICKS)
            // Schedule next timer normally.
            break;

        if (unlikely(!repeat_count)) {
            // Check if there are too many repeat timers
            if (diff < (int32_t)(-timer_from_us(100000)))
                try_shutdown("Rescheduled timer in the past");
            if (sched_check_set_tasks_busy())
                return;
            repeat_count = TIMER_IDLE_REPEAT_COUNT;
        }

        // Next timer in the past or near future - wait for it to be ready
        while (unlikely(diff > 0))
            diff = next - timer_read_time();
    }

    // Update next wake time and schedule libevent timer
    next_wake_counter = next;
    must_wake_timers = 0;
    
    // Schedule the next libevent timer
    if (event_base && timer_event) {
        struct timespec next_ts = timespec_from_time(next);
        struct timeval timeout = timespec_to_timeval(next_ts);
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        
        // Calculate relative timeout
        if (timeout.tv_sec > now_tv.tv_sec || 
            (timeout.tv_sec == now_tv.tv_sec && timeout.tv_usec > now_tv.tv_usec)) {
            timeout.tv_sec -= now_tv.tv_sec;
            if (timeout.tv_usec < now_tv.tv_usec) {
                timeout.tv_sec--;
                timeout.tv_usec += 1000000;
            }
            timeout.tv_usec -= now_tv.tv_usec;
        } else {
            // Timer is already due, schedule immediately
            timeout.tv_sec = 0;
            timeout.tv_usec = 1;
        }
        
        evtimer_add(timer_event, &timeout);
    }
}

// Activate timer dispatch as soon as possible
void
timer_kick(void)
{
    timer_dispatch();
}

// Callback function for libevent timer
static void
timer_callback(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    (void)arg;
    
    if (must_wake_timers) {
        timer_dispatch();
    }
}

void
timer_init(void)
{
    // Initialize timespec_to_time() and timespec_from_time()
    struct timespec curtime = timespec_read();
    start_sec = curtime.tv_sec + 1;
    next_wake = curtime;
    next_wake_counter = timespec_to_time(curtime);
    must_wake_timers = 1;
    
    // Initialize libevent
    event_base = event_base_new();
    if (!event_base) {
        try_shutdown("Failed to create libevent base");
        return;
    }
    
    // Create timer event
    timer_event = evtimer_new(event_base, timer_callback, NULL);
    if (!timer_event) {
        event_base_free(event_base);
        event_base = NULL;
        try_shutdown("Failed to create libevent timer");
        return;
    }
    
    timer_kick();
}
DECL_INIT(timer_init);

// Cleanup function for shutdown
void
timer_cleanup(void)
{
    if (timer_event) {
        event_free(timer_event);
        timer_event = NULL;
    }
    
    if (event_base) {
        event_base_free(event_base);
        event_base = NULL;
    }
}

/****************************************************************
 * Interrupt wrappers
 ****************************************************************/

void
irq_disable(void)
{
}

void
irq_enable(void)
{
}

irqstatus_t
irq_save(void)
{
    return 0;
}

void
irq_restore(irqstatus_t flag)
{
}

void
irq_wait(void)
{
    event_base_loop(event_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
}

void
irq_poll(void)
{
}
