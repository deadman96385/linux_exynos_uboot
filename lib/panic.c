/*
 *  linux/lib/vsprintf.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/* vsprintf.c -- Lars Wirzenius & Linus Torvalds. */
/*
 * Wirzenius wrote this portably, Torvalds fucked it up :-)
 */

#include <hang.h>
#if !defined(CONFIG_PANIC_HANG)
#include <command.h>
#endif
#include <linux/delay.h>
#include <stdio.h>
#include <string.h>
#include <vsprintf.h>

static void panic_finish(void) __attribute__ ((noreturn));

char exynos_panic_buf[128] = {0};

static void panic_finish(void)
{
	extern const char *exynos_current_initcall;
	extern void exynos_draw_text(int x0, int y0, const char *str, unsigned int fg, unsigned int bg);

	exynos_draw_text(30, 600, "PANIC IN:", 0x00FF8000, 0x00000000);
	if (exynos_current_initcall)
		exynos_draw_text(30, 680, exynos_current_initcall, 0x00FF8000, 0x00000000);
	if (exynos_panic_buf[0])
		exynos_draw_text(30, 760, exynos_panic_buf, 0x00FF3333, 0x00000000);

	putc('\n');
#if defined(CONFIG_PANIC_HANG)
	hang();
#else
	flush();  /* flush the panic message before reset */

	do_reset(NULL, 0, 0, NULL);
#endif
	while (1)
		;
}

void panic_str(const char *str)
{
	puts(str);
	strncpy(exynos_panic_buf, str, sizeof(exynos_panic_buf) - 1);
	panic_finish();
}

void panic(const char *fmt, ...)
{
#if CONFIG_IS_ENABLED(PRINTF)
	va_list args;
	va_start(args, fmt);
	vsnprintf(exynos_panic_buf, sizeof(exynos_panic_buf), fmt, args);
	va_end(args);

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
#endif
	panic_finish();
}

void __assert_fail(const char *assertion, const char *file, unsigned int line,
		   const char *function)
{
	/* This will not return */
	panic("%s:%u: %s: Assertion `%s' failed.", file, line, function,
	      assertion);
}
