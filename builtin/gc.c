/*
 * git gc builtin command
 *
 * Cleanup unreachable files and optimize the repository.
 *
 * Copyright (c) 2007 James Bowes
 *
 * Based on git-gc.sh, which is
 *
 * Copyright (c) 2006 Shawn O. Pearce
 */

#include "builtin.h"
#include "repository.h"
#include "config.h"
#include "tempfile.h"
#include "lockfile.h"
#include "parse-options.h"
#include "run-command.h"
#include "sigchain.h"
#include "argv-array.h"
#include "commit.h"
#include "commit-graph.h"
#include "packfile.h"
#include "object-store.h"
#include "pack.h"
#include "pack-objects.h"
#include "blob.h"
#include "tree.h"
#include "promisor-remote.h"
#include "remote.h"
#include "midx.h"
#include "refs.h"
#include "object-store.h"

#define FAILED_RUN "failed to run %s"

static const char * const builtin_gc_usage[] = {
	N_("git gc [<options>]"),
	NULL
};

static int pack_refs = 1;
static int prune_reflogs = 1;
static int aggressive_depth = 50;
static int aggressive_window = 250;
static int gc_auto_threshold = 6700;
static int gc_auto_pack_limit = 50;
static int detach_auto = 1;
static timestamp_t gc_log_expire_time;
static const char *gc_log_expire = "1.day.ago";
static const char *prune_expire = "2.weeks.ago";
static const char *prune_worktrees_expire = "3.months.ago";
static unsigned long big_pack_threshold;
static unsigned long max_delta_cache_size = DEFAULT_DELTA_CACHE_SIZE;

static struct argv_array pack_refs_cmd = ARGV_ARRAY_INIT;
static struct argv_array reflog = ARGV_ARRAY_INIT;
static struct argv_array repack = ARGV_ARRAY_INIT;
static struct argv_array prune = ARGV_ARRAY_INIT;
static struct argv_array prune_worktrees = ARGV_ARRAY_INIT;
static struct argv_array rerere = ARGV_ARRAY_INIT;

static struct tempfile *pidfile;
static struct lock_file log_lock;

static struct string_list pack_garbage = STRING_LIST_INIT_DUP;

static void clean_pack_garbage(void)
{
	int i;
	for (i = 0; i < pack_garbage.nr; i++)
		unlink_or_warn(pack_garbage.items[i].string);
	string_list_clear(&pack_garbage, 0);
}

static void report_pack_garbage(unsigned seen_bits, const char *path)
{
	if (seen_bits == PACKDIR_FILE_IDX)
		string_list_append(&pack_garbage, path);
}

static void process_log_file(void)
{
	struct stat st;
	if (fstat(get_lock_file_fd(&log_lock), &st)) {
		/*
		 * Perhaps there was an i/o error or another
		 * unlikely situation.  Try to make a note of
		 * this in gc.log along with any existing
		 * messages.
		 */
		int saved_errno = errno;
		fprintf(stderr, _("Failed to fstat %s: %s"),
			get_tempfile_path(log_lock.tempfile),
			strerror(saved_errno));
		fflush(stderr);
		commit_lock_file(&log_lock);
		errno = saved_errno;
	} else if (st.st_size) {
		/* There was some error recorded in the lock file */
		commit_lock_file(&log_lock);
	} else {
		/* No error, clean up any old gc.log */
		unlink(git_path("gc.log"));
		rollback_lock_file(&log_lock);
	}
}

static void process_log_file_at_exit(void)
{
	fflush(stderr);
	process_log_file();
}

static void process_log_file_on_signal(int signo)
{
	process_log_file();
	sigchain_pop(signo);
	raise(signo);
}

static int gc_config_is_timestamp_never(struct repository *r,
					const char *var)
{
	const char *value;
	timestamp_t expire;

	if (!repo_config_get_value(r, var, &value) && value) {
		if (parse_expiry_date(value, &expire))
			die(_("failed to parse '%s' value '%s'"), var, value);
		return expire == 0;
	}
	return 0;
}

static void gc_config(struct repository *r)
{
	const char *value;

	if (!repo_config_get_value(r, "gc.packrefs", &value)) {
		if (value && !strcmp(value, "notbare"))
			pack_refs = -1;
		else
			pack_refs = git_config_bool("gc.packrefs", value);
	}

	if (gc_config_is_timestamp_never(r, "gc.reflogexpire") &&
	    gc_config_is_timestamp_never(r, "gc.reflogexpireunreachable"))
		prune_reflogs = 0;

	repo_config_get_int(r, "gc.aggressivewindow", &aggressive_window);
	repo_config_get_int(r, "gc.aggressivedepth", &aggressive_depth);
	repo_config_get_int(r, "gc.auto", &gc_auto_threshold);
	repo_config_get_int(r, "gc.autopacklimit", &gc_auto_pack_limit);
	repo_config_get_bool(r, "gc.autodetach", &detach_auto);
	repo_config_get_expiry(r, "gc.pruneexpire", &prune_expire);
	repo_config_get_expiry(r, "gc.worktreepruneexpire", &prune_worktrees_expire);
	repo_config_get_expiry(r, "gc.logexpiry", &gc_log_expire);

	repo_config_get_ulong(r, "gc.bigpackthreshold", &big_pack_threshold);
	repo_config_get_ulong(r, "pack.deltacachesize", &max_delta_cache_size);

	repo_config(r, git_default_config, NULL);
}

