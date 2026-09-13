#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>

static void die(const char *message)
{
	fprintf(stderr, "criu-shim: %s\n", message);
	exit(EXIT_FAILURE);
}

static const char *option_value(int argc, char **argv, const char *short_name,
				const char *long_name)
{
	int i;

	for (i = 2; i + 1 < argc; i++) {
		if (!strcmp(argv[i], short_name) || !strcmp(argv[i], long_name))
			return argv[i + 1];
	}
	return NULL;
}

static const char *option_value_equals(int argc, char **argv,
					const char *long_name)
{
	int i;
	size_t length = strlen(long_name);

	for (i = 2; i < argc; i++)
		if (!strncmp(argv[i], long_name, length) && argv[i][length] == '=')
			return argv[i] + length + 1;
	return NULL;
}

static void ensure_log_parent(const char *path)
{
	char parent[PATH_MAX];
	char *slash;

	if (!path || strlen(path) >= sizeof(parent))
		return;
	strcpy(parent, path);
	slash = strrchr(parent, '/');
	if (!slash)
		return;
	*slash = '\0';
	if (*parent)
		mkdir(parent, 0755);
}

static int run_converter(const char *converter, const char *snapshot,
			 const char *images)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return -1;
	if (!child) {
		execl(converter, converter, snapshot, "-D", images, (char *)NULL);
		_exit(127);
	}
	if (waitpid(child, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int dump(int argc, char **argv)
{
	const char *pid = option_value(argc, argv, "-t", "--tree");
	const char *images = option_value(argc, argv, "-D", "--images-dir");
	const char *log_file = option_value(argc, argv, "-l", "--log-file");
	const char *debug_dir = getenv("CRIU_DEBUG_DIR");
	const char *converter = getenv("CRIU_CONVERTER");
	char dump_path[PATH_MAX];
	char snapshot[PATH_MAX];
	FILE *dump_file;
	FILE *target_file;
	FILE *log_stream;
	int rc;

	if (!log_file)
		log_file = option_value_equals(argc, argv, "--log-file");
	/* zdtm.py always consumes --log-file, including when dump is rejected. */
	if (log_file) {
		ensure_log_parent(log_file);
		log_stream = fopen(log_file, "a");
		if (!log_stream)
			return EXIT_FAILURE;
		fprintf(log_stream, "criu-shim: kernel-module dump path\n");
		fclose(log_stream);
	}

	if (!pid || !images)
		die("dump requires --tree/-t and --images-dir/-D");
	if (!debug_dir)
		debug_dir = "/sys/kernel/debug/criu";
	if (!converter)
		converter = "criu-module-convert";
	if (snprintf(dump_path, sizeof(dump_path), "%s/dump", debug_dir) >=
	    (int)sizeof(dump_path))
		die("debugfs dump path is too long");
	{
		char target_path[PATH_MAX];
		if (snprintf(target_path, sizeof(target_path), "%s/target", debug_dir) >=
		    (int)sizeof(target_path))
			die("debugfs target path is too long");
		target_file = fopen(target_path, "w");
		if (!target_file) {
			fprintf(stderr, "criu-shim: cannot select target %s: %s\n",
				pid, strerror(errno));
			return EXIT_FAILURE;
		}
		if (fprintf(target_file, "%s\n", pid) < 0 || fclose(target_file) != 0) {
			fprintf(stderr, "criu-shim: cannot select target %s: %s\n",
				pid, strerror(errno));
			return EXIT_FAILURE;
		}
	}
	if (snprintf(snapshot, sizeof(snapshot), "%s/.criu-module-snapshot.bin",
		     images) >= (int)sizeof(snapshot))
		die("snapshot path is too long");

	dump_file = fopen(dump_path, "w");
	if (!dump_file) {
		fprintf(stderr, "criu-shim: cannot open %s: %s\n", dump_path,
			strerror(errno));
		return EXIT_FAILURE;
	}
	if (fprintf(dump_file, "%s %s\n", pid, snapshot) < 0 ||
	    fclose(dump_file) != 0) {
		fprintf(stderr, "criu-shim: cannot request module dump: %s\n",
			strerror(errno));
		return EXIT_FAILURE;
	}

	rc = run_converter(converter, snapshot, images);
	if (rc != 0) {
		fprintf(stderr, "criu-shim: converter failed (rc=%d)\n", rc);
		return EXIT_FAILURE;
	}
	if (log_file) {
		log_stream = fopen(log_file, "a");
		if (!log_stream)
			return EXIT_FAILURE;
		fclose(log_stream);
	}
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *real_criu;

	if (argc < 2)
		die("missing CRIU action");
	if (!strcmp(argv[1], "dump"))
		return dump(argc, argv);

	real_criu = getenv("CRIU_REAL_BIN");
	if (!real_criu)
		real_criu = "criu";
	argv[0] = (char *)real_criu;
	execvp(real_criu, argv);
	fprintf(stderr, "criu-shim: cannot execute %s: %s\n", real_criu,
		strerror(errno));
	return EXIT_FAILURE;
}
