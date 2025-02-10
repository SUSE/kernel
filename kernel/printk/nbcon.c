// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2022 Linutronix GmbH, John Ogness
// Copyright (C) 2022 Intel, Thomas Gleixner

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kthread.h>
#include <linux/minmax.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/syscore_ops.h>
#include <linux/types.h>
#include "internal.h"
#include "printk_ringbuffer.h"
/*
 * Printk console printing implementation for consoles which does not depend
 * on the legacy style console_lock mechanism.
 *
 * The state of the console is maintained in the "nbcon_state" atomic
 * variable.
 *
 * The console is locked when:
 *
 *   - The 'prio' field contains the priority of the context that owns the
 *     console. Only higher priority contexts are allowed to take over the
 *     lock. A value of 0 (NBCON_PRIO_NONE) means the console is not locked.
 *
 *   - The 'cpu' field denotes on which CPU the console is locked. It is used
 *     to prevent busy waiting on the same CPU. Also it informs the lock owner
 *     that it has lost the lock in a more complex scenario when the lock was
 *     taken over by a higher priority context, released, and taken on another
 *     CPU with the same priority as the interrupted owner.
 *
 * The acquire mechanism uses a few more fields:
 *
 *   - The 'req_prio' field is used by the handover approach to make the
 *     current owner aware that there is a context with a higher priority
 *     waiting for the friendly handover.
 *
 *   - The 'unsafe' field allows to take over the console in a safe way in the
 *     middle of emitting a message. The field is set only when accessing some
 *     shared resources or when the console device is manipulated. It can be
 *     cleared, for example, after emitting one character when the console
 *     device is in a consistent state.
 *
 *   - The 'unsafe_takeover' field is set when a hostile takeover took the
 *     console in an unsafe state. The console will stay in the unsafe state
 *     until re-initialized.
 *
 * The acquire mechanism uses three approaches:
 *
 *   1) Direct acquire when the console is not owned or is owned by a lower
 *      priority context and is in a safe state.
 *
 *   2) Friendly handover mechanism uses a request/grant handshake. It is used
 *      when the current owner has lower priority and the console is in an
 *      unsafe state.
 *
 *      The requesting context:
 *
 *        a) Sets its priority into the 'req_prio' field.
 *
 *        b) Waits (with a timeout) for the owning context to unlock the
 *           console.
 *
 *        c) Takes the lock and clears the 'req_prio' field.
 *
 *      The owning context:
 *
 *        a) Observes the 'req_prio' field set on exit from the unsafe
 *           console state.
 *
 *        b) Gives up console ownership by clearing the 'prio' field.
 *
 *   3) Unsafe hostile takeover allows to take over the lock even when the
 *      console is an unsafe state. It is used only in panic() by the final
 *      attempt to flush consoles in a try and hope mode.
 *
 *      Note that separate record buffers are used in panic(). As a result,
 *      the messages can be read and formatted without any risk even after
 *      using the hostile takeover in unsafe state.
 *
 * The release function simply clears the 'prio' field.
 *
 * All operations on @console::nbcon_state are atomic cmpxchg based to
 * handle concurrency.
 *
 * The acquire/release functions implement only minimal policies:
 *
 *   - Preference for higher priority contexts.
 *   - Protection of the panic CPU.
 *
 * All other policy decisions must be made at the call sites:
 *
 *   - What is marked as an unsafe section.
 *   - Whether to spin-wait if there is already an owner and the console is
 *     in an unsafe state.
 *   - Whether to attempt an unsafe hostile takeover.
 *
 * The design allows to implement the well known:
 *
 *     acquire()
 *     output_one_printk_record()
 *     release()
 *
 * The output of one printk record might be interrupted with a higher priority
 * context. The new owner is supposed to reprint the entire interrupted record
 * from scratch.
 */

/**
 * nbcon_state_set - Helper function to set the console state
 * @con:	Console to update
 * @new:	The new state to write
 *
 * Only to be used when the console is not yet or no longer visible in the
 * system. Otherwise use nbcon_state_try_cmpxchg().
 */
static inline void nbcon_state_set(struct console *con, struct nbcon_state *new)
{
	atomic_set(&ACCESS_PRIVATE(con, nbcon_state), new->atom);
}

/**
 * nbcon_state_read - Helper function to read the console state
 * @con:	Console to read
 * @state:	The state to store the result
 */
static inline void nbcon_state_read(struct console *con, struct nbcon_state *state)
{
	state->atom = atomic_read(&ACCESS_PRIVATE(con, nbcon_state));
}

/**
 * nbcon_state_try_cmpxchg() - Helper function for atomic_try_cmpxchg() on console state
 * @con:	Console to update
 * @cur:	Old/expected state
 * @new:	New state
 *
 * Return: True on success. False on fail and @cur is updated.
 */
static inline bool nbcon_state_try_cmpxchg(struct console *con, struct nbcon_state *cur,
					   struct nbcon_state *new)
{
	return atomic_try_cmpxchg(&ACCESS_PRIVATE(con, nbcon_state), &cur->atom, new->atom);
}

/**
 * nbcon_seq_read - Read the current console sequence
 * @con:	Console to read the sequence of
 *
 * Return:	Sequence number of the next record to print on @con.
 */
u64 nbcon_seq_read(struct console *con)
{
	unsigned long nbcon_seq = atomic_long_read(&ACCESS_PRIVATE(con, nbcon_seq));

	return __ulseq_to_u64seq(prb, nbcon_seq);
}

/**
 * nbcon_seq_force - Force console sequence to a specific value
 * @con:	Console to work on
 * @seq:	Sequence number value to set
 *
 * Only to be used during init (before registration) or in extreme situations
 * (such as panic with CONSOLE_REPLAY_ALL).
 */
void nbcon_seq_force(struct console *con, u64 seq)
{
	/*
	 * If the specified record no longer exists, the oldest available record
	 * is chosen. This is especially important on 32bit systems because only
	 * the lower 32 bits of the sequence number are stored. The upper 32 bits
	 * are derived from the sequence numbers available in the ringbuffer.
	 */
	u64 valid_seq = max_t(u64, seq, prb_first_valid_seq(prb));

	atomic_long_set(&ACCESS_PRIVATE(con, nbcon_seq), __u64seq_to_ulseq(valid_seq));
}

/**
 * nbcon_seq_try_update - Try to update the console sequence number
 * @ctxt:	Pointer to an acquire context that contains
 *		all information about the acquire mode
 * @new_seq:	The new sequence number to set
 *
 * @ctxt->seq is updated to the new value of @con::nbcon_seq (expanded to
 * the 64bit value). This could be a different value than @new_seq if
 * nbcon_seq_force() was used or the current context no longer owns the
 * console. In the later case, it will stop printing anyway.
 */
static void nbcon_seq_try_update(struct nbcon_context *ctxt, u64 new_seq)
{
	unsigned long nbcon_seq = __u64seq_to_ulseq(ctxt->seq);
	struct console *con = ctxt->console;

	if (atomic_long_try_cmpxchg(&ACCESS_PRIVATE(con, nbcon_seq), &nbcon_seq,
				    __u64seq_to_ulseq(new_seq))) {
		ctxt->seq = new_seq;
	} else {
		ctxt->seq = nbcon_seq_read(con);
	}
}

