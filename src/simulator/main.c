// Main starting point for host simulator.
//
// Copyright (C) 2016-2018  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "sched.h" // sched_main

#include <event2/event.h> // libevent
#include <event2/event_struct.h> // libevent structures
#include <setjmp.h> // jmp_buf

static struct event_base *event_base;
static jmp_buf shutdown_jmp;

// Main entry point for simulator.
int main(void)
{
    event_base = event_base_new();
    if (!event_base) {
        printf("Failed to create libevent base\n");
        return 1;
    }

    extern void ctr_run_initfuncs(void);
    ctr_run_initfuncs();

    sendf("starting");

    irq_disable();
    int ret = setjmp(shutdown_jmp);
    if (ret)
        run_shutdown(ret);
    irq_enable();

    while (1) {
        event_base_loop(event_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
        uint32_t start = timer_read_time();
        for (;;) {
            // Check if can sleep
            irq_poll();
            if (SchedStatus.tasks_status != TS_REQUESTED) {
                start -= timer_read_time();
                irq_disable();
                if (SchedStatus.tasks_status != TS_REQUESTED) {
                    // Sleep processor (only run timers) until tasks woken
                    SchedStatus.tasks_status = SchedStatus.tasks_busy = TS_IDLE;
                    do {
                        irq_wait();
                    } while (SchedStatus.tasks_status != TS_REQUESTED);
                }
                irq_enable();
                start += timer_read_time();
            }
            SchedStatus.tasks_status = TS_RUNNING;

            // Run all tasks
            extern void ctr_run_taskfuncs(void);
            ctr_run_taskfuncs();

            // Update statistics
            uint32_t cur = timer_read_time();
            stats_update(start, cur);
            start = cur;
        }
    }

    return 0;
}
