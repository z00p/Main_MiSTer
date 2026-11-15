#ifndef PITREX_H
#define PITREX_H

// Vectrex-PiTrex: a program picked in the Vectrex core's OSD (a .um2 UVMC2 cartridge
// image or a .ptx PiTrex program) runs on the ARM as a child process of Main.
// Built as its own Main (main-pitrex), loaded with main= in the core's ini section.

// Input poll timeout (ms) while a program runs: Main sleeps in poll() and leaves
// CPU1 to the program. Input events still wake it at once.
#define PITREX_POLL_MS 2

int  is_pitrex();
int  pitrex_active();
int  pitrex_file_selected(const char *path); // 1: ours (.um2/.ptx), not sent to the core
void pitrex_poll();
void pitrex_stop();

// The core's OSD menu: Main's own lines, to build the UVMC2 card (pitrex.cpp). pitrex_confstr adds them to the core's conf
// string (our core only); pitrex_osd_option runs one when it is picked and returns 1 (0: not one of ours).
#include <stddef.h>
void pitrex_confstr(char *cfg, size_t size);
int  pitrex_osd_option(const char *opt);

// main-pitrex --um2run ...: run um2run instead of Main (pitrex_worker.cpp)
int  pitrex_worker_main(int argc, char *argv[]);

// main-pitrex --make-card [--if-needed | --check]: build the UVMC2 card instead of running Main (pitrex_card.cpp)
#include "pitrex_card.h"

#endif