bool printk_threads_enabled __ro_after_init;

/**
 * nbcon_context_try_acquire_direct - Try to acquire directly
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * Acquire the console when it is released. Also acquire the console when
 * the current owner has a lower priority and the console is in a safe state.
 *
 * Return:	0 on success. Otherwise, an error code on failure. Also @cur
 *		is updated to the latest state when failed to modify it.
 *
 * Errors:
 *
 *	-EPERM:		A panic is in progress and this is not the panic CPU.
 *			Or the current owner or waiter has the same or higher
 *			priority. No acquire method can be successful in
 *			this case.
 *
 *	-EBUSY:		The current owner has a lower priority but the console
 *			in an unsafe state. The caller should try using
 *			the handover acquire method.
 */
static int nbcon_context_try_acquire_direct(struct nbcon_context *ctxt,
					    struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	do {
		if (other_cpu_in_panic())
			return -EPERM;

		if (ctxt->prio <= cur->prio || ctxt->prio <= cur->req_prio)
			return -EPERM;

		if (cur->unsafe)
			return -EBUSY;

		/*
		 * The console should never be safe for a direct acquire
		 * if an unsafe hostile takeover has ever happened.
		 */
		WARN_ON_ONCE(cur->unsafe_takeover);

		new.atom = cur->atom;
		new.prio	= ctxt->prio;
		new.req_prio	= NBCON_PRIO_NONE;
		new.unsafe	= cur->unsafe_takeover;
		new.cpu		= cpu;

	} while (!nbcon_state_try_cmpxchg(con, cur, &new));

	return 0;
}

static bool nbcon_waiter_matches(struct nbcon_state *cur, int expected_prio)
{
	/*
	 * The request context is well defined by the @req_prio because:
	 *
	 * - Only a context with a higher priority can take over the request.
	 * - There are only three priorities.
	 * - Only one CPU is allowed to request PANIC priority.
	 * - Lower priorities are ignored during panic() until reboot.
	 *
	 * As a result, the following scenario is *not* possible:
	 *
	 * 1. Another context with a higher priority directly takes ownership.
	 * 2. The higher priority context releases the ownership.
	 * 3. A lower priority context takes the ownership.
	 * 4. Another context with the same priority as this context
	 *    creates a request and starts waiting.
	 */

	return (cur->req_prio == expected_prio);
}

/**
 * nbcon_context_try_acquire_requested - Try to acquire after having
 *					 requested a handover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * This is a helper function for nbcon_context_try_acquire_handover().
 * It is called when the console is in an unsafe state. The current
 * owner will release the console on exit from the unsafe region.
 *
 * Return:	0 on success and @cur is updated to the new console state.
 *		Otherwise an error code on failure.
 *
 * Errors:
 *
 *	-EPERM:		A panic is in progress and this is not the panic CPU
 *			or this context is no longer the waiter.
 *
 *	-EBUSY:		The console is still locked. The caller should
 *			continue waiting.
 *
 * Note: The caller must still remove the request when an error has occurred
 *       except when this context is no longer the waiter.
 */
static int nbcon_context_try_acquire_requested(struct nbcon_context *ctxt,
					       struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	/* Note that the caller must still remove the request! */
	if (other_cpu_in_panic())
		return -EPERM;

	/*
	 * Note that the waiter will also change if there was an unsafe
	 * hostile takeover.
	 */
	if (!nbcon_waiter_matches(cur, ctxt->prio))
		return -EPERM;

	/* If still locked, caller should continue waiting. */
	if (cur->prio != NBCON_PRIO_NONE)
		return -EBUSY;

	/*
	 * The previous owner should have never released ownership
	 * in an unsafe region.
	 */
	WARN_ON_ONCE(cur->unsafe);

	new.atom = cur->atom;
	new.prio	= ctxt->prio;
	new.req_prio	= NBCON_PRIO_NONE;
	new.unsafe	= cur->unsafe_takeover;
	new.cpu		= cpu;

	if (!nbcon_state_try_cmpxchg(con, cur, &new)) {
		/*
		 * The acquire could fail only when it has been taken
		 * over by a higher priority context.
		 */
		WARN_ON_ONCE(nbcon_waiter_matches(cur, ctxt->prio));
		return -EPERM;
	}

	/* Handover success. This context now owns the console. */
	return 0;
}

/**
 * nbcon_context_try_acquire_handover - Try to acquire via handover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * The function must be called only when the context has higher priority
 * than the current owner and the console is in an unsafe state.
 * It is the case when nbcon_context_try_acquire_direct() returns -EBUSY.
 *
 * The function sets "req_prio" field to make the current owner aware of
 * the request. Then it waits until the current owner releases the console,
 * or an even higher context takes over the request, or timeout expires.
 *
 * The current owner checks the "req_prio" field on exit from the unsafe
 * region and releases the console. It does not touch the "req_prio" field
 * so that the console stays reserved for the waiter.
 *
 * Return:	0 on success. Otherwise, an error code on failure. Also @cur
 *		is updated to the latest state when failed to modify it.
 *
 * Errors:
 *
 *	-EPERM:		A panic is in progress and this is not the panic CPU.
 *			Or a higher priority context has taken over the
 *			console or the handover request.
 *
 *	-EBUSY:		The current owner is on the same CPU so that the hand
 *			shake could not work. Or the current owner is not
 *			willing to wait (zero timeout). Or the console does
 *			not enter the safe state before timeout passed. The
 *			caller might still use the unsafe hostile takeover
 *			when allowed.
 *
 *	-EAGAIN:	@cur has changed when creating the handover request.
 *			The caller should retry with direct acquire.
 */
