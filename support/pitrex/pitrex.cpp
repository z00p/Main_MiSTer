// Vectrex-PiTrex support.
//
// A .um2 file picked in the OSD is run by um2run, which is built into this Main
// (main-pitrex --um2run ..., see pitrex_worker.cpp), with the settings of its sidecar
// <image>.um2.cfg and mister_run.sh's placement and priority; a .ptx file is copied to
// /tmp/ptx and run. The program runs in its own process group and is stopped on the next pick and
// before any core load. Problems are shown on the Vectrex (um2run --message) and in
// the OSD. The rules are pitrexd.sh's, which did this from outside Main.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../user_io.h"
#include "../../hardware.h"
#include "../../menu.h"
#include "pitrex.h"
#include "pitrex_lib.h"

#define PITREX_LOG       "/tmp/pitrexd.log"
#define PITREX_IRQ_SAVED "/tmp/ft_irq_saved"
#define PITREX_PTX_DIR   "/tmp/ptx"
#define PITREX_UM2_LOG   "/tmp/um2.log"
#ifndef PITREX_MAIN_FILE
#define PITREX_MAIN_FILE "/tmp/pitrex_main"
#endif

// core options set from a sidecar
#define PITREX_ST_ORIENT "[76:75]" // Orientation: 0 normal, 1 90 degrees CCW, 2 CW
#define PITREX_ST_VIEW   "[65]"    // View Area: 0 standard, 1 open matte

enum
{
	ST_IDLE,
	ST_NOTICE, // a notice on the Vectrex for a moment, then after_notice
	ST_CHECK,  // make_card.sh --check runs
	ST_RUN
};

static pitrex_dirs dirs;
static pitrex_um2_plan plan;
static int state = ST_IDLE;
static int after_notice;
static pid_t group;        // the program or message started last (pid = process group)
static int group_game;     // group is a .um2 game (its end is looked at)
static int group_status;   // its wait status, once reaped
static int group_reaped;
static char self_exe[1024]; // this Main: um2run is its worker mode
static pid_t check_pid;
static unsigned long timer;
static unsigned long alive_timer;
static char last_img[1024];
static unsigned long last_timer;
static char notice[256];

int is_pitrex()
{
	// This Main is loaded with main= in the core's own ini section, so it only ever runs for our core.
	// The name check is a safety net: the core's own name is Vectrex-PiTrex; with a setname it can also run as VECTREX.
	const char *orig = user_io_get_core_name(1);
	return !strcasecmp(orig, "Vectrex-PiTrex") || !strcasecmp(orig, "VECTREX");
}

int pitrex_active()
{
	return group || check_pid;
}

void pitrex_log(const char *fmt, ...)
{
	char msg[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	printf("PiTrex: %s\n", msg);

	FILE *f = fopen(PITREX_LOG, "a");
	if (f)
	{
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		fprintf(f, "%02d:%02d:%02d main: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
		fclose(f);
	}
}

static void init()
{
	static int done = 0;
	if (done) return;
	done = 1;

	pitrex_dirs_init(&dirs, getRootDir());

	// pitrexd (the helper for the stock Main) stands aside while this file names a live main-pitrex:
	// "<pid> <program path>"; the path too: every core load restarts Main as a new process, so the pid
	// can later belong to another program (the stock Main, after a change to another core)
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) exe[n] = 0;
	FILE *f = n > 0 ? fopen(PITREX_MAIN_FILE, "w") : NULL;
	if (f)
	{
		fprintf(f, "%d %s\n", (int)getpid(), exe);
		fclose(f);
	}
#ifdef PITREX_WORKER_EXE
	snprintf(self_exe, sizeof(self_exe), "%s", PITREX_WORKER_EXE); // PC test: a stand-in for this Main
#else
	snprintf(self_exe, sizeof(self_exe), "%s", n > 0 ? exe : "/proc/self/exe");
#endif
	pitrex_log("main-pitrex: picks are started by Main");
}

static int running()
{
	if (!group) return 0;
	int status;
	if (!group_reaped && waitpid(group, &status, WNOHANG) == group)
	{
		group_status = status;
		group_reaped = 1;
	}
	if (pitrex_group_alive(group)) return 1;
	group = 0;
	return 0;
}

static void group_started(pid_t pid, int game)
{
	group = pid;
	group_game = game;
	group_reaped = 0;
	group_status = 0;
}

static void stop_group()
{
	if (!group) return;
	if (pitrex_group_stop(group, "um2run", 2000)) pitrex_log("program did not exit on SIGTERM: killed");
	group = 0;
	group_game = 0;
	pitrex_irq_restore(PITREX_IRQ_SAVED);
}

static void stop_all()
{
	if (check_pid)
	{
		kill(-check_pid, SIGKILL);
		waitpid(check_pid, NULL, 0);
		check_pid = 0;
	}
	stop_group();
	state = ST_IDLE;
}

// A pick that cannot start says why on the Vectrex for 20 s. Never over a running program.
static void show(const char *text)
{
	if (running()) return;

	static const char *env[] = { "PTX_BACKEND=mister", NULL };
	const char *argv[] = { self_exe, "--um2run", "--message", text, NULL };
	pitrex_spawn_opt o = { "/tmp", "/tmp/um2msg.log", 0, 49, 1, env };
	pid_t pid = pitrex_spawn(self_exe, argv, &o);
	group_started(pid > 0 ? pid : 0, 0);
}

// ... and in the OSD, once it is closed
static void refuse(const char *text)
{
	show(text);
	snprintf(notice, sizeof(notice), "%s", text);
	for (char *p = notice; *p; p++) if (*p == '|') *p = '\n';
}

static void notice_then(int next)
{
	state = ST_NOTICE;
	after_notice = next;
	timer = GetTimer(3000);
}

static void apply_display()
{
	int ob, vb;
	if (!pitrex_display_bits(plan.orient, plan.view, plan.cfg, &ob, &vb)) return;

	// only the core's own settings: a core running as VECTREX shares them with the official core
	const char *name = user_io_get_core_name();
	if (strcmp(name, "Vectrex-PiTrex"))
	{
		pitrex_log("orient/view skipped: the core runs as '%s' (its settings file is not touched); load it from the PiTrex & UVMC2 menu", name);
		return;
	}

	int changed = 0;
	if (ob >= 0 && (int)user_io_status_get(PITREX_ST_ORIENT) != ob)
	{
		user_io_status_set(PITREX_ST_ORIENT, ob);
		changed = 1;
	}
	if (vb >= 0 && (int)user_io_status_get(PITREX_ST_VIEW) != vb)
	{
		user_io_status_set(PITREX_ST_VIEW, vb);
		changed = 1;
	}

	if (changed)
	{
		char *cfgname = user_io_create_config_name(1);
		user_io_status_save(cfgname);
		pitrex_log("orient=%s view=%s: set in the core and saved in %s", plan.orient[0] ? plan.orient : "-", plan.view[0] ? plan.view : "-", cfgname);
	}
}

static void launch_um2()
{
	apply_display();

	// No black-screen guard by default: Main starts the game after the core load and its video mode
	// change, and idles while the game runs, so core 1 gets its own CPU at once (a sidecar can still
	// set settle=/cpu1_after=). With the guard, games ran slow and their sound was rough for the first
	// 10 s; without it, Pac-Man and Punch-Out started clean (John, 10 Nov 2026).
	//
	// Scheduling: mister_run.sh's own words (rt nonice irq1 share). Tried 10 Nov 2026: rt0 (core 1 at
	// normal priority on CPU1, no fixed share) was clean with a wired pad, but a Bluetooth pad worked
	// only in the OSD, not in the game, at nice -20 and at nice 0; at nice 0 the static came back too.
	char sched[256];
	if (pitrex_um2_sched(dirs.um2, getenv("UM2_SCHED"), sched, sizeof(sched))) pitrex_log("FAIR_TEST: [%s]", sched[0] ? sched : "plain");

	static pitrex_um2_launch l;
	pitrex_um2_launch_make(&dirs, &plan, sched, getenv("PTX_LEAD"), &l);

	// argv: this Main in worker mode, then um2run's own arguments
	const char *argv[100];
	int n = 0;
	argv[n++] = self_exe;
	argv[n++] = "--um2run";
	for (int i = 1; i < l.argc && n < 99; i++) argv[n++] = l.argv[i];
	argv[n] = NULL;

	pitrex_irq_restore(PITREX_IRQ_SAVED);
	if (l.irq1) pitrex_irq_to_cpu1("/proc", PITREX_IRQ_SAVED);
	rename(PITREX_UM2_LOG, "/tmp/um2.prev.log");

	pitrex_spawn_opt o = { PathIsDir(dirs.um2, 0) ? dirs.um2 : "/tmp", PITREX_UM2_LOG, 0, l.rt_prio, 1, l.env };
	pid_t pid = pitrex_spawn(self_exe, argv, &o);
	if (pid < 0)
	{
		pitrex_irq_restore(PITREX_IRQ_SAVED);
		pitrex_log("cannot start um2run (%s --um2run)", self_exe);
		state = ST_IDLE;
		return;
	}

	group_started(pid, 1);
	alive_timer = GetTimer(100);
	state = ST_RUN;

	const char *name = strrchr(plan.img, '/');
	pitrex_log("started um2run %s frame_cycles=%s card=%s%s%s", name ? name + 1 : plan.img, plan.fc,
		plan.sd[0] ? plan.sd : "none", plan.args[0] ? " args=" : "", plan.args);
}

static void check_card()
{
	char mc[1100];
	snprintf(mc, sizeof(mc), "%s/make_card.sh", dirs.pitrex);
	if (plan.check_card && pitrex_is_file(mc))
	{
		const char *argv[] = { "bash", mc, "--check", NULL };
		pitrex_spawn_opt o = { dirs.pitrex, NULL, 0, 0, 0, NULL };
		check_pid = pitrex_spawn("/bin/bash", argv, &o);
		if (check_pid > 0)
		{
			state = ST_CHECK;
			timer = GetTimer(30000);
			return;
		}
		check_pid = 0;
	}
	launch_um2();
}

static void start_um2(const char *img)
{
	const char *msg = pitrex_um2_refusal(&dirs, img);
	if (msg)
	{
		refuse(msg);
		return;
	}

	pitrex_um2_plan_make(&dirs, img, &plan);
	stop_all();

	if (plan.scale_card_missing)
	{
		show(PITREX_MSG_NO_SCALE);
		notice_then(ST_CHECK);
		return;
	}
	check_card();
}

static void start_ptx(const char *f)
{
	stop_all();

	char name[256], dst[512];
	pitrex_ptx_name(f, name, sizeof(name));
	snprintf(dst, sizeof(dst), "%s/%s", PITREX_PTX_DIR, name);
	mkdir(PITREX_PTX_DIR, 0755);
	if (!pitrex_copy_file(f, dst, 0755))
	{
		pitrex_log("REFUSED %s: cannot copy it to %s", f, PITREX_PTX_DIR);
		refuse(PITREX_MSG_NO_START);
		return;
	}

	rename("/tmp/pitrex.log", "/tmp/pitrex.prev.log");
	static const char *env[] = { "PTX_BACKEND=mister", NULL };
	const char *argv[] = { dst, NULL };
	pitrex_spawn_opt o = { PathIsDir(dirs.pitrex, 0) ? dirs.pitrex : "/tmp", "/tmp/pitrex.log", 0, 49, 1, env };
	pid_t pid = pitrex_spawn(dst, argv, &o);
	if (pid < 0)
	{
		pitrex_log("cannot start %s", dst);
		return;
	}
	group_started(pid, 0);
	alive_timer = GetTimer(100);
	state = ST_RUN;
	pitrex_log("started %s from %s", name, f);
}

// A game that ended without Main stopping it: put the USB interrupt back, and say why
// (in the log, and on the Vectrex if it died by itself; mister_run.sh did this before).
static void game_ended()
{
	group_game = 0;
	pitrex_irq_restore(PITREX_IRQ_SAVED);
	if (!group_reaped) return;

	int cause = pitrex_um2_reset_cause(PITREX_UM2_LOG);
	int rc = pitrex_exit_code(group_status);
	const char *name = strrchr(plan.img, '/');
	name = name ? name + 1 : plan.img;

	switch (pitrex_um2_end(group_status, cause))
	{
	case PITREX_END_RESET:
		pitrex_log("um2run %s stopped: the core was reset from the OSD (cause 0x%02x)", name, cause);
		break;

	case PITREX_END_CRASH:
		{
			pitrex_log("um2run %s ended by itself, exit status %d, see %s", name, rc, PITREX_UM2_LOG);
			char msg[64];
			snprintf(msg, sizeof(msg), "GAME STOPPED|EXIT STATUS %d", rc);
			show(msg);
		}
		break;

	default:
		break;
	}
}

int pitrex_file_selected(const char *path)
{
	char img[1024];
	snprintf(img, sizeof(img), "%s", getFullPath(path));
	int kind = pitrex_file_kind(img);
	if (!kind) return 0;

	init();
	if (!pitrex_is_file(img))
	{
		pitrex_log("REFUSED %s: no such file", img);
		refuse(PITREX_MSG_NO_FILE);
		return 1;
	}

	// Main can report one MGL file load twice: leave a program just started from the same file alone
	if (!strcmp(img, last_img) && !CheckTimer(last_timer) && (state != ST_IDLE || running())) return 1;
	snprintf(last_img, sizeof(last_img), "%s", img);
	last_timer = GetTimer(10000);

	if (kind == PITREX_KIND_UM2) start_um2(img);
	else start_ptx(img);
	return 1;
}

void pitrex_poll()
{
	init();

	if (notice[0] && !user_io_osd_is_visible())
	{
		Info(notice, 4000);
		notice[0] = 0;
	}

	switch (state)
	{
	case ST_NOTICE:
		if (CheckTimer(timer))
		{
			stop_group();
			if (after_notice == ST_CHECK) check_card();
			else launch_um2();
		}
		break;

	case ST_CHECK:
		{
			int status = 0;
			if (waitpid(check_pid, &status, WNOHANG) == check_pid)
			{
				check_pid = 0;
				if (WIFEXITED(status) && WEXITSTATUS(status) == 1)
				{
					// a game runs on the card as it is; say so for a moment if the folder has moved on
					pitrex_log("card out of date: starting on the current card");
					show(PITREX_MSG_OLD_CARD);
					notice_then(ST_RUN);
				}
				else launch_um2();
			}
			else if (CheckTimer(timer))
			{
				kill(-check_pid, SIGKILL);
				waitpid(check_pid, NULL, 0);
				check_pid = 0;
				pitrex_log("card check took over 30 s: starting on the current card");
				launch_um2();
			}
		}
		break;

	default:
		break;
	}

	if (group && CheckTimer(alive_timer))
	{
		alive_timer = GetTimer(100);
		int game = group_game;
		if (!running())
		{
			if (state == ST_RUN) state = ST_IDLE;
			if (game) game_ended();
		}
	}
}

// Before any core load: nothing of ours may write to the FPGA after it.
void pitrex_stop()
{
	if (!group && !check_pid)
	{
		state = ST_IDLE;
		return;
	}
	stop_all();
	pitrex_log("core change: stopped PiTrex program(s)");
}
