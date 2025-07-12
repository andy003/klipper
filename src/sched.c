// 基础调度函数和启动/关闭代码。
//
// Copyright (C) 2016-2024  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <setjmp.h> // setjmp
#include "autoconf.h" // CONFIG_*
#include "basecmd.h" // stats_update
#include "board/io.h" // readb
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_from_us
#include "board/pgm.h" // READP
#include "command.h" // shutdown
#include "sched.h" // sched_check_periodic
#include "stepper.h" // stepper_event

// 定时器静态变量声明
static struct timer periodic_timer, sentinel_timer, deleted_timer;

// 调度状态结构体：包含定时器列表和任务状态信息
static struct {
    struct timer *timer_list, *last_insert;  // 定时器链表和最后插入的定时器
    int8_t tasks_status, tasks_busy;          // 任务状态和忙碌状态
    uint8_t shutdown_status, shutdown_reason; // 关机状态和原因
} SchedStatus = {.timer_list = &periodic_timer, .last_insert = &periodic_timer};


/****************************************************************
 * 定时器管理
 ****************************************************************/

// 周期性定时器简化了定时器代码，确保定时器列表中始终有定时器
// 并且始终有一个不远的将来要执行的定时器
static uint_fast8_t
periodic_event(struct timer *t)
{
    // 确保统计任务定期运行
    sched_wake_tasks();
    // 重新调度定时器 - 100毫秒后再次触发
    periodic_timer.waketime += timer_from_us(100000);
    // 更新哨兵定时器的唤醒时间
    sentinel_timer.waketime = periodic_timer.waketime + 0x80000000;
    return SF_RESCHEDULE;  // 返回重新调度标志
}

// 周期性定时器定义：每100毫秒触发一次
static struct timer periodic_timer = {
    .func = periodic_event,
    .next = &sentinel_timer,
};

// 哨兵定时器总是定时器列表中的最后一个定时器
// 它的存在使代码在遍历定时器列表时避免检查NULL
// 由于哨兵定时器的唤醒时间总是等于（周期性定时器的唤醒时间 + 0x80000000）
// 任何添加的定时器必须始终有一个小于这两个定时器之一的唤醒时间
static uint_fast8_t
sentinel_event(struct timer *t)
{
    // 如果哨兵定时器被调用，说明系统出现严重错误
    shutdown("sentinel timer called");
}

// 哨兵定时器定义：用于标记定时器列表的结束
static struct timer sentinel_timer = {
    .func = sentinel_event,
    .waketime = 0x80000000,
};

// 在定时器列表中找到定时器的位置并插入它
static void __always_inline
insert_timer(struct timer *pos, struct timer *t, uint32_t waketime)
{
    struct timer *prev;
    // 遍历定时器列表，找到合适的插入位置
    for (;;) {
        prev = pos;
        if (CONFIG_MACH_AVR)
            // AVR的微优化 - 减少寄存器压力
            asm("" : "+r"(prev));
        pos = pos->next;
        // 如果当前定时器的唤醒时间早于下一个定时器，则插入在此位置
        if (timer_is_before(waketime, pos->waketime))
            break;
    }
    // 插入新定时器到链表中
    t->next = pos;
    prev->next = t;
}

// 在指定时间调度函数调用
void
sched_add_timer(struct timer *add)
{
    uint32_t waketime = add->waketime;
    irqstatus_t flag = irq_save();  // 保存中断状态并禁用中断
    struct timer *tl = SchedStatus.timer_list;
    
    // 如果这个定时器比所有其他已调度的定时器都早
    if (unlikely(timer_is_before(waketime, tl->waketime))) {
        // 检查定时器是否太接近当前时间
        if (timer_is_before(waketime, timer_read_time()))
            try_shutdown("Timer too close");
        
        // 将新定时器设置为第一个要执行的定时器
        if (tl == &deleted_timer)
            add->next = deleted_timer.next;
        else
            add->next = tl;
        deleted_timer.waketime = waketime;
        deleted_timer.next = add;
        SchedStatus.timer_list = &deleted_timer;
        timer_kick();  // 通知硬件定时器
    } else {
        // 在合适的位置插入定时器
        insert_timer(tl, add, waketime);
    }
    irq_restore(flag);  // 恢复中断状态
}