static int too_many_loose_objects(struct repository *r)
{
	/*
	 * Quickly check if a "gc" is needed, by estimating how
	 * many loose objects there are.  Because SHA-1 is evenly
	 * distributed, we can check only one and get a reasonable
	 * estimate.
	 */
	DIR *dir;
	struct dirent *ent;
	int auto_threshold;
	int num_loose = 0;
	int needed = 0;
	const unsigned hexsz_loose = the_hash_algo->hexsz - 2;

	dir = opendir(repo_git_path(r, "objects/17"));
	if (!dir)
		return 0;

	auto_threshold = DIV_ROUND_UP(gc_auto_threshold, 256);
	while ((ent = readdir(dir)) != NULL) {
		if (strspn(ent->d_name, "0123456789abcdef") != hexsz_loose ||
		    ent->d_name[hexsz_loose] != '\0')
			continue;
		if (++num_loose > auto_threshold) {
			needed = 1;
			break;
		}
	}
	closedir(dir);
	return needed;
}

static struct packed_git *find_base_packs(struct repository *r,
					  struct string_list *packs,
					  unsigned long limit)
{
	struct packed_git *p, *base = NULL;

	for (p = get_all_packs(r); p; p = p->next) {
		if (!p->pack_local)
			continue;
		if (limit) {
			if (p->pack_size >= limit)
				string_list_append(packs, p->pack_name);
		} else if (!base || base->pack_size < p->pack_size) {
			base = p;
		}
	}

	if (base)
		string_list_append(packs, base->pack_name);

	return base;
}

static int too_many_packs(struct repository *r)
{
	struct packed_git *p;
	int cnt;

	if (gc_auto_pack_limit <= 0)
		return 0;

	for (cnt = 0, p = get_all_packs(r); p; p = p->next) {
		if (!p->pack_local)
			continue;
		if (p->pack_keep)
			continue;
		/*
		 * Perhaps check the size of the pack and count only
		 * very small ones here?
		 */
		cnt++;
	}
	return gc_auto_pack_limit < cnt;
}

static uint64_t total_ram(void)
{
#if defined(HAVE_SYSINFO)
	struct sysinfo si;

	if (!sysinfo(&si))
		return si.totalram;
#elif defined(HAVE_BSD_SYSCTL) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM))
	int64_t physical_memory;
	int mib[2];
	size_t length;

	mib[0] = CTL_HW;
# if defined(HW_MEMSIZE)
	mib[1] = HW_MEMSIZE;
# else
	mib[1] = HW_PHYSMEM;
# endif
	length = sizeof(int64_t);
	if (!sysctl(mib, 2, &physical_memory, &length, NULL, 0))
		return physical_memory;
#elif defined(GIT_WINDOWS_NATIVE)
	MEMORYSTATUSEX memInfo;

	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	if (GlobalMemoryStatusEx(&memInfo))
		return memInfo.ullTotalPhys;
#endif
	return 0;
}

static uint64_t estimate_repack_memory(struct packed_git *pack)
{
	unsigned long nr_objects = approximate_object_count();
	size_t os_cache, heap;

	if (!pack || !nr_objects)
		return 0;

	/*
	 * First we have to scan through at least one pack.
	 * Assume enough room in OS file cache to keep the entire pack
	 * or we may accidentally evict data of other processes from
	 * the cache.
	 */
	os_cache = pack->pack_size + pack->index_size;
	/* then pack-objects needs lots more for book keeping */
	heap = sizeof(struct object_entry) * nr_objects;
	/*
	 * internal rev-list --all --objects takes up some memory too,
	 * let's say half of it is for blobs
	 */
	heap += sizeof(struct blob) * nr_objects / 2;
	/*
	 * and the other half is for trees (commits and tags are
	 * usually insignificant)
	 */
	heap += sizeof(struct tree) * nr_objects / 2;
	/* and then obj_hash[], underestimated in fact */
	heap += sizeof(struct object *) * nr_objects;
	/* revindex is used also */
	heap += sizeof(struct revindex_entry) * nr_objects;
	/*
	 * read_sha1_file() (either at delta calculation phase, or
	 * writing phase) also fills up the delta base cache
	 */
	heap += delta_base_cache_limit;
	/* and of course pack-objects has its own delta cache */
	heap += max_delta_cache_size;

	return os_cache + heap;
}

static int keep_one_pack(struct string_list_item *item, void *data)
{
	argv_array_pushf(&repack, "--keep-pack=%s", basename(item->string));
	return 0;
}

static void add_repack_all_option(struct string_list *keep_pack)
{
	if (prune_expire && !strcmp(prune_expire, "now"))
		argv_array_push(&repack, "-a");
	else {
		argv_array_push(&repack, "-A");
		if (prune_expire)
			argv_array_pushf(&repack, "--unpack-unreachable=%s", prune_expire);
	}

	if (keep_pack)
		for_each_string_list(keep_pack, keep_one_pack, NULL);
}

static void add_repack_incremental_option(void)
{
	argv_array_push(&repack, "--no-write-bitmap-index");
}

