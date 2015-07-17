#ifndef __STM32_INTERRUPT_H__
#define __STM32_INTERRUPT_H__

#define __sei()		__asm__ __volatile__("cpsie i" ::: "memory")
#define __cli()		__asm__ __volatile__("cpsid i" ::: "memory")

#define __dmb()		__asm__ __volatile__("dmb" ::: "memory")
#define __dsb()		__asm__ __volatile__("dsb" ::: "memory")
#define __isb()		__asm__ __volatile__("isb" ::: "memory")

#define __irq_save(flag) \
	__asm__ __volatile__("mrs %0, primask" : "=r"(flag) :: "memory")
#define __irq_restore(flag) \
	__asm__ __volatile__("msr primask, %0" :: "r"(flag) : "memory")

#define ISR_REGISTER(vector_nr, func)	({ \
		extern unsigned int _ram_start; \
		*((unsigned int *)&_ram_start + vector_nr) = (unsigned int)func; \
		dmb(); \
	})
#define __register_isr(nirq, func)	ISR_REGISTER(nirq, func)
#define __get_active_irq()		(GET_PSR() & 0x1ff)

#define __SET_IRQ(on, irq_nr) ( \
		*(volatile unsigned int *)(NVIC_BASE + ((irq_nr) / 32 * 4)) = \
		MASK_RESET(*(volatile unsigned int *)(NVIC_BASE + ((irq_nr) / 32 * 4)), 1 << ((irq_nr) % 32)) \
		| ((on) << ((irq_nr) % 32)) \
	)
void SET_IRQ(int on, unsigned int irq_nr);

#define GET_PC() ({ unsigned int __pc; \
		__asm__ __volatile__("mov %0, pc" : "=r" (__pc)); \
		__pc; })
#define GET_SP() ({ unsigned int __sp; \
		__asm__ __volatile__("mov %0, sp" : "=r" (__sp)); \
		__sp; })
#define GET_KSP() ({ unsigned int __ksp; \
		__asm__ __volatile__("mrs %0, msp" : "=r" (__ksp)); \
		__ksp; })
#define GET_USP() ({ unsigned int __usp; \
		__asm__ __volatile__("mrs %0, psp" : "=r" (__usp)); \
		__usp; })
#define GET_PSR() ({ unsigned int __psr; \
		__asm__ __volatile__("mrs %0, psr" : "=r" (__psr)); \
		__psr; })
#define GET_LR() ({ unsigned int __lr; \
		__asm__ __volatile__("mov %0, lr" : "=r" (__lr)); \
		__lr; })
#define GET_INT() ({ unsigned int __primask; \
		__asm__ __volatile__("mrs %0, primask" : "=r" (__primask)); \
		__primask; })
#define GET_CON() ({ unsigned int __control; \
		__asm__ __volatile__("mrs %0, control" : "=r" (__control)); \
		__control; })
#define SET_SP(sp) __asm__ __volatile__("mov sp, %0" :: "r"(sp) : "memory")
#define SET_KSP(sp) __asm__ __volatile__("msr msp, %0" :: "r"(sp) : "memory")
#define SET_USP(sp) __asm__ __volatile__("msr psp, %0" :: "r"(sp) : "memory")
#define __setusp(sp)		SET_USP(sp)
#define __setksp(sp)		SET_KSP(sp)
#define __get_ret_addr()	GET_LR()
#define __getpc()		GET_PC()
#define __getusp()		GET_USP()
#define __getsp()		GET_SP()
#define __setsp(sp)		SET_SP(sp)

#define __nop()			__asm__ __volatile__("nop" ::: "memory")

#endif /* __STM32_INTERRUPT_H__ */