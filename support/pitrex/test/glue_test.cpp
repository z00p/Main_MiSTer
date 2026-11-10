// PC test of support/pitrex/pitrex.cpp, the part that runs inside Main, with the Main
// functions it calls replaced by stubs. Run through run_tests.sh, which makes the card
// folder (real mister_run.sh, a fake um2run that logs to $EV) and builds this with
// -DPITREX_CPUS=<a file saying 0-1>.
//   glue_test <storage root> <events file>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../../../cfg.h"
#include "../../../user_io.h"
#include "../../../file_io.h"
#include "../../../hardware.h"
#include "../../../menu.h"
#include "../pitrex.h"

#ifndef PITREX_MAIN_FILE
#define PITREX_MAIN_FILE "/tmp/pitrex_main"
#endif

// ---- stubs for Main ----
cfg_t cfg;
static char core_name[32] = "Vectrex-PiTrex";
static char orig_name[32] = "VECTREX";
static const char *root;
static char osd_visible;
static uint32_t st_orient, st_view;
static int status_sets, status_saves;
static char saved_name[64];
static char info_text[256];

char *user_io_get_core_name(int orig) { return orig ? orig_name : core_name; }
const char *getRootDir() { return root; }
const char *getFullPath(const char *name)
{
	static char p[2048];
	if (name[0] == '/') snprintf(p, sizeof(p), "%s", name);
	else snprintf(p, sizeof(p), "%s/%s", root, name);
	return p;
}
int PathIsDir(const char *name, int)
{
	struct stat st;
	return !stat(name, &st) && S_ISDIR(st.st_mode);
}
char user_io_osd_is_visible() { return osd_visible; }
uint32_t user_io_status_get(const char *opt, int) { return !strcmp(opt, "[76:75]") ? st_orient : !strcmp(opt, "[65]") ? st_view : 0xBAD; }
void user_io_status_set(const char *opt, uint32_t v, int)
{
	status_sets++;
	if (!strcmp(opt, "[76:75]")) st_orient = v;
	else if (!strcmp(opt, "[65]")) st_view = v;
	else st_orient = 0xBAD;
}
int user_io_status_save(const char *f) { status_saves++; snprintf(saved_name, sizeof(saved_name), "%s", f); return 1; }
char *user_io_create_config_name(int) { static char s[] = "Vectrex-PiTrex_v2.CFG"; return s; }
unsigned long GetTimer(unsigned long offset)
{
	struct timespec tp;
	clock_gettime(CLOCK_BOOTTIME, &tp);
	return (unsigned long)(tp.tv_sec * 1000 + tp.tv_nsec / 1000000) + offset;
}
unsigned long CheckTimer(unsigned long t) { return !t || GetTimer(0) >= t; }
void Info(const char *message, int, int, int, int) { snprintf(info_text, sizeof(info_text), "%s", message); }

// ---- test helpers ----
static const char *ev;
static int fails;
static unsigned long max_poll;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) fails++;
}

// what Main's loop does: user_io_poll (pitrex_poll) about every PITREX_POLL_MS
static void run_for(int ms)
{
	unsigned long end = GetTimer(ms);
	while (!CheckTimer(end))
	{
		unsigned long t = GetTimer(0);
		if (is_pitrex()) pitrex_poll();
		t = GetTimer(0) - t;
		if (t > max_poll) max_poll = t;
		usleep(PITREX_POLL_MS * 1000);
	}
}

static int count(const char *text)
{
	FILE *f = fopen(ev, "r");
	if (!f) return 0;
	char line[2048];
	int n = 0;
	while (fgets(line, sizeof(line), f)) if (strstr(line, text)) n++;
	fclose(f);
	return n;
}

// a game ("bus") or a message of this test's own card folder is running
// (only ours: other um2run jobs may run on the same PC)
static int alive(const char *what)
{
	char pattern[1024], cmd[1200];
	if (!strcmp(what, "bus")) snprintf(pattern, sizeof(pattern), "um2run [-]-bus .*%s/", root);
	else snprintf(pattern, sizeof(pattern), "%s/pitrex/um2run/um2run [-]-message", root);
	snprintf(cmd, sizeof(cmd), "pgrep -f '%s' > /dev/null", pattern);
	return !system(cmd);
}

static int to_core;   // files Main sent to the core (user_io_file_tx)

static void pick(const char *rel)
{
	// what menu.cpp does: ours (.um2/.ptx) are started by the hook and not sent to the core
	if (!(is_pitrex() && pitrex_file_selected(rel))) to_core++;
}

