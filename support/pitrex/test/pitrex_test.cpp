// PC test driver for support/pitrex/pitrex_lib.cpp (not part of the Main build: the
// Makefile only compiles support/*/*.cpp). Run through run_tests.sh.
//
//   pitrex_test plan <root> <cpus file> <core name> <image>   prints what a pick would do,
//                                                            in the format of run_tests.sh's pitrexd harness
//   pitrex_test get <file> <key>                             a sidecar value, as "[value]"
//   pitrex_test launch <root> <image> <UM2_SCHED> <PTX_LEAD>  how um2run is started (priority, environment,
//                                                            arguments), in the format of run_tests.sh's
//                                                            harness around the real mister_run.sh
//   pitrex_test proc <scratch dir>                           child process tests (PASS/FAIL lines)
//   pitrex_test unit <scratch dir>                           small rules (PASS/FAIL lines)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "../pitrex_lib.h"

static int quiet = 0;
static int fails = 0;

void pitrex_log(const char *fmt, ...)
{
	if (quiet) return;
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "LOG ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) fails++;
}

// Main shows messages with its own um2run (main-pitrex --um2run --message): nothing has to be installed
static void show(const pitrex_dirs *, const char *text)
{
	printf("SHOW %s\n", text);
}

static int do_plan(const char *root, const char *cpus, const char *core, const char *img)
{
	pitrex_dirs d;
	pitrex_dirs_init(&d, root);
	d.cpus = cpus;

	const char *msg = pitrex_um2_refusal(&d, img);
	if (msg)
	{
		show(&d, msg);
		printf("REFUSED\n");
		return 0;
	}

	pitrex_um2_plan p;
	pitrex_um2_plan_make(&d, img, &p);
	if (p.scale_card_missing) show(&d, PITREX_MSG_NO_SCALE);

	char mc[1100];
	snprintf(mc, sizeof(mc), "%s/make_card.sh", d.pitrex);
	if (p.check_card && pitrex_is_file(mc))
	{
		const char *argv[] = { "bash", mc, "--check", NULL };
		pitrex_spawn_opt o = { d.pitrex, NULL, 0, 0, 0, NULL };
		pid_t pid = pitrex_spawn("/bin/bash", argv, &o);
		int status = 0;
		if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 1) show(&d, PITREX_MSG_OLD_CARD);
	}

	int ob, vb;
	if (pitrex_display_bits(p.orient, p.view, p.cfg, &ob, &vb) && !strcmp(core, "Vectrex-PiTrex"))
	{
		char a[8], b[8];
		snprintf(a, sizeof(a), "%d", ob);
		snprintf(b, sizeof(b), "%d", vb);
		printf("DISPLAY %s %s\n", ob < 0 ? "-" : a, vb < 0 ? "-" : b);
	}

	char buf[4096];
	const char *argv[64];
	int argc = pitrex_um2_argv(&p, buf, sizeof(buf), argv, 64);
	printf("RUN SETTLE=%s C1=%s", p.settle, p.cpu1_after);
	for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
	printf("\n");
	return 0;
}

static int do_launch(const char *root, const char *img, const char *env, const char *lead)
{
	quiet = 1;
	pitrex_dirs d;
	pitrex_dirs_init(&d, root);
	pitrex_um2_plan p;
	pitrex_um2_plan_make(&d, img, &p);

	char sched[256];
	pitrex_um2_sched(d.um2, env, sched, sizeof(sched));
	static pitrex_um2_launch l;
	pitrex_um2_launch_make(&d, &p, sched, lead, &l);

	printf("CHRT %s\n", l.rt_prio ? "-r 50" : "-o 0");
	printf("ENV %s %s %s\n", l.env[0], l.env[1], l.env[2]);
	printf("ARGS");
	for (int i = 1; i < l.argc; i++) printf(" %s", l.argv[i]);
	printf("\n");
	printf("IRQ1 %d\n", l.irq1);
	return 0;
}

static double now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static int file_has(const char *path, const char *text)
{
	char buf[4096] = {};
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return strstr(buf, text) != NULL;
}

static void write_file(const char *path, const char *text, int mode)
{
	FILE *f = fopen(path, "w");
	if (!f) return;
	fputs(text, f);
	fclose(f);
	chmod(path, mode);
}

static int do_proc(const char *dir)
{
	char um2run[512], runner[512], stubborn[512], log[512], out[512], saved[512];
	snprintf(um2run, sizeof(um2run), "%s/um2run", dir);
	snprintf(runner, sizeof(runner), "%s/mister_run.sh", dir);
	snprintf(stubborn, sizeof(stubborn), "%s/stubborn", dir);
	snprintf(log, sizeof(log), "%s/events.log", dir);
	snprintf(out, sizeof(out), "%s/out.log", dir);
	snprintf(saved, sizeof(saved), "%s/ft_irq_saved", dir);

	// a fake um2run: runs until SIGTERM, then exits 143 like the real one under mister_run.sh
	char text[2048];
	snprintf(text, sizeof(text), "#!/bin/bash\ntrap 'echo um2run-term >> %s; exit 143' TERM\nwhile :; do sleep 0.05; done\n", log);
	write_file(um2run, text, 0755);
	// a fake mister_run.sh: um2run in a background subshell that puts things back after it, as the real one does
	snprintf(text, sizeof(text),
		"#!/bin/sh\necho \"args $* settle=$SETTLE c1=$CPU1_AFTER pwd=$(pwd)\"\n"
		"( ./um2run; echo \"restored rc=$?\" >> %s ) > /dev/null 2>&1 &\necho started\n", log);
	write_file(runner, text, 0755);
	snprintf(text, sizeof(text), "#!/bin/bash\ntrap '' TERM\nwhile :; do sleep 0.05; done\n");
	write_file(stubborn, text, 0755);
	unlink(log);
	unlink(out);

	// 1. start as Main does: sh ./mister_run.sh in its own group, output appended to the log
	const char *env[] = { "SETTLE=8", "CPU1_AFTER=", NULL };
	const char *argv[] = { "sh", "./mister_run.sh", "/x/Game.um2", "--frame-cycles", "30000", NULL };
	pitrex_spawn_opt o = { dir, out, 1, 0, 0, env };
	pid_t g = pitrex_spawn("/bin/sh", argv, &o);
	check(g > 0, "spawn returns a pid");
	usleep(500000);
	waitpid(g, NULL, WNOHANG);
	check(file_has(out, "args /x/Game.um2 --frame-cycles 30000 settle=8 c1= pwd="), "arguments, environment and output file reach the child");
	char pwd[600];
	snprintf(pwd, sizeof(pwd), "pwd=%s", dir);
	check(file_has(out, pwd), "the child runs in the given directory");
	check(pitrex_group_alive(g), "the group lives on after mister_run.sh returns (um2run in the background)");
	check(getpgid(g) == g || getpgid(g) < 0, "the child leads its own process group");

	// 2. stop: um2run gets SIGTERM, the subshell still runs its clean-up, all gone well within 2 s
	double t0 = now();
	int killed = pitrex_group_stop(g, "um2run", 2000);
	double dt = now() - t0;
	check(!killed, "stop needed no SIGKILL");
	check(dt < 1.0, "stop took under 1 s");
	usleep(100000);
	check(file_has(log, "um2run-term"), "um2run got SIGTERM");
	check(file_has(log, "restored rc=143"), "mister_run.sh's subshell ran its clean-up after um2run (USB interrupt put back)");
	check(!pitrex_group_alive(g), "nothing of the group is left");

	// 3. a program that ignores SIGTERM is killed after the timeout
	const char *argv2[] = { "stubborn", NULL };
	pitrex_spawn_opt o2 = { dir, NULL, 0, 0, 1, NULL };
	g = pitrex_spawn(stubborn, argv2, &o2);
	usleep(300000);
	t0 = now();
	killed = pitrex_group_stop(g, "um2run", 600);
	dt = now() - t0;
	check(killed == 1, "a program that ignores SIGTERM is killed");
	check(dt >= 0.55 && dt < 1.5, "after the timeout");
	check(!pitrex_group_alive(g), "and is gone");

	// 4. a program that is not um2run (a .ptx) gets SIGTERM as a group
	snprintf(text, sizeof(text), "#!/bin/bash\ntrap 'echo ptx-term >> %s; exit 0' TERM\nwhile :; do sleep 0.05; done\n", log);
	write_file(stubborn, text, 0755);
	g = pitrex_spawn(stubborn, argv2, &o2);
	usleep(300000);
	killed = pitrex_group_stop(g, "um2run", 2000);
	usleep(100000);
	check(!killed && file_has(log, "ptx-term"), "a .ptx program is stopped with SIGTERM to its group");

	// 5. the saved USB interrupt mask is put back (here: the file is used up; /proc/irq needs root)
	write_file(saved, "34 1\n", 0644);
	check(pitrex_irq_restore(saved) == 1 && access(saved, F_OK), "saved IRQ mask read and the file removed");
	check(pitrex_irq_restore(saved) == 0, "nothing saved: nothing done");

	// 6. no zombie or stray child left
	check(waitpid(-1, NULL, WNOHANG) <= 0, "no child left to reap");
	return 0;
}

static int do_unit(const char *dir)
{
	char s[256], p[1100], q[1100];

	// the USB interrupt to CPU1, on a made-up /proc
	snprintf(p, sizeof(p), "%s/irq/57", dir);
	snprintf(q, sizeof(q), "mkdir -p %s", p);
	if (system(q)) return 1;
	snprintf(p, sizeof(p), "%s/interrupts", dir);
	write_file(p, "           CPU0       CPU1\n 29:      12345          0     GIC-0  29 Level     twd\n 57:     999999          0     GIC-0 157 Level     ffb40000.usb, dwc2_hsotg:usb1\n", 0644);
	snprintf(p, sizeof(p), "%s/irq/57/smp_affinity", dir);
	write_file(p, "3\n", 0644);
	snprintf(q, sizeof(q), "%s/saved", dir);
	check(pitrex_irq_to_cpu1(dir, q) == 57, "irq1: the dwc2 line is found (57)");
	check(file_has(q, "57 3"), "irq1: old mask saved as '57 3'");
	check(file_has(p, "2"), "irq1: mask is now 2 (CPU1)");
	snprintf(p, sizeof(p), "%s/interrupts", dir);
	write_file(p, "           CPU0       CPU1\n 29:      12345          0     GIC-0  29 Level     twd\n", 0644);
	unlink(q);
	check(pitrex_irq_to_cpu1(dir, q) == -1 && access(q, F_OK), "irq1: no dwc2 line: nothing done, nothing saved");

	// what um2run said when the core dropped it
	snprintf(p, sizeof(p), "%s/um2.log", dir);
	write_file(p, "frame 1\num2run: the core was reset (cause 0x04)\nmore\n", 0644);
	check(pitrex_um2_reset_cause(p) == 0x04, "reset cause 0x04 read from the log");
	write_file(p, "um2run: the core was reset (cause 0x04)\num2run: the core was reset (cause 0x81)\n", 0644);
	check(pitrex_um2_reset_cause(p) == 0x81, "reset cause: the last one counts");
	write_file(p, "nothing here\n", 0644);
	check(pitrex_um2_reset_cause(p) == -1, "no reset line: -1");
	snprintf(p, sizeof(p), "%s/none.log", dir);
	check(pitrex_um2_reset_cause(p) == -1, "no log: -1");

	// exit codes as the shell gives them
	check(pitrex_exit_code(3 << 8) == 3, "exit code: exit(3) is 3");
	check(pitrex_exit_code(SIGKILL) == 137, "exit code: SIGKILL is 137");
	check(pitrex_exit_code(SIGSEGV | 0x80) == 139, "exit code: SIGSEGV with a core dump is 139");

	// what Main does when a game ends by itself (mister_run.sh's rules)
	check(pitrex_um2_end(0, -1) == PITREX_END_QUIET, "end: exit 0 is quiet");
	check(pitrex_um2_end(SIGTERM, -1) == PITREX_END_QUIET, "end: SIGTERM is quiet");
	check(pitrex_um2_end(SIGKILL, -1) == PITREX_END_QUIET, "end: SIGKILL is quiet");
	check(pitrex_um2_end(3 << 8, -1) == PITREX_END_CRASH, "end: exit 3, no reset: GAME STOPPED");
	check(pitrex_um2_end(SIGSEGV, -1) == PITREX_END_CRASH, "end: SIGSEGV: GAME STOPPED");
	check(pitrex_um2_end(3 << 8, 0x04) == PITREX_END_RESET, "end: exit 3 after an OSD reset (0x04): logged only");
	check(pitrex_um2_end(3 << 8, 0x08) == PITREX_END_RESET, "end: exit 3 after a file load (0x08): logged only");
	check(pitrex_um2_end(3 << 8, 0x40) == PITREX_END_RESET, "end: exit 3 after a BIOS load (0x40): logged only");
	check(pitrex_um2_end(3 << 8, 0x01) == PITREX_END_CRASH, "end: exit 3 after another drop cause (0x01): GAME STOPPED");
	check(pitrex_um2_end(0, 0x04) == PITREX_END_QUIET, "end: exit 0 is quiet even after a reset");
	check(pitrex_file_kind("/a/b/Game.um2") == PITREX_KIND_UM2, "kind .um2");
	check(pitrex_file_kind("/a/b/GAME.UM2") == PITREX_KIND_UM2, "kind .UM2 (any case)");
	check(pitrex_file_kind("/a/b/prog.ptx") == PITREX_KIND_PTX, "kind .ptx");
	check(pitrex_file_kind("/a/b/cart.bin") == 0, "kind .bin: not ours");
	check(pitrex_file_kind("/a/b.um2/cart") == 0, "kind: a dot in a folder name does not count");
	check(pitrex_file_kind("/a/b/Game.um2.cfg") == 0, "kind: a sidecar is not a game");
	pitrex_ptx_name("/x/My Game.v2.ptx", s, sizeof(s));
	check(!strcmp(s, "My_Game.v2"), "ptx name: last extension off, spaces as _ (as ${name%.*} and ${name// /_})");
	pitrex_ptx_name("/x/plain", s, sizeof(s));
	check(!strcmp(s, "plain"), "ptx name without an extension");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "plan")) return do_plan(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 4 && !strcmp(argv[1], "get"))
	{
		char v[1024];
		pitrex_cfg_get(argv[2], argv[3], v, sizeof(v));
		printf("[%s]\n", v);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "proc"))
	{
		quiet = 1;
		do_proc(argv[2]);
		return fails ? 1 : 0;
	}
	if (argc == 6 && !strcmp(argv[1], "launch")) return do_launch(argv[2], argv[3], argv[4], argv[5]);
	if (argc == 3 && !strcmp(argv[1], "unit"))
	{
		do_unit(argv[2]);
		return fails ? 1 : 0;
	}
	fprintf(stderr, "usage: see the top of pitrex_test.cpp\n");
	return 2;
}
