// Vectrex-PiTrex helpers: the rules pitrexd.sh applies to a picked file, and the
// child processes. Nothing here uses Main, so it builds and runs on a PC too.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include "pitrex_lib.h"

extern char **environ;

#ifndef PITREX_CPUS
#define PITREX_CPUS "/sys/devices/system/cpu/online"
#endif

void pitrex_dirs_init(pitrex_dirs *d, const char *root)
{
	snprintf(d->pitrex, sizeof(d->pitrex), "%s/pitrex", root);
	snprintf(d->um2, sizeof(d->um2), "%s/um2run", d->pitrex);
	snprintf(d->sd_dir, sizeof(d->sd_dir), "%s/uvmc2", d->pitrex);
	d->cpus = PITREX_CPUS;
}

int pitrex_is_file(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISREG(st.st_mode);
}

int pitrex_is_exec(const char *path)
{
	return pitrex_is_file(path) && !access(path, X_OK);
}

int pitrex_file_kind(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext || strchr(ext, '/')) return 0;
	if (!strcasecmp(ext, ".um2")) return PITREX_KIND_UM2;
	if (!strcasecmp(ext, ".ptx")) return PITREX_KIND_PTX;
	return 0;
}

static const char *base_name(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static int all_digits(const char *s)
{
	for (; *s; s++) if (*s < '0' || *s > '9') return 0;
	return 1;
}

static int has_space(const char *s)
{
	for (; *s; s++) if (isspace((unsigned char)*s)) return 1;
	return 0;
}

static void lower(char *out, size_t size, const char *s)
{
	size_t i = 0;
	for (; s[i] && i + 1 < size; i++) out[i] = (char)tolower((unsigned char)s[i]);
	out[i] = 0;
}

// "key=value" in a sidecar, as pitrexd's cfg_get: every CR dropped, the last
// matching line wins, spaces around the key and the '=' and at the end of the
// value are ignored; the key is case sensitive. Returns 1 if the key was found.
int pitrex_cfg_get(const char *file, const char *key, char *out, size_t size)
{
	out[0] = 0;
	FILE *f = fopen(file, "rb");
	if (!f) return 0;

	size_t klen = strlen(key);
	char *line = NULL;
	size_t cap = 0, len = 0;
	int found = 0, c = 0;
	while (c != EOF)
	{
		c = fgetc(f);
		if (c == '\r') continue;
		if (c != '\n' && c != EOF)
		{
			if (len + 2 > cap)
			{
				cap = cap ? cap * 2 : 256;
				line = (char *)realloc(line, cap);
				if (!line) break;
			}
			line[len++] = (char)c;
			continue;
		}
		if (c == EOF && !len) break;
		if (!line) { len = 0; continue; }
		line[len] = 0;
		len = 0;

		const char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (strncmp(p, key, klen)) continue;
		p += klen;
		while (isspace((unsigned char)*p)) p++;
		if (*p != '=') continue;
		p++;
		while (isspace((unsigned char)*p)) p++;
		snprintf(out, size, "%s", p);
		found = 1;
	}
	free(line);
	fclose(f);

	size_t n = strlen(out);
	while (n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
	return found;
}

const char *pitrex_um2_refusal(const pitrex_dirs *d, const char *img)
{
	char online[64] = {};
	FILE *f = fopen(d->cpus, "r");
	if (f)
	{
		if (!fgets(online, sizeof(online), f)) online[0] = 0;
		fclose(f);
	}
	online[strcspn(online, "\r\n")] = 0;

	if (strcmp(online, "0-1"))
	{
		// BigMig leaves Linux on maxcpus=1 until a normal reboot: um2run would starve Main
		pitrex_log("REFUSED %s: Linux has CPUs '%s' online, um2run needs 0-1 (reboot out of BigMig)", base_name(img), online);
		return PITREX_MSG_ONE_CPU;
	}

	char a[1100], b[1100];
	snprintf(a, sizeof(a), "%s/um2run", d->um2);
	snprintf(b, sizeof(b), "%s/mister_run.sh", d->um2);
	if (!pitrex_is_file(a) || !pitrex_is_file(b))
	{
		pitrex_log("REFUSED %s: %s/um2run or mister_run.sh missing", base_name(img), d->um2);
		return PITREX_MSG_NO_UM2RUN;
	}
	return NULL;
}

void pitrex_um2_plan_make(const pitrex_dirs *d, const char *img, pitrex_um2_plan *p)
{
	char name[256], path[2100];

	memset(p, 0, sizeof(*p));
	snprintf(p->img, sizeof(p->img), "%s", img);
	snprintf(p->cfg, sizeof(p->cfg), "%s.cfg", img);
	lower(name, sizeof(name), base_name(img));

	// frame_cycles: default 50 Hz; games that only run right at 60 Hz need no sidecar for it
	pitrex_cfg_get(p->cfg, "frame_cycles", p->fc, sizeof(p->fc));
	if (!p->fc[0] || !all_digits(p->fc))
	{
		if (p->fc[0]) pitrex_log("%s: bad frame_cycles '%s' ignored", p->cfg, p->fc);
		strcpy(p->fc, strstr(name, "punch") ? "25000" : "30000");
	}

	// sd=: next to the game, then in the card folder, then beside um2run
	pitrex_cfg_get(p->cfg, "sd", p->sd, sizeof(p->sd));
	if (p->sd[0])
	{
		if (p->sd[0] != '/')
		{
			char dir[1024], rel[1024];
			snprintf(rel, sizeof(rel), "%s", p->sd);
			snprintf(dir, sizeof(dir), "%s", img);
			char *s = strrchr(dir, '/');
			if (s) *s = 0;

			snprintf(path, sizeof(path), "%s/%s", dir, rel);
			if (pitrex_is_file(path)) snprintf(p->sd, sizeof(p->sd), "%s", path);
			else
			{
				char in_sd[2100], in_um2[2100];
				snprintf(in_sd, sizeof(in_sd), "%s/%s", d->sd_dir, rel);
				snprintf(in_um2, sizeof(in_um2), "%s/%s", d->um2, rel);
				if (pitrex_is_file(in_sd) || !pitrex_is_file(in_um2)) snprintf(p->sd, sizeof(p->sd), "%s", in_sd);
				else snprintf(p->sd, sizeof(p->sd), "%s", in_um2);
			}
		}
		if (has_space(p->sd))
		{
			pitrex_log("%s: sd path has spaces, no card", p->cfg);
			p->sd[0] = 0;
		}
		if (p->sd[0] && !pitrex_is_file(p->sd))
		{
			pitrex_log("%s: card %s not found, no card", p->cfg, p->sd);
			p->sd[0] = 0;
		}
	}
	else
	{
		snprintf(path, sizeof(path), "%s/sd.img", d->sd_dir);
		if (pitrex_is_file(path))
		{
			snprintf(p->sd, sizeof(p->sd), "%s", path);

			// a game that wants its own calibration scale runs on the copy of sd.img that make_card.sh makes for it
			pitrex_cfg_get(p->cfg, "scale", p->sc, sizeof(p->sc));
			if (!p->sc[0])
			{
				const char *v = strstr(name, "vector");
				if (v && strstr(v + 6, "pinball")) strcpy(p->sc, "118");
			}

			size_t n = strlen(p->sc);
			if (n >= 1 && n <= 3 && all_digits(p->sc) && p->sc[0] != '0')
			{
				snprintf(path, sizeof(path), "%s/sd_scale%s.img", d->sd_dir, p->sc);
				if (pitrex_is_file(path)) snprintf(p->sd, sizeof(p->sd), "%s", path);
				else
				{
					pitrex_log("%s: no card with scale %s yet (make_card.sh makes it): starting on sd.img", base_name(img), p->sc);
					p->scale_card_missing = 1;
				}
			}
			else if (p->sc[0] && strcmp(p->sc, "0"))
			{
				pitrex_log("%s: bad scale '%s' ignored", p->cfg, p->sc);
			}
		}
	}

	pitrex_cfg_get(p->cfg, "args", p->args, sizeof(p->args));
	pitrex_cfg_get(p->cfg, "settle", p->settle, sizeof(p->settle));
	pitrex_cfg_get(p->cfg, "cpu1_after", p->cpu1_after, sizeof(p->cpu1_after));
	if (!all_digits(p->settle)) p->settle[0] = 0;
	if (!all_digits(p->cpu1_after)) p->cpu1_after[0] = 0;
	pitrex_cfg_get(p->cfg, "orient", p->orient, sizeof(p->orient));
	pitrex_cfg_get(p->cfg, "view", p->view, sizeof(p->view));

	// a game runs on the card as it is; a made card (sd.img or a scale copy) is checked against the folder first
	char made[2100];
	snprintf(path, sizeof(path), "%s/sd.img", d->sd_dir);
	snprintf(made, sizeof(made), "%s/sd_scale%s.img", d->sd_dir, p->sc);
	p->check_card = p->sd[0] && (!strcmp(p->sd, path) || !strcmp(p->sd, made));
}

struct arg_buf
{
	char *buf;
	size_t size, used;
	const char **argv;
	int argc, max;
};

static void arg_add(arg_buf *a, const char *s, size_t len)
{
	if (a->argc + 1 >= a->max || a->used + len + 1 > a->size) return;
	memcpy(a->buf + a->used, s, len);
	a->buf[a->used + len] = 0;
	a->argv[a->argc++] = a->buf + a->used;
	a->used += len + 1;
}

static void arg_add(arg_buf *a, const char *s)
{
	arg_add(a, s, strlen(s));
}

int pitrex_um2_argv(const pitrex_um2_plan *p, char *buf, size_t size, const char **argv, int max)
{
	arg_buf a = { buf, size, 0, argv, 0, max };

	arg_add(&a, "sh");
	arg_add(&a, "./mister_run.sh");
	arg_add(&a, p->img);
	arg_add(&a, "--frame-cycles");
	arg_add(&a, p->fc);
	if (p->sd[0])
	{
		arg_add(&a, "--sd");
		arg_add(&a, p->sd);
	}
	arg_add(&a, "--secs");
	arg_add(&a, "1000000000");

	// args= is split into words, as the shell does
	const char *s = p->args;
	while (*s)
	{
		while (*s == ' ' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;
		const char *e = s;
		while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
		arg_add(&a, s, (size_t)(e - s));
		s = e;
	}

	argv[a.argc] = NULL;
	return a.argc;
}

int pitrex_display_bits(const char *orient, const char *view, const char *cfg, int *ob, int *vb)
{
	char o[32], v[32];

	*ob = *vb = -1;
	if (!orient[0] && !view[0]) return 0;
	lower(o, sizeof(o), orient);
	lower(v, sizeof(v), view);

	if (!o[0]) *ob = -1;
	else if (!strcmp(o, "0")) *ob = 0;
	else if (!strcmp(o, "ccw")) *ob = 1;
	else if (!strcmp(o, "cw")) *ob = 2;
	else pitrex_log("%s: bad orient '%s' ignored (0, ccw or cw)", cfg, orient);

	if (!v[0]) *vb = -1;
	else if (!strcmp(v, "standard")) *vb = 0;
	else if (!strcmp(v, "open")) *vb = 1;
	else pitrex_log("%s: bad view '%s' ignored (standard or open)", cfg, view);

	return (*ob >= 0 || *vb >= 0);
}

void pitrex_ptx_name(const char *path, char *out, size_t size)
{
	snprintf(out, size, "%s", base_name(path));
	char *dot = strrchr(out, '.');
	if (dot) *dot = 0;
	for (char *p = out; *p; p++) if (*p == ' ') *p = '_';
}

int pitrex_copy_file(const char *from, const char *to, int mode)
{
	int in = open(from, O_RDONLY);
	if (in < 0) return 0;
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0)
	{
		close(in);
		return 0;
	}

	char buf[16384];
	int ok = 1;
	ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0)
	{
		if (write(out, buf, n) != n)
		{
			ok = 0;
			break;
		}
	}
	if (n < 0) ok = 0;
	close(in);
	if (close(out)) ok = 0;
	if (ok) ok = !chmod(to, mode);
	return ok;
}

pid_t pitrex_spawn(const char *path, const char *const argv[], const pitrex_spawn_opt *o)
{
	// Everything the child needs is made before fork: Main has threads, so the child
	// may only use async-signal-safe calls until exec.
	int n = 0, k = 0;
	while (environ && environ[n]) n++;
	if (o->env) while (o->env[k]) k++;

	const char **envp = (const char **)malloc((n + k + 1) * sizeof(char *));
	if (!envp) return -1;
	int m = 0;
	for (int i = 0; i < n; i++)
	{
		int replaced = 0;
		for (int j = 0; j < k && !replaced; j++)
		{
			size_t len = strcspn(o->env[j], "=") + 1;
			if (!strncmp(environ[i], o->env[j], len)) replaced = 1;
		}
		if (!replaced) envp[m++] = environ[i];
	}
	for (int j = 0; j < k; j++) envp[m++] = o->env[j];
	envp[m] = NULL;

	long maxfd = sysconf(_SC_OPEN_MAX);
	if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
	const char *out = o->out ? o->out : "/dev/null";
	int flags = O_WRONLY | O_CREAT | (o->append ? O_APPEND : O_TRUNC);

	pid_t pid = fork();
	if (!pid)
	{
		setpgid(0, 0);

		// Main's signal state is not the program's
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (int s = 1; s < NSIG; s++) sigaction(s, &sa, NULL);
		if (o->nohup)
		{
			sa.sa_handler = SIG_IGN;
			sigaction(SIGHUP, &sa, NULL);
		}
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);

		// nor is its CPU (Main is pinned to CPU1) or its priority
		struct sched_param sp;
		memset(&sp, 0, sizeof(sp));
		if (o->rt_prio > 0)
		{
			sp.sched_priority = o->rt_prio;
			sched_setscheduler(0, SCHED_RR, &sp);
		}
		else
		{
			sched_setscheduler(0, SCHED_OTHER, &sp);
			setpriority(PRIO_PROCESS, 0, 0);
		}
		cpu_set_t cpus;
		CPU_ZERO(&cpus);
		CPU_SET(0, &cpus);
		CPU_SET(1, &cpus);
		sched_setaffinity(0, sizeof(cpus), &cpus);

		int fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) dup2(fd, 0);
		fd = open(out, flags, 0644);
		if (fd < 0) fd = open("/dev/null", O_WRONLY);
		if (fd >= 0)
		{
			dup2(fd, 1);
			dup2(fd, 2);
		}
		for (int i = 3; i < maxfd; i++) close(i);

		if (o->cwd && chdir(o->cwd)) _exit(126);
		execve(path, (char *const *)argv, (char *const *)envp);
		_exit(127);
	}

	if (pid > 0) setpgid(pid, pid);
	free(envp);
	return pid;
}