int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	root = argv[1];
	ev = argv[2];
	setenv("EV", ev, 1);
	setvbuf(stdout, NULL, _IONBF, 0);

	char path[2048];
	const char *U = "games/Vectrex-PiTrex/UVMC2";

	printf("== only for our core (no ini switch: this Main is loaded with main= for our core)\n");
	strcpy(orig_name, "GBA");
	check(!is_pitrex(), "another core: not active");
	strcpy(orig_name, "Vectrex-PiTrex");
	check(is_pitrex(), "the core under its own name (Vectrex-PiTrex, as on the MiSTer): active");
	strcpy(orig_name, "VECTREX");
	check(is_pitrex(), "the core as VECTREX (setname Vectrex-PiTrex): active");
	check(!pitrex_active(), "nothing running: Main's poll timeout stays 0");
	cfg.log_file_entry = 1;
	run_for(20);
	check(cfg.log_file_entry == 1, "log_file_entry left as the ini has it");
	{
		char want[1200], got[1200] = "", exe[1024] = "";
		ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		if (n > 0) exe[n] = 0;
		snprintf(want, sizeof(want), "%d %s\n", (int)getpid(), exe);
		FILE *mf = fopen(PITREX_MAIN_FILE, "r");
		if (mf) { if (!fgets(got, sizeof(got), mf)) got[0] = 0; fclose(mf); }
		check(!strcmp(got, want), "pitrex_main names this Main (pid and program) for pitrexd to stand aside");
	}

	printf("== a pick starts the game through mister_run.sh\n");
	snprintf(path, sizeof(path), "%s/cart.bin", U);
	pick(path);
	run_for(200);
	check(!pitrex_active() && count("um2run") == 0, "a Vectrex cart pick does nothing");
	check(to_core == 1, "... and the cart goes to the core");
	snprintf(path, sizeof(path), "%s/Plain.um2", U);
	pick(path);
	check(to_core == 1, "a .um2 is not sent to the core");
	check(pitrex_active(), "Plain.um2: active at once (no card, no check)");
	run_for(1500);
	check(count("IMG=Plain.um2 FC=30000 SD= ARGS=--bus --hold") == 1 && count("--secs 1000000000") == 1, "um2run --bus --hold ... --frame-cycles 30000 --secs 1000000000 Plain.um2, no card");
	check(count("--cpu 0 --cpu1 1 --donate 6000") == 1 && count("--no-nice") == 1 && count("--cpu1-normal") == 0,
		"mister_run.sh's default words (rt nonice irq1 share)");
	check(count("--settle 1 --cpu1-after 0") == 1, "no black-screen guard by default (settle 1, core 1 on its CPU at once)");
	check(alive("bus"), "um2run is running");

	printf("== the same report again within 10 s is not a new pick\n");
	pick(path);
	run_for(300);
	check(count("IMG=Plain.um2") == 1, "no second start");

	printf("== another pick stops the game first\n");
	snprintf(path, sizeof(path), "%s/Fast.um2", U);
	pick(path);
	run_for(1500);
	check(count("TERM Plain.um2") == 1, "Plain.um2 got SIGTERM");
	check(count("IMG=Fast.um2 FC=25000") == 1, "Fast.um2 started with its sidecar frame_cycles");

	printf("== core change (fpga_load_rbf) stops it\n");
	unsigned long t = GetTimer(0);
	pitrex_stop();
	t = GetTimer(0) - t;
	check(count("TERM Fast.um2") == 1 && !alive("bus"), "game stopped");
	check(t < 1000, "within 1 s");
	check(!pitrex_active(), "Main's poll timeout back to 0");
	check(waitpid(-1, NULL, WNOHANG) <= 0, "no child left to reap");

	printf("== game ends by itself\n");
	snprintf(path, sizeof(path), "%s/Short.um2", U);
	pick(path);
	run_for(3500);
	check(count("IMG=Short.um2") == 1 && !pitrex_active(), "after the game ends Main's poll timeout is back to 0");

	printf("== card out of date: notice on the Vectrex for 3 s, then the game; Main never waits\n");
	snprintf(path, sizeof(path), "%s/pitrex/uvmc2/sd.img", root);
	fclose(fopen(path, "w"));
	snprintf(path, sizeof(path), "%s/pitrex/make_card.sh", root);
	FILE *f = fopen(path, "w"); fprintf(f, "#!/bin/sh\nsleep 0.3\nexit 1\n"); fclose(f);
	snprintf(path, sizeof(path), "%s/Card1.um2", U);
	max_poll = 0;
	pick(path);
	run_for(1000);
	check(count("--message CARD OUT OF DATE|RUN PITREX MAKE|UVMC2 CARD|FROM SCRIPTS") == 1, "notice shown after the check");
	check(count("IMG=Card1.um2") == 0, "game not yet started");
	run_for(3000);
	check(count("TERM message") >= 1, "notice stopped");
	check(count("IMG=Card1.um2 FC=30000 SD=sd.img ") == 1, "then the game on sd.img");
	printf("        longest poll call: %lu ms\n", max_poll);
	check(max_poll < 250, "no poll call took 250 ms or more (the 0.3 s check and the 3 s notice run beside Main)");
	pitrex_stop();

	printf("== Vector Pinball without its scale card: notice, then check, then the game\n");
	snprintf(path, sizeof(path), "%s/pitrex/make_card.sh", root);
	f = fopen(path, "w"); fprintf(f, "#!/bin/sh\nexit 0\n"); fclose(f);
	snprintf(path, sizeof(path), "%s/Vector Pinball.um2", U);
	pick(path);
	run_for(500);
	check(count("--message NO CARD FOR THIS GAME") == 1, "scale notice");
	run_for(4500);
	check(count("IMG=Vector Pinball.um2 FC=30000 SD=sd.img ") == 1, "game on sd.img");
	pitrex_stop();
	snprintf(path, sizeof(path), "%s/pitrex/uvmc2/sd_scale118.img", root);
	fclose(fopen(path, "w"));
	snprintf(path, sizeof(path), "%s/Vector Pinball.um2", U);
	int before = count("--message NO CARD");
	strcpy(core_name, "Vectrex-PiTrex");
	pick(path); // within 10 s, but nothing runs: a new pick
	run_for(1500);
	check(count("IMG=Vector Pinball.um2 FC=30000 SD=sd_scale118.img ") == 1 && count("--message NO CARD") == before, "with sd_scale118.img: straight to the game on it");
	pitrex_stop();

	printf("== orient/view: set in the core and saved, only when they change\n");
	snprintf(path, sizeof(path), "%s/Land.um2", U);
	pick(path);
	run_for(1500);
	check(st_orient == 2 && st_view == 1 && status_saves == 1 && !strcmp(saved_name, "Vectrex-PiTrex_v2.CFG"), "orient=cw view=open: [76:75]=2 [65]=1, saved in Vectrex-PiTrex_v2.CFG");
	pitrex_stop();
	run_for(50);
	int sets = status_sets;
	pick(path);
	run_for(1500);
	check(status_sets == sets && status_saves == 1, "same again: nothing set, nothing saved");
	pitrex_stop();
	strcpy(core_name, "VECTREX");
	st_orient = 0;
	snprintf(path, sizeof(path), "%s/Land.um2", U);
	run_for(50);
	pick(path);
	run_for(1500);
	check(st_orient == 0 && count("IMG=Land.um2") == 3, "core running as VECTREX: settings left alone, game starts");
	strcpy(core_name, "Vectrex-PiTrex");
	pitrex_stop();

	printf("== refusals: on the Vectrex and in the OSD\n");
	snprintf(path, sizeof(path), "%s/Missing.um2", U);
	osd_visible = 1;
	pick(path);
	run_for(200);
	check(count("--message GAME FILE|NOT FOUND") == 1, "missing file: message on the Vectrex");
	check(!info_text[0], "OSD notice waits while the OSD is open");
	osd_visible = 0;
	run_for(50);
	check(!strcmp(info_text, "GAME FILE\nNOT FOUND"), "then shown in the OSD");
	pitrex_stop();

	printf("== .ptx: copied to /tmp/ptx and run at SCHED_RR 49 (here: as it is)\n");
	snprintf(path, sizeof(path), "%s/My Prog.ptx", U);
	pick(path);
	run_for(500);
	check(count("ptx My_Prog") == 1, "runs as /tmp/ptx/My_Prog");
	pitrex_stop();
	check(count("TERM ptx") == 1, "stopped with SIGTERM");

	printf("== Main's process is left as it was\n");
	check(waitpid(-1, NULL, WNOHANG) <= 0, "no child left to reap");
	check(!alive("bus") && !alive("message"), "no program left running");

	printf("\n%s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
