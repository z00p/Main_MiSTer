// Vectrex-PiTrex: um2run as a worker mode of this Main.
//
// "main-pitrex --um2run <um2run arguments>" runs um2run (support/pitrex/um2run, a copy of
// Vectrex_MiSTer's pitrex/um2run) instead of Main. pitrex.cpp starts it as a child process
// for a picked .um2 game, so nothing but this one binary has to be installed. main() hands
// over before any of Main's own start-up: the worker never touches the OSD, input or the
// FPGA as Main does, only what um2run itself uses.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#include "pitrex.h"

extern "C" int um2run_main(int argc, char **argv);

// um2run maps the cartridge's address space at fixed addresses with MAP_FIXED (um2run.c: PSRAM_BASE
// 0x11000000 and its mirror, IMG_BASE 0x20000000, STUB_BASE, the peripheral blocks at 0x40000000,
// 0x50000000 and 0x70001000, ROMCOPY_BASE 0x80000000), which would silently replace anything already
// mapped there. As its own static binary that space was always free; check it still is.
#define UM2_SPACE_LOW  0x10000000UL
#define UM2_SPACE_HIGH 0x80008000UL

static int space_free()
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return 1;

	int ok = 1;
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		unsigned long lo, hi;
		if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
		if (lo < UM2_SPACE_HIGH && hi > UM2_SPACE_LOW)
		{
			fprintf(stderr, "um2run: the guest address space %08lx-%08lx is in use: %s", UM2_SPACE_LOW, UM2_SPACE_HIGH, line);
			ok = 0;
		}
	}
	fclose(f);
	return ok;
}

int pitrex_worker_main(int argc, char *argv[])
{
	// named um2run, as before: pitrex_group_stop, pitrexd and killall find it by that name
	prctl(PR_SET_NAME, "um2run", 0, 0, 0);
	if (!space_free()) return 2;

	argv[1] = (char *)"um2run";
	return um2run_main(argc - 1, argv + 1);
}
