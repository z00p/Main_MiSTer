#ifndef PITREX_LIB_H
#define PITREX_LIB_H

// Vectrex-PiTrex helpers that need nothing from Main (sidecar rules, card choice,
// child processes), so they can be tested on a PC. See pitrex.cpp.

#include <sys/types.h>
#include <stddef.h>

#define PITREX_KIND_UM2 1
#define PITREX_KIND_PTX 2

// Messages shown on the Vectrex ('|' = new line, 16 characters a line at most).
#define PITREX_MSG_ONE_CPU   "LINUX ON ONE CPU|REBOOT MISTER|THEN PICK AGAIN"
#define PITREX_MSG_NO_UM2RUN "UM2RUN MISSING|RUN PITREX|UPDATE"
#define PITREX_MSG_NO_FILE   "GAME FILE|NOT FOUND"
#define PITREX_MSG_NO_SCALE  "NO CARD FOR THIS GAME|RUN PITREX MAKE|UVMC2 CARD|FROM SCRIPTS"
#define PITREX_MSG_OLD_CARD  "CARD OUT OF DATE|RUN PITREX MAKE|UVMC2 CARD|FROM SCRIPTS"
#define PITREX_MSG_NO_START  "CANNOT START|PROGRAM"

struct pitrex_dirs
{
	char pitrex[1024];   // <storage>/pitrex
	char um2[1024];      // <storage>/pitrex/um2run: um2run, mister_run.sh
	char sd_dir[1024];   // <storage>/pitrex/uvmc2: card images
	const char *cpus;    // /sys/devices/system/cpu/online
};

// How a .um2 file is started: the sidecar <image>.um2.cfg read with pitrexd's rules.
struct pitrex_um2_plan
{
	char img[1024];
	char cfg[1040];
	char fc[32];          // --frame-cycles
	char sd[1024];        // --sd, "" = no card
	char sc[32];          // scale asked for ("" = none)
	char args[1024];      // sidecar args=, appended last
	char settle[32];      // SETTLE for mister_run.sh ("" = its default)
	char cpu1_after[32];  // CPU1_AFTER
	char orient[32];      // sidecar orient= / view= as written
	char view[32];
	int scale_card_missing; // say PITREX_MSG_NO_SCALE for a moment first
	int check_card;         // the card is a made one: ask make_card.sh --check first
};

void pitrex_dirs_init(pitrex_dirs *d, const char *root);

int  pitrex_is_file(const char *path);
int  pitrex_is_exec(const char *path);
int  pitrex_file_kind(const char *path);
int  pitrex_cfg_get(const char *file, const char *key, char *out, size_t size);

// NULL if a .um2 file can start, else the message for the Vectrex (the reason is logged).
const char *pitrex_um2_refusal(const pitrex_dirs *d, const char *img);
void pitrex_um2_plan_make(const pitrex_dirs *d, const char *img, pitrex_um2_plan *p);
// argv for "sh ./mister_run.sh ..." (cwd = d->um2). buf holds the strings. Returns argc.
int  pitrex_um2_argv(const pitrex_um2_plan *p, char *buf, size_t size, const char **argv, int max);
// orient=/view= to status values: -1 = leave alone. Returns 0 when there is nothing to set.
int  pitrex_display_bits(const char *orient, const char *view, const char *cfg, int *ob, int *vb);

// Name a .ptx program runs under: its file name without the extension, spaces as '_'.
void pitrex_ptx_name(const char *path, char *out, size_t size);
int  pitrex_copy_file(const char *from, const char *to, int mode);

// Child processes. Each runs in its own process group (pgid = pid).
struct pitrex_spawn_opt
{
	const char *cwd;
	const char *out;        // stdout and stderr, NULL = /dev/null
	int append;
	int rt_prio;            // 0: SCHED_OTHER, nice 0; >0: SCHED_RR at that priority
	int nohup;
	const char *const *env; // "KEY=value" added to (or replacing in) the environment, NULL-terminated
};

pid_t pitrex_spawn(const char *path, const char *const argv[], const pitrex_spawn_opt *o);
int   pitrex_group_alive(pid_t pgid);
// SIGTERM the members named 'first' (all if none is), wait up to timeout_ms for the group
// to end, then SIGKILL it. Reaps pgid itself. Returns 1 if it had to kill.
int   pitrex_group_stop(pid_t pgid, const char *first, int timeout_ms);
// The USB interrupt mask mister_run.sh saved, put back if it is still there.
int   pitrex_irq_restore(const char *saved);

// Implemented by the user of this file (pitrex.cpp, or a test).
void pitrex_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
