/**
 * @file
 * @brief
 *
 * @date 08.02.12
 * @author Anton Bulychev
 * @author Ilia Vaprol
 * @author Eldar Abusalimov
 */

#ifndef KERNEL_SPINLOCK_H_
#define KERNEL_SPINLOCK_H_

#include <assert.h>

#include <sys/types.h>
#include <linux/compiler.h>

#include <hal/cpu.h>
#include <hal/ipl.h>
#include <kernel/critical.h>
#include <module/embox/arch/libarch.h>

#include <util/lang.h>
#include <util/macro.h>

#define SPIN_CONTENTION_LIMIT 0x10000000

#ifdef SPIN_CONTENTION_LIMIT
# define __SPIN_CONTENTION_FIELD       unsigned int contention_count;
# define __SPIN_CONTENTION_FIELD_INIT .contention_count = SPIN_CONTENTION_LIMIT,
#else /* SPIN_CONTENTION_LIMIT */
# define __SPIN_CONTENTION_FIELD
# define __SPIN_CONTENTION_FIELD_INIT
#endif /* SPIN_CONTENTION_LIMIT */

typedef struct {
	unsigned long l;
	__SPIN_CONTENTION_FIELD
} spinlock_t;

#define SPIN_STATIC_UNLOCKED { .l = __SPIN_UNLOCKED, __SPIN_CONTENTION_FIELD_INIT }
#define SPIN_STATIC_LOCKED   { .l = __SPIN_LOCKED,   __SPIN_CONTENTION_FIELD_INIT }

#define SPIN_UNLOCKED (spinlock_t) SPIN_STATIC_UNLOCKED
#define SPIN_LOCKED   (spinlock_t) SPIN_STATIC_LOCKED

#define __SPIN_UNLOCKED 0
#define __SPIN_LOCKED   1

#ifdef SMP

static inline int __spin_trylock_smp(spinlock_t *lock) {
#ifdef __HAVE_ARCH_CMPXCHG
	return (__SPIN_UNLOCKED == cmpxchg(&lock->l, __SPIN_UNLOCKED, __SPIN_LOCKED));
#else /* !__HAVE_ARCH_CMPXCHG */
	return __sync_bool_compare_and_swap(&lock->l, __SPIN_UNLOCKED, __SPIN_LOCKED);
#endif /* __HAVE_ARCH_CMPXCHG */
}

#else /* !SMP */

static inline int __spin_trylock_smp(spinlock_t *lock) {
	return 1;
}

#endif /* SMP */

static inline int __spin_trylock(spinlock_t *lock) {
	int ret = __spin_trylock_smp(lock);
#ifdef SPIN_CONTENTION_LIMIT
	if (ret)
		lock->contention_count = SPIN_CONTENTION_LIMIT;
	else
		// TODO this must be atomic dec
		assert(lock->contention_count--, "deadlock");
#endif
	return ret;
}

static inline void __spin_lock(spinlock_t *lock) {
	while (!__spin_trylock(lock))
		;
}

static inline void __spin_unlock(spinlock_t *lock) {
#ifdef SMP
	*lock = SPIN_UNLOCKED;
#endif
	__barrier();
}

/**
 * spin_trylock -- try to lock object without waiting
 * @param lock  object to lock
 * @retval      1 if successfully blocked otherwise 1
 */
static inline int spin_trylock(spinlock_t *lock) {
	int ret;
	__critical_count_add(__CRITICAL_COUNT(CRITICAL_SCHED_LOCK));
	ret = __spin_trylock(lock);
	if (!ret)
		__critical_count_sub(__CRITICAL_COUNT(CRITICAL_SCHED_LOCK));
	return ret;
}

/**
 * spin_lock -- try to lock object or wait until it's will done
 * @param lock  object to lock
 */
static inline void spin_lock(spinlock_t *lock) {
	while (!spin_trylock(lock))
		;
}

/**
 * spin_unlock -- unlock blocked object
 * @param lock  object to unlock
 */
static inline void spin_unlock(spinlock_t *lock) {
	__spin_unlock(lock);
	__critical_count_sub(__CRITICAL_COUNT(CRITICAL_SCHED_LOCK));
}

static inline ipl_t spin_lock_ipl(spinlock_t *lock) {
	ipl_t ipl = 0;

	while (1) {
		ipl = ipl_save();
		if (spin_trylock(lock))
			break;
		ipl_restore(ipl);
	}

	return ipl;
}

static inline void spin_unlock_ipl(spinlock_t *lock, ipl_t ipl) {
	__spin_unlock(lock);
	ipl_restore(ipl);  /* implies optimization barrier */
	__critical_count_sub(__CRITICAL_COUNT(CRITICAL_SCHED_LOCK));
}

static inline void spin_lock_ipl_disable(spinlock_t *lock) {
	ipl_t ipl = 0;

	while (1) {
		ipl = ipl_save();
		if (spin_trylock(lock))
			break;
		ipl_restore(ipl);
	}
}

static inline void spin_unlock_ipl_enable(spinlock_t *lock) {
	__spin_unlock(lock);
	ipl_enable();  /* implies optimization barrier */
	__critical_count_sub(__CRITICAL_COUNT(CRITICAL_SCHED_LOCK));
}

/**
 * Spin until either @a lock is acquired or @a cond becomes @c false.
 * @return @a cond value. In case of a non-zero value the spin is locked.
 * Unlocking it in the latter case is up to the client.
 */
#define SPIN_LOCK_COND(lock, cond) \
	({                                       \
		spinlock_t *__lock = (lock);         \
		typeof(cond) __cond;                 \
		                                     \
		do {                                 \
			__cond = (cond);                 \
			if (!__cond)                     \
				break;                       \
		} while (!spin_trylock(__lock));     \
		                                     \
		if (__cond) {                        \
			/* just been locked */           \
			__cond = (cond);                 \
			if (!__cond)                     \
				spin_unlock(__lock);         \
		}                                    \
		                                     \
		__cond;                              \
	})

/**
 * 'if' statement based on #SPIN_LOCK_COND(). True branch gets executed with
 * a non-zero value of @a cond and spin locked. False branch gets no locks.
 * No explicit unlocking is required.
 * Making 'return' stmt inside the block will leave the spin locked.
 * Also 'break'/'continue' will not work as expected.
 */
#define spin_protected_if(lock, cond) \
	__spin_protected_if(lock, cond, \
		MACRO_GUARD(__done), \
		MACRO_GUARD(__lock), \
		MACRO_GUARD(__cond))

#define __spin_protected_if(lock, cond, __done, __lock, __cond) \
	for (int __done = 0;                              !__done; ) \
	for (spinlock_t *__lock = (lock);                 !__done; ) \
	for (int __cond = !!SPIN_LOCK_COND(__lock, cond); !__done;   \
			({ if (__cond) spin_unlock(__lock); }))              \
	while (!__done && (__done = 1)) /* break/continue control this loop */  \
		if (__cond)

#define SPIN_PROTECTED_DO(lock, expr) \
	__lang_surround(expr,             \
		spinlock_t *__lock = (lock);  \
		spin_lock(__lock),            \
		spin_unlock(__lock))

#define SPIN_IPL_PROTECTED_DO(lock, expr) \
	__lang_surround(expr,                    \
		spinlock_t *__lock = (lock);         \
		ipl_t __ipl = spin_lock_ipl(__lock), \
		spin_unlock_ipl(__lock, __ipl))

#endif /* !KERNEL_SPINLOCK_H_ */