static int need_to_gc(struct repository *r)
{
	/*
	 * Setting gc.auto to 0 or negative can disable the
	 * automatic gc.
	 */
	if (gc_auto_threshold <= 0)
		return 0;

	/*
	 * If there are too many loose objects, but not too many
	 * packs, we run "repack -d -l".  If there are too many packs,
	 * we run "repack -A -d -l".  Otherwise we tell the caller
	 * there is no need.
	 */
	if (too_many_packs(r)) {
		struct string_list keep_pack = STRING_LIST_INIT_NODUP;

		if (big_pack_threshold) {
			find_base_packs(r, &keep_pack, big_pack_threshold);
			if (keep_pack.nr >= gc_auto_pack_limit) {
				big_pack_threshold = 0;
				string_list_clear(&keep_pack, 0);
				find_base_packs(r, &keep_pack, 0);
			}
		} else {
			struct packed_git *p = find_base_packs(r, &keep_pack, 0);
			uint64_t mem_have, mem_want;

			mem_have = total_ram();
			mem_want = estimate_repack_memory(p);

			/*
			 * Only allow 1/2 of memory for pack-objects, leave
			 * the rest for the OS and other processes in the
			 * system.
			 */
			if (!mem_have || mem_want < mem_have / 2)
				string_list_clear(&keep_pack, 0);
		}

		add_repack_all_option(&keep_pack);
		string_list_clear(&keep_pack, 0);
	} else if (too_many_loose_objects(r))
		add_repack_incremental_option();
	else
		return 0;

	if (run_hook_le(NULL, "pre-auto-gc", NULL))
		return 0;
	return 1;
}

/* return NULL on success, else hostname running the gc */
static const char *lock_repo_for_gc(int force, pid_t* ret_pid)
{
	struct lock_file lock = LOCK_INIT;
	char my_host[HOST_NAME_MAX + 1];
	struct strbuf sb = STRBUF_INIT;
	struct stat st;
	uintmax_t pid;
	FILE *fp;
	int fd;
	char *pidfile_path;

	if (is_tempfile_active(pidfile))
		/* already locked */
		return NULL;

	if (xgethostname(my_host, sizeof(my_host)))
		xsnprintf(my_host, sizeof(my_host), "unknown");

	pidfile_path = git_pathdup("gc.pid");
	fd = hold_lock_file_for_update(&lock, pidfile_path,
				       LOCK_DIE_ON_ERROR);
	if (!force) {
		static char locking_host[HOST_NAME_MAX + 1];
		static char *scan_fmt;
		int should_exit;

		if (!scan_fmt)
			scan_fmt = xstrfmt("%s %%%ds", "%"SCNuMAX, HOST_NAME_MAX);
		fp = fopen(pidfile_path, "r");
		memset(locking_host, 0, sizeof(locking_host));
		should_exit =
			fp != NULL &&
			!fstat(fileno(fp), &st) &&
			/*
			 * 12 hour limit is very generous as gc should
			 * never take that long. On the other hand we
			 * don't really need a strict limit here,
			 * running gc --auto one day late is not a big
			 * problem. --force can be used in manual gc
			 * after the user verifies that no gc is
			 * running.
			 */
			time(NULL) - st.st_mtime <= 12 * 3600 &&
			fscanf(fp, scan_fmt, &pid, locking_host) == 2 &&
			/* be gentle to concurrent "gc" on remote hosts */
			(strcmp(locking_host, my_host) || !kill(pid, 0) || errno == EPERM);
		if (fp != NULL)
			fclose(fp);
		if (should_exit) {
			if (fd >= 0)
				rollback_lock_file(&lock);
			*ret_pid = pid;
			free(pidfile_path);
			return locking_host;
		}
	}

	strbuf_addf(&sb, "%"PRIuMAX" %s",
		    (uintmax_t) getpid(), my_host);
	write_in_full(fd, sb.buf, sb.len);
	strbuf_release(&sb);
	commit_lock_file(&lock);
	pidfile = register_tempfile(pidfile_path);
	free(pidfile_path);
	return NULL;
}

/*
 * Returns 0 if there was no previous error and gc can proceed, 1 if
 * gc should not proceed due to an error in the last run. Prints a
 * message and returns -1 if an error occurred while reading gc.log
 */
static int report_last_gc_error(struct repository *r)
{
	struct strbuf sb = STRBUF_INIT;
	int ret = 0;
	ssize_t len;
	struct stat st;
	struct strbuf gc_log_path = STRBUF_INIT;

	strbuf_repo_git_path(&gc_log_path, r, "gc.log");

	if (stat(gc_log_path.buf, &st)) {
		if (errno == ENOENT)
			goto done;

		ret = error_errno(_("cannot stat '%s'"), gc_log_path.buf);
		goto done;
	}

	if (st.st_mtime < gc_log_expire_time)
		goto done;

	len = strbuf_read_file(&sb, gc_log_path.buf, 0);
	if (len < 0)
		ret = error_errno(_("cannot read '%s'"), gc_log_path.buf);
	else if (len > 0) {
		/*
		 * A previous gc failed.  Report the error, and don't
		 * bother with an automatic gc run since it is likely
		 * to fail in the same way.
		 */
		warning(_("The last gc run reported the following. "
			       "Please correct the root cause\n"
			       "and remove %s.\n"
			       "Automatic cleanup will not be performed "
			       "until the file is removed.\n\n"
			       "%s"),
			    gc_log_path.buf, sb.buf);
		ret = 1;
	}
	strbuf_release(&sb);
done:
	strbuf_release(&gc_log_path);
	return ret;
}

