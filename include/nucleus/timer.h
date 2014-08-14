/*
 * Copyright (C) 2001,2002,2003 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _XENO_NUCLEUS_TIMER_H
#define _XENO_NUCLEUS_TIMER_H

#include <nucleus/queue.h>

#if defined(__KERNEL__) || defined(__XENO_UVM__) || defined(__XENO_SIM__)

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
/* Number of outstanding timers (hint only) -- must be ^2 */
#define XNTIMER_WHEELSIZE 64
#define XNTIMER_WHEELMASK (XNTIMER_WHEELSIZE - 1)
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */

#define XNTIMER_DEQUEUED  0x00000001
#define XNTIMER_KILLED    0x00000002

/* These flags are available to the real-time interfaces */
#define XNTIMER_SPARE0  0x01000000
#define XNTIMER_SPARE1  0x02000000
#define XNTIMER_SPARE2  0x04000000
#define XNTIMER_SPARE3  0x08000000
#define XNTIMER_SPARE4  0x10000000
#define XNTIMER_SPARE5  0x20000000
#define XNTIMER_SPARE6  0x40000000
#define XNTIMER_SPARE7  0x80000000

#define XNTIMER_LOPRIO  (-999999999)
#define XNTIMER_STDPRIO 0
#define XNTIMER_HIPRIO  999999999

#define XNTIMER_KEEPER_ID 0

typedef struct {
    xnholder_t link;
    xnticks_t key;
    int prio;

#define link2tlholder(laddr) \
    ((xntlholder_t *)(((char *)laddr) - offsetof(xntlholder_t, link)))

} xntlholder_t;
#define xntlholder_date(h)      ((h)->key)
#define xntlholder_prio(h)      ((h)->prio)
#define xntlholder_init(h)      inith(&(h)->link)
#define xntlist_init(q)         initq(q)
#define xntlist_head(q)                         \
    ({ xnholder_t *_h = getheadq(q);            \
        !_h ? NULL : link2tlholder(_h);         \
    })

static inline void xntlist_insert(xnqueue_t *q, xntlholder_t *holder)
{
    xnholder_t *p;

    /* Insert the new timer at the proper place in the single
       queue managed when running in aperiodic mode. O(N) here,
       but users of the aperiodic mode need to pay a price for
       the increased flexibility... */

    for (p = q->head.last; p != &q->head; p = p->last)
        if (holder->key > link2tlholder(p)->key ||
            (holder->key == link2tlholder(p)->key &&
             holder->prio <= link2tlholder(p)->prio))
            break;

    insertq(q,p->next,&holder->link);
}

#define xntlist_remove(q, h)  removeq((q),&(h)->link)

#if defined(CONFIG_XENO_OPT_TIMER_HEAP)
#include <nucleus/bheap.h>
typedef bheaph_t xntimerh_t;
#define xntimerh_date(h)       bheaph_key(h)
#define xntimerh_prio(h)       bheaph_prio(h)
#define xntimerh_init(h)       bheaph_init(h)
typedef DECLARE_BHEAP_CONTAINER(xntimerq_t, CONFIG_XENO_OPT_TIMER_HEAP_CAPACITY);
#define xntimerq_init(q)       bheap_init((q), CONFIG_XENO_OPT_TIMER_HEAP_CAPACITY)
#define xntimerq_destroy(q)    bheap_destroy(q)
#define xntimerq_head(q)       bheap_gethead(q)
#define xntimerq_insert(q, h)  bheap_insert((q),(h))
#define xntimerq_remove(q, h)  bheap_delete((q),(h))

#else /* CONFIG_XENO_OPT_TIMER_LIST */
typedef xntlholder_t xntimerh_t;
#define xntimerh_date(h)       xntlholder_date(h)
#define xntimerh_prio(h)       xntlholder_prio(h)
#define xntimerh_init(h)       xntlholder_init(h)
typedef xnqueue_t xntimerq_t;
#define xntimerq_init(q)       xntlist_init(q)
#define xntimerq_destroy(q)    do { } while (0)
#define xntimerq_head(q)       xntlist_head(q)
#define xntimerq_insert(q,h)   xntlist_insert((q),(h))
#define xntimerq_remove(q, h)  xntlist_remove((q),(h))

#endif /* CONFIG_XENO_OPT_TIMER_LIST */

struct xnsched;

typedef struct xntimer {

    xntimerh_t aplink;          /* Link in aperiodic timers list. */

#define aplink2timer(laddr) \
    ((xntimer_t *)(((char *)(laddr)) - (int)(&((xntimer_t *)0)->aplink)))

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
    xntlholder_t plink;         /* Link in periodic timers wheel. */

#define plink2timer(laddr) \
    ((xntimer_t *)(((char *)(laddr)) - (int)(&((xntimer_t *)0)->plink)))
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */

    xnflags_t status;		/* !< Timer status. */

    xnticks_t interval;		/* !< Periodic interval (in ticks, 0 == one shot). */

    struct xnsched *sched;      /* !< Sched structure to which the timer is
                                   attached. */

    void (*handler)(void *cookie); /* !< Timeout handler. */

    void *cookie;	/* !< Cookie to pass to the timeout handler. */

    XNARCH_DECL_DISPLAY_CONTEXT();

} xntimer_t;

