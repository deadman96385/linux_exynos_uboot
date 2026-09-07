/* SPDX-License-Identifier: GPL-2.0+ */
/* Temporary, opt-in bring-up markers for the j7y17lte. */

#ifdef J7Y17LTE_EARLY_MARKERS
	.macro	j7y17lte_mark value
	/*
	 * The diagnostic wrapper creates a valid ramoops record and places its
	 * two-byte STAGE value at physical address 0x46e00040. Keep this macro
	 * stackless and position-independent so it is safe before crt0.
	 */
	movz	x11, #0x46e0, lsl #16
	movz	w12, #\value
	strh	w12, [x11, #0x40]
	dsb	sy
	.endm
#else
	.macro	j7y17lte_mark value
	.endm
#endif