static int nbcon_context_try_acquire_handover(struct nbcon_context *ctxt,
					      struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;
	int timeout;
	int request_err = -EBUSY;

	/*
	 * Check that the handover is called when the direct acquire failed
	 * with -EBUSY.
	 */
	WARN_ON_ONCE(ctxt->prio <= cur->prio || ctxt->prio <= cur->req_prio);
	WARN_ON_ONCE(!cur->unsafe);

	/* Handover is not possible on the same CPU. */
	if (cur->cpu == cpu)
		return -EBUSY;

	/*
	 * Console stays unsafe after an unsafe takeover until re-initialized.
	 * Waiting is not going to help in this case.
	 */
	if (cur->unsafe_takeover)
		return -EBUSY;

	/* Is the caller willing to wait? */
	if (ctxt->spinwait_max_us == 0)
		return -EBUSY;

	/*
	 * Setup a request for the handover. The caller should try to acquire
	 * the console directly when the current state has been modified.
	 */
	new.atom = cur->atom;
	new.req_prio = ctxt->prio;
	if (!nbcon_state_try_cmpxchg(con, cur, &new))
		return -EAGAIN;

	cur->atom = new.atom;

	/* Wait until there is no owner and then acquire the console. */
	for (timeout = ctxt->spinwait_max_us; timeout >= 0; timeout--) {
		/* On successful acquire, this request is cleared. */
		request_err = nbcon_context_try_acquire_requested(ctxt, cur);
		if (!request_err)
			return 0;

		/*
		 * If the acquire should be aborted, it must be ensured
		 * that the request is removed before returning to caller.
		 */
		if (request_err == -EPERM)
			break;

		udelay(1);

		/* Re-read the state because some time has passed. */
		nbcon_state_read(con, cur);
	}

	/* Timed out or aborted. Carefully remove handover request. */
	do {
		/*
		 * No need to remove request if there is a new waiter. This
		 * can only happen if a higher priority context has taken over
		 * the console or the handover request.
		 */
		if (!nbcon_waiter_matches(cur, ctxt->prio))
			return -EPERM;

		/* Unset request for handover. */
		new.atom = cur->atom;
		new.req_prio = NBCON_PRIO_NONE;
		if (nbcon_state_try_cmpxchg(con, cur, &new)) {
			/*
			 * Request successfully unset. Report failure of
			 * acquiring via handover.
			 */
			cur->atom = new.atom;
			return request_err;
		}

		/*
		 * Unable to remove request. Try to acquire in case
		 * the owner has released the lock.
		 */
	} while (nbcon_context_try_acquire_requested(ctxt, cur));

	/* Lucky timing. The acquire succeeded while removing the request. */
	return 0;
}

/**
 * nbcon_context_try_acquire_hostile - Acquire via unsafe hostile takeover
 * @ctxt:	The context of the caller
 * @cur:	The current console state
 *
 * Acquire the console even in the unsafe state.
 *
 * It can be permitted by setting the 'allow_unsafe_takeover' field only
 * by the final attempt to flush messages in panic().
 *
 * Return:	0 on success. -EPERM when not allowed by the context.
 */
static int nbcon_context_try_acquire_hostile(struct nbcon_context *ctxt,
					     struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state new;

	if (!ctxt->allow_unsafe_takeover)
		return -EPERM;

	/* Ensure caller is allowed to perform unsafe hostile takeovers. */
	if (WARN_ON_ONCE(ctxt->prio != NBCON_PRIO_PANIC))
		return -EPERM;

	/*
	 * Check that try_acquire_direct() and try_acquire_handover() returned
	 * -EBUSY in the right situation.
	 */
	WARN_ON_ONCE(ctxt->prio <= cur->prio || ctxt->prio <= cur->req_prio);
	WARN_ON_ONCE(cur->unsafe != true);

	do {
		new.atom = cur->atom;
		new.cpu			= cpu;
		new.prio		= ctxt->prio;
		new.unsafe		|= cur->unsafe_takeover;
		new.unsafe_takeover	|= cur->unsafe;

	} while (!nbcon_state_try_cmpxchg(con, cur, &new));

	return 0;
}

static struct printk_buffers panic_nbcon_pbufs;

/**
 * nbcon_context_try_acquire - Try to acquire nbcon console
 * @ctxt:	The context of the caller
 *
 * Context:	Under @ctxt->con->device_lock() or local_irq_save().
 * Return:	True if the console was acquired. False otherwise.
 *
 * If the caller allowed an unsafe hostile takeover, on success the
 * caller should check the current console state to see if it is
 * in an unsafe state. Otherwise, on success the caller may assume
 * the console is not in an unsafe state.
 */
static bool nbcon_context_try_acquire(struct nbcon_context *ctxt)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	int err;

	nbcon_state_read(con, &cur);
try_again:
	err = nbcon_context_try_acquire_direct(ctxt, &cur);
	if (err != -EBUSY)
		goto out;

	err = nbcon_context_try_acquire_handover(ctxt, &cur);
	if (err == -EAGAIN)
		goto try_again;
	if (err != -EBUSY)
		goto out;

	err = nbcon_context_try_acquire_hostile(ctxt, &cur);
out:
	if (err)
		return false;

	/* Acquire succeeded. */

	/* Assign the appropriate buffer for this context. */
	if (atomic_read(&panic_cpu) == cpu)
		ctxt->pbufs = &panic_nbcon_pbufs;
	else
		ctxt->pbufs = con->pbufs;

	/* Set the record sequence for this context to print. */
	ctxt->seq = nbcon_seq_read(ctxt->console);

	return true;
}

static bool nbcon_owner_matches(struct nbcon_state *cur, int expected_cpu,
				int expected_prio)
{
	/*
	 * Since consoles can only be acquired by higher priorities,
	 * owning contexts are uniquely identified by @prio. However,
	 * since contexts can unexpectedly lose ownership, it is
	 * possible that later another owner appears with the same
	 * priority. For this reason @cpu is also needed.
	 */

	if (cur->prio != expected_prio)
		return false;

	if (cur->cpu != expected_cpu)
		return false;

	return true;
}

/**
 * nbcon_context_release - Release the console
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 */
static void nbcon_context_release(struct nbcon_context *ctxt)
{
	unsigned int cpu = smp_processor_id();
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	struct nbcon_state new;

	nbcon_state_read(con, &cur);

	do {
		if (!nbcon_owner_matches(&cur, cpu, ctxt->prio))
			break;

		new.atom = cur.atom;
		new.prio = NBCON_PRIO_NONE;

		/*
		 * If @unsafe_takeover is set, it is kept set so that
		 * the state remains permanently unsafe.
		 */
		new.unsafe |= cur.unsafe_takeover;

	} while (!nbcon_state_try_cmpxchg(con, &cur, &new));

	ctxt->pbufs = NULL;
}

/**
 * nbcon_context_can_proceed - Check whether ownership can proceed
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 * @cur:	The current console state
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * Must be invoked when entering the unsafe state to make sure that it still
 * owns the lock. Also must be invoked when exiting the unsafe context
 * to eventually free the lock for a higher priority context which asked
 * for the friendly handover.
 *
 * It can be called inside an unsafe section when the console is just
 * temporary in safe state instead of exiting and entering the unsafe
 * state.
 *
 * Also it can be called in the safe context before doing an expensive
 * safe operation. It does not make sense to do the operation when
 * a higher priority context took the lock.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 */
static bool nbcon_context_can_proceed(struct nbcon_context *ctxt, struct nbcon_state *cur)
{
	unsigned int cpu = smp_processor_id();

	/* Make sure this context still owns the console. */
	if (!nbcon_owner_matches(cur, cpu, ctxt->prio))
		return false;

	/* The console owner can proceed if there is no waiter. */
	if (cur->req_prio == NBCON_PRIO_NONE)
		return true;

	/*
	 * A console owner within an unsafe region is always allowed to
	 * proceed, even if there are waiters. It can perform a handover
	 * when exiting the unsafe region. Otherwise the waiter will
	 * need to perform an unsafe hostile takeover.
	 */
	if (cur->unsafe)
		return true;

	/* Waiters always have higher priorities than owners. */
	WARN_ON_ONCE(cur->req_prio <= cur->prio);

	/*
	 * Having a safe point for take over and eventually a few
	 * duplicated characters or a full line is way better than a
	 * hostile takeover. Post processing can take care of the garbage.
	 * Release and hand over.
	 */
	nbcon_context_release(ctxt);

	/*
	 * It is not clear whether the waiter really took over ownership. The
	 * outermost callsite must make the final decision whether console
	 * ownership is needed for it to proceed. If yes, it must reacquire
	 * ownership (possibly hostile) before carefully proceeding.
	 *
	 * The calling context no longer owns the console so go back all the
	 * way instead of trying to implement reacquire heuristics in tons of
	 * places.
	 */
	return false;
}