static void gc_before_repack(void)
{
	/*
	 * We may be called twice, as both the pre- and
	 * post-daemonized phases will call us, but running these
	 * commands more than once is pointless and wasteful.
	 */
	static int done = 0;
	if (done++)
		return;

	if (pack_refs && run_command_v_opt(pack_refs_cmd.argv, RUN_GIT_CMD))
		die(FAILED_RUN, pack_refs_cmd.argv[0]);

	if (prune_reflogs && run_command_v_opt(reflog.argv, RUN_GIT_CMD))
		die(FAILED_RUN, reflog.argv[0]);
}

int cmd_gc(int argc, const char **argv, const char *prefix)
{
	struct repository *r = the_repository;
	int aggressive = 0;
	int auto_gc = 0;
	int quiet = 0;
	int force = 0;
	const char *name;
	pid_t pid;
	int daemonized = 0;
	int keep_base_pack = -1;
	timestamp_t dummy;

	struct option builtin_gc_options[] = {
		OPT__QUIET(&quiet, N_("suppress progress reporting")),
		{ OPTION_STRING, 0, "prune", &prune_expire, N_("date"),
			N_("prune unreferenced objects"),
			PARSE_OPT_OPTARG, NULL, (intptr_t)prune_expire },
		OPT_BOOL(0, "aggressive", &aggressive, N_("be more thorough (increased runtime)")),
		OPT_BOOL_F(0, "auto", &auto_gc, N_("enable auto-gc mode"),
			   PARSE_OPT_NOCOMPLETE),
		OPT_BOOL_F(0, "force", &force,
			   N_("force running gc even if there may be another gc running"),
			   PARSE_OPT_NOCOMPLETE),
		OPT_BOOL(0, "keep-largest-pack", &keep_base_pack,
			 N_("repack all other packs except the largest pack")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_gc_usage, builtin_gc_options);

	argv_array_pushl(&pack_refs_cmd, "pack-refs", "--all", "--prune", NULL);
	argv_array_pushl(&reflog, "reflog", "expire", "--all", NULL);
	argv_array_pushl(&repack, "repack", "-d", "-l", NULL);
	argv_array_pushl(&prune, "prune", "--expire", NULL);
	argv_array_pushl(&prune_worktrees, "worktree", "prune", "--expire", NULL);
	argv_array_pushl(&rerere, "rerere", "gc", NULL);

	/* default expiry time, overwritten in gc_config */
	gc_config(r);
	if (parse_expiry_date(gc_log_expire, &gc_log_expire_time))
		die(_("failed to parse gc.logexpiry value %s"), gc_log_expire);

	if (pack_refs < 0)
		pack_refs = !is_bare_repository();

	argc = parse_options(argc, argv, prefix, builtin_gc_options,
			     builtin_gc_usage, 0);
	if (argc > 0)
		usage_with_options(builtin_gc_usage, builtin_gc_options);

	if (prune_expire && parse_expiry_date(prune_expire, &dummy))
		die(_("failed to parse prune expiry value %s"), prune_expire);

	if (aggressive) {
		argv_array_push(&repack, "-f");
		if (aggressive_depth > 0)
			argv_array_pushf(&repack, "--depth=%d", aggressive_depth);
		if (aggressive_window > 0)
			argv_array_pushf(&repack, "--window=%d", aggressive_window);
	}
	if (quiet)
		argv_array_push(&repack, "-q");

	if (auto_gc) {
		/*
		 * Auto-gc should be least intrusive as possible.
		 */
		if (!need_to_gc(r))
			return 0;
		if (!quiet) {
			if (detach_auto)
				fprintf(stderr, _("Auto packing the repository in background for optimum performance.\n"));
			else
				fprintf(stderr, _("Auto packing the repository for optimum performance.\n"));
			fprintf(stderr, _("See \"git help gc\" for manual housekeeping.\n"));
		}
		if (detach_auto) {
			int ret = report_last_gc_error(r);
			if (ret < 0)
				/* an I/O error occurred, already reported */
				exit(128);
			if (ret == 1)
				/* Last gc --auto failed. Skip this one. */
				return 0;

			if (lock_repo_for_gc(force, &pid))
				return 0;
			gc_before_repack(); /* dies on failure */
			delete_tempfile(&pidfile);

			/*
			 * failure to daemonize is ok, we'll continue
			 * in foreground
			 */
			daemonized = !daemonize();
		}
	} else {
		struct string_list keep_pack = STRING_LIST_INIT_NODUP;

		if (keep_base_pack != -1) {
			if (keep_base_pack)
				find_base_packs(r, &keep_pack, 0);
		} else if (big_pack_threshold) {
			find_base_packs(r, &keep_pack, big_pack_threshold);
		}

		add_repack_all_option(&keep_pack);
		string_list_clear(&keep_pack, 0);
	}

	name = lock_repo_for_gc(force, &pid);
	if (name) {
		if (auto_gc)
			return 0; /* be quiet on --auto */
		die(_("gc is already running on machine '%s' pid %"PRIuMAX" (use --force if not)"),
		    name, (uintmax_t)pid);
	}

	if (daemonized) {
		hold_lock_file_for_update(&log_lock,
					  git_path("gc.log"),
					  LOCK_DIE_ON_ERROR);
		dup2(get_lock_file_fd(&log_lock), 2);
		sigchain_push_common(process_log_file_on_signal);
		atexit(process_log_file_at_exit);
	}

	gc_before_repack();

	if (!repository_format_precious_objects) {
		close_object_store(r->objects);
		if (run_command_v_opt(repack.argv, RUN_GIT_CMD))
			die(FAILED_RUN, repack.argv[0]);

		if (prune_expire) {
			argv_array_push(&prune, prune_expire);
			if (quiet)
				argv_array_push(&prune, "--no-progress");
			if (has_promisor_remote())
				argv_array_push(&prune,
						"--exclude-promisor-objects");
			if (run_command_v_opt(prune.argv, RUN_GIT_CMD))
				die(FAILED_RUN, prune.argv[0]);
		}
	}

	if (prune_worktrees_expire) {
		argv_array_push(&prune_worktrees, prune_worktrees_expire);
		if (run_command_v_opt(prune_worktrees.argv, RUN_GIT_CMD))
			die(FAILED_RUN, prune_worktrees.argv[0]);
	}

	if (run_command_v_opt(rerere.argv, RUN_GIT_CMD))
		die(FAILED_RUN, rerere.argv[0]);

	report_garbage = report_pack_garbage;
	reprepare_packed_git(r);
	if (pack_garbage.nr > 0) {
		close_object_store(r->objects);
		clean_pack_garbage();
	}

	prepare_repo_settings(r);
	if (r->settings.gc_write_commit_graph == 1)
		write_commit_graph_reachable(r->objects->odb,
					     !quiet && !daemonized ? COMMIT_GRAPH_WRITE_PROGRESS : 0,
					     NULL);

	if (auto_gc && too_many_loose_objects(r))
		warning(_("There are too many unreachable loose objects; "
			"run 'git prune' to remove them."));

	if (!daemonized)
		unlink(git_path("gc.log"));

	return 0;
}

#define MAX_NUM_TASKS 5

static const char * const builtin_maintenance_usage[] = {
	N_("git maintenance run [<options>]"),
	NULL
};

struct maintenance_opts {
	int auto_flag;
	int quiet;
	int tasks_selected;
} opts;

/* Remember to update object flag allocation in object.h */
#define PARENT1		(1u<<16)

static int num_commits_not_in_graph = 0;
static int limit_commits_not_in_graph = 100;

static int dfs_on_ref(const char *refname,
		      const struct object_id *oid, int flags,
		      void *cb_data)
{
	int result = 0;
	struct object_id peeled;
	struct commit_list *stack = NULL;
	struct commit *commit;

	if (!peel_ref(refname, &peeled))
		oid = &peeled;
	if (oid_object_info(the_repository, oid, NULL) != OBJ_COMMIT)
		return 0;

	commit = lookup_commit(the_repository, oid);
	if (!commit)
		return 0;
	if (parse_commit(commit))
		return 0;

	commit_list_append(commit, &stack);

	while (!result && stack) {
		struct commit_list *parent;

		commit = pop_commit(&stack);

		for (parent = commit->parents; parent; parent = parent->next) {
			if (parse_commit(parent->item) ||
			    commit_graph_position(parent->item) != COMMIT_NOT_FROM_GRAPH ||
			    parent->item->object.flags & PARENT1)
				continue;

			parent->item->object.flags |= PARENT1;
			num_commits_not_in_graph++;

			if (num_commits_not_in_graph >= limit_commits_not_in_graph) {
				result = 1;
				break;
			}

			commit_list_append(parent->item, &stack);
		}
	}

	free_commit_list(stack);
	return result;
}

static int should_write_commit_graph(struct repository *r)
{
	int result;

	repo_config_get_int(r, "maintenance.commit-graph.auto",
			    &limit_commits_not_in_graph);

	if (!limit_commits_not_in_graph)
		return 0;
	if (limit_commits_not_in_graph < 0)
		return 1;

	result = for_each_ref(dfs_on_ref, NULL);

	clear_commit_marks_all(PARENT1);

	return result;
}

static int run_write_commit_graph(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;

	argv_array_pushl(&cmd, "-C", r->worktree,
			 "commit-graph", "write",
			 "--split", "--reachable",
			 NULL);

	if (opts.quiet)
		argv_array_pushl(&cmd, "--no-progress", NULL);

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}

static int run_verify_commit_graph(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;

	argv_array_pushl(&cmd, "-C", r->worktree,
			 "commit-graph", "verify",
			 "--shallow", NULL);

	if (opts.quiet)
		argv_array_pushl(&cmd, "--no-progress", NULL);

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}

static int maintenance_task_commit_graph(struct repository *r)
{
	char *chain_path;

	/* Skip commit-graph when --auto is specified. */
	if (opts.auto_flag)
		return 0;

	close_object_store(r->objects);
	if (run_write_commit_graph(r)) {
		error(_("failed to write commit-graph"));
		return 1;
	}

	if (!run_verify_commit_graph(r))
		return 0;

	warning(_("commit-graph verify caught error, rewriting"));

	chain_path = get_commit_graph_chain_filename(r->objects->odb);
	if (unlink(chain_path)) {
		UNLEAK(chain_path);
		die(_("failed to remove commit-graph at %s"), chain_path);
	}
	free(chain_path);

	if (!run_write_commit_graph(r))
		return 0;

	error(_("failed to rewrite commit-graph"));
	return 1;
}

static int fetch_remote(struct repository *r, const char *remote)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	struct strbuf refmap = STRBUF_INIT;

	argv_array_pushl(&cmd, "-C", r->worktree,
			 "fetch", remote, "--prune",
			 "--no-tags", "--refmap=", NULL);

	strbuf_addf(&refmap, "+refs/heads/*:refs/hidden/%s/*", remote);
	argv_array_push(&cmd, refmap.buf);

	if (opts.quiet)
		argv_array_push(&cmd, "--quiet");

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);

	strbuf_release(&refmap);
	return result;
}

