#include <swilib.h>
#include "fwlib.h"

/* D80: the table the SDK's calls go through (__sys_switab_addres, set by the loader: ElfLoader 3 = the swi.blib
 * merged table via pLIB_TOP, missing = 0xFFFF0000 | n << 4). AddrLibrary() (D79) is the static flash Library
 * table without the swi.blib entries: it said "missing" on the phone with the new swi.blib. */
int lib_check(const uint16_t *nums, int n, int *bad)
{
	const uint32_t *lib = (const uint32_t *)__sys_switab_addres;
	int i;

	for (i = 0; i < n; i++) {
		uint32_t v = lib ? lib[nums[i] & 0xFFF] : 0xFFFFFFFFu;
		int ok = nums[i] & 0x8000 ? v >= 0xA8000000u && v < 0xA9000000u : v >= 0xA0000000u && v < 0xA2000000u;
		if (!ok) {
			*bad = nums[i];
			return 0;
		}
	}
	return 1;
}