/**
 * nbcon_can_proceed - Check whether ownership can proceed
 * @wctxt:	The write context that was handed to the write function
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * It is used in nbcon_enter_unsafe() to make sure that it still owns the
 * lock. Also it is used in nbcon_exit_unsafe() to eventually free the lock
 * for a higher priority context which asked for the friendly handover.
 *
 * It can be called inside an unsafe section when the console is just
 * temporary in safe state instead of exiting and entering the unsafe state.
 *
 * Also it can be called in the safe context before doing an expensive safe
 * operation. It does not make sense to do the operation when a higher
 * priority context took the lock.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 */
bool nbcon_can_proceed(struct nbcon_write_context *wctxt)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);
	struct console *con = ctxt->console;
	struct nbcon_state cur;

	nbcon_state_read(con, &cur);

	return nbcon_context_can_proceed(ctxt, &cur);
}
EXPORT_SYMBOL_GPL(nbcon_can_proceed);

#define nbcon_context_enter_unsafe(c)	__nbcon_context_update_unsafe(c, true)
#define nbcon_context_exit_unsafe(c)	__nbcon_context_update_unsafe(c, false)

/**
 * __nbcon_context_update_unsafe - Update the unsafe bit in @con->nbcon_state
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 * @unsafe:	The new value for the unsafe bit
 *
 * Return:	True if the unsafe state was updated and this context still
 *		owns the console. Otherwise false if ownership was handed
 *		over or taken.
 *
 * This function allows console owners to modify the unsafe status of the
 * console.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 *
 * Internal helper to avoid duplicated code.
 */
static bool __nbcon_context_update_unsafe(struct nbcon_context *ctxt, bool unsafe)
{
	struct console *con = ctxt->console;
	struct nbcon_state cur;
	struct nbcon_state new;

	nbcon_state_read(con, &cur);

	do {
		/*
		 * The unsafe bit must not be cleared if an
		 * unsafe hostile takeover has occurred.
		 */
		if (!unsafe && cur.unsafe_takeover)
			goto out;

		if (!nbcon_context_can_proceed(ctxt, &cur))
			return false;

		new.atom = cur.atom;
		new.unsafe = unsafe;
	} while (!nbcon_state_try_cmpxchg(con, &cur, &new));

	cur.atom = new.atom;
out:
	return nbcon_context_can_proceed(ctxt, &cur);
}

/**
 * nbcon_enter_unsafe - Enter an unsafe region in the driver
 * @wctxt:	The write context that was handed to the write function
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 */
bool nbcon_enter_unsafe(struct nbcon_write_context *wctxt)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);

	return nbcon_context_enter_unsafe(ctxt);
}
EXPORT_SYMBOL_GPL(nbcon_enter_unsafe);

/**
 * nbcon_exit_unsafe - Exit an unsafe region in the driver
 * @wctxt:	The write context that was handed to the write function
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context.
 */
bool nbcon_exit_unsafe(struct nbcon_write_context *wctxt)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);

	return nbcon_context_exit_unsafe(ctxt);
}
EXPORT_SYMBOL_GPL(nbcon_exit_unsafe);

/**
 * nbcon_reacquire - Reacquire a console after losing ownership while printing
 * @wctxt:	The write context that was handed to the write callback
 *
 * Since ownership can be lost at any time due to handover or takeover, a
 * printing context _must_ be prepared to back out immediately and
 * carefully. However, there are scenarios where the printing context must
 * reacquire ownership in order to finalize or revert hardware changes.
 *
 * This function allows a printing context to reacquire ownership using the
 * same priority as its previous ownership.
 *
 * Note that after a successful reacquire the printing context will have no
 * output buffer because that has been lost. This function cannot be used to
 * resume printing.
 */
void nbcon_reacquire(struct nbcon_write_context *wctxt)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);
	struct console *con = ctxt->console;
	struct nbcon_state cur;

	while (!nbcon_context_try_acquire(ctxt))
		cpu_relax();

	wctxt->outbuf = NULL;
	wctxt->len = 0;
	nbcon_state_read(con, &cur);
	wctxt->unsafe_takeover = cur.unsafe_takeover;
}
EXPORT_SYMBOL_GPL(nbcon_reacquire);

/**
 * nbcon_emit_next_record - Emit a record in the acquired context
 * @wctxt:	The write context that will be handed to the write function
 * @use_atomic:	True if the write_atomic() callback is to be used
 *
 * Return:	True if this context still owns the console. False if
 *		ownership was handed over or taken.
 *
 * When this function returns false then the calling context no longer owns
 * the console and is no longer allowed to go forward. In this case it must
 * back out immediately and carefully. The buffer content is also no longer
 * trusted since it no longer belongs to the calling context. If the caller
 * wants to do more it must reacquire the console first.
 *
 * When true is returned, @wctxt->ctxt.backlog indicates whether there are
 * still records pending in the ringbuffer,
 */