static int fill_each_remote(struct remote *remote, void *cbdata)
{
	struct string_list *remotes = (struct string_list *)cbdata;

	string_list_append(remotes, remote->name);
	return 0;
}

static int maintenance_task_fetch(struct repository *r)
{
	int result = 0;
	struct string_list_item *item;
	struct string_list remotes = STRING_LIST_INIT_DUP;

	if (for_each_remote(fill_each_remote, &remotes)) {
		error(_("failed to fill remotes"));
		result = 1;
		goto cleanup;
	}

	/*
	 * Do not modify the result based on the success of the 'fetch'
	 * operation, as a loss of network could cause 'fetch' to fail
	 * quickly. We do not want that to stop the rest of our
	 * background operations.
	 */
	for (item = remotes.items;
	     item && item < remotes.items + remotes.nr;
	     item++)
		fetch_remote(r, item->string);

cleanup:
	string_list_clear(&remotes, 0);
	return result;
}

static int maintenance_task_gc(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;

	argv_array_pushl(&cmd, "gc", NULL);

	if (opts.auto_flag)
		argv_array_pushl(&cmd, "--auto", NULL);
	if (opts.quiet)
		argv_array_pushl(&cmd, "--quiet", NULL);

	close_object_store(r->objects);
	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}


static int prune_packed(struct repository *r)
{
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "-C", r->worktree, "prune-packed", NULL);

	if (opts.quiet)
		argv_array_push(&cmd, "--quiet");

	return run_command_v_opt(cmd.argv, RUN_GIT_CMD);
}

struct write_loose_object_data {
	FILE *in;
	int count;
	int batch_size;
};

static int loose_object_auto_limit = 100;

static int loose_object_count(const struct object_id *oid,
			       const char *path,
			       void *data)
{
	int *count = (int*)data;
	if (++(*count) >= loose_object_auto_limit)
		return 1;
	return 0;
}

static int loose_object_auto_condition(struct repository *r)
{
	int count = 0;

	repo_config_get_int(r, "maintenance.loose-objects.auto",
			    &loose_object_auto_limit);

	if (!loose_object_auto_limit)
		return 0;
	if (loose_object_auto_limit < 0)
		return 1;

	return for_each_loose_file_in_objdir(r->objects->odb->path,
					     loose_object_count,
					     NULL, NULL, &count);
}

static int loose_object_exists(const struct object_id *oid,
			       const char *path,
			       void *data)
{
	return 1;
}

static int write_loose_object_to_stdin(const struct object_id *oid,
				       const char *path,
				       void *data)
{
	struct write_loose_object_data *d = (struct write_loose_object_data *)data;

	fprintf(d->in, "%s\n", oid_to_hex(oid));

	return ++(d->count) > d->batch_size;
}

static int pack_loose(struct repository *r)
{
	int result = 0;
	struct write_loose_object_data data;
	struct strbuf prefix = STRBUF_INIT;
	struct child_process *pack_proc;

	/*
	 * Do not start pack-objects process
	 * if there are no loose objects.
	 */
	if (!for_each_loose_file_in_objdir(r->objects->odb->path,
					   loose_object_exists,
					   NULL, NULL, NULL))
		return 0;

	pack_proc = xmalloc(sizeof(*pack_proc));

	child_process_init(pack_proc);

	strbuf_addstr(&prefix, r->objects->odb->path);
	strbuf_addstr(&prefix, "/pack/loose");

	argv_array_pushl(&pack_proc->args, "git", "-C", r->worktree,
			 "pack-objects", NULL);
	if (opts.quiet)
		argv_array_push(&pack_proc->args, "--quiet");
	argv_array_push(&pack_proc->args, prefix.buf);

	pack_proc->in = -1;

	if (start_command(pack_proc)) {
		error(_("failed to start 'git pack-objects' process"));
		result = 1;
		goto cleanup;
	}

	data.in = xfdopen(pack_proc->in, "w");
	data.count = 0;
	data.batch_size = 50000;

	for_each_loose_file_in_objdir(r->objects->odb->path,
				      write_loose_object_to_stdin,
				      NULL,
				      NULL,
				      &data);

	fclose(data.in);

	if (finish_command(pack_proc)) {
		error(_("failed to finish 'git pack-objects' process"));
		result = 1;
	}

cleanup:
	strbuf_release(&prefix);
	free(pack_proc);
	return result;
}

static int maintenance_task_loose_objects(struct repository *r)
{
	return prune_packed(r) || pack_loose(r);
}

static int pack_files_auto_condition(struct repository *r)
{
	struct packed_git *p;
	int enabled;
	int pack_files_auto_limit = 10;
	int count = 0;

	if (repo_config_get_bool(r, "core.multiPackIndex", &enabled) ||
	    !enabled)
		return 0;

	repo_config_get_int(r, "maintenance.pack-files.auto",
			    &pack_files_auto_limit);

	if (!pack_files_auto_limit)
		return 0;
	if (pack_files_auto_limit < 0)
		return 1;

	for (p = get_packed_git(r);
	     count < pack_files_auto_limit && p;
	     p = p->next) {
		if (!p->multi_pack_index)
			count++;
	}

	return count >= pack_files_auto_limit;
}

static int multi_pack_index_write(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "-C", r->worktree,
			 "multi-pack-index", "write", NULL);

	if (opts.quiet)
		argv_array_push(&cmd, "--no-progress");

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}

static int rewrite_multi_pack_index(struct repository *r)
{
	char *midx_name = get_midx_filename(r->objects->odb->path);

	unlink(midx_name);
	free(midx_name);

	if (multi_pack_index_write(r)) {
		error(_("failed to rewrite multi-pack-index"));
		return 1;
	}

	return 0;
}

static int multi_pack_index_verify(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "-C", r->worktree,
			 "multi-pack-index", "verify", NULL);

	if (opts.quiet)
		argv_array_push(&cmd, "--no-progress");

	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}

static int multi_pack_index_expire(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	argv_array_pushl(&cmd, "-C", r->worktree,
			 "multi-pack-index", "expire", NULL);

	if (opts.quiet)
		argv_array_push(&cmd, "--no-progress");

	close_object_store(r->objects);
	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	argv_array_clear(&cmd);

	return result;
}

#define TWO_GIGABYTES (2147483647)
#define UNSET_BATCH_SIZE ((unsigned long)-1)

static off_t get_auto_pack_size(struct repository *r)
{
	/*
	 * The "auto" value is special: we optimize for
	 * one large pack-file (i.e. from a clone) and
	 * expect the rest to be small and they can be
	 * repacked quickly.
	 *
	 * The strategy we select here is to select a
	 * size that is one more than the second largest
	 * pack-file. This ensures that we will repack
	 * at least two packs if there are three or more
	 * packs.
	 */
	off_t max_size = 0;
	off_t second_largest_size = 0;
	off_t result_size;
	struct packed_git *p;

	reprepare_packed_git(r);
	for (p = get_all_packs(r); p; p = p->next) {
		if (p->pack_size > max_size) {
			second_largest_size = max_size;
			max_size = p->pack_size;
		} else if (p->pack_size > second_largest_size)
			second_largest_size = p->pack_size;
	}

	result_size = second_largest_size + 1;

	/* But limit ourselves to a batch size of 2g */
	if (result_size > TWO_GIGABYTES)
		result_size = TWO_GIGABYTES;

	return result_size;
}

static int multi_pack_index_repack(struct repository *r)
{
	int result;
	struct argv_array cmd = ARGV_ARRAY_INIT;
	struct strbuf batch_arg = STRBUF_INIT;

	argv_array_pushl(&cmd, "-C", r->worktree,
			 "multi-pack-index", "repack", NULL);

	if (opts.quiet)
		argv_array_push(&cmd, "--no-progress");

	strbuf_addf(&batch_arg, "--batch-size=%"PRIuMAX,
			    (uintmax_t)get_auto_pack_size(r));
	argv_array_push(&cmd, batch_arg.buf);

	close_object_store(r->objects);
	result = run_command_v_opt(cmd.argv, RUN_GIT_CMD);
	strbuf_release(&batch_arg);

	if (result && multi_pack_index_verify(r)) {
		warning(_("multi-pack-index verify failed after repack"));
		result = rewrite_multi_pack_index(r);
	}

	return result;
}

static int maintenance_task_pack_files(struct repository *r)
{
	if (multi_pack_index_write(r)) {
		error(_("failed to write multi-pack-index"));
		return 1;
	}

	if (multi_pack_index_verify(r)) {
		warning(_("multi-pack-index verify failed after initial write"));
		return rewrite_multi_pack_index(r);
	}

	if (multi_pack_index_expire(r)) {
		error(_("multi-pack-index expire failed"));
		return 1;
	}

	if (multi_pack_index_verify(r)) {
		warning(_("multi-pack-index verify failed after expire"));
		return rewrite_multi_pack_index(r);
	}

	if (multi_pack_index_repack(r)) {
		error(_("multi-pack-index repack failed"));
		return 1;
	}

	return 0;
}

typedef int maintenance_task_fn(struct repository *r);

/*
 * An auto condition function returns 1 if the task should run
 * and 0 if the task should NOT run. See needs_to_gc() for an
 * example.
 */
typedef int maintenance_auto_fn(struct repository *r);

struct maintenance_task {
	struct hashmap_entry ent;
	const char *name;
	maintenance_task_fn *fn;
	maintenance_auto_fn *auto_condition;
	int task_order;
	unsigned enabled:1,
		 selected:1;
};

static int task_entry_cmp(const void *unused_cmp_data,
			  const struct hashmap_entry *eptr,
			  const struct hashmap_entry *entry_or_key,
			  const void *keydata)
{
	const struct maintenance_task *e1, *e2;
	const char *name = keydata;

	e1 = container_of(eptr, const struct maintenance_task, ent);
	e2 = container_of(entry_or_key, const struct maintenance_task, ent);

	return strcasecmp(e1->name, name ? name : e2->name);
}

struct maintenance_task *tasks[MAX_NUM_TASKS];
int num_tasks;
struct hashmap task_map;

static int compare_tasks_by_selection(const void *a_, const void *b_)
{
	const struct maintenance_task *a, *b;
	a = (const struct maintenance_task *)a_;
	b = (const struct maintenance_task *)b_;

	return b->task_order - a->task_order;
}

static int maintenance_run(struct repository *r)
{
	int i;
	int result = 0;
	struct lock_file lk;
	char *lock_path = xstrfmt("%s/maintenance", r->objects->odb->path);

	if (hold_lock_file_for_update(&lk, lock_path, LOCK_NO_DEREF) < 0) {
		/*
		 * Another maintenance command is running.
		 *
		 * If --auto was provided, then it is likely due to a
		 * recursive process stack. Do not report an error in
		 * that case.
		 */
		if (!opts.auto_flag && !opts.quiet)
			error(_("lock file '%s' exists, skipping maintenance"),
			      lock_path);
		free(lock_path);
		return 0;
	}
	free(lock_path);

	if (opts.tasks_selected)
		QSORT(tasks, num_tasks, compare_tasks_by_selection);

	for (i = 0; !result && i < num_tasks; i++) {
		if (opts.tasks_selected && !tasks[i]->selected)
			continue;

		if (!opts.tasks_selected && !tasks[i]->enabled)
			continue;

		if (opts.auto_flag &&
		    (!tasks[i]->auto_condition ||
		     !tasks[i]->auto_condition(r)))
			continue;

		result = tasks[i]->fn(r);
	}

	rollback_lock_file(&lk);
	return result;
}

static void initialize_tasks(struct repository *r)
{
	int i;
	struct strbuf config_name = STRBUF_INIT;
	num_tasks = 0;

	for (i = 0; i < MAX_NUM_TASKS; i++)
		tasks[i] = xcalloc(1, sizeof(struct maintenance_task));

	tasks[num_tasks]->name = "fetch";
	tasks[num_tasks]->fn = maintenance_task_fetch;
	num_tasks++;

	tasks[num_tasks]->name = "loose-objects";
	tasks[num_tasks]->fn = maintenance_task_loose_objects;
	tasks[num_tasks]->auto_condition = loose_object_auto_condition;
	num_tasks++;

	tasks[num_tasks]->name = "pack-files";
	tasks[num_tasks]->fn = maintenance_task_pack_files;
	tasks[num_tasks]->auto_condition = pack_files_auto_condition;
	num_tasks++;

	tasks[num_tasks]->name = "gc";
	tasks[num_tasks]->fn = maintenance_task_gc;
	tasks[num_tasks]->auto_condition = need_to_gc;
	tasks[num_tasks]->enabled = 1;
	num_tasks++;

	tasks[num_tasks]->name = "commit-graph";
	tasks[num_tasks]->fn = maintenance_task_commit_graph;
	tasks[num_tasks]->auto_condition = should_write_commit_graph;
	num_tasks++;

	hashmap_init(&task_map, task_entry_cmp, NULL, MAX_NUM_TASKS);

	for (i = 0; i < num_tasks; i++) {
		int config_value;

		hashmap_entry_init(&tasks[i]->ent,
				   strihash(tasks[i]->name));
		hashmap_add(&task_map, &tasks[i]->ent);

		strbuf_setlen(&config_name, 0);
		strbuf_addf(&config_name, "maintenance.%s.enabled", tasks[i]->name);

		if (!repo_config_get_bool(r, config_name.buf, &config_value))
			tasks[i]->enabled = config_value;
	}

	strbuf_release(&config_name);
}

static int task_option_parse(const struct option *opt,
			     const char *arg, int unset)
{
	struct maintenance_task *task;
	struct maintenance_task key;

	BUG_ON_OPT_NEG(unset);

	if (!arg || !strlen(arg)) {
		error(_("--task requires a value"));
		return 1;
	}

	opts.tasks_selected++;

	key.name = arg;
	hashmap_entry_init(&key.ent, strihash(key.name));

	task = hashmap_get_entry(&task_map, &key, ent, NULL);

	if (!task) {
		error(_("'%s' is not a valid task"), arg);
		return 1;
	}

	if (task->selected) {
		error(_("task '%s' cannot be selected multiple times"), arg);
		return 1;
	}

	task->selected = 1;
	task->task_order = opts.tasks_selected;

	return 0;
}

int cmd_maintenance(int argc, const char **argv, const char *prefix)
{
	struct repository *r = the_repository;

	static struct option builtin_maintenance_options[] = {
		OPT_BOOL(0, "auto", &opts.auto_flag,
			 N_("run tasks based on the state of the repository")),
		OPT_BOOL(0, "quiet", &opts.quiet,
			 N_("do not report progress or other information over stderr")),
		OPT_CALLBACK_F(0, "task", NULL, N_("task"),
			N_("run a specific task"),
			PARSE_OPT_NONEG, task_option_parse),
		OPT_END()
	};

	memset(&opts, 0, sizeof(opts));

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_maintenance_usage,
				   builtin_maintenance_options);

	opts.quiet = !isatty(2);
	gc_config(r);
	initialize_tasks(r);

	argc = parse_options(argc, argv, prefix,
			     builtin_maintenance_options,
			     builtin_maintenance_usage,
			     PARSE_OPT_KEEP_UNKNOWN);

	if (argc == 1) {
		if (!strcmp(argv[0], "run"))
			return maintenance_run(r);
	}

	usage_with_options(builtin_maintenance_usage,
			   builtin_maintenance_options);
}