#if defined(CONFIG_SMP)
#define xntimer_sched(t)          ((t)->sched)
#else /* !CONFIG_SMP */
#define xntimer_sched(t)          xnpod_current_sched()
#endif /* !CONFIG_SMP */
#define xntimer_interval(t)       ((t)->interval)
#define xntimer_set_cookie(t,c)   ((t)->cookie = (c))

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
#define xntimer_set_priority(t,p) ({                    \
            xntimer_t *_t = (t);                        \
            unsigned prio = (p);                        \
            xntimerh_prio(&(_t)->aplink) = prio;        \
            xntlholder_prio(&(_t)->plink) = prio;       \
        })
#else /* !CONFIG_XENO_OPT_TIMING_PERIODIC */
#define xntimer_set_priority(t,p) (xntimerh_prio(&(t)->aplink) = (p))
#endif /* !CONFIG_XENO_OPT_TIMING_PERIODIC */

static inline int xntimer_active_p (xntimer_t *timer)
{
    return timer->sched != NULL;
}

static inline int xntimer_running_p (xntimer_t *timer)
{
    return !testbits(timer->status,XNTIMER_DEQUEUED);
}

typedef struct xntmops {

    void (*do_tick)(void);
    xnticks_t (*get_jiffies)(void);
    xnticks_t (*get_raw_clock)(void);
    void (*do_timer_start)(xntimer_t *timer,
			   xnticks_t value,
			   xnticks_t interval);
    void (*do_timer_stop)(xntimer_t *timer);
    xnticks_t (*get_timer_date)(xntimer_t *timer);
    xnticks_t (*get_timer_timeout)(xntimer_t *timer);
    xnticks_t (*get_timer_raw_expiry)(xntimer_t *timer);
    void (*set_timer_remote)(xntimer_t *timer);
    const char *(*get_type)(void);
    void (*freeze)(void);

} xntmops_t;

#ifdef __cplusplus
extern "C" {
#endif

extern xntmops_t *nktimer;

void xntimer_init(xntimer_t *timer,
		  void (*handler)(void *cookie),
		  void *cookie);

void xntimer_destroy(xntimer_t *timer);

void xntimer_start(xntimer_t *timer,
		   xnticks_t value,
		   xnticks_t interval);

/*!
 * \fn int xntimer_stop(xntimer_t *timer)
 *
 * \brief Disarm a timer.
 *
 * This service deactivates a timer previously armed using
 * xntimer_start(). Once disarmed, the timer can be subsequently
 * re-armed using the latter service.
 *
 * @param timer The address of a valid timer descriptor.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

static inline void xntimer_stop(xntimer_t *timer)
{
    /* Careful: the do_timer_stop() helper is expected to preserve the
       date field of the stopped timer, so that subsequent calls to
       xntimer_get_timeout() would still work on such timer as
       expected. */
    if (!testbits(timer->status,XNTIMER_DEQUEUED))
	nktimer->do_timer_stop(timer);
}

static inline xnticks_t xntimer_get_jiffies(void)
{
#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
    return nktimer->get_jiffies();
#else /* !CONFIG_XENO_OPT_TIMING_PERIODIC */
    return xnarch_get_cpu_time();
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */
}

static inline xnticks_t xntimer_get_rawclock(void)
{
#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
    return nktimer->get_raw_clock();
#else /* !CONFIG_XENO_OPT_TIMING_PERIODIC */
    return xnarch_get_cpu_tsc();
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */
}

static inline xnticks_t xntimer_get_raw_expiry (xntimer_t *timer)
{
#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
    return nktimer->get_timer_raw_expiry(timer);
#else /* !CONFIG_XENO_OPT_TIMING_PERIODIC */
    return xntimerh_date(&timer->aplink);
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */
}

void xntimer_freeze(void);

xnticks_t xntimer_get_date(xntimer_t *timer);

xnticks_t xntimer_get_timeout(xntimer_t *timer);

void xntimer_set_periodic_mode(void);

void xntimer_set_aperiodic_mode(void);

#if defined(CONFIG_SMP)
int xntimer_set_sched(xntimer_t *timer, struct xnsched *sched);
#else /* ! CONFIG_SMP */
#define xntimer_set_sched(timer,sched)
#endif /* CONFIG_SMP */

#ifdef __cplusplus
}
#endif

#endif /* __KERNEL__ || __XENO_UVM__ || __XENO_SIM__ */

#endif /* !_XENO_NUCLEUS_TIMER_H */