static bool nbcon_emit_next_record(struct nbcon_write_context *wctxt, bool use_atomic)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);
	struct console *con = ctxt->console;
	bool is_extended = console_srcu_read_flags(con) & CON_EXTENDED;
	struct printk_message pmsg = {
		.pbufs = ctxt->pbufs,
	};
	unsigned long con_dropped;
	struct nbcon_state cur;
	unsigned long dropped;
	unsigned long ulseq;

	/*
	 * The printk buffers are filled within an unsafe section. This
	 * prevents NBCON_PRIO_NORMAL and NBCON_PRIO_EMERGENCY from
	 * clobbering each other.
	 */

	if (!nbcon_context_enter_unsafe(ctxt))
		return false;

	ctxt->backlog = printk_get_next_message(&pmsg, ctxt->seq, is_extended, true);
	if (!ctxt->backlog)
		return nbcon_context_exit_unsafe(ctxt);

	/*
	 * @con->dropped is not protected in case of an unsafe hostile
	 * takeover. In that situation the update can be racy so
	 * annotate it accordingly.
	 */
	con_dropped = data_race(READ_ONCE(con->dropped));

	dropped = con_dropped + pmsg.dropped;
	if (dropped && !is_extended)
		console_prepend_dropped(&pmsg, dropped);

	/*
	 * If the previous owner was assigned the same record, this context
	 * has taken over ownership and is replaying the record. Prepend a
	 * message to let the user know the record is replayed.
	 */
	ulseq = atomic_long_read(&ACCESS_PRIVATE(con, nbcon_prev_seq));
	if (__ulseq_to_u64seq(prb, ulseq) == pmsg.seq) {
		console_prepend_replay(&pmsg);
	} else {
		/*
		 * Ensure this context is still the owner before trying to
		 * update @nbcon_prev_seq. Otherwise the value in @ulseq may
		 * not be from the previous owner.
		 */
		nbcon_state_read(con, &cur);
		if (!nbcon_context_can_proceed(ctxt, &cur))
			return false;

		atomic_long_try_cmpxchg(&ACCESS_PRIVATE(con, nbcon_prev_seq), &ulseq,
					__u64seq_to_ulseq(pmsg.seq));
	}

	if (!nbcon_context_exit_unsafe(ctxt))
		return false;

	/* For skipped records just update seq/dropped in @con. */
	if (pmsg.outbuf_len == 0)
		goto update_con;

	/* Initialize the write context for driver callbacks. */
	wctxt->outbuf = &pmsg.pbufs->outbuf[0];
	wctxt->len = pmsg.outbuf_len;
	nbcon_state_read(con, &cur);
	wctxt->unsafe_takeover = cur.unsafe_takeover;

	if (use_atomic &&
	    con->write_atomic) {
		con->write_atomic(con, wctxt);

	} else if (!use_atomic &&
		   con->write_thread) {
		con->write_thread(con, wctxt);

	} else {
		/*
		 * This function should never be called for legacy consoles.
		 * Handle it as if ownership was lost and try to continue.
		 */
		WARN_ON_ONCE(1);
		nbcon_context_release(ctxt);
		return false;
	}

	if (!wctxt->outbuf) {
		/*
		 * Ownership was lost and reacquired by the driver.
		 * Handle it as if ownership was lost.
		 */
		nbcon_context_release(ctxt);
		return false;
	}

	/*
	 * Since any dropped message was successfully output, reset the
	 * dropped count for the console.
	 */
	dropped = 0;
update_con:
	/*
	 * The dropped count and the sequence number are updated within an
	 * unsafe section. This limits update races to the panic context and
	 * allows the panic context to win.
	 */

	if (!nbcon_context_enter_unsafe(ctxt))
		return false;

	if (dropped != con_dropped) {
		/* Counterpart to the READ_ONCE() above. */
		WRITE_ONCE(con->dropped, dropped);
	}

	nbcon_seq_try_update(ctxt, pmsg.seq + 1);

	return nbcon_context_exit_unsafe(ctxt);
}

/**
 * nbcon_kthread_should_wakeup - Check whether a printer thread should wakeup
 * @con:	Console to operate on
 * @ctxt:	The nbcon context from nbcon_context_try_acquire()
 *
 * Return:	True if the thread should shutdown or if the console is
 *		allowed to print and a record is available. False otherwise.
 *
 * After the thread wakes up, it must first check if it should shutdown before
 * attempting any printing.
 */
static bool nbcon_kthread_should_wakeup(struct console *con, struct nbcon_context *ctxt)
{
	bool ret = false;
	short flags;
	int cookie;

	if (kthread_should_stop())
		return true;

	cookie = console_srcu_read_lock();

	flags = console_srcu_read_flags(con);
	if (console_is_usable(con, flags, false)) {
		/* Bring the sequence in @ctxt up to date */
		ctxt->seq = nbcon_seq_read(con);

		ret = prb_read_valid(prb, ctxt->seq, NULL);
	}

	console_srcu_read_unlock(cookie);
	return ret;
}

/**
 * nbcon_kthread_func - The printer thread function
 * @__console:	Console to operate on
 *
 * Return:	0
 */
static int nbcon_kthread_func(void *__console)
{
	struct console *con = __console;
	struct nbcon_write_context wctxt = {
		.ctxt.console	= con,
		.ctxt.prio	= NBCON_PRIO_NORMAL,
	};
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(&wctxt, ctxt);
	short con_flags;
	bool backlog;
	int cookie;
	int ret;

wait_for_event:
	/*
	 * Guarantee this task is visible on the rcuwait before
	 * checking the wake condition.
	 *
	 * The full memory barrier within set_current_state() of
	 * ___rcuwait_wait_event() pairs with the full memory
	 * barrier within rcuwait_has_sleeper().
	 *
	 * This pairs with rcuwait_has_sleeper:A and nbcon_kthread_wake:A.
	 */
	ret = rcuwait_wait_event(&con->rcuwait,
				 nbcon_kthread_should_wakeup(con, ctxt),
				 TASK_INTERRUPTIBLE); /* LMM(nbcon_kthread_func:A) */

	if (kthread_should_stop())
		return 0;

	/* Wait was interrupted by a spurious signal, go back to sleep. */
	if (ret)
		goto wait_for_event;

	do {
		backlog = false;

		cookie = console_srcu_read_lock();

		con_flags = console_srcu_read_flags(con);

		if (console_is_usable(con, con_flags, false)) {
			unsigned long lock_flags;

			con->device_lock(con, &lock_flags);

			/*
			 * Ensure this stays on the CPU to make handover and
			 * takeover possible.
			 */
			cant_migrate();

			if (nbcon_context_try_acquire(ctxt)) {
				/*
				 * If the emit fails, this context is no
				 * longer the owner.
				 */
				if (nbcon_emit_next_record(&wctxt, false)) {
					nbcon_context_release(ctxt);
					backlog = ctxt->backlog;
				}
			}

			con->device_unlock(con, lock_flags);
		}

		console_srcu_read_unlock(cookie);

	} while (backlog);

	goto wait_for_event;
}

/**
 * nbcon_irq_work - irq work to wake printk thread
 * @irq_work:	The irq work to operate on
 */
static void nbcon_irq_work(struct irq_work *irq_work)
{
	struct console *con = container_of(irq_work, struct console, irq_work);

	nbcon_kthread_wake(con);
}

static inline bool rcuwait_has_sleeper(struct rcuwait *w)
{
	bool has_sleeper;

	rcu_read_lock();
	/*
	 * Guarantee any new records can be seen by tasks preparing to wait
	 * before this context checks if the rcuwait is empty.
	 *
	 * This full memory barrier pairs with the full memory barrier within
	 * set_current_state() of ___rcuwait_wait_event(), which is called
	 * after prepare_to_rcuwait() adds the waiter but before it has
	 * checked the wait condition.
	 *
	 * This pairs with nbcon_kthread_func:A.
	 */
	smp_mb(); /* LMM(rcuwait_has_sleeper:A) */
	has_sleeper = !!rcu_dereference(w->task);
	rcu_read_unlock();

	return has_sleeper;
}

/**
 * nbcon_wake_threads - Wake up printing threads using irq_work
 */
