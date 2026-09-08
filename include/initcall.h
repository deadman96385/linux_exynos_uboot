/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2011 The Chromium OS Authors.
 */

#ifndef __INITCALL_H
#define __INITCALL_H

#include <asm/types.h>
#include <event.h>
#include <hang.h>

_Static_assert(EVT_COUNT < 256, "Can only support 256 event types with 8 bits");

extern int exynos_initcall_step;
extern const char *exynos_current_initcall;
extern void exynos_report_initcall_step(const char *name, int step);
extern void exynos_report_initcall_fail(const char *name, int step, int ret);

#define INITCALL(_call) \
	do { \
		int __s = exynos_initcall_step++; \
		exynos_report_initcall_step(#_call, __s); \
		int __ret = _call(); \
		if (__ret) { \
			exynos_report_initcall_fail(#_call, __s, __ret); \
			printf("%s(): initcall %s() failed (ret=%d)\n", __func__, \
			       #_call, __ret); \
			hang(); \
		} \
	} while (0)

#define INITCALL_EVT(_evt) \
	do { \
		int __s = exynos_initcall_step++; \
		exynos_report_initcall_step(event_type_name(_evt), __s); \
		if (event_notify_null(_evt)) { \
			exynos_report_initcall_fail(event_type_name(_evt), __s, -1); \
			printf("%s(): event %d/%s failed\n", __func__, _evt, \
			       event_type_name(_evt)); \
			hang(); \
		} \
	} while (0)

#if defined(CONFIG_WATCHDOG) || defined(CONFIG_HW_WATCHDOG)
#define WATCHDOG_INIT() INITCALL(init_func_watchdog_init)
#define WATCHDOG_RESET() INITCALL(init_func_watchdog_reset)
#else
#define WATCHDOG_INIT()
#define WATCHDOG_RESET()
#endif

#endif