// 删除定时器时使用的占位定时器
static uint_fast8_t
deleted_event(struct timer *t)
{
    return SF_DONE;  // 返回完成标志
}

// 删除定时器的占位定时器定义
static struct timer deleted_timer = {
    .func = deleted_event,
};

// 删除可能正在运行的定时器
void
sched_del_timer(struct timer *del)
{
    irqstatus_t flag = irq_save();  // 保存中断状态并禁用中断
    
    if (SchedStatus.timer_list == del) {
        // 删除下一个活动定时器 - 用deleted_timer替换
        deleted_timer.waketime = del->waketime;
        deleted_timer.next = del->next;
        SchedStatus.timer_list = &deleted_timer;
    } else {
        // 在定时器列表中查找并删除（如果存在）
        struct timer *pos;
        for (pos = SchedStatus.timer_list; pos->next; pos = pos->next) {
            if (pos->next == del) {
                pos->next = del->next;
                break;
            }
        }
    }
    // 更新最后插入的定时器指针
    if (SchedStatus.last_insert == del)
        SchedStatus.last_insert = &periodic_timer;
    irq_restore(flag);  // 恢复中断状态
}

// 调用下一个定时器 - 从板硬件中断代码调用
unsigned int
sched_timer_dispatch(void)
{
    // 调用定时器回调函数
    struct timer *t = SchedStatus.timer_list;
    uint_fast8_t res;
    uint32_t updated_waketime;
    
    // 步进器的内联优化
    if (CONFIG_INLINE_STEPPER_HACK && likely(!t->func)) {
        res = stepper_event(t);
        updated_waketime = t->waketime;
    } else {
        res = t->func(t);
        updated_waketime = t->waketime;
    }

    // 更新定时器列表（如果需要，重新调度当前定时器）
    unsigned int next_waketime = updated_waketime;
    if (unlikely(res == SF_DONE)) {
        // 定时器完成，从列表中移除
        next_waketime = t->next->waketime;
        SchedStatus.timer_list = t->next;
        if (SchedStatus.last_insert == t)
            SchedStatus.last_insert = t->next;
    } else if (!timer_is_before(updated_waketime, t->next->waketime)) {
        // 定时器需要重新调度
        next_waketime = t->next->waketime;
        SchedStatus.timer_list = t->next;
        struct timer *pos = SchedStatus.last_insert;
        if (timer_is_before(updated_waketime, pos->waketime))
            pos = SchedStatus.timer_list;
        insert_timer(pos, t, updated_waketime);
        SchedStatus.last_insert = t;
    }

    return next_waketime;
}

// 删除所有用户定时器
void
sched_timer_reset(void)
{
    SchedStatus.timer_list = &deleted_timer;
    deleted_timer.waketime = periodic_timer.waketime;
    deleted_timer.next = SchedStatus.last_insert = &periodic_timer;
    periodic_timer.next = &sentinel_timer;
    timer_kick();  // 通知硬件定时器
}


/****************************************************************
 * 任务管理
 ****************************************************************/

// 任务状态常量定义
#define TS_IDLE      -1   // 空闲状态，表示没有任务需要运行
#define TS_REQUESTED 0    // 请求状态，表示有任务被唤醒，等待运行
#define TS_RUNNING   1    // 运行状态，表示任务正在运行

// 标记至少有一个任务准备运行
void
sched_wake_tasks(void)
{
    SchedStatus.tasks_status = TS_REQUESTED;
}

// 检查任务是否繁忙（从低级定时器调度代码调用）
uint8_t
sched_check_set_tasks_busy(void)
{
    // 如果任务在两次连续调用之间从未空闲，则返回繁忙
    if (SchedStatus.tasks_busy >= TS_REQUESTED)
        return 1;
    SchedStatus.tasks_busy = SchedStatus.tasks_status;
    return 0;
}

// 标记任务准备运行
void
sched_wake_task(struct task_wake *w)
{
    sched_wake_tasks();
    writeb(&w->wake, 1);  // 设置唤醒标志
}

// 检查任务是否准备运行（如sched_wake_task所示）
uint8_t
sched_check_wake(struct task_wake *w)
{
    if (!readb(&w->wake))
        return 0;
    writeb(&w->wake, 0);  // 清除唤醒标志
    return 1;
}

// 主任务调度循环
static void
run_tasks(void)
{
    uint32_t start = timer_read_time();
    for (;;) {
        // 检查是否可以休眠
        irq_poll();
        if (SchedStatus.tasks_status != TS_REQUESTED) {
            start -= timer_read_time();
            irq_disable();
            if (SchedStatus.tasks_status != TS_REQUESTED) {
                // 让处理器休眠（只运行定时器）直到任务被唤醒
                SchedStatus.tasks_status = SchedStatus.tasks_busy = TS_IDLE;
                do {
                    irq_wait();  // 等待中断
                } while (SchedStatus.tasks_status != TS_REQUESTED);
            }
            irq_enable();
            start += timer_read_time();
        }
        SchedStatus.tasks_status = TS_RUNNING;

        // 运行所有任务
        extern void ctr_run_taskfuncs(void);
        ctr_run_taskfuncs();

        // 更新统计信息
        uint32_t cur = timer_read_time();
        stats_update(start, cur);
        start = cur;
    }
}


/****************************************************************
 * 关机处理
 ****************************************************************/

// 如果机器处于紧急停止状态，返回true
uint8_t
sched_is_shutdown(void)
{
    return !!SchedStatus.shutdown_status;
}

// 从关机状态转换出来
void
sched_clear_shutdown(void)
{
    if (!SchedStatus.shutdown_status)
        shutdown("Shutdown cleared when not shutdown");
    if (SchedStatus.shutdown_status == 2)
        // 如果仍在处理关机，忽略清除关机的尝试
        return;
    SchedStatus.shutdown_status = 0;
}

// 调用所有关机函数（由DECL_SHUTDOWN声明）
static void
run_shutdown(int reason)
{
    irq_disable();  // 禁用中断
    uint32_t cur = timer_read_time();
    if (!SchedStatus.shutdown_status)
        SchedStatus.shutdown_reason = reason;
    SchedStatus.shutdown_status = 2;  // 设置为正在关机状态
    sched_timer_reset();  // 重置所有定时器
    extern void ctr_run_shutdownfuncs(void);
    ctr_run_shutdownfuncs();  // 运行关机函数
    SchedStatus.shutdown_status = 1;  // 设置为已关机状态
    irq_enable();  // 重新启用中断

    // 发送关机消息
    sendf("shutdown clock=%u static_string_id=%hu", cur
          , SchedStatus.shutdown_reason);
}

// 报告最后的关机原因代码
void
sched_report_shutdown(void)
{
    sendf("is_shutdown static_string_id=%hu", SchedStatus.shutdown_reason);
}

// 如果尚未在关机过程中，则关闭机器
void __always_inline
sched_try_shutdown(uint_fast8_t reason)
{
    if (!SchedStatus.shutdown_status)
        sched_shutdown(reason);
}

// 用于立即跳转到关机处理程序的跳转缓冲区
static jmp_buf shutdown_jmp;

// 强制机器立即运行关机处理程序
void
sched_shutdown(uint_fast8_t reason)
{
    irq_disable();  // 禁用中断
    longjmp(shutdown_jmp, reason);  // 跳转到关机处理程序
}


/****************************************************************
 * 启动处理
 ****************************************************************/

// 程序的主循环
void
sched_main(void)
{
    // 运行所有初始化函数
    extern void ctr_run_initfuncs(void);
    ctr_run_initfuncs();

    // 发送启动消息
    sendf("starting");

    // 设置关机跳转点
    irq_disable();
    int ret = setjmp(shutdown_jmp);
    if (ret)
        run_shutdown(ret);  // 如果收到关机信号，运行关机处理程序
    irq_enable();

    // 开始运行任务调度循环
    run_tasks();
}