void nbcon_wake_threads(void)
{
	struct console *con;
	int cookie;

	cookie = console_srcu_read_lock();
	for_each_console_srcu(con) {
		/*
		 * Only schedule irq_work if the printing thread is
		 * actively waiting. If not waiting, the thread will
		 * notice by itself that it has work to do.
		 */
		if (con->kthread && rcuwait_has_sleeper(&con->rcuwait))
			irq_work_queue(&con->irq_work);
	}
	console_srcu_read_unlock(cookie);
}

/* Track the nbcon emergency nesting per CPU. */
static DEFINE_PER_CPU(unsigned int, nbcon_pcpu_emergency_nesting);
static unsigned int early_nbcon_pcpu_emergency_nesting __initdata;

/**
 * nbcon_get_cpu_emergency_nesting - Get the per CPU emergency nesting pointer
 *
 * Return:	Either a pointer to the per CPU emergency nesting counter of
 *		the current CPU or to the init data during early boot.
 */
static __ref unsigned int *nbcon_get_cpu_emergency_nesting(void)
{
	/*
	 * The value of __printk_percpu_data_ready gets set in normal
	 * context and before SMP initialization. As a result it could
	 * never change while inside an nbcon emergency section.
	 */
	if (!printk_percpu_data_ready())
		return &early_nbcon_pcpu_emergency_nesting;

	return this_cpu_ptr(&nbcon_pcpu_emergency_nesting);
}

/**
 * nbcon_get_default_prio - The appropriate nbcon priority to use for nbcon
 *				printing on the current CPU
 *
 * Context:	Any context which could not be migrated to another CPU.
 * Return:	The nbcon_prio to use for acquiring an nbcon console in this
 *		context for printing.
 */
enum nbcon_prio nbcon_get_default_prio(void)
{
	unsigned int *cpu_emergency_nesting;

	if (this_cpu_in_panic())
		return NBCON_PRIO_PANIC;

	cpu_emergency_nesting = nbcon_get_cpu_emergency_nesting();
	if (*cpu_emergency_nesting)
		return NBCON_PRIO_EMERGENCY;

	return NBCON_PRIO_NORMAL;
}

/*
 * nbcon_emit_one - Print one record for an nbcon console using the
 *			specified callback
 * @wctxt:	An initialized write context struct to use for this context
 * @use_atomic:	True if the write_atomic() callback is to be used
 *
 * Return:	True, when a record has been printed and there are still
 *		pending records. The caller might want to continue flushing.
 *
 *		False, when there is no pending record, or when the console
 *		context cannot be acquired, or the ownership has been lost.
 *		The caller should give up. Either the job is done, cannot be
 *		done, or will be handled by the owning context.
 *
 * This is an internal helper to handle the locking of the console before
 * calling nbcon_emit_next_record().
 */
static bool nbcon_emit_one(struct nbcon_write_context *wctxt, bool use_atomic)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(wctxt, ctxt);

	if (!nbcon_context_try_acquire(ctxt))
		return false;

	/*
	 * nbcon_emit_next_record() returns false when the console was
	 * handed over or taken over. In both cases the context is no
	 * longer valid.
	 *
	 * The higher priority printing context takes over responsibility
	 * to print the pending records.
	 */
	if (!nbcon_emit_next_record(wctxt, use_atomic))
		return false;

	nbcon_context_release(ctxt);

	return ctxt->backlog;
}

/**
 * nbcon_legacy_emit_next_record - Print one record for an nbcon console
 *					in legacy contexts
 * @con:	The console to print on
 * @handover:	Will be set to true if a printk waiter has taken over the
 *		console_lock, in which case the caller is no longer holding
 *		both the console_lock and the SRCU read lock. Otherwise it
 *		is set to false.
 * @cookie:	The cookie from the SRCU read lock.
 * @use_atomic:	True if the write_atomic() callback is to be used
 *
 * Context:	Any context except NMI.
 * Return:	True, when a record has been printed and there are still
 *		pending records. The caller might want to continue flushing.
 *
 *		False, when there is no pending record, or when the console
 *		context cannot be acquired, or the ownership has been lost.
 *		The caller should give up. Either the job is done, cannot be
 *		done, or will be handled by the owning context.
 *
 * This function is meant to be called by console_flush_all() to print records
 * on nbcon consoles from legacy context (printing via console unlocking).
 * Essentially it is the nbcon version of console_emit_next_record().
 */
bool nbcon_legacy_emit_next_record(struct console *con, bool *handover,
				   int cookie, bool use_atomic)
{
	struct nbcon_write_context wctxt = { };
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(&wctxt, ctxt);
	unsigned long flags;
	bool progress;

	ctxt->console = con;

	if (use_atomic) {
		/* Use the same procedure as console_emit_next_record(). */
		printk_safe_enter_irqsave(flags);
		console_lock_spinning_enable();
		stop_critical_timings();

		ctxt->prio = nbcon_get_default_prio();
		progress = nbcon_emit_one(&wctxt, use_atomic);

		start_critical_timings();
		*handover = console_lock_spinning_disable_and_check(cookie);
		printk_safe_exit_irqrestore(flags);
	} else {
		*handover = false;

		con->device_lock(con, &flags);
		cant_migrate();

		ctxt->prio = nbcon_get_default_prio();
		progress = nbcon_emit_one(&wctxt, use_atomic);

		con->device_unlock(con, flags);
	}

	return progress;
}

/**
 * __nbcon_atomic_flush_pending_con - Flush specified nbcon console using its
 *					write_atomic() callback
 * @con:			The nbcon console to flush
 * @stop_seq:			Flush up until this record
 * @allow_unsafe_takeover:	True, to allow unsafe hostile takeovers
 *
 * Return:	0 if @con was flushed up to @stop_seq Otherwise, error code on
 *		failure.
 *
 * Errors:
 *
 *	-EPERM:		Unable to acquire console ownership.
 *
 *	-EAGAIN:	Another context took over ownership while printing.
 *
 *	-ENOENT:	A record before @stop_seq is not available.
 *
 * If flushing up to @stop_seq was not successful, it only makes sense for the
 * caller to try again when -EAGAIN was returned. When -EPERM is returned,
 * this context is not allowed to acquire the console. When -ENOENT is
 * returned, it cannot be expected that the unfinalized record will become
 * available.
 */
static int __nbcon_atomic_flush_pending_con(struct console *con, u64 stop_seq,
					    bool allow_unsafe_takeover)
{
	struct nbcon_write_context wctxt = { };
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(&wctxt, ctxt);
	int err = 0;

	ctxt->console			= con;
	ctxt->spinwait_max_us		= 2000;
	ctxt->prio			= nbcon_get_default_prio();
	ctxt->allow_unsafe_takeover	= allow_unsafe_takeover;

	if (!nbcon_context_try_acquire(ctxt))
		return -EPERM;

	while (nbcon_seq_read(con) < stop_seq) {
		/*
		 * nbcon_emit_next_record() returns false when the console was
		 * handed over or taken over. In both cases the context is no
		 * longer valid.
		 */
		if (!nbcon_emit_next_record(&wctxt, true))
			return -EAGAIN;

		if (!ctxt->backlog) {
			/* Are there reserved but not yet finalized records? */
			if (nbcon_seq_read(con) < stop_seq)
				err = -ENOENT;
			break;
		}
	}

	nbcon_context_release(ctxt);
	return err;
}

