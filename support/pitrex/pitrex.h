#ifndef PITREX_H
#define PITREX_H

// Vectrex-PiTrex: a program picked in the Vectrex core's OSD (a .um2 UVMC2 cartridge
// image or a .ptx PiTrex program) runs on the ARM as a child process of Main.
// Off unless the core runs with PITREX=1 in its ini section.

// Input poll timeout (ms) while a program runs: Main sleeps in poll() and leaves
// CPU1 to the program. Input events still wake it at once.
#define PITREX_POLL_MS 2

int  is_pitrex();
int  pitrex_active();
void pitrex_file_selected(const char *path);
void pitrex_poll();
void pitrex_stop();

#endif
