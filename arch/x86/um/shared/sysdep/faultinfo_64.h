/*
 * Copyright (C) 2004 Fujitsu Siemens Computers GmbH
 * Author: Bodo Stroesser <bstroesser@fujitsu-siemens.com>
 * Licensed under the GPL
 */

#ifndef __FAULTINFO_X86_64_H
#define __FAULTINFO_X86_64_H

/* this structure contains the full arch-specific faultinfo
 * from the traps.
 * On i386, ptrace_faultinfo unfortunately doesn't provide
 * all the info, since trap_no is missing.
 * All common elements are defined at the same position in
 * both structures, thus making it easy to copy the
 * contents without knowledge about the structure elements.
 */
struct faultinfo {
        int error_code; /* in ptrace_faultinfo misleadingly called is_write */
        unsigned long cr2; /* in ptrace_faultinfo called addr */
        int trap_no; /* missing in ptrace_faultinfo */
};

#define FAULT_WRITE(fi) ((fi).error_code & 2)
#define FAULT_ADDRESS(fi) ((fi).cr2)

/* This is Page Fault */
#define SEGV_IS_FIXABLE(fi)	((fi)->trap_no == 14)

#define PTRACE_FULL_FAULTINFO 1

#define ___backtrack_faulted(_faulted)					\
	asm volatile (							\
		"movq $__get_kernel_nofault_faulted_%=,%1\n"		\
		"mov $0, %0\n"						\
		"jmp _end_%=\n"						\
		"__get_kernel_nofault_faulted_%=:\n"			\
		"mov $1, %0;"						\
		"_end_%=:"						\
		: "=r" (_faulted),					\
		  "=m" (current->thread.segv_continue) ::		\
	)

#endif