/**
 * nbcon_atomic_flush_pending_con - Flush specified nbcon console using its
 *					write_atomic() callback
 * @con:			The nbcon console to flush
 * @stop_seq:			Flush up until this record
 * @allow_unsafe_takeover:	True, to allow unsafe hostile takeovers
 *
 * This will stop flushing before @stop_seq if another context has ownership.
 * That context is then responsible for the flushing. Likewise, if new records
 * are added while this context was flushing and there is no other context
 * to handle the printing, this context must also flush those records.
 */
static void nbcon_atomic_flush_pending_con(struct console *con, u64 stop_seq,
					   bool allow_unsafe_takeover)
{
	unsigned long flags;
	int err;

again:
	/*
	 * Atomic flushing does not use console driver synchronization (i.e.
	 * it does not hold the port lock for uart consoles). Therefore IRQs
	 * must be disabled to avoid being interrupted and then calling into
	 * a driver that will deadlock trying to acquire console ownership.
	 */
	local_irq_save(flags);

	err = __nbcon_atomic_flush_pending_con(con, stop_seq, allow_unsafe_takeover);

	local_irq_restore(flags);

	/*
	 * If there was a new owner (-EPERM, -EAGAIN), that context is
	 * responsible for completing.
	 *
	 * Do not wait for records not yet finalized (-ENOENT) to avoid a
	 * possible deadlock. They will either get flushed by the writer or
	 * eventually skipped on panic CPU.
	 */
	if (err)
		return;

	/*
	 * If flushing was successful but more records are available, this
	 * context must flush those remaining records if the printer thread
	 * is not available do it.
	 */
	if ((!con->kthread || (system_state > SYSTEM_RUNNING)) &&
	    prb_read_valid(prb, nbcon_seq_read(con), NULL)) {
		stop_seq = prb_next_reserve_seq(prb);
		goto again;
	}
}

/**
 * __nbcon_atomic_flush_pending - Flush all nbcon consoles using their
 *					write_atomic() callback
 * @stop_seq:			Flush up until this record
 * @allow_unsafe_takeover:	True, to allow unsafe hostile takeovers
 */
static void __nbcon_atomic_flush_pending(u64 stop_seq, bool allow_unsafe_takeover)
{
	struct console *con;
	int cookie;

	cookie = console_srcu_read_lock();
	for_each_console_srcu(con) {
		short flags = console_srcu_read_flags(con);

		if (!(flags & CON_NBCON))
			continue;

		if (!console_is_usable(con, flags, true))
			continue;

		if (nbcon_seq_read(con) >= stop_seq)
			continue;

		nbcon_atomic_flush_pending_con(con, stop_seq, allow_unsafe_takeover);
	}
	console_srcu_read_unlock(cookie);
}

/**
 * nbcon_atomic_flush_pending - Flush all nbcon consoles using their
 *				write_atomic() callback
 *
 * Flush the backlog up through the currently newest record. Any new
 * records added while flushing will not be flushed. This is to avoid
 * one CPU printing unbounded because other CPUs continue to add records.
 */
void nbcon_atomic_flush_pending(void)
{
	__nbcon_atomic_flush_pending(prb_next_reserve_seq(prb), false);
}

/**
 * nbcon_atomic_flush_unsafe - Flush all nbcon consoles using their
 *	write_atomic() callback and allowing unsafe hostile takeovers
 *
 * Flush the backlog up through the currently newest record. Unsafe hostile
 * takeovers will be performed, if necessary.
 */
void nbcon_atomic_flush_unsafe(void)
{
	__nbcon_atomic_flush_pending(prb_next_reserve_seq(prb), true);
}

/**
 * nbcon_cpu_emergency_enter - Enter an emergency section where printk()
 *				messages for that CPU are only stored
 *
 * Upon exiting the emergency section, all stored messages are flushed.
 *
 * Context:	Any context. Disables preemption.
 *
 * When within an emergency section, no printing occurs on that CPU. This
 * is to allow all emergency messages to be dumped into the ringbuffer before
 * flushing the ringbuffer. The actual printing occurs when exiting the
 * outermost emergency section.
 */
void nbcon_cpu_emergency_enter(void)
{
	unsigned int *cpu_emergency_nesting;

	preempt_disable();

	cpu_emergency_nesting = nbcon_get_cpu_emergency_nesting();
	(*cpu_emergency_nesting)++;
}

/**
 * nbcon_cpu_emergency_exit - Exit an emergency section and flush the
 *				stored messages
 *
 * Flushing only occurs when exiting all nesting for the CPU.
 *
 * Context:	Any context. Enables preemption.
 */
void nbcon_cpu_emergency_exit(void)
{
	unsigned int *cpu_emergency_nesting;
	bool do_trigger_flush = false;

	cpu_emergency_nesting = nbcon_get_cpu_emergency_nesting();

	/*
	 * Flush the messages before enabling preemtion to see them ASAP.
	 *
	 * Reduce the risk of potential softlockup by using the
	 * flush_pending() variant which ignores messages added later. It is
	 * called before decrementing the counter so that the printing context
	 * for the emergency messages is NBCON_PRIO_EMERGENCY.
	 */
	if (*cpu_emergency_nesting == 1) {
		nbcon_atomic_flush_pending();

		/*
		 * Safely attempt to flush the legacy consoles in this
		 * context. Otherwise an irq_work context is triggered
		 * to handle it.
		 */
		do_trigger_flush = true;
		if (!force_printkthreads() &&
		    printing_via_unlock &&
		    !is_printk_legacy_deferred()) {
			if (console_trylock()) {
				do_trigger_flush = false;
				console_unlock();
			}
		}
	}

	if (!WARN_ON_ONCE(*cpu_emergency_nesting == 0))
		(*cpu_emergency_nesting)--;

	preempt_enable();

	if (do_trigger_flush)
		printk_trigger_flush();
}

/**
 * nbcon_cpu_emergency_flush - Explicitly flush consoles while
 *				within emergency context
 *
 * Both nbcon and legacy consoles are flushed.
 *
 * It should be used only when there are too many messages printed
 * in emergency context, for example, printing backtraces of all
 * CPUs or processes. It is typically needed when the watchdogs
 * need to be touched as well.
 */
void nbcon_cpu_emergency_flush(void)
{
	bool is_emergency;

	/*
	 * If this context is not an emergency context, preemption might be
	 * enabled. To be sure, disable preemption when checking if this is
	 * an emergency context.
	 */
	preempt_disable();
	is_emergency = (*nbcon_get_cpu_emergency_nesting() != 0);
	preempt_enable();

	/* The explicit flush is needed only in the emergency context. */
	if (!is_emergency)
		return;

	nbcon_atomic_flush_pending();

	if (!force_printkthreads() &&
	    printing_via_unlock &&
	    !is_printk_legacy_deferred()) {
		if (console_trylock())
			console_unlock();
	}
}

