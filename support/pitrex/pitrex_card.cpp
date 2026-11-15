// SPDX-License-Identifier: GPL-2.0-or-later
// Vectrex-PiTrex: build the UVMC2 card images (main-pitrex --make-card). A port of Vectrex_MiSTer's
// pitrex/scripts/make_card.sh and its python helper, which stay the reference: the same rules, messages,
// exit codes and files, so that both can build the same card from the same folder (tests in test/).
//
//   <fat>/games/Vectrex-PiTrex/UVMC2/  the files from a real UVMC2 SD card and the .um2 games, all in one folder (input)
//   <fat>/pitrex/uvmc2/card/           the same input, older installs: used instead, games copied out, while it has files of its own
//   <fat>/pitrex/uvmc2/sd.img          the card every UVMC2 game without an sd= key in its sidecar sees (output)
//
// The card is a FAT32 image of a whole card without a partition table ("superfloppy"). Rebuilding keeps the player's saves: what
// the games wrote to the old sd.img (every file the last build did not put there, and all config and scores folders) is copied
// into the new one, and the old image is kept as sd.img.bak. sd.img.list records what a build put on the card.
// Older installs: BattleZone gets bz_sd.img (its own saves) and a sidecar sd=bz_sd.img; .um2 games in the card folder are copied
// to the games folder. Scale cards: a game whose sidecar says scale=N (Vector Pinball: 118 built in) gets sd_scale<N>.img, a copy of
// sd.img with config/uvm2.cfg saying `scale N`. Starting calibration: sd.img and the scale cards get `zero 72` in config/uvm2.cfg
// unless that file already has a zero line. Overlay art is copied next to each .um2 as <game>.art (never replacing one).
//
// What changes from the script: loop devices and mounts are made with the kernel calls (no losetup or mount), copies and the
// helper's work are done here (no python). mkfs.vfat still formats, so the images come out as the script's do.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/loop.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pitrex_card.h"
#include "pitrex_lib.h"

typedef std::string str;

namespace {

// ---------------------------------------------------------------------------------------------
// Settings for this run (make_card.sh's variables)

str FAT, T, UV, OUT, GAMES, CARD, UM2, ART_OLD, ART_NEW, ZERO_PRESET;
bool ONE = true;
const long long MAX_MIB = 16000;
const long long BIG_MIB = 2000;   // above this, 4 KiB clusters: the FAT of a big card with 512-byte clusters would be hundreds of MiB
const long long BLANK_MIB = 64;
const char *LABEL = "UVM2SD";
const char *IN_USE = "a UVMC2 game is running and may be writing to its card: stop it first (Scripts > PiTrex_Stop)";

volatile sig_atomic_t g_sig;

// cleanup state
struct Mount { str dir; int loop; };
std::vector<Mount> MOUNTS;     // newest last
std::vector<int> LOOPS;        // loop devices attached and not yet mounted (fds)
std::vector<str> TMPFILES;
str WORK, LOCK, SAVES;
std::vector<int> LOCKFDS;

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	fputs("make_card: ", stdout);
	vprintf(fmt, a);
	fputc('\n', stdout);
	fflush(stdout);
	va_end(a);
}

[[noreturn]] void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void die(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	fputs("make_card: ERROR: ", stdout);
	vprintf(fmt, a);
	fputc('\n', stdout);
	fflush(stdout);
	va_end(a);
	exit(1);
}

void check_sig()
{
	if (g_sig)
	{
		g_sig = 0;
		puts("make_card: interrupted");
		fflush(stdout);
		exit(1);
	}
}

// ---------------------------------------------------------------------------------------------
// Small file helpers

const char *c(const str &s) { return s.c_str(); }
str base(const str &p) { size_t i = p.rfind('/'); return i == str::npos ? p : p.substr(i + 1); }
str dir_of(const str &p) { size_t i = p.rfind('/'); return i == str::npos ? p : p.substr(0, i); }   // ${p%/*}
str no_ext(const str &n) { size_t i = n.rfind('.'); return i == str::npos ? n : n.substr(0, i); }   // ${n%.*}
str lower(str s) { for (auto &ch : s) ch = (char)tolower((unsigned char)ch); return s; }
bool ends_with(const str &s, const char *e) { size_t n = strlen(e); return s.size() >= n && !s.compare(s.size() - n, n, e); }

bool is_file(const str &p) { struct stat st; return !stat(c(p), &st) && S_ISREG(st.st_mode); }    // [ -f ]
bool is_dir(const str &p) { struct stat st; return !stat(c(p), &st) && S_ISDIR(st.st_mode); }     // [ -d ]
bool is_link(const str &p) { struct stat st; return !lstat(c(p), &st) && S_ISLNK(st.st_mode); }
bool lexists(const str &p) { struct stat st; return !lstat(c(p), &st); }
long long fsize(const str &p) { struct stat st; return stat(c(p), &st) ? 0 : (long long)st.st_size; }

// the names in a folder, sorted (bytes, as python sorts its str and `sort` in the C locale); false if it cannot be read
bool list_dir(const str &d, std::vector<str> &out, bool hidden = true)
{
	out.clear();
	DIR *dp = opendir(c(d));
	if (!dp) return false;
	while (struct dirent *e = readdir(dp))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (!hidden && e->d_name[0] == '.') continue;
		out.push_back(e->d_name);
	}
	closedir(dp);
	std::sort(out.begin(), out.end());
	return true;
}

bool mkdir_p(const str &p)
{
	if (p.empty() || is_dir(p)) return true;
	str parent = dir_of(p);
	if (parent != p && !parent.empty() && !mkdir_p(parent)) return false;
	return !mkdir(c(p), 0777) || errno == EEXIST;
}

