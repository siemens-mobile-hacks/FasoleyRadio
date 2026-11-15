/* Firmware functions the elf needs from the function library (ElfLoader / ElfPack): the new entries 4E1-4E9 and
 * 8471 (signatures/README.md) instead of the addresses that were hard-coded up to D72. */
#ifndef RADIO_FWLIB_H
#define RADIO_FWLIB_H
#include <stdint.h>

#define LIB_MISSING	"No functions 4E1-4E9 in the library: flash the new library"

/* every number in the library: a function entry points into flash, a RAM entry (0x8000 | n) into RAM.
 * 0 = one missing (*bad = its number; 0xFFFFFFFF, a stub elsewhere, an old library) */
int lib_check(const uint16_t *nums, int n, int *bad);

#endif