/*
 * nbcon_kthread_stop - Stop a printer thread
 * @con:	Console to operate on
 */
static void nbcon_kthread_stop(struct console *con)
{
	lockdep_assert_console_list_lock_held();

	if (!con->kthread)
		return;

	kthread_stop(con->kthread);
	con->kthread = NULL;
}

/**
 * nbcon_kthread_create - Create a printer thread
 * @con:	Console to operate on
 *
 * If it fails, let the console proceed. The atomic part might
 * be usable and useful.
 */
void nbcon_kthread_create(struct console *con)
{
	struct task_struct *kt;

	lockdep_assert_console_list_lock_held();

	if (!(con->flags & CON_NBCON) || !con->write_thread)
		return;

	if (!printk_threads_enabled || con->kthread)
		return;

	/*
	 * Printer threads cannot be started as long as any boot console is
	 * registered because there is no way to synchronize the hardware
	 * registers between boot console code and regular console code.
	 */
	if (have_boot_console)
		return;

	kt = kthread_run(nbcon_kthread_func, con, "pr/%s%d", con->name, con->index);
	if (IS_ERR(kt)) {
		con_printk(KERN_ERR, con, "failed to start printing thread\n");
		return;
	}

	con->kthread = kt;

	/*
	 * It is important that console printing threads are scheduled
	 * shortly after a printk call and with generous runtime budgets.
	 */
	sched_set_normal(con->kthread, -20);
}

static int __init printk_setup_threads(void)
{
	struct console *con;

	console_list_lock();
	printk_threads_enabled = true;
	for_each_console(con)
		nbcon_kthread_create(con);
	if (force_printkthreads() && printing_via_unlock)
		nbcon_legacy_kthread_create();
	console_list_unlock();
	return 0;
}
early_initcall(printk_setup_threads);

/**
 * nbcon_alloc - Allocate buffers needed by the nbcon console
 * @con:	Console to allocate buffers for
 *
 * Return:	True on success. False otherwise and the console cannot
 *		be used.
 *
 * This is not part of nbcon_init() because buffer allocation must
 * be performed earlier in the console registration process.
 */
bool nbcon_alloc(struct console *con)
{
	if (con->flags & CON_BOOT) {
		/*
		 * Boot console printing is synchronized with legacy console
		 * printing, so boot consoles can share the same global printk
		 * buffers.
		 */
		con->pbufs = &printk_shared_pbufs;
	} else {
		con->pbufs = kmalloc(sizeof(*con->pbufs), GFP_KERNEL);
		if (!con->pbufs) {
			con_printk(KERN_ERR, con, "failed to allocate printing buffer\n");
			return false;
		}
	}

	return true;
}

/**
 * nbcon_init - Initialize the nbcon console specific data
 * @con:	Console to initialize
 * @init_seq:	Sequence number of the first record to be emitted
 *
 * nbcon_alloc() *must* be called and succeed before this function
 * is called.
 */
void nbcon_init(struct console *con, u64 init_seq)
{
	struct nbcon_state state = { };

	/* nbcon_alloc() must have been called and successful! */
	BUG_ON(!con->pbufs);

	rcuwait_init(&con->rcuwait);
	init_irq_work(&con->irq_work, nbcon_irq_work);
	nbcon_seq_force(con, init_seq);
	atomic_long_set(&ACCESS_PRIVATE(con, nbcon_prev_seq), -1UL);
	nbcon_state_set(con, &state);
	nbcon_kthread_create(con);
}

/**
 * nbcon_free - Free and cleanup the nbcon console specific data
 * @con:	Console to free/cleanup nbcon data
 */
void nbcon_free(struct console *con)
{
	struct nbcon_state state = { };

	nbcon_kthread_stop(con);
	nbcon_state_set(con, &state);

	/* Boot consoles share global printk buffers. */
	if (!(con->flags & CON_BOOT))
		kfree(con->pbufs);

	con->pbufs = NULL;
}

/**
 * nbcon_device_try_acquire - Try to acquire nbcon console and enter unsafe
 *				section
 * @con:	The nbcon console to acquire
 *
 * Context:	Under the locking mechanism implemented in
 *		@con->device_lock() including disabling migration.
 * Return:	True if the console was acquired. False otherwise.
 *
 * Console drivers will usually use their own internal synchronization
 * mechasism to synchronize between console printing and non-printing
 * activities (such as setting baud rates). However, nbcon console drivers
 * supporting atomic consoles may also want to mark unsafe sections when
 * performing non-printing activities in order to synchronize against their
 * atomic_write() callback.
 *
 * This function acquires the nbcon console using priority NBCON_PRIO_NORMAL
 * and marks it unsafe for handover/takeover.
 */
bool nbcon_device_try_acquire(struct console *con)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(con, nbcon_device_ctxt);

	cant_migrate();

	memset(ctxt, 0, sizeof(*ctxt));
	ctxt->console	= con;
	ctxt->prio	= NBCON_PRIO_NORMAL;

	if (!nbcon_context_try_acquire(ctxt))
		return false;

	if (!nbcon_context_enter_unsafe(ctxt))
		return false;

	return true;
}
EXPORT_SYMBOL_GPL(nbcon_device_try_acquire);

/**
 * nbcon_device_release - Exit unsafe section and release the nbcon console
 * @con:	The nbcon console acquired in nbcon_device_try_acquire()
 */
void nbcon_device_release(struct console *con)
{
	struct nbcon_context *ctxt = &ACCESS_PRIVATE(con, nbcon_device_ctxt);
	int cookie;

	if (!nbcon_context_exit_unsafe(ctxt))
		return;

	nbcon_context_release(ctxt);

	/*
	 * This context must flush any new records added while the console
	 * was locked. The console_srcu_read_lock must be taken to ensure
	 * the console is usable throughout flushing.
	 */
	cookie = console_srcu_read_lock();
	if (console_is_usable(con, console_srcu_read_flags(con), true) &&
	    (!con->kthread || (system_state > SYSTEM_RUNNING)) &&
	    prb_read_valid(prb, nbcon_seq_read(con), NULL)) {
		__nbcon_atomic_flush_pending_con(con, prb_next_reserve_seq(prb), false);
	}
	console_srcu_read_unlock(cookie);
}
EXPORT_SYMBOL_GPL(nbcon_device_release);

/**
 * printk_kthread_shutdown - shutdown all threaded printers
 *
 * On system shutdown all threaded printers are stopped. This allows printk
 * to transition back to atomic printing, thus providing a robust mechanism
 * for the final shutdown/reboot messages to be output.
 */
static void printk_kthread_shutdown(void)
{
	struct console *con;

	console_list_lock();
	for_each_console(con) {
		if (con->flags & CON_NBCON)
			nbcon_kthread_stop(con);
	}
	console_list_unlock();
}

static struct syscore_ops printk_syscore_ops = {
	.shutdown = printk_kthread_shutdown,
};

static int __init printk_init_ops(void)
{
	register_syscore_ops(&printk_syscore_ops);
	return 0;
}
device_initcall(printk_init_ops);