int pitrex_group_alive(pid_t pgid)
{
	if (pgid <= 0) return 0;
	return !kill(-pgid, 0) || errno == EPERM;
}

// SIGTERM the processes of group pgid named 'name' (from /proc/<pid>/stat). Returns how many.
static int term_named(pid_t pgid, const char *name)
{
	DIR *dir = opendir("/proc");
	if (!dir) return 0;

	int cnt = 0;
	struct dirent *de;
	while ((de = readdir(dir)))
	{
		if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;

		char path[64], stat[512];
		snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		ssize_t n = read(fd, stat, sizeof(stat) - 1);
		close(fd);
		if (n <= 0) continue;
		stat[n] = 0;

		// pid (comm) state ppid pgrp ...
		char *l = strchr(stat, '(');
		char *r = strrchr(stat, ')');
		if (!l || !r || r < l) continue;
		*r = 0;
		char st;
		int ppid, pgrp;
		if (sscanf(r + 1, " %c %d %d", &st, &ppid, &pgrp) != 3) continue;
		if (pgrp != pgid || strcmp(l + 1, name)) continue;
		if (!kill(atoi(de->d_name), SIGTERM)) cnt++;
	}
	closedir(dir);
	return cnt;
}

int pitrex_group_stop(pid_t pgid, const char *first, int timeout_ms)
{
	if (pgid <= 0) return 0;

	// mister_run.sh waits for um2run and then puts the USB interrupt back: let it
	if (!first || !term_named(pgid, first)) kill(-pgid, SIGTERM);

	for (int t = 0; t < timeout_ms; t += 20)
	{
		waitpid(pgid, NULL, WNOHANG);
		if (!pitrex_group_alive(pgid)) return 0;
		usleep(20000);
	}

	kill(-pgid, SIGKILL);
	for (int i = 0; i < 50; i++)
	{
		waitpid(pgid, NULL, WNOHANG);
		if (!pitrex_group_alive(pgid)) break;
		usleep(10000);
	}
	return 1;
}

int pitrex_irq_restore(const char *saved)
{
	FILE *f = fopen(saved, "r");
	if (!f) return 0;

	unsigned irq;
	char mask[64];
	int ok = (fscanf(f, "%u %63s", &irq, mask) == 2);
	fclose(f);
	if (ok)
	{
		char path[64];
		snprintf(path, sizeof(path), "/proc/irq/%u/smp_affinity", irq);
		f = fopen(path, "w");
		if (f)
		{
			fprintf(f, "%s\n", mask);
			fclose(f);
		}
	}
	unlink(saved);
	return ok;
}
