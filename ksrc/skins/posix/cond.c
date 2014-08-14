/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@laposte.net>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/**
 * @ingroup posix
 * @defgroup posix_cond Condition variables services.
 *
 * Condition variables services.
 *
 * A condition variable is a synchronization object that allows threads to
 * suspend execution until some predicate on shared data is satisfied. The basic
 * operations on conditions are: signal the condition (when the predicate
 * becomes true), and wait for the condition, suspending the thread execution
 * until another thread signals the condition.
 *
 * A condition variable must always be associated with a mutex, to avoid the
 * race condition where a thread prepares to wait on a condition variable and
 * another thread signals the condition just before the first thread actually
 * waits on it.
 *
 * Before it can be used, a condition variable has to be initialized with
 * pthread_cond_init(). An attribute object, which reference may be passed to
 * this service, allows to select the features of the created condition
 * variable, namely the @a clock used by the pthread_cond_timedwait() service
 * (@a CLOCK_REALTIME is used by default).
 *
 * There is no support for the @a pshared attribute; condition variables created
 * by Xenomai POSIX skin may be shared by kernel-space modules and user-space
 * processes through shared memory.
 *
 * Note that only pthread_cond_init() may be used to initialize a condition
 * variable, using the static initializer @a PTHREAD_COND_INITIALIZER is
 * not supported.
 *
 *@{*/

#include <posix/mutex.h>
#include <posix/cond.h>

typedef struct pse51_cond {
    xnsynch_t synchbase;
    xnholder_t link;            /* Link in pse51_condq */

#define link2cond(laddr)                                                \
    ((pse51_cond_t *)(((char *)laddr) - offsetof(pse51_cond_t, link)))

    pthread_condattr_t attr;
    struct __shadow_mutex *mutex;
} pse51_cond_t;

static pthread_condattr_t default_cond_attr;

static xnqueue_t pse51_condq;

static void cond_destroy_internal (pse51_cond_t *cond)

{
    removeq(&pse51_condq, &cond->link);
    /* synchbase wait queue may not be empty only when this function is called
       from pse51_cond_pkg_cleanup, hence the absence of xnpod_schedule(). */
    xnsynch_destroy(&cond->synchbase);
    xnfree(cond);
}

/**
 * Initialize a condition variable.
 *
 * This service initializes the condition variable @a cnd, using the condition
 * variable attributes object @a attr. If @a attr is @a NULL or this service is
 * called from user-space, default attributes are used (see
 * pthread_condattr_init()).
 *
 * @param cnd the condition variable to be initialized;
 *
 * @param attr the condition variable attributes object.
 *
 * @return 0 on succes,
 * @return an error number if:
 * - EINVAL, the condition variable attributes object @a attr is invalid or
 *   uninitialized;
 * - EBUSY, the condition variable @a cnd was already initialized;
 * - ENOMEM, insufficient memory exists in the system heap to initialize the
 *   condition variable, increase CONFIG_XENO_OPT_SYS_HEAPSZ.
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_init.html
 * 
 */
int pthread_cond_init (pthread_cond_t *cnd, const pthread_condattr_t *attr)

{
    struct __shadow_cond *shadow = &((union __xeno_cond *) cnd)->shadow_cond;
    xnflags_t synch_flags = XNSYNCH_PRIO | XNSYNCH_NOPIP;
    pse51_cond_t *cond;
    spl_t s;

    if (!attr)
        attr = &default_cond_attr;

    xnlock_get_irqsave(&nklock, s);

    if (attr->magic != PSE51_COND_ATTR_MAGIC)
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    if (shadow->magic == PSE51_COND_MAGIC)
        {
        xnholder_t *holder;
        for(holder = getheadq(&pse51_condq); holder;
            holder = nextq(&pse51_condq, holder))
            if (holder == &shadow->cond->link)
                {
                /* cond is already in the queue. */
                xnlock_put_irqrestore(&nklock, s);
                return EBUSY;
                }
        }

    cond = (pse51_cond_t *) xnmalloc(sizeof(*cond));
    if (!cond)
        {
        xnlock_put_irqrestore(&nklock, s);
        return ENOMEM;
        }

    shadow->magic = PSE51_COND_MAGIC;
    shadow->cond = cond;

    xnsynch_init(&cond->synchbase, synch_flags);
    inith(&cond->link);
    cond->attr = *attr;
    cond->mutex = NULL;

    appendq(&pse51_condq, &cond->link);

    xnlock_put_irqrestore(&nklock, s);

    return 0;    
}

