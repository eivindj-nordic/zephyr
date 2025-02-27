/*
 * Copyright (c) 2019 Carlo Caione <ccaione@baylibre.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ARM64 Cortex-A ISRs wrapper
 */

#include <toolchain.h>
#include <linker/sections.h>
#include <offsets_short.h>
#include <arch/cpu.h>
#include <sw_isr_table.h>
#include <drivers/interrupt_controller/gic.h>
#include "macro_priv.inc"

_ASM_FILE_PROLOGUE

GDATA(_sw_isr_table)

/*
 * Wrapper around ISRs when inserted in software ISR table
 *
 * When inserted in the vector table, _isr_wrapper() demuxes the ISR table
 * using the running interrupt number as the index, and invokes the registered
 * ISR with its corresponding argument. When returning from the ISR, it
 * determines if a context switch needs to happen.
 */

GTEXT(_isr_wrapper)
SECTION_FUNC(TEXT, _isr_wrapper)

	/* ++_current_cpu->nested to be checked by arch_is_in_isr() */
	get_cpu	x0
	ldr	w1, [x0, #___cpu_t_nested_OFFSET]
	add	w2, w1, #1
	str	w2, [x0, #___cpu_t_nested_OFFSET]

	/* If not nested: switch to IRQ stack and save current sp on it. */
	cbnz	w1, 1f
	ldr	x1, [x0, #___cpu_t_irq_stack_OFFSET]
	mov	x2, sp
	mov	sp, x1
	str	x2, [sp, #-16]!
1:
#ifdef CONFIG_SCHED_THREAD_USAGE
	bl	z_sched_usage_stop
#endif

#ifdef CONFIG_TRACING
	bl	sys_trace_isr_enter
#endif

	/* Get active IRQ number from the interrupt controller */
#if !defined(CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER)
	bl	arm_gic_get_active
#else
	bl	z_soc_irq_get_active
#endif /* !CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER */

#if CONFIG_GIC_VER >= 3
	/*
	 * Ignore Special INTIDs 1020..1023 see 2.2.1 of Arm Generic Interrupt Controller
	 * Architecture Specification GIC architecture version 3 and version 4
	 */
	cmp	x0, 1019
	b.le	oob
	cmp	x0, 1023
	b.gt	oob
	b	spurious_continue

oob:
#endif
	/* IRQ out of bounds */
	mov	x1, #(CONFIG_NUM_IRQS - 1)
	cmp	x0, x1
	b.hi	spurious_continue

	stp	x0, xzr, [sp, #-16]!

	/* Retrieve the interrupt service routine */
	ldr	x1, =_sw_isr_table
	add	x1, x1, x0, lsl #4	/* table is 16-byte wide */
	ldp	x0, x3, [x1] /* arg in x0, ISR in x3 */

	/*
	 * Call the ISR. Unmask and mask again the IRQs to support nested
	 * exception handlers
	 */
	msr	daifclr, #(DAIFCLR_IRQ_BIT)
	blr	x3
	msr	daifset, #(DAIFSET_IRQ_BIT)

	/* Signal end-of-interrupt */
	ldp	x0, xzr, [sp], #16

spurious_continue:
#if !defined(CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER)
	bl	arm_gic_eoi
#else
	bl	z_soc_irq_eoi
#endif /* !CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER */

#ifdef CONFIG_TRACING
	bl	sys_trace_isr_exit
#endif

GTEXT(z_arm64_irq_done)
z_arm64_irq_done:
	/* if (--_current_cpu->nested != 0) exit */
	get_cpu	x0
	ldr	w1, [x0, #___cpu_t_nested_OFFSET]
	subs	w1, w1, #1
	str	w1, [x0, #___cpu_t_nested_OFFSET]
	bne	exit

	/* No more nested: retrieve the task's stack. */
	ldr	x0, [sp]
	mov	sp, x0

	/*
	 * z_arch_get_next_switch_handle() is returning:
	 *
	 * - The next thread to schedule in x0
	 * - The current thread in x1. This value is returned using the
	 *   **old_thread parameter, so we need to make space on the stack for
	 *   that.
	 */
	sub	sp, sp, #16
	mov	x0, sp
	bl	z_arch_get_next_switch_handle
	ldp	x1, xzr, [sp], #16

	/*
	 * x0: 1st thread in the ready queue
	 * x1: _current thread
	 */

#ifdef CONFIG_SMP
	/*
	 * 2 possibilities here:
	 *  - x0 != NULL (implies x0 != x1): we need to context switch and set
	 *    the switch_handle in the context switch code
	 *  - x0 == NULL: no context switch
	 */
	cmp	x0, #0x0
	bne	switch

	/*
	 * No context switch. Restore x0 from x1 (they are the same thread).
	 * See also comments to z_arch_get_next_switch_handle()
	 */
	mov	x0, x1
	b	exit
switch:
#else
	cmp	x0, x1
	beq	exit
#endif
	/* Switch thread */
	bl	z_arm64_context_switch

exit:
#ifdef CONFIG_STACK_SENTINEL
	bl	z_check_stack_sentinel
#endif
	b	z_arm64_exit_exc

