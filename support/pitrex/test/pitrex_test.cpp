// PC test driver for support/pitrex/pitrex_lib.cpp (not part of the Main build: the
// Makefile only compiles support/*/*.cpp). Run through run_tests.sh.
//
//   pitrex_test plan <root> <cpus file> <core name> <image>   prints what a pick would do,
//                                                            in the format of run_tests.sh's pitrexd harness
//   pitrex_test get <file> <key>                             a sidecar value, as "[value]"
//   pitrex_test proc <scratch dir>                           child process tests (PASS/FAIL lines)
//   pitrex_test unit                                         small rules (PASS/FAIL lines)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
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

static void show(const pitrex_dirs *d, const char *text)
{
	char exe[1100];
	snprintf(exe, sizeof(exe), "%s/um2run", d->um2);
	if (pitrex_is_exec(exe)) printf("SHOW %s\n", text);
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

// run_tests.sh copies this executable as um2run: its main thread name differs from its executable
static int do_um2run(const char *log)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &set, NULL) || prctl(PR_SET_NAME, "u2core0", 0, 0, 0)) return 1;
	FILE *f = fopen(log, "a");
	if (!f) return 1;
	fputs("renamed u2core0\n", f);
	fflush(f);
	int sig;
	if (sigwait(&set, &sig)) { fclose(f); return 1; }
	fputs("um2run-term\n", f);
	fclose(f);
	return 143;
}

static int do_proc(const char *dir)
{
	char runner[512], stubborn[512], log[512], out[512], saved[512];
	snprintf(runner, sizeof(runner), "%s/mister_run.sh", dir);
	snprintf(stubborn, sizeof(stubborn), "%s/stubborn", dir);
	snprintf(log, sizeof(log), "%s/events.log", dir);
	snprintf(out, sizeof(out), "%s/out.log", dir);
	snprintf(saved, sizeof(saved), "%s/ft_irq_saved", dir);

	char text[2048];
	// a fake mister_run.sh: um2run in a background subshell that puts things back after it, as the real one does
	snprintf(text, sizeof(text),
		"#!/bin/sh\necho \"args $* settle=$SETTLE c1=$CPU1_AFTER pwd=$(pwd)\"\n"
		"( trap 'echo runner-term >> %s; exit 1' TERM; ./um2run worker %s; echo \"restored rc=$?\" >> %s ) > /dev/null 2>&1 &\necho started\n", log, log, log);
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
	check(file_has(log, "renamed u2core0"), "um2run renamed its main thread before stop");

	// 2. stop: um2run gets SIGTERM, the subshell still runs its clean-up, all gone well within 2 s
	double t0 = now();
	int killed = pitrex_group_stop(g, "um2run", 2000);
	double dt = now() - t0;
	check(!killed, "stop needed no SIGKILL");
	check(dt < 1.0, "stop took under 1 s");
	usleep(100000);
	check(file_has(log, "um2run-term"), "um2run got SIGTERM");
	check(file_has(log, "restored rc=143"), "mister_run.sh's subshell ran its clean-up after um2run (USB interrupt put back)");
	check(!file_has(log, "runner-term"), "renamed um2run gets SIGTERM before the rest of its group");
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

static int do_unit()
{
	char s[256];
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
	if (argc == 3 && !strcmp(argv[1], "worker")) return do_um2run(argv[2]);
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
	if (argc == 2 && !strcmp(argv[1], "unit"))
	{
		do_unit();
		return fails ? 1 : 0;
	}
	fprintf(stderr, "usage: see the top of pitrex_test.cpp\n");
	return 2;
}