/**
 * Destroy a condition variable.
 *
 * This service destroys the condition variable @a cnd, if no thread is
 * currently blocked on it. The condition variable becomes invalid for all
 * condition variable services (they all return the EINVAL error) except
 * pthread_cond_init().
 *
 * @param cnd the condition variable to be destroyed.
 *
 * @return 0 on succes,
 * @return an error number if:
 * - EINVAL, the condition variable @a cnd is invalid;
 * - EBUSY, some thread is currently blocked on the condition variable.
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_destroy.html
 * 
 */
int pthread_cond_destroy (pthread_cond_t *cnd)

{
    struct __shadow_cond *shadow = &((union __xeno_cond *) cnd)->shadow_cond;
    pse51_cond_t *cond;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (!pse51_obj_active(shadow, PSE51_COND_MAGIC, struct __shadow_cond))
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    cond = shadow->cond;

    if (xnsynch_nsleepers(&cond->synchbase))
	{
        xnlock_put_irqrestore(&nklock, s);
        return EBUSY;
	}

    cond_destroy_internal(cond);
    pse51_mark_deleted(shadow);
    
    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

/* must be called with nklock locked, interrupts off. */
static inline int mutex_save_count(xnthread_t *cur,
                                   struct __shadow_mutex *shadow,
                                   unsigned *count_ptr)
{
    pse51_mutex_t *mutex;

    if (!pse51_obj_active(shadow, PSE51_MUTEX_MAGIC, struct __shadow_mutex))
        return EINVAL;

    mutex = shadow->mutex;

    if (xnsynch_owner(&mutex->synchbase) != cur || mutex->count == 0)
        return EPERM;

    *count_ptr = mutex->count;
    mutex->count = 0;

    xnsynch_wakeup_one_sleeper(&mutex->synchbase);
    /* Do not reschedule here, releasing the mutex and suspension must be done
       atomically in pthread_cond_*wait. */
    
    return 0;
}

/* must be called with nklock locked, interrupts off. */
static inline void mutex_restore_count(xnthread_t *cur,
                                       struct __shadow_mutex *shadow,
                                       unsigned count)
{
    pse51_mutex_t *mutex = shadow->mutex;

    /* Relock the mutex */
    mutex_timedlock_internal(cur, shadow, XN_INFINITE);

    /* Restore the mutex lock count. */
    mutex->count = count;
}

int pse51_cond_timedwait_internal(struct __shadow_cond *shadow,
                                  struct __shadow_mutex *mutex,
                                  xnticks_t to)
{
    xnthread_t *cur = xnpod_current_thread();
    pse51_cond_t *cond;
    unsigned count;
    spl_t s;
    int err;

    if (!shadow || !mutex)
        return EINVAL;

    if (xnpod_unblockable_p())
        return EPERM;

    xnlock_get_irqsave(&nklock, s);

    thread_cancellation_point(cur);

    cond = shadow->cond;

    /* If another thread waiting for cond does not use the same mutex */
    if (!pse51_obj_active(shadow, PSE51_COND_MAGIC, struct __shadow_cond)
       || (cond->mutex && cond->mutex != mutex))
	{
        err = EINVAL;
        goto unlock_and_return;
	}

    err = clock_adjust_timeout(&to, cond->attr.clock);

    if(err)
        goto unlock_and_return;
    
    /* Unlock mutex, with its previous recursive lock count stored
       in "count". */
    err = mutex_save_count(cur, mutex, &count);

    if(err)
        goto unlock_and_return;

    /* Bind mutex to cond. */
    if (cond->mutex == NULL)
        {
        cond->mutex = mutex;
        ++mutex->mutex->condvars;
        }

    /* Wait for another thread to signal the condition. */
    xnsynch_sleep_on(&cond->synchbase, to);

    /* There are four possible wakeup conditions :
       - cond_signal / cond_broadcast, no status bit is set, and the function
         should return 0 ;
       - timeout, the status XNTIMEO is set, and the function should return
         ETIMEDOUT ;
       - pthread_kill, the status bit XNBREAK is set, but ignored, the function
         simply returns EINTR (used only by the user-space interface, replaced
         by 0 anywhere else), causing a wakeup, spurious or not whether
         pthread_cond_signal was called between pthread_kill and the moment
         when xnsynch_sleep_on returned ;
       - pthread_cancel, no status bit is set, but cancellation specific bits are
         set, and tested only once the mutex is reacquired, so that the
         cancellation handler can be called with the mutex locked, as required by
         the specification.
    */

    err = 0;
    
    if (xnthread_test_flags(cur, XNBREAK))
        err = EINTR;
    else if (xnthread_test_flags(cur, XNTIMEO))
        err = ETIMEDOUT;

    /* Unbind mutex and cond, if no other thread is waiting, if the job was not
       already done. */
    if (!xnsynch_nsleepers(&cond->synchbase) && cond->mutex == mutex)
	{
        --mutex->mutex->condvars;
        cond->mutex = NULL;
	}

    /* relock mutex */
    mutex_restore_count(cur, mutex, count);

    thread_cancellation_point(cur);

  unlock_and_return:
    xnlock_put_irqrestore(&nklock, s);

    return err;
}

/**
 * Wait on a condition variable.
 *
 * This service atomically unlocks the mutex @a mx, and block the calling thread
 * until the condition variable @a cnd is signalled using pthread_cond_signal()
 * or pthread_cond_broadcast(). When the condition is signaled, this service
 * re-acquire the mutex before returning.
 *
 * Spurious wakeups occur if a signal is delivered to the blocked thread, so, an
 * application should not assume that the condition changed upon successful
 * return from this service.
 *
 * Even if the mutex @a mx is recursive and its recursion count is greater than
 * one on entry, it is unlocked before blocking the caller, and the recursion
 * count is restored once the mutex is re-acquired by this service before
 * returning.
 *
 * Once a thread is blocked on a condition variable, a dynamic binding is formed
 * between the condition vairable @a cnd and the mutex @a mx; if another thread
 * calls this service specifying @a cnd as a condition variable but another
 * mutex than @a mx, this service returns immediately with the EINVAL status.
 *
 * This service is a cancellation point for Xenomai POSIX skin threads
 * (created with the pthread_create() service). When such a thread is cancelled
 * while blocked in a call to this service, the mutex @a mx is re-acquired
 * before the cancellation cleanup handlers are called.
 *
 * @param cnd the condition variable to wait for;
 *
 * @param mx the mutex associated with @a cnd.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the specified condition variable or mutex is invalid;
 * - EPERM, the caller context is invalid;
 * - EINVAL, another thread is currently blocked on @a cnd using another mutex
 *   than @a mx;
 * - EPERM, the specified mutex is not owned by the caller.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread;
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_wait.html
 * 
 */
int pthread_cond_wait (pthread_cond_t *cnd, pthread_mutex_t *mx)

{
    struct __shadow_mutex *mutex = &((union __xeno_mutex *) mx)->shadow_mutex;
    struct __shadow_cond *cond = &((union __xeno_cond *) cnd)->shadow_cond;
    int err;

    err = pse51_cond_timedwait_internal(cond, mutex, XN_INFINITE);

    return err == EINTR ? 0 : err;
}

/**
 * Wait a bounded time on a condition variable.
 *
 * This service is equivalent to pthread_cond_wait(), except that the calling
 * thread remains blocked on the condition variable @a cnd only until the
 * timeout specified by @a abstime expires.
 *
 * The timeout @a abstime is expressed as an absolute value of the @a clock
 * attribute passed to pthread_cond_init(). By default, @a CLOCK_REALTIME is
 * used.
 *
 * @param cnd the condition variable to wait for;
 *
 * @param mx the mutex associated with @a cnd;
 *
 * @param abstime the timeout, expressed as an absolute value of the clock
 * attribute passed to pthread_cond_init().
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the specified condition variable, mutex or timeout is invalid;
 * - EPERM, the caller context is invalid;
 * - EINVAL, another thread is currently blocked on @a cnd using another mutex
 *   than @a mx;
 * - EPERM, the specified mutex is not owned by the caller;
 * - ETIMEDOUT, the specified timeout expired.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread;
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_timedwait.html
 * 
 */
int pthread_cond_timedwait (pthread_cond_t *cnd,
			    pthread_mutex_t *mx,
			    const struct timespec *abstime)

{
    struct __shadow_mutex *mutex = &((union __xeno_mutex *) mx)->shadow_mutex;
    struct __shadow_cond *cond = &((union __xeno_cond *) cnd)->shadow_cond;
    int err;

    err = pse51_cond_timedwait_internal(cond, mutex, ts2ticks_ceil(abstime)+1);

    return err == EINTR ? 0 : err;
}

/**
 * Signal a condition variable.
 *
 * This service unblocks one thread blocked on the condition variable @a cnd.
 *
 * If more than one thread is blocked on the specified condition variable, the
 * highest priority thread is unblocked.
 *
 * @param cnd the condition variable to be signalled.
 *
 * @return 0 on succes,
 * @return an error number if:
 * - EINVAL, the condition variable is invalid.
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_signal.html.
 * 
 */
int pthread_cond_signal (pthread_cond_t *cnd)

{
    struct __shadow_cond *shadow = &((union __xeno_cond *) cnd)->shadow_cond;
    pse51_cond_t *cond;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (!pse51_obj_active(shadow, PSE51_COND_MAGIC, struct __shadow_cond))
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    cond = shadow->cond;

    /* FIXME: If the mutex associated with cnd is owned by the current thread,
       we could postpone rescheduling until pthread_mutex_unlock is called, this
       would save two useless context switches. */
    if (xnsynch_wakeup_one_sleeper(&cond->synchbase) != NULL)
        xnpod_schedule();

    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

/**
 * Broadcast a condition variable.
 *
 * This service unblocks all threads blocked on the condition variable @a cnd.
 *
 * @param cnd the condition variable to be signalled.
 *
 * @return 0 on succes,
 * @return an error number if:
 * - EINVAL, the condition variable is invalid.
 *
 * @see http://www.opengroup.org/onlinepubs/000095399/functions/pthread_cond_broadcast.html
 * 
 */
int pthread_cond_broadcast (pthread_cond_t *cnd)

{
    struct __shadow_cond *shadow = &((union __xeno_cond *) cnd)->shadow_cond;
    pse51_cond_t *cond;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (!pse51_obj_active(shadow, PSE51_COND_MAGIC, struct __shadow_cond))
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    cond = shadow->cond;

    if(xnsynch_flush(&cond->synchbase, 0) == XNSYNCH_RESCHED)
        xnpod_schedule();

    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

void pse51_cond_pkg_init (void)

{
    initq(&pse51_condq);
    pthread_condattr_init(&default_cond_attr);
}

void pse51_cond_pkg_cleanup (void)

{
    xnholder_t *holder;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    while ((holder = getheadq(&pse51_condq)) != NULL)
        {
#ifdef CONFIG_XENO_OPT_DEBUG
        xnprintf("Posix condition variable %p was not destroyed, destroying"
                 " now.\n", link2cond(holder));
#endif /* CONFIG_XENO_OPT_DEBUG */
        cond_destroy_internal(link2cond(holder));
        }

    xnlock_put_irqrestore(&nklock, s);
}

/*@}*/

EXPORT_SYMBOL(pthread_cond_init);
EXPORT_SYMBOL(pthread_cond_destroy);
EXPORT_SYMBOL(pthread_cond_wait);
EXPORT_SYMBOL(pthread_cond_timedwait);
EXPORT_SYMBOL(pthread_cond_signal);
EXPORT_SYMBOL(pthread_cond_broadcast);
