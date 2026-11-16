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
#include <string>
#include <vector>

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
static char info_text[1024];

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
	if (!strcmp(what, "bus")) snprintf(pattern, sizeof(pattern), "um2run [-]-um2run --bus .*%s/", root);
	else snprintf(pattern, sizeof(pattern), "%s/pitrex/um2run/um2run [-]-um2run --message", root);
	snprintf(cmd, sizeof(cmd), "pgrep -f '%s' > /dev/null", pattern);
	return !system(cmd);
}

// lines of Main's log (/tmp/pitrexd.log) written since 'from' (its size before) that hold text
static int log_count(long from, const char *text)
{
	FILE *f = fopen("/tmp/pitrexd.log", "r");
	if (!f) return 0;
	fseek(f, from, SEEK_SET);
	char line[2048];
	int n = 0;
	while (fgets(line, sizeof(line), f)) if (strstr(line, text)) n++;
	fclose(f);
	return n;
}

static long log_size()
{
	struct stat st;
	return stat("/tmp/pitrexd.log", &st) ? 0 : (long)st.st_size;
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

	printf("== a pick starts the game: this Main, as main-pitrex --um2run\n");
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
		"mister_run.sh's default words (rt nonice irq1 share): Bluetooth needs the share");
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
	check(count("--message GAME STOPPED") == 0, "exit 0: nothing shown");

	printf("== game dies by itself: said on the Vectrex and in the log\n");
	long lf = log_size();
	snprintf(path, sizeof(path), "%s/Crash.um2", U);
	pick(path);
	run_for(1500);
	check(count("um2run --message GAME STOPPED|EXIT STATUS 3") == 1, "GAME STOPPED|EXIT STATUS 3 on the Vectrex");
	check(log_count(lf, "um2run Crash.um2 ended by itself, exit status 3") == 1, "... and in the log");
	pitrex_stop();
	check(!alive("message") && !pitrex_active(), "the message stops with the next core change");

	printf("== game ends after a reset from the OSD: logged, nothing shown\n");
	lf = log_size();
	int msgs = count("--message GAME STOPPED");
	snprintf(path, sizeof(path), "%s/Reset.um2", U);
	pick(path);
	run_for(1500);
	check(count("--message GAME STOPPED") == msgs, "nothing shown on the Vectrex");
	check(log_count(lf, "um2run Reset.um2 stopped: the core was reset from the OSD (cause 0x04)") == 1, "the reset is in the log");
	check(!pitrex_active(), "nothing left running");
	check(count("not started as main-pitrex --um2run") == 0, "every start was main-pitrex --um2run");

	printf("== card out of date: notice on the Vectrex for 3 s, then the game; Main never waits\n");
	snprintf(path, sizeof(path), "%s/pitrex/uvmc2/sd.img", root);
	fclose(fopen(path, "w"));
	// what the stand-in's --make-card --check does (run_tests.sh): this script, if there is one
	snprintf(path, sizeof(path), "%s/pitrex/card_check.sh", root);
	FILE *f = fopen(path, "w"); fprintf(f, "sleep 0.3\nexit 1\n"); fclose(f);
	snprintf(path, sizeof(path), "%s/Card1.um2", U);
	max_poll = 0;
	pick(path);
	run_for(1000);
	check(count("--message CARD OUT OF DATE|RUN PITREX MAKE|UVMC2 CARD|FROM SCRIPTS") == 1, "notice shown after the check");
	check(count("IMG=Card1.um2") == 0, "game not yet started");
	run_for(3000);
	check(count("TERM message") >= 1, "notice stopped");
	check(count("IMG=Card1.um2 FC=30000 SD=sd.img ") == 1, "then the game on sd.img");
	check(count("make_card [--check]") == 1, "the check was main-pitrex --make-card --check");
	printf("        longest poll call: %lu ms\n", max_poll);
	check(max_poll < 250, "no poll call took 250 ms or more (the 0.3 s check and the 3 s notice run beside Main)");
	pitrex_stop();

	printf("== Vector Pinball without its scale card: notice, then check, then the game\n");
	snprintf(path, sizeof(path), "%s/pitrex/card_check.sh", root);
	f = fopen(path, "w"); fprintf(f, "exit 0\n"); fclose(f);
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

	printf("== the core's OSD: Main's own lines build the UVMC2 card, with the progress in the OSD\n");
	{
		// The menu's two rules (menu.cpp, top level): drawing counts only the lines it shows, a pick counts every field from 'A'
		// up (page items and '-' aside). The field a pick of the drawn line <label> lands on:
		auto picked = [](const char *cs, const char *label) -> std::string {
			std::vector<std::string> f;
			for (const char *a = cs;; )
			{
				const char *e = strchr(a, ';');
				f.push_back(e ? std::string(a, e - a) : std::string(a));
				if (!e) break;
				a = e + 1;
			}
			auto paged = [](const std::string &p) { return p[0] == 'P' && p.size() > 2 && p[2] != ','; };
			int drawn = -1;
			size_t i;
			for (i = 2; i < f.size() && !f[i].empty(); i++)
			{
				if (paged(f[i]) || !strchr("FSTRtrOoP", f[i][0])) continue;
				drawn++;
				if (f[i].find(label) != std::string::npos) break;
			}
			if (i >= f.size() || f[i].empty()) return "(not drawn)";
			int entry = 0;
			for (i = 2; i < f.size() && !f[i].empty(); i++)
			{
				if (paged(f[i]) || f[i][0] < 'A') continue;
				if (entry++ == drawn) return f[i];
			}
			return "(none)";
		};

		// the end of the real core's conf string (Vectrex.sv, 5 Nov build): hidden fields after the menu's last line
		char cs[1024] = "Vectrex-PiTrex;;-;F3,UM2,Load UVMC2;P1,Video Profiles;P1O[5],Overlay,Off,On;-;R7,Reset;"
			"J1,Button 1,Button 2,Pause;jn,A,B,Select;I,Overlay On,Profile: Neon Blue;v,2;V,v1.0.261105";
		check(picked(cs, "Rebuild card") == "(not drawn)", "(the menu rules, as copied: our lines not there before)");
		std::string old = std::string(cs) + ";-;R[61],Build UVMC2 card;R[62],Rebuild card if changed";
		check(picked(old.c_str(), "Build UVMC2 card").compare(0, 3, "J1,") == 0 && picked(old.c_str(), "Rebuild card").compare(0, 3, "jn,") == 0,
			"(the menu rules, as copied: lines appended at the end are picked as J1 and jn, as on John's MiSTer)");
		pitrex_confstr(cs, sizeof(cs));
		check(!strcmp(cs, "Vectrex-PiTrex;;-;F3,UM2,Load UVMC2;P1,Video Profiles;P1O[5],Overlay,Off,On;-;R7,Reset;"
			"-;R[61],Build UVMC2 card;R[62],Rebuild card if changed;"
			"J1,Button 1,Button 2,Pause;jn,A,B,Select;I,Overlay On,Profile: Neon Blue;v,2;V,v1.0.261105"),
			"two lines after the core's last menu line (Reset), before its hidden fields");
		check(picked(cs, "Build UVMC2 card") == "R[61],Build UVMC2 card" && picked(cs, "Rebuild card") == "R[62],Rebuild card if changed",
			"a pick of either line lands on that line (appended after J1/jn/I/v/V, it hit J1 and jn: John's MiSTer, 16 Nov)");
		check(picked(cs, "Reset") == "R7,Reset", "... and the core's own Reset still on Reset");
		char cs1[512] = "Vectrex-PiTrex;;-;F3,UM2,Load UVMC2;R7,Reset;V,v1.0.261115";
		pitrex_confstr(cs1, sizeof(cs1));
		check(!strcmp(cs1, "Vectrex-PiTrex;;-;F3,UM2,Load UVMC2;R7,Reset;-;R[61],Build UVMC2 card;R[62],Rebuild card if changed;V,v1.0.261115"),
			"only a version field after the menu: before it");
		char cs2[512] = "Vectrex-PiTrex;;-;R7,Reset;";
		pitrex_confstr(cs2, sizeof(cs2));
		check(!strcmp(cs2, "Vectrex-PiTrex;;-;R7,Reset;-;R[61],Build UVMC2 card;R[62],Rebuild card if changed"), "no hidden fields: at the end, no empty field after a trailing ;");
		char cs5[512] = "Vectrex-PiTrex;;J1,A,B;-;R7,Reset;V,v1";
		pitrex_confstr(cs5, sizeof(cs5));
		check(!strcmp(cs5, "Vectrex-PiTrex;;J1,A,B;-;R7,Reset;-;R[61],Build UVMC2 card;R[62],Rebuild card if changed;V,v1"),
			"a hidden field before menu lines: only the trailing ones count");
		char cs3[512] = "VECTREX;;-;R0,Reset;V,v1";
		pitrex_confstr(cs3, sizeof(cs3));
		check(!strcmp(cs3, "VECTREX;;-;R0,Reset;V,v1"), "another core (the official Vectrex): nothing added");
		char cs4[40] = "Vectrex-PiTrex;;-;R7,Reset";
		pitrex_confstr(cs4, sizeof(cs4));
		check(!strcmp(cs4, "Vectrex-PiTrex;;-;R7,Reset"), "too long for the buffer: left as the core sent it");
		check(!pitrex_osd_option("R7,Reset") && !pitrex_osd_option("R[61],Build") && !pitrex_osd_option("T[61],Build UVMC2 card"),
			"the core's own lines are not ours (nothing started)");
	}
	snprintf(path, sizeof(path), "%s/pitrex/card_build.sh", root);
	f = fopen(path, "w");
	fprintf(f, "echo 'make_card: card folder: 3 files, 1 MiB (skipped: nothing)'\necho 'make_card: copying 50 pct' >&2\nsleep 1\n"
		"echo 'make_card: done: %s/pitrex/uvmc2/sd.img (the old one is sd.img.bak)'\nexit 0\n", root);
	fclose(f);
	info_text[0] = 0;
	max_poll = 0;
	check(pitrex_osd_option("R[61],Build UVMC2 card") == 1, "Build UVMC2 card: ours, handled");
	run_for(500);
	check(count("make_card []") == 1, "... as main-pitrex --make-card (a full build)");
	check(count("make_card [] osd=1") == 1 && count("make_card [--check] osd=1") == 0, "... asked for its card and step lines (MAKE_CARD_OSD=1; the card check is not)");
	check(strstr(info_text, "Building the UVMC2 card") && strstr(info_text, "Copying the files: 50 %"), "progress in the OSD");
	check(pitrex_osd_option("R[62],Rebuild card if changed") == 1 && count("make_card [--if-needed]") == 0, "a second build while one runs: not started");
	snprintf(path, sizeof(path), "%s/Plain.um2", U);
	int plain_starts = count("IMG=Plain.um2");
	pick(path);
	run_for(300);
	check(count("--message CARD BEING BUILT|PLEASE WAIT") == 1 && count("IMG=Plain.um2") == plain_starts, "a game picked meanwhile: refused (the card is locked)");
	osd_visible = 1;
	run_for(1500);
	check(!strstr(info_text, "UVMC2 card built"), "the result waits while the OSD is open");
	osd_visible = 0;
	run_for(100);
	check(strstr(info_text, "UVMC2 card built\n\ncard folder: 3 files, 1 MiB") && strstr(info_text, "done: pitrex/uvmc2/sd.img"), "then the summary, storage folder left out");
	check(!strstr(info_text, "copying") && !strstr(info_text, root), "... without the progress lines");
	check(max_poll < 250, "Main never waited for the builder");
	check(count("TERM message") >= 1 && count("--message CARD READY|PICK A GAME") == 1,
		"CARD BEING BUILT on the Vectrex replaced by CARD READY at the end (it stayed until the next pick: John's MiSTer, 16 Nov)");
	pitrex_stop();

	// the card is current: the builder only does its chores (as make_card.sh on John's MiSTer, 16 Nov)
	snprintf(path, sizeof(path), "%s/pitrex/card_build.sh", root);
	f = fopen(path, "w");
	fprintf(f, "echo 'make_card: overlay VecDrive.um2: kept yours'\nexit 0\n");
	fclose(f);
	check(pitrex_osd_option("R[62],Rebuild card if changed") == 1, "Rebuild card if changed, card current: handled");
	run_for(800);
	check(!!strstr(info_text, "UVMC2 card\n\noverlay VecDrive.um2: kept\n  yours\nThe card is up to date:\n  nothing to build."),
		"... the chores, then 'up to date'");
	f = fopen(path, "w");
	fprintf(f, "exit 0\n");
	fclose(f);
	pitrex_osd_option("R[62],Rebuild card if changed");
	run_for(800);
	check(!!strstr(info_text, "UVMC2 card\n\nThe card is up to date:\n  nothing to build."), "... and with nothing said at all");
	f = fopen(path, "w");
	fprintf(f, "echo 'make_card: overlay VecDrive.um2: kept yours'\necho 'make_card: done: sd.img'\nexit 0\n");
	fclose(f);
	pitrex_osd_option("R[62],Rebuild card if changed");
	run_for(800);
	check(!strstr(info_text, "up to date"), "... but not when it built one");
	check(count("--message CARD READY") == 1, "... and nothing on the Vectrex when no game was picked meanwhile");

	// the builder's card and step lines: the quiet steps show, and a card's "done" is not the end (John's MiSTer, 16 Nov)
	f = fopen(path, "w");
	fprintf(f, "echo 'make_card: card folder: 3 files, 1 MiB (skipped: nothing)'\n"
		"echo 'make_card: osd card 1 2 sd.img'\necho 'make_card: osd step Formatting'\nsleep 1\n"
		"echo 'make_card: formatting'\necho 'make_card: osd step Copying the files'\necho 'make_card: copying 40 pct'\nsleep 1\n"
		"echo 'make_card: done: sd.img (the old one is sd.img.bak)'\n"
		"echo 'make_card: osd card 2 2 sd_scale118.img'\necho 'make_card: osd step Copying sd.img'\nsleep 1\n"
		"echo 'make_card: done: sd_scale118.img, the card for games with scale 118'\nexit 0\n");
	fclose(f);
	pitrex_osd_option("R[61],Build UVMC2 card");
	run_for(600);
	check(!strcmp(info_text, "Building the UVMC2 card\n\nCard 1 of 2: sd.img\nFormatting...\n"), "the step being done, with the card");
	run_for(1000);
	check(!strcmp(info_text, "Building the UVMC2 card\n\nCard 1 of 2: sd.img\nCopying the files: 40 %\n"), "... the copy with its percentage");
	snprintf(path, sizeof(path), "%s/Plain.um2", U);
	pick(path);
	run_for(100);
	snprintf(path, sizeof(path), "%s/My Prog.ptx", U);
	pick(path);
	run_for(900);
	check(!strcmp(info_text, "Building the UVMC2 card\n\nCard 2 of 2: sd_scale118.img\nCopying sd.img...\n"), "... the next card, after the first one's done");
	run_for(800);
	check(strstr(info_text, "UVMC2 card built\n\ncard folder: 3 files, 1 MiB") && strstr(info_text, "done: sd_scale118.img") && !strstr(info_text, "osd") &&
		!strstr(info_text, "Copying"), "the summary without the card and step lines");
	check(count("ptx My_Prog") == 2 && count("--message CARD READY") == 1, "a program started meanwhile: left alone at the end (nothing put over it)");
	pitrex_stop();
	snprintf(path, sizeof(path), "%s/pitrex/card_build.sh", root);

	snprintf(path, sizeof(path), "%s/pitrex/card_build.sh", root);
	f = fopen(path, "w");
	// as on John's MiSTer (16 Nov): the look at the folder first, then the refusal
	fprintf(f, "echo 'make_card: overlay VecDrive.um2: kept yours'\n"
		"echo 'make_card: card folder: 108 files, 175 MiB (skipped: 96 .um2/.uf2 files, 1 README.txt)'\nsleep 1\n"
		"echo 'make_card: ERROR: a UVMC2 game is running and may be writing to its card: stop it first (Scripts > PiTrex_Stop)'\nexit 1\n");
	fclose(f);
	check(pitrex_osd_option("R[62],Rebuild card if changed") == 1, "Rebuild card if changed: ours, handled");
	snprintf(path, sizeof(path), "%s/Plain.um2", U);
	pick(path);
	run_for(1500);
	check(count("make_card [--if-needed]") == 4, "... as main-pitrex --make-card --if-needed (the 4th, after the three above)");
	// wrapped at 30 characters a line
	check(!strcmp(info_text, "UVMC2 card NOT built\n\nERROR: a game is using the\n  card. Reset the core (OSD\n  Reset), then try again."),
		"a refusal: said in the OSD in Main's words, and only that (not the look at the folder before it)");
	check(!strstr(info_text, "PiTrex_Stop"), "... not the script's");
	check(count("--message CARD NOT BUILT|SEE THE OSD") == 1, "... and on the Vectrex, which said CARD BEING BUILT");
	pitrex_stop();

	snprintf(path, sizeof(path), "%s/pitrex/card_build.sh", root);
	f = fopen(path, "w");
	fprintf(f, "echo 'make_card: card folder: 3 files, 1 MiB (skipped: nothing)'\necho 'make_card: card size: 64 MiB, FAT32 with 512-byte clusters'\n"
		"echo 'mkfs.fat: unable to open sd.img.new'\necho 'make_card: ERROR: mkfs.vfat failed'\nexit 1\n");
	fclose(f);
	pitrex_osd_option("R[61],Build UVMC2 card");
	run_for(800);
	check(!strcmp(info_text, "UVMC2 card NOT built\n\nERROR: mkfs.vfat failed\nmkfs.fat: unable to open\n  sd.img.new"),
		"a failure: the error first, then the tool's own words");
	check(count("--message CARD NOT BUILT") == 1, "... nothing on the Vectrex when no game was picked meanwhile");
	pitrex_stop();

	printf("== Main's process is left as it was\n");
	check(waitpid(-1, NULL, WNOHANG) <= 0, "no child left to reap");
	check(!alive("bus") && !alive("message"), "no program left running");

	printf("\n%s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