int rm_cb(const char *p, const struct stat *, int flag, struct FTW *)
{
	if (flag == FTW_DP) rmdir(p); else unlink(p);
	return 0;
}
void rm_rf(const str &p) { if (lexists(p)) nftw(c(p), rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

// whole file as text (no line ends taken off); "" when it cannot be read
str slurp(const str &p)
{
	str s;
	FILE *f = fopen(c(p), "rb");
	if (!f) return s;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}
// as "$(cat file)": trailing newlines dropped
str cat_trim(const str &p) { str s = slurp(p); while (!s.empty() && s.back() == '\n') s.pop_back(); return s; }

bool write_file(const str &p, const str &text)
{
	FILE *f = fopen(c(p), "wb");
	if (!f) return false;
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	return fclose(f) == 0 && ok;
}

// copy a file's bytes and its times (shutil.copyfile + utime: a FAT file system refuses chmod). sparse: a whole card image, as cp
// copies it: runs of zeros are left as holes (the bytes read back the same) and the times are not copied. Returns 0 or an errno.
int copy_file(const str &s, const str &d, bool sparse = false)
{
	struct stat st;
	int in = open(c(s), O_RDONLY | O_CLOEXEC);
	if (in < 0) return errno;
	if (fstat(in, &st)) { int e = errno; close(in); return e; }
	int out = open(c(d), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (out < 0) { int e = errno; close(in); return e; }

	static char buf[1 << 16];
	static const char zero[1 << 16] = {};
	int err = 0;
	off_t pos = 0;
	for (;;)
	{
		check_sig();
		ssize_t n = read(in, buf, sizeof(buf));
		if (n < 0) { if (errno == EINTR) continue; err = errno; break; }
		if (!n) break;
		if (sparse && n == (ssize_t)sizeof(buf) && !memcmp(buf, zero, sizeof(buf)))
		{
			pos += n;
			if (lseek(out, pos, SEEK_SET) < 0) { err = errno; break; }
			continue;
		}
		for (ssize_t w = 0; w < n;)
		{
			ssize_t k = write(out, buf + w, n - w);
			if (k < 0) { if (errno == EINTR) continue; err = errno; break; }
			w += k;
		}
		if (err) break;
		pos += n;
	}
	if (!err && sparse && ftruncate(out, pos)) err = errno;
	if (close(out) && !err) err = errno;
	close(in);
	if (!err && !sparse)
	{
		struct timespec ts[2] = { st.st_atim, st.st_mtim };
		utimensat(AT_FDCWD, c(d), ts, 0);
	}
	return err;
}

// statvfs free space in MiB, -1 if it cannot be read
long long free_mib(const str &p)
{
	struct statvfs v;
	if (statvfs(c(p), &v)) return -1;
	return (long long)((unsigned long long)v.f_bavail * v.f_frsize >> 20);
}

bool mkimg(const str &p, long long mib)
{
	int fd = open(c(p), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0) return false;
	bool ok = !ftruncate(fd, (off_t)mib << 20);
	return !close(fd) && ok;
}

bool in_path(const char *name)
{
	const char *path = getenv("PATH");
	if (!path) path = "/usr/sbin:/usr/bin:/sbin:/bin";
	str all = path;
	size_t i = 0;
	for (;;)
	{
		size_t j = all.find(':', i);
		str d = all.substr(i, j == str::npos ? str::npos : j - i);
		if (d.empty()) d = ".";
		if (!access(c(d + "/" + name), X_OK) && is_file(d + "/" + name)) return true;
		if (j == str::npos) return false;
		i = j + 1;
	}
}

// run a program from PATH, stdout and stderr to out ("" = /dev/null). Returns its exit status (127 if it cannot start).
int run(const std::vector<str> &args, const str &out)
{
	std::vector<char *> av;
	for (auto &a : args) av.push_back((char *)c(a));
	av.push_back(NULL);
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0) return 127;
	if (!pid)
	{
		int fd = open(out.empty() ? "/dev/null" : c(out), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		execvp(av[0], av.data());
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR) return 127;
		if (g_sig) { kill(pid, SIGTERM); waitpid(pid, &status, 0); check_sig(); }
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void cat_out(const str &p) { str s = slurp(p); fputs(c(s), stdout); fflush(stdout); }

// a sidecar value: Main's reader, so that the card builder and the game start read a sidecar alike
str cfg_get(const str &file, const char *key)
{
	char v[1024];
	pitrex_cfg_get(c(file), key, v, sizeof(v));
	return v;
}

// ---------------------------------------------------------------------------------------------
// The folder walk and the skip rules (the python helper's walk and why_skip)

// Not copied to the card: cart images and firmware, menu previews (the pictures and videos in ASSETS folders; other files there,
// such as a game's sound, do go on the card), macOS and Windows clutter, and the README.txt PiTrex_Setup leaves in the card folder
// (it carries the marker line below).
const char *JUNK[] = { "__macosx", ".ds_store", ".spotlight-v100", ".trashes", ".fseventsd", ".temporaryitems",
	"thumbs.db", "desktop.ini", "system volume information", "$recycle.bin" };
const char *MARK = "(This file is not copied to the card.)";
const char *PREVIEW[] = { ".mp4", ".mov", ".avi", ".mkv", ".webm", ".png", ".jpg", ".jpeg", ".gif", ".bmp" };

const char *why_skip(const str &path, const str &name, bool isdir, bool top)
{
	str n = lower(name);
	for (auto j : JUNK) if (n == j) return "macOS/Windows files";
	if (!n.compare(0, 2, "._")) return "macOS/Windows files";
	if (isdir) return NULL;
	for (auto e : PREVIEW) if (ends_with(n, e) && lower(base(dir_of(path))) == "assets") return "menu pictures and videos (ASSETS)";
	if (ends_with(n, ".um2") || ends_with(n, ".uf2")) return ".um2/.uf2 files";
	for (auto e : { ".um2.cfg", ".um2.load", ".img", ".img.list", ".img.bak", ".lock" })
		if (ends_with(n, e)) return "game settings and card images";
	if (ends_with(n, ".art")) return "overlay art";
	if (top && n == "readme.txt")
	{
		FILE *f = fopen(c(path), "rb");
		if (f)
		{
			char buf[4097];
			size_t k = fread(buf, 1, 4096, f);
			fclose(f);
			if (memmem(buf, k, MARK, strlen(MARK))) return "README.txt (this folder's notes)";
		}
	}
	return NULL;
}

struct Ent { str rel, path; bool dir; };

// os.walk top down: a folder's (kept) subfolders, then its files, then each subfolder's tree in turn.
// Links to folders are listed (made on the card as folders) but not entered, as os.walk does.
void walk_in(const str &root, const str &rel, bool top, std::vector<Ent> &out, std::map<str, int> &skips)
{
	std::vector<str> names, dirs, files, keep;
	if (!list_dir(root, names)) return;
	for (auto &n : names) (is_dir(root + "/" + n) ? dirs : files).push_back(n);
	for (auto &d : dirs)
	{
		str p = root + "/" + d;
		if (const char *w = why_skip(p, d, true, top)) { skips[w]++; continue; }
		keep.push_back(d);
		out.push_back({ rel.empty() ? d : rel + "/" + d, p, true });
	}
	for (auto &f : files)
	{
		str p = root + "/" + f;
		if (const char *w = why_skip(p, f, false, top)) skips[w]++;
		else if (is_file(p)) out.push_back({ rel.empty() ? f : rel + "/" + f, p, false });
	}
	for (auto &d : keep)
	{
		str p = root + "/" + d;
		if (!is_link(p)) walk_in(p, rel.empty() ? d : rel + "/" + d, false, out, skips);
	}
}

std::vector<Ent> walk(const str &src, std::map<str, int> *skips = NULL)
{
	std::vector<Ent> out;
	std::map<str, int> s;
	walk_in(src, "", true, out, skips ? *skips : s);
	return out;
}

// every folder and file under root (all_files: no skip rules)
void all_in(const str &root, const str &rel, std::vector<Ent> &out)
{
	std::vector<str> names;
	if (!list_dir(root, names)) return;
	std::vector<str> dirs;
	for (auto &n : names)
	{
		str p = root + "/" + n;
		bool d = is_dir(p);
		out.push_back({ rel.empty() ? n : rel + "/" + n, p, d });
		if (d && !is_link(p)) dirs.push_back(n);
	}
	for (auto &d : dirs) all_in(root + "/" + d, rel.empty() ? d : rel + "/" + d, out);
}

// characters in a UTF-8 name (python's len)
long long chars(const str &s) { long long n = 0; for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) n++; return n; }

// Rough space on a FAT32 card with 512-byte clusters: files rounded up to whole clusters, one cluster per folder, 32-byte directory
// entries plus long-name entries.
long long fat_bytes(const std::vector<Ent> &entries)
{
	long long total = 512;
	for (auto &e : entries)
	{
		total += 32 * (2 + chars(base(e.rel)) / 13);
		total += e.dir ? 512 : (fsize(e.path) + 511) / 512 * 512;
	}
	return total;
}

long long card_mib(long long fat)
{
	const long long mib = 1 << 20;
	long long need = fat * 5 / 4 + 16 * mib;   // contents x 1.25 + 16 MiB for new settings and scores
	long long card = std::max(64LL, (need + mib - 1) / mib);
	// near the limit: the 1.25 headroom may go, the 16 MiB for saves stays
	if (card > MAX_MIB && fat + 16 * mib <= MAX_MIB * mib) card = MAX_MIB;
	return card;
}

// what the card is built from (compared with <card>.list): "size mtime path" per file, as one text without the last line end
str list_of(const str &src)
{
	str s;
	char line[64];
	for (auto &e : walk(src))
	{
		if (e.dir) continue;
		struct stat st;
		if (stat(c(e.path), &st)) continue;
		snprintf(line, sizeof(line), "%lld %lld ", (long long)st.st_size, (long long)st.st_mtime);
		if (!s.empty()) s += '\n';
		s += line + e.rel;
	}
	return s;
}

// existing entry in parent whose name matches name ignoring case, else name itself
str find_ci(const str &parent, const str &name)
{
	std::vector<str> names;
	if (list_dir(parent, names))
		for (auto &e : names) if (lower(e) == lower(name)) return e;
	return name;
}

// ---------------------------------------------------------------------------------------------
// The helper's commands

// copy what goes on the card into dst; prints how far a big copy has got (every 5 %) and returns the number of files
long long copy_tree(const str &src, const str &dst)
{
	std::vector<Ent> entries = walk(src);
	long long total = 0, done = 0, n = 0, shown = 0;
	for (auto &e : entries) if (!e.dir) total += fsize(e.path);
	if (!total) total = 1;
	for (auto &e : entries)
	{
		str d = dst + "/" + e.rel;
		int err = 0;
		if (e.dir) { if (!is_dir(d) && mkdir(c(d), 0777)) err = errno; }
		else
		{
			err = copy_file(e.path, d);
			if (!err)
			{
				n++;
				done += fsize(e.path);
				long long pct = done * 20 / total * 5;
				if (pct != shown)
				{
					shown = pct;
					fprintf(stderr, "make_card: copying %lld pct\n", pct);
					fflush(stderr);
				}
			}
		}
		if (err) { fprintf(stderr, "make_card: ERROR: cannot copy %s to the card: %s\n", c(e.rel), strerror(err)); exit(1); }
	}
	return n;
}

bool in_save_dir(const str &rel)
{
	size_t i = 0;
	for (;;)
	{
		size_t j = rel.find('/', i);
		if (j == str::npos) return false;   // the last part is the file
		str p = lower(rel.substr(i, j - i));
		if (p == "config" || p == "scores") return true;
		i = j + 1;
	}
}

// Copy the player's files out of an old card into dst (same paths); returns the number of files. The games do not all save in the
// same place (config/ and scores/ at the top, Punch-Out in PORTDATA/Punchout/CONFIG, BattleZone makes a BATTLEZONE folder), so kept are:
//   - every file in a folder named config or scores (any case, any depth), and
//   - if this builder made the card (its .list exists): every file and folder it did not put there, i.e. what the games wrote;
//   - otherwise (a card made by hand): every file and folder the card folder does not provide.
struct SavesOut
{
	std::map<str, bool> built, built_dirs;
	str dst;
	long long n = 0;

	void tree(const str &dir, const str &rel)
	{
		std::vector<str> names, dirs, files;
		if (!list_dir(dir, names)) return;
		for (auto &x : names) (is_dir(dir + "/" + x) ? dirs : files).push_back(x);
		for (auto &d : dirs)
		{
			str r = rel.empty() ? d : rel + "/" + d;
			if (!built_dirs.count(lower(r)) && !mkdir_p(dst + "/" + r)) throw errno;
		}
		for (auto &f : files)
		{
			str r = rel.empty() ? f : rel + "/" + f;
			if (in_save_dir(r) || !built.count(lower(r)))
			{
				str d = dst + "/" + r;
				if (!mkdir_p(dir_of(d))) throw errno;
				if (int e = copy_file(dir + "/" + f, d)) throw e;
				n++;
			}
		}
		for (auto &d : dirs) tree(dir + "/" + d, rel.empty() ? d : rel + "/" + d);
	}
};

long long saves_out(const str &mnt, const str &dst, const str &listfile, const str &card)
{
	SavesOut s;
	s.dst = dst;
	if (is_file(listfile))
	{
		str text = slurp(listfile);
		size_t i = 0;
		while (i < text.size())
		{
			size_t j = text.find('\n', i);
			str line = text.substr(i, j == str::npos ? str::npos : j - i);
			i = j == str::npos ? text.size() : j + 1;
			size_t a = line.find(' '), b = a == str::npos ? str::npos : line.find(' ', a + 1);
			if (b != str::npos) s.built[lower(line.substr(b + 1))] = true;
		}
	}
	else
	{
		for (auto &e : walk(card)) if (!e.dir) s.built[lower(e.rel)] = true;
	}
	for (auto &b : s.built)
		for (size_t k = b.first.find('/'); k != str::npos; k = b.first.find('/', k + 1)) s.built_dirs[b.first.substr(0, k)] = true;
	try { s.tree(mnt, ""); }
	catch (int) { return -1; }
	return s.n;
}

// copy the tree src into dst (existing folders reused, matched ignoring case; files replaced)
long long merge(const str &src, const str &dst)
{
	std::vector<str> names;
	if (!list_dir(src, names)) throw errno;
	long long n = 0;
	for (auto &e : names)
	{
		str s = src + "/" + e, d = dst + "/" + find_ci(dst, e);
		if (is_dir(s))
		{
			if (!is_dir(d) && mkdir(c(d), 0777)) throw errno;
			n += merge(s, d);
		}
		else if (is_file(s))
		{
			if (int err = copy_file(s, d)) throw err;
			n++;
		}
	}
	return n;
}

bool same_file(const str &a, const str &b)
{
	if (!is_file(b) || fsize(a) != fsize(b)) return false;
	FILE *fa = fopen(c(a), "rb"), *fb = fopen(c(b), "rb");
	bool same = fa && fb;
	static char x[1 << 16], y[1 << 16];
	while (same)
	{
		size_t na = fread(x, 1, sizeof(x), fa), nb = fread(y, 1, sizeof(y), fb);
		if (na != nb || memcmp(x, y, na)) same = false;
		if (!na) break;
	}
	if (fa) fclose(fa);
	if (fb) fclose(fb);
	return same;
}

// Copy the .um2 files of the card folder (any case, any depth, same skip rules for folders) to the games folder, keeping their
// subfolders. A copy, never a move, and nothing is ever overwritten. One line per file: C (copied), S (already there, same),
// D (a different file is there), E (error).
void um2_copy(const str &dir, const str &rel, bool top, const str &dst, std::vector<str> &lines)
{
	std::vector<str> names, dirs, files;
	if (!list_dir(dir, names)) return;
	for (auto &x : names) (is_dir(dir + "/" + x) ? dirs : files).push_back(x);
	for (auto &f : files)
	{
		if (!ends_with(lower(f), ".um2") || !f.compare(0, 2, "._")) continue;
		str p = dir + "/" + f;
		if (!is_file(p)) continue;
		str r = rel.empty() ? f : rel + "/" + f;
		str d = dst + "/" + r;
		if (lexists(d)) { lines.push_back((same_file(p, d) ? "S " : "D ") + r); continue; }
		int err = 0;
		if (!is_dir(dir_of(d)) && !mkdir_p(dir_of(d))) err = errno;
		if (!err)
		{
			// under a temporary name first, so an interrupted copy never looks like a game
			char tail[64];
			snprintf(tail, sizeof(tail), ".%d.part", (int)getpid());
			str tmp = dir_of(d) + "/." + f + tail;
			err = copy_file(p, tmp);
			if (!err && rename(c(tmp), c(d))) err = errno;
			if (lexists(tmp)) unlink(c(tmp));
		}
		lines.push_back(err ? "E " + r + ": " + strerror(err) : "C " + r);
	}
	for (auto &d : dirs)
	{
		if (why_skip(dir + "/" + d, d, true, top)) continue;
		if (!is_link(dir + "/" + d)) um2_copy(dir + "/" + d, rel.empty() ? d : rel + "/" + d, false, dst, lines);
	}
}

// python's whitespace for a latin-1 text (str.strip, str.split)
bool pyspace(unsigned char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r') || (ch >= 0x1c && ch <= 0x1f) || ch == 0x85 || ch == 0xa0; }
str rstrip_py(str s) { while (!s.empty() && pyspace((unsigned char)s.back())) s.pop_back(); return s; }
str first_word(const str &s)
{
	size_t i = 0;
	while (i < s.size() && pyspace((unsigned char)s[i])) i++;
	size_t j = i;
	while (j < s.size() && !pyspace((unsigned char)s[j])) j++;
	return s.substr(i, j - i);
}
std::vector<str> split_nl(const str &text)
{
	std::vector<str> out;
	size_t i = 0;
	for (;;)
	{
		size_t j = text.find('\n', i);
		out.push_back(text.substr(i, j == str::npos ? str::npos : j - i));
		if (j == str::npos) return out;
		i = j + 1;
	}
}
str rstrip_cr(str s) { while (!s.empty() && s.back() == '\r') s.pop_back(); return s; }

// config/uvm2.cfg on a mounted card (folder and file matched ignoring case, as the games' FAT does); the folder is made if missing
str uvm2_cfg(const str &mnt)
{
	str d = mnt + "/" + find_ci(mnt, "config");
	if (!is_dir(d) && mkdir(c(d), 0777)) return "";
	return d + "/" + find_ci(d, "uvm2.cfg");
}
// Padded to 512 bytes: the SDK's writer only overwrites a file of one sector or more, so a game's wizard can still save to it.
bool write_cfg(const str &p, const std::vector<str> &lines)
{
	str text;
	for (size_t i = 0; i < lines.size(); i++) { if (i) text += '\n'; text += lines[i]; }
	text += '\n';
	if (text.size() < 512) text += str(511 - text.size(), ' ') + "\n";
	return write_file(p, text);
}

// config/uvm2.cfg says `scale n`: the other lines of an existing file are kept (a calibration made with a game's wizard, say), any
// scale line is replaced
bool scalecfg(const str &mnt, int n)
{
	str p = uvm2_cfg(mnt);
	if (p.empty()) return false;
	std::vector<str> lines = { "scale " + std::to_string(n) };
	if (is_file(p))
		for (auto &l : split_nl(slurp(p)))
		{
			str line = rstrip_cr(l);
			str w = first_word(line);
			if (!w.empty() && w != "scale") lines.push_back(rstrip_py(line));
		}
	return write_cfg(p, lines);
}

// The starting calibration: config/uvm2.cfg gets `zero n` only when it has no zero line: one already there may be the player's own
// calibration, and that always wins. So the preset is a starting value, never an override. 1 = added, 0 = there was one, -1 = error.
int zerocfg(const str &mnt, int n)
{
	str p = uvm2_cfg(mnt);
	if (p.empty()) return -1;
	std::vector<str> lines;
	if (is_file(p))
		for (auto &l : split_nl(slurp(p)))
			if (!first_word(l).empty()) lines.push_back(rstrip_py(rstrip_cr(l)));
	for (auto &l : lines) if (first_word(l) == "zero") return 0;
	lines.push_back("zero " + std::to_string(n));
	return write_cfg(p, lines) ? 1 : -1;
}

// ---------------------------------------------------------------------------------------------
// Overlay art (art_lib.sh)

// the file (isdir: the folder) in dir called name, ignoring case; "" if none. An exact name first, then the folder's entries in
// order, without hidden ones (a shell glob).
str art_ci(const str &dir, const str &name, bool isdir = false)
{
	auto ok = [isdir](const str &p) { return isdir ? is_dir(p) : is_file(p); };
	if (ok(dir + "/" + name)) return dir + "/" + name;
	std::vector<str> names;
	if (list_dir(dir, names, false))
		for (auto &e : names) if (lower(e) == lower(name) && ok(dir + "/" + e)) return dir + "/" + e;
	return "";
}
str art_path(const str &dir, const str &name)
{
	if (name.find('/') != str::npos) return art_ci(dir + "/" + dir_of(name), base(name));
	return art_ci(dir, name);
}
// _90CW / _90CCW when the sidecar rotates the display: those games take the art from the overlays' Tate folder
str art_tate(const str &cfg)
{
	str o = lower(cfg_get(cfg, "orient"));
	return o == "cw" ? "_90CW" : o == "ccw" ? "_90CCW" : "";
}
// view=open: loaded art makes the core select View Area Standard, so such a game gets no art
bool art_open(const str &cfg) { return lower(cfg_get(cfg, "view")) == "open"; }
str art_sub(const str &s, const str &n, const std::vector<str> &dirs)
{
	for (str d : dirs)
	{
		if (!s.empty()) { d = art_ci(d, "Tate", true); if (d.empty()) continue; }
		str a = art_ci(d, n + s + ".art");
		if (!a.empty()) return a;
	}
	return "";
}
// the file a sidecar's overlay= names: a path is relative to the game's folder; a bare file name is looked up there, then in the
// two overlay folders. An absolute path is used as it is.
str art_named(const str &g, const str &ov)
{
	if (ov.empty()) return "";
	if (ov[0] == '/') return art_ci(dir_of(ov), base(ov));
	str a = art_path(dir_of(g), ov);
	if (!a.empty() || ov.find('/') != str::npos) return a;
	for (auto &d : { ART_NEW, ART_OLD }) { a = art_ci(d, ov); if (!a.empty()) return a; }
	return "";
}
// Copy the art for a game next to it as <game>.art, where Main's same-name lookup finds it. Says "copied <art>", "kept yours" (a
// .art is already there and differs: never replaced), or nothing (no art for the game, or our own copy is already there).
str art_place(const str &g)
{
	str cfg = g + ".cfg", n = no_ext(base(g)), src;
	if (art_open(cfg)) return "";
	str ov = cfg_get(cfg, "overlay"), s = art_tate(cfg);
	if (!s.empty())
	{
		str st = ov.empty() ? n : no_ext(base(ov));
		src = art_sub(s, st, { ART_OLD, ART_NEW });
		if (src.empty()) return "";
	}
	else if (!ov.empty())
	{
		src = art_named(g, ov);
		if (src.empty()) return "overlay=" + ov + " not found";
	}
	else
	{
		src = art_sub("", n, { ART_OLD, ART_NEW });
		if (src.empty()) return "";
	}
	str dest = art_ci(dir_of(g), n + ".art");
	if (!dest.empty()) return same_file(src, dest) ? "" : "kept yours";
	dest = dir_of(g) + "/" + n + ".art";
	str tmp = dir_of(g) + "/." + n + ".art.part";
	if (!copy_file(src, tmp) && !rename(c(tmp), c(dest))) return "copied " + base(src);
	unlink(c(tmp));
	return "could not copy " + base(src);
}

// ---------------------------------------------------------------------------------------------
// The .um2 games: find -type f -iname '*.um2' ! -name '._*' | sort (no links followed, every folder)

void find_files(const str &dir, std::vector<str> &out)
{
	std::vector<str> names;
	if (!list_dir(dir, names)) return;
	for (auto &n : names)
	{
		str p = dir + "/" + n;
		struct stat st;
		if (lstat(c(p), &st)) continue;
		if (S_ISDIR(st.st_mode)) find_files(p, out);
		else if (S_ISREG(st.st_mode)) out.push_back(p);
	}
}
std::vector<str> find_um2(const str &dir)
{
	std::vector<str> all, out;
	find_files(dir, all);
	for (auto &p : all)
	{
		str n = base(p);
		if (ends_with(lower(n), ".um2") && n.compare(0, 2, "._")) out.push_back(p);
	}
	std::sort(out.begin(), out.end());
	return out;
}

// BattleZone: -iname '*attle*one*.um2'
bool is_battlezone(const str &p)
{
	str n = lower(base(p));
	if (!ends_with(n, ".um2") || !n.compare(0, 2, "._")) return false;
	str stem = n.substr(0, n.size() - 4);
	size_t a = stem.find("attle");
	return a != str::npos && stem.find("one", a + 5) != str::npos;
}

// the card pitrexd (and Main) would use for sd=<name> next to a game in folder dir
str resolve(const str &name, const str &dir)
{
	if (!name.empty() && name[0] == '/') return name;
	if (is_file(dir + "/" + name)) return dir + "/" + name;
	if (is_file(UV + "/" + name) || !is_file(UM2 + "/" + name)) return UV + "/" + name;
	return UM2 + "/" + name;
}

// the scale a game's card should carry, "" for none: the rule Main uses to start it (pitrex_um2_plan_make).
// Built in: Vector Pinball draws its table about 1.35 times too tall for the core at the SDK's default scale (160); 118 fits it.
str game_scale(const str &g)
{
	str cfg = g + ".cfg";
	if (!cfg_get(cfg, "sd").empty()) return "";
	str s = cfg_get(cfg, "scale");
	if (s.empty())
	{
		str n = lower(base(g));
		size_t v = n.find("vector");
		if (v != str::npos && n.find("pinball", v + 6) != str::npos) s = "118";
	}
	if (s.size() < 1 || s.size() > 3 || s[0] < '1' || s[0] > '9') return "";
	for (char ch : s) if (ch < '0' || ch > '9') return "";
	return s;
}

// ---------------------------------------------------------------------------------------------
// Loop devices and mounts (the kernel calls losetup and mount would make)

int loop_attach(const str &img, bool ro)
{
	int file = open(c(img), (ro ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	if (file < 0) die("losetup failed for %s", c(img));
	int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	for (int tries = 0; tries < 64; tries++)
	{
		int n = ctl >= 0 ? ioctl(ctl, LOOP_CTL_GET_FREE) : tries;
		if (n < 0) break;
		char dev[32];
		snprintf(dev, sizeof(dev), "/dev/loop%d", n);
		int lfd = -1;
		for (int w = 0; w < 50 && lfd < 0; w++)
		{
			lfd = open(dev, (ro ? O_RDONLY : O_RDWR) | O_CLOEXEC);
			if (lfd < 0 && errno == ENOENT && ctl >= 0) usleep(10000); else if (lfd < 0) break;
		}
		if (lfd < 0) continue;
		if (ioctl(lfd, LOOP_SET_FD, file))
		{
			close(lfd);
			if (errno == EBUSY) continue;   // taken meanwhile: the next free one
			break;
		}
		struct loop_info64 info;
		memset(&info, 0, sizeof(info));
		info.lo_flags = LO_FLAGS_AUTOCLEAR | (ro ? LO_FLAGS_READ_ONLY : 0);
		snprintf((char *)info.lo_file_name, sizeof(info.lo_file_name), "%s", c(img));
		ioctl(lfd, LOOP_SET_STATUS64, &info);
		close(file);
		if (ctl >= 0) close(ctl);
		return lfd;
	}
	close(file);
	if (ctl >= 0) close(ctl);
	die("no free loop device");
}

str loop_name(int lfd)
{
	struct stat st;
	char dev[32] = "/dev/loop?";
	if (!fstat(lfd, &st)) snprintf(dev, sizeof(dev), "/dev/loop%u", minor(st.st_rdev));
	return dev;
}

void loop_free(int lfd)
{
	ioctl(lfd, LOOP_CLR_FD, 0);
	close(lfd);
}

// attach <image> <mount point> [ro]: loop device + mount, remembered for cleanup. false (with the reason said) if it will not mount.
bool attach(const str &img, const str &mnt, bool ro = false)
{
	int lfd = loop_attach(img, ro);
	LOOPS.push_back(lfd);
	mkdir_p(mnt);
	str dev = loop_name(lfd);
	if (mount(c(dev), c(mnt), "vfat", ro ? MS_RDONLY : 0, NULL))
	{
		printf("mount: mounting %s on %s failed: %s\n", c(dev), c(mnt), strerror(errno));
		fflush(stdout);
		return false;
	}
	LOOPS.pop_back();
	MOUNTS.push_back({ mnt, lfd });
	return true;
}

// detach <mount point>: unmount and free its loop device
void detach(const str &mnt)
{
	for (size_t i = MOUNTS.size(); i-- > 0;)
	{
		if (MOUNTS[i].dir != mnt) continue;
		if (umount2(c(mnt), 0)) die("cannot unmount %s", c(mnt));
		loop_free(MOUNTS[i].loop);
		MOUNTS.erase(MOUNTS.begin() + i);
		return;
	}
}

// ---------------------------------------------------------------------------------------------
// Cleanup: whatever happens, unmount, detach the loop devices and remove the half-built files.

void cleanup()
{
	for (size_t i = MOUNTS.size(); i-- > 0;)
	{
		if (umount2(c(MOUNTS[i].dir), 0)) umount2(c(MOUNTS[i].dir), MNT_DETACH);
		loop_free(MOUNTS[i].loop);
	}
	MOUNTS.clear();
	for (int l : LOOPS) loop_free(l);
	LOOPS.clear();
	for (auto &f : TMPFILES) unlink(c(f));
	TMPFILES.clear();
	if (!WORK.empty())
	{
		std::vector<str> names;
		if (list_dir(WORK, names))
			for (auto &n : names)
			{
				if (!n.compare(0, 3, "mnt")) rmdir(c(WORK + "/" + n));
				else if (ends_with(n, ".log")) unlink(c(WORK + "/" + n));
			}
		rmdir(c(WORK));
		WORK.clear();
	}
	// the saves folder is ours (mkdtemp) and never a mount point
	if (!SAVES.empty() && !SAVES.compare(0, UV.size() + 12, UV + "/.make_card.")) rm_rf(SAVES);
	SAVES.clear();
	if (!LOCK.empty()) rm_rf(LOCK);
	LOCK.clear();
	for (int fd : LOCKFDS) close(fd);
	LOCKFDS.clear();
}

void on_signal(int) { g_sig = 1; }

// ---------------------------------------------------------------------------------------------
// The build

str SC_SCALES_TEXT;
std::vector<str> SC_SCALES;
bool IF_NEEDED, CHECK;

// <card>.lock, held (flock) for the rest of the run; a game's um2run holds it while it uses the card
void lock_card(const str &card)
{
	int fd = open(c(card + ".lock"), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
	if (fd < 0) die("cannot open %s.lock", c(card));
	if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); die("%s", IN_USE); }
	LOCKFDS.push_back(fd);
}

// Nothing for a card and no card yet: a blank one, so that games can keep scores and settings. It takes no mounting. The empty .list
// makes the next build with files in the card folder replace it, keeping what games saved on it.
void blank_card()
{
	if (is_file(OUT) || !in_path("mkfs.vfat")) return;
	mkdir_p(UV);
	int fd = open(c(OUT + ".lock"), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
	if (fd < 0) die("cannot open %s.lock", c(OUT));
	if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); die("%s", IN_USE); }
	struct Unlock { int fd; ~Unlock() { close(fd); } } unlock = { fd };
	if (is_file(OUT)) return;
	long long free = free_mib(UV);
	if (free < 0) return;
	if (free < BLANK_MIB + 4) { say("no room for a blank card in %s: games play, but cannot save.", c(UV)); return; }
	str nw = OUT + ".new";
	TMPFILES.push_back(nw);
	if (!mkimg(nw, BLANK_MIB) || run({ "mkfs.vfat", "-F", "32", "-s", "1", "-n", LABEL, nw }, ""))
	{
		unlink(c(nw));
		TMPFILES.clear();
		say("could not make a blank card: games play, but cannot save.");
		return;
	}
	rename(c(nw), c(OUT));
	TMPFILES.clear();
	write_file(OUT + ".list", "");
	say("made a blank card, %s, so that games can save. Games that need ROMs want a real one: see ADDING_GAMES.md.", c(OUT));
}

// every scale card exists and was made from sd.img's current build (same .list; the games' saves do not count)
bool scale_cards_ok()
{
	for (auto &n : SC_SCALES)
	{
		str t = UV + "/sd_scale" + n + ".img";
		if (!is_file(t) || cat_trim(t + ".list") != cat_trim(OUT + ".list")) return false;
	}
	return true;
}

// saves_from <card> <folder>: the player's files of an old card (see saves_out)
void saves_from(const str &img, const str &dst)
{
	mkdir_p(dst);
	if (!is_file(img)) return;
	if (!attach(img, WORK + "/mnt_old", true))
		die("cannot mount the old %s (is it a FAT card image?). Move it away to build a new one.", c(base(img)));
	long long n = saves_out(WORK + "/mnt_old", dst, img + ".list", CARD);
	if (n < 0) die("cannot copy the saves out of %s", c(base(img)));
	detach(WORK + "/mnt_old");
	if (!n) say("the old %s has no saves yet", c(base(img)));
	else if (is_file(img + ".list")) say("kept the saves of the old %s (%lld file(s) the games wrote)", c(base(img)), n);
	else say("kept %lld file(s) of the old %s that are not in the card folder (it was not made by this script)", n, c(base(img)));
}

// enough free space in a folder for mib more?
void need_on(const str &dir, long long mib)
{
	long long free = free_mib(dir);
	if (free < 0) die("cannot check the free space in %s", c(dir));
	if (free < mib + 4)
	{
		say("not enough free space in %s: %lld MiB needed, %lld MiB free. Nothing was changed.", c(dir), mib, free);
		exit(1);
	}
}

// put_saves <card> <saves folder>
void put_saves(const str &card, const str &saves)
{
	std::vector<str> names;
	if (!list_dir(saves, names) || names.empty()) return;
	if (!attach(card, WORK + "/mnt_new")) die("cannot mount %s", c(base(card)));
	long long n;
	try { n = merge(saves, WORK + "/mnt_new"); }
	catch (int e) { fprintf(stderr, "make_card: ERROR: cannot put the saves back: %s\n", strerror(e)); exit(1); }
	detach(WORK + "/mnt_new");
	str shown = ends_with(card, ".new") ? card.substr(0, card.size() - 4) : card;
	if (n) say("put the %lld saved file(s) back into %s", n, c(shown));
}

// the starting calibration (ZERO_PRESET), after the saves are back so that a calibration the player made wins
void zero_preset(const str &mnt, const str &name)
{
	char *end;
	long z = strtol(c(ZERO_PRESET), &end, 10);
	while (*end == ' ' || *end == '\t') end++;
	if (ZERO_PRESET.empty() || *end || z <= 0) return;
	int r = zerocfg(mnt, (int)z);
	if (r < 0) die("cannot write config/uvm2.cfg on %s", c(name));
	if (r == 1) say("%s: added the starting calibration zero %s (config/uvm2.cfg had no zero line)", c(name), c(ZERO_PRESET));
}

// old card to .bak (one backup, replaced each time), new card to its name
void swap(const str &card)
{
	str b = base(card);
	if (is_file(card))
	{
		unlink(c(card + ".bak"));
		if (rename(c(card), c(card + ".bak"))) die("cannot rename %s to %s.bak", c(b), c(b));
	}
	if (rename(c(card + ".new"), c(card)))
	{
		rename(c(card + ".bak"), c(card));
		die("cannot rename %s.new to %s", c(b), c(b));
	}
}

void fsck(const str &img, const str &name)
{
	if (run({ "fsck.vfat", "-n", img }, WORK + "/fsck.log"))
	{
		cat_out(WORK + "/fsck.log");
		die("the new card %s has errors (fsck.vfat)", c(name));
	}
}

// Scale cards, made from the finished sd.img: its files and saves, then the card's own saves (they win), then config/uvm2.cfg with the
// scale. Also on their own (--if-needed), without rebuilding sd.img.
void scale_cards()
{
	for (auto &n : SC_SCALES)
	{
		str t = UV + "/sd_scale" + n + ".img";
		need_on(UV, (fsize(OUT) + 1048575) / 1048576);
		saves_from(t, SAVES + "/scale" + n);
		unlink(c(t + ".new"));
		TMPFILES = { t + ".new" };
		if (copy_file(OUT, t + ".new", true)) die("cannot write %s.new", c(t));
		put_saves(t + ".new", SAVES + "/scale" + n);
		if (!attach(t + ".new", WORK + "/mnt_new")) die("cannot mount %s.new", c(base(t)));
		if (!scalecfg(WORK + "/mnt_new", atoi(c(n)))) die("cannot write config/uvm2.cfg on %s", c(base(t)));
		zero_preset(WORK + "/mnt_new", base(t));
		detach(WORK + "/mnt_new");
		if (in_path("fsck.vfat")) fsck(t + ".new", base(t));
		bool b = is_file(t);
		swap(t);
		TMPFILES.clear();
		if (is_file(OUT + ".list"))
		{
			if (!copy_file(OUT + ".list", t + ".list.new")) rename(c(t + ".list.new"), c(t + ".list"));
		}
		else unlink(c(t + ".list"));
		if (b) say("done: %s, the card for games with scale %s (the old one is %s.bak)", c(t), c(n), c(base(t)));
		else say("done: %s, the card for games with scale %s", c(t), c(n));
	}
	sync();
}

int build(const char *prog, const char *mode)
{
	const char *e;
	FAT = (e = getenv("PITREX_FAT")) && *e ? e : "/media/fat";
	T = (e = getenv("PITREX_TMP")) && *e ? e : "/tmp";
	ZERO_PRESET = (e = getenv("PITREX_ZERO_PRESET")) && *e ? e : "72";
	UV = FAT + "/pitrex/uvmc2";
	OUT = UV + "/sd.img";
	GAMES = FAT + "/games/Vectrex-PiTrex/UVMC2";
	UM2 = FAT + "/pitrex/um2run";
	ART_OLD = FAT + "/games/Vectrex/Overlays";
	ART_NEW = FAT + "/games/Vectrex-PiTrex/Overlays";

	// one folder for everything, unless an older install has files in the card folder
	CARD = GAMES;
	ONE = true;
	{
		std::vector<str> names;
		if (list_dir(UV + "/card", names))
			for (auto &n : names)
				if (n != "README.txt" && !ends_with(lower(n), ".lock")) { CARD = UV + "/card"; ONE = false; break; }
	}

	IF_NEEDED = !strcmp(mode, "--if-needed");
	CHECK = !strcmp(mode, "--check");
	if (*mode && !IF_NEEDED && !CHECK)
	{
		printf("usage: %s --make-card [--if-needed | --check]\n", prog);
		return 2;
	}

	// the scales the games' cards need (in game order, each once)
	for (auto &g : find_um2(GAMES))
	{
		str s = game_scale(g);
		if (!s.empty() && std::find(SC_SCALES.begin(), SC_SCALES.end(), s) == SC_SCALES.end()) SC_SCALES.push_back(s);
	}

	// --check: nothing to build, or the card (and the scale cards) match the folder: 0. A card made by hand (no .list) is left alone: 0.
	if (CHECK)
	{
		str list = list_of(CARD);
		if (list.empty()) return 0;
		if (!is_file(OUT)) return 1;
		if (!is_file(OUT + ".list")) return 0;
		if (list != cat_trim(OUT + ".list")) return 1;
		return scale_cards_ok() ? 0 : 1;
	}

	// 1. What is in the card folder
	{
		std::vector<str> names;
		if (!is_dir(CARD) || !list_dir(CARD, names) || names.empty())
		{
			blank_card();
			if (IF_NEEDED) return 0;
			say("the card folder %s is empty or missing, so there is nothing to put on a card.", c(CARD));
			say("Copy the files from your UVMC2 SD card (or just your ROM zips, in a roms folder) into it, then run this again.");
			say("See ADDING_GAMES.md in the PiTrex guide. Games that need no card (Veccy Road, the demos) play without one.");
			return 1;
		}
	}

	// One folder for the whole SD card: the .um2 games in an older card folder are not put on the card, they are copied (subfolders
	// kept) to the games folder, where the OSD finds them. Done on every run, also when the card needs no rebuild, and before the
	// BattleZone search below, so a BattleZone image that comes this way gets its card and sidecar on this run.
	mkdir_p(GAMES);
	int num2 = 0, ncopied = 0;
	if (!ONE)
	{
		std::vector<str> lines;
		um2_copy(CARD, "", true, GAMES, lines);
		for (auto &l : lines)
		{
			num2++;
			if (l[0] == 'C') ncopied++;
			else if (l[0] == 'D') say("%s: a different file of that name is already in %s, so it was not copied (left as it is)", c(l.substr(2)), c(GAMES));
			else if (l[0] == 'E') say("could not copy %s to %s", c(l.substr(2)), c(GAMES));
		}
	}
	if (ncopied) say("copied %d .um2 game(s) from the card folder to %s (the OSD's UVMC2 folder)", ncopied, c(GAMES));

	// Overlay art next to each game (no card rebuild for it: .art files are not on the card)
	for (auto &g : find_um2(GAMES))
	{
		str r = art_place(g);
		if (!r.empty()) say("overlay %s: %s", c(base(g)), c(r));
	}

	std::map<str, int> skips;
	std::vector<Ent> entries = walk(CARD, &skips);
	long long nfiles = 0, data = 0;
	for (auto &x : entries) if (!x.dir) { nfiles++; data += fsize(x.path); }
	long long mib = card_mib(fat_bytes(entries));
	const long long M = 1 << 20;
	char datas[64];
	if (data < M) snprintf(datas, sizeof(datas), "%lld KiB", (data + 1023) / 1024);
	else snprintf(datas, sizeof(datas), "%lld MiB", (data + M / 2) / M);
	str skipped;
	for (auto &s : skips) { if (!skipped.empty()) skipped += ", "; skipped += std::to_string(s.second) + " " + s.first; }
	if (skipped.empty()) skipped = "nothing";

	if (!nfiles)
	{
		if (ONE)
		{
			blank_card();
			if (IF_NEEDED) return 0;
			say("no card files in %s, only games: nothing to build.", c(CARD));
			return 0;
		}
		if (num2 > 0)
		{
			// only games: they are in the games folder now, and there is nothing to make a card from
			blank_card();
			if (IF_NEEDED && !ncopied) return 0;
			say("the card folder has only .um2 games, which go in the games folder, so there was nothing to build a card from.");
			say("Games that need ROMs or other files from the card: copy the rest of your UVMC2 SD card into %s too, then run this again.", c(CARD));
			return 0;
		}
		blank_card();
		if (IF_NEEDED) return 0;
		say("nothing to put on a card in %s (skipped: %s).", c(CARD), c(skipped));
		say("Copy the files from your UVMC2 SD card (or just your ROM zips, in a roms folder) into it, then run this again.");
		return 1;
	}

	// BattleZone images and the bz_sd.img each would use (normally one, <uvmc2>/bz_sd.img)
	std::vector<str> bz_imgs, bz_targets;
	if (!ONE) for (auto &g : find_um2(GAMES)) if (is_battlezone(g)) bz_imgs.push_back(g);
	for (auto &g : bz_imgs)
	{
		str t = resolve("bz_sd.img", dir_of(g));
		// a sidecar that names another card (or none): nothing would use bz_sd.img, so it is not made (reported at the end)
		if (is_file(g + ".cfg"))
		{
			str sd = cfg_get(g + ".cfg", "sd");
			if (sd.empty() || resolve(sd, dir_of(g)) != t) continue;
		}
		// Main and pitrexd refuse card paths with spaces, so such a card would never be used
		bool space = false;
		for (char ch : t) if (isspace((unsigned char)ch)) space = true;
		if (space) { say("BattleZone: %s has a space in its path, which pitrexd does not accept: not made", c(t)); continue; }
		if (std::find(bz_targets.begin(), bz_targets.end(), t) == bz_targets.end()) bz_targets.push_back(t);
	}

	str LIST = list_of(CARD);
	bool sc_only = false;
	if (IF_NEEDED && is_file(OUT))
	{
		bool bz_missing = false;
		for (auto &t : bz_targets) if (!is_file(t)) bz_missing = true;
		if (bz_missing) say("BattleZone has no card of its own yet: building the cards");
		else
		{
			bool uptodate = false;
			if (is_file(OUT + ".list"))
			{
				// built by this builder: rebuild if the folder's file list (names, sizes, times) changed
				uptodate = LIST == cat_trim(OUT + ".list");
			}
			else
			{
				// a card made some other way: rebuild only if the folder has a file newer than it
				struct stat so;
				stat(c(OUT), &so);
				std::vector<str> all;
				find_files(CARD, all);
				uptodate = true;
				for (auto &p : all)
				{
					str n = lower(base(p));
					if (ends_with(n, ".um2") || ends_with(n, ".art") || ends_with(n, ".lock")) continue;
					struct stat st;
					if (!stat(c(p), &st) && st.st_mtime > so.st_mtime) { uptodate = false; break; }
				}
			}
			if (uptodate)
			{
				// sd.img is fine: only the scale cards, if one is missing or was made from an older sd.img
				if (scale_cards_ok()) return 0;
				say("a game's card with its own scale is missing or older than sd.img: making it");
				sc_only = true;
			}
			else say("the card folder has changed since sd.img was made: building it again");
		}
	}

	if (!sc_only) say("card folder: %lld files, %s (skipped: %s)", nfiles, datas, c(skipped));
	if (!sc_only && mib > MAX_MIB)
	{
		say("the card would be %lld MiB, more than the %lld MiB this script makes.", mib, MAX_MIB);
		say("Remove files the games do not need from %s (for example ROM sets of games you do not have as .um2), then run this again.", c(CARD));
		return 1;
	}

	// 2. Checks before anything is written
	if (geteuid()) die("run this as root (it mounts the card image)");
	if (!in_path("mkfs.vfat")) die("mkfs.vfat not found");
	str lock = T + "/make_card.lock";
	if (mkdir(c(lock), 0777))
	{
		str old = cat_trim(lock + "/pid");
		if (!old.empty() && is_dir("/proc/" + old)) die("make_card is already running (process %s)", c(old));
		rm_rf(lock);
		if (mkdir(c(lock), 0777)) die("cannot create %s", c(lock));
	}
	LOCK = lock;
	write_file(LOCK + "/pid", std::to_string((int)getpid()) + "\n");
	if (!mkdir_p(UV)) die("cannot create %s", c(UV));

	// every card this run may write is locked first: a game's um2run that has one open makes the build stop here
	lock_card(OUT);
	for (auto &t : bz_targets) lock_card(t);
	for (auto &n : SC_SCALES) lock_card(UV + "/sd_scale" + n + ".img");

	// saves folders of a run that was killed hard (we hold the lock, so no other run uses them)
	{
		std::vector<str> names;
		if (list_dir(UV, names))
			for (auto &n : names) if (!n.compare(0, 11, ".make_card.") && is_dir(UV + "/" + n)) rm_rf(UV + "/" + n);
	}
	{
		str w = T + "/make_card.XXXXXX";
		if (!mkdtemp(&w[0])) die("cannot create a work folder in %s", c(T));
		WORK = w;
	}

	// 3. Saves from the old cards (see saves_out for what is kept). They go to a folder next to the cards, not to /tmp: on the
	// MiSTer /tmp is in RAM, and a card made by hand may carry big files.
	{
		str s = UV + "/.make_card.XXXXXX";
		if (!mkdtemp(&s[0])) die("cannot create a work folder in %s", c(UV));
		SAVES = s;
	}
	if (sc_only) { scale_cards(); return 0; }
	saves_from(OUT, SAVES + "/main");
	for (size_t i = 0; i < bz_targets.size(); i++) saves_from(bz_targets[i], SAVES + "/bz" + std::to_string(i + 1));

	// the card holds the folder's files plus the saves (the largest set of saves counts)
	{
		long long extra = 0;
		std::vector<str> names;
		list_dir(SAVES, names, false);
		for (auto &n : names)
		{
			std::vector<Ent> all;
			all_in(SAVES + "/" + n, "", all);
			extra = std::max(extra, fat_bytes(all));
		}
		if (names.empty()) extra = 512;
		mib = card_mib(fat_bytes(walk(CARD)) + extra);
	}
	if (mib > MAX_MIB)
	{
		say("with the saves of the old card, the card would be %lld MiB, more than the %lld MiB this script makes.", mib, MAX_MIB);
		say("Remove files the games do not need from %s, then run this again. Nothing was changed.", c(CARD));
		return 1;
	}

	// Free space: the new card and every BattleZone copy exist next to the old ones until the end.
	struct stat su;
	stat(c(UV), &su);
	long long need = mib;
	for (auto &t : bz_targets)
	{
		struct stat st;
		if (!stat(c(dir_of(t)), &st) && st.st_dev == su.st_dev) need += mib;
		else need_on(dir_of(t), mib);
	}
	need_on(UV, need);
	int secs = mib > BIG_MIB ? 8 : 1;
	say("card size: %lld MiB, FAT32 with %d-byte clusters", mib, secs * 512);

	// 4. Build the new card under a temporary name. Cluster size: FAT32 needs at least 65525 clusters. With 512-byte clusters (-s 1,
	// the layout um2run and the games were tested with) a 64 MiB card has about 129000; at 2000 MiB about 4 million. Bigger: 4 KiB.
	str NEW = OUT + ".new";
	unlink(c(NEW));
	TMPFILES = { NEW };
	if (!mkimg(NEW, mib)) die("cannot create %s", c(NEW));
	say("formatting");
	if (run({ "mkfs.vfat", "-F", "32", "-s", std::to_string(secs), "-n", LABEL, NEW }, WORK + "/mkfs.log"))
	{
		cat_out(WORK + "/mkfs.log");
		die("mkfs.vfat failed");
	}
	say("formatted");
	if (!attach(NEW, WORK + "/mnt_new")) die("cannot mount the new card image");
	long long copied = copy_tree(CARD, WORK + "/mnt_new");
	detach(WORK + "/mnt_new");
	say("copied %lld files", copied);

	// BattleZone copies that already exist get the new files plus their own saves: copy the card now, before sd.img's saves go in.
	// A BattleZone card made for the first time starts with sd.img's saves (below).
	std::vector<bool> bz_old(bz_targets.size());
	for (size_t i = 0; i < bz_targets.size(); i++)
	{
		str t = bz_targets[i];
		if (!is_file(t)) continue;
		unlink(c(t + ".new"));
		TMPFILES.push_back(t + ".new");
		if (copy_file(NEW, t + ".new", true)) die("cannot write %s.new", c(t));
		bz_old[i] = true;
	}

	put_saves(NEW, SAVES + "/main");

	for (size_t i = 0; i < bz_targets.size(); i++)
	{
		str t = bz_targets[i];
		if (!bz_old[i])
		{
			unlink(c(t + ".new"));
			TMPFILES.push_back(t + ".new");
			if (copy_file(NEW, t + ".new", true)) die("cannot write %s.new", c(t));
		}
		else put_saves(t + ".new", SAVES + "/bz" + std::to_string(i + 1));
	}

	// the starting calibration, on sd.img only: BattleZone's own cards are left alone (it zeroes with its own sequence, not the runtime's)
	if (!attach(NEW, WORK + "/mnt_new")) die("cannot mount the new card image");
	zero_preset(WORK + "/mnt_new", base(OUT));
	detach(WORK + "/mnt_new");

	// check what was built (fsck.vfat is part of dosfstools; skipped if missing)
	if (in_path("fsck.vfat"))
	{
		fsck(NEW, base(NEW));
		for (auto &t : bz_targets) fsck(t + ".new", base(t) + ".new");
		say("checked (fsck.vfat): no errors");
	}

	// 5. Swap in: old card to .bak (one backup, replaced each time), new card to its name
	bool had_old = is_file(OUT);
	swap(OUT);
	TMPFILES.clear();
	if (write_file(OUT + ".list.new", LIST + "\n")) rename(c(OUT + ".list.new"), c(OUT + ".list"));
	if (had_old) say("done: %s (the old one is sd.img.bak)", c(OUT));
	else say("done: %s", c(OUT));
	for (auto &t : bz_targets)
	{
		TMPFILES = { t + ".new" };
		bool b = is_file(t);
		swap(t);
		TMPFILES.clear();
		if (write_file(t + ".list.new", LIST + "\n")) rename(c(t + ".list.new"), c(t + ".list"));
		if (b) say("done: %s, BattleZone's own card (the old one is %s.bak)", c(t), c(base(t)));
		else say("done: %s, BattleZone's own card", c(t));
	}
	sync();
	scale_cards();

	// 6. BattleZone sidecars: write one only where there is none; never change an existing one
	for (auto &g : bz_imgs)
	{
		str name = base(g);
		if (is_file(g + ".cfg"))
		{
			str sd = cfg_get(g + ".cfg", "sd");
			if (sd.empty())
				say("BattleZone: %s.cfg has no sd= line, so this game uses sd.img (shared) and bz_sd.img was not made. To give it its own card, add the line sd=bz_sd.img and build again. The file was left as it is.", c(name));
			else if (resolve(sd, dir_of(g)) == resolve("bz_sd.img", dir_of(g)))
				say("BattleZone: %s.cfg already says sd=%s (left as it is)", c(name), c(sd));
			else
				say("BattleZone: %s.cfg says sd=%s, so bz_sd.img was not made. The file was left as it is; to use a card of its own, change it to sd=bz_sd.img and build again.", c(name), c(sd));
		}
		else if (write_file(g + ".cfg", "sd=bz_sd.img\n")) say("BattleZone: wrote %s.cfg with sd=bz_sd.img (its own card)", c(name));
		else say("BattleZone: could not write %s.cfg: add a file with the line sd=bz_sd.img next to the game yourself", c(name));
	}
	return 0;
}

} // namespace

int pitrex_card_main(const char *prog, const char *mode)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	atexit(cleanup);
	int r = build(prog, mode ? mode : "");
	check_sig();
	return r;
}
