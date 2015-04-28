#include <time.h>
#include <foundation.h>

unsigned long volatile __attribute__((section(".data"))) jiffies;
uint64_t __attribute__((section(".data"))) jiffies_64;

DEFINE_SPINLOCK(lock_jiffies);

inline unsigned long long __get_jiffies_64()
{
	return jiffies_64;
}

unsigned long long get_jiffies_64()
{
	unsigned long long stamp;

	do {
		if (!is_spin_locked(lock_jiffies))
			stamp = __get_jiffies_64();
	} while (is_spin_locked(lock_jiffies));

	return stamp;
}

inline void update_tick(unsigned delta)
{
	/* In multi processor system, jiffies_64 is global, meaning it is
	 * accessed by all processors while others related to a scheduler
	 * are accessed by only its processor. */

	preempt_disable();
	spin_lock(lock_jiffies);

	jiffies_64 += delta;

	spin_unlock(lock_jiffies);
	preempt_enable();
}