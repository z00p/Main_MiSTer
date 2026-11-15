#ifndef PITREX_CARD_H
#define PITREX_CARD_H

// Vectrex-PiTrex: the UVMC2 card builder (pitrex_card.cpp), Vectrex_MiSTer's make_card.sh in C++.
//
// "main-pitrex --make-card [--if-needed | --check]" builds the card as make_card.sh does, with the
// same messages, exit codes, files and test hooks (PITREX_FAT, PITREX_TMP, PITREX_ZERO_PRESET).
// It needs no python, bash, losetup or mount: only mkfs.vfat (and fsck.vfat, when there is one).
// Main runs it as a child process: for the card check before a game and from the OSD.

// mode: "", "--if-needed" or "--check". Returns the exit status.
int pitrex_card_main(const char *prog, const char *mode);

#endif
