/* monolithctl - send a command to a running MONOLITH over its IPC socket.
 *
 *   monolithctl slot.next | slot.prev | slot.goto N | slot.recent
 *   monolithctl slot.move-left | slot.move-right | slot.swap N
 *   monolithctl windows-mode.toggle | window.close | exec CMD...
 *   monolithctl reload | query | quit
 *
 * Socket: $MONOLITH_SOCK, else $XDG_RUNTIME_DIR/monolith-$WAYLAND_DISPLAY.sock
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fprintf(stderr, "usage: monolithctl <action> [args...]   (e.g. slot.next, query, reload)\n");
		return argc < 2 ? 1 : 0;
	}

	char path[256];
	const char *env = getenv("MONOLITH_SOCK");
	if (env && *env) {
		snprintf(path, sizeof path, "%s", env);
	} else {
		const char *rt = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY");
		if (!rt || !wd) {
			fprintf(stderr, "monolithctl: set MONOLITH_SOCK (printed by monolith at startup)\n");
			return 1;
		}
		snprintf(path, sizeof path, "%s/monolith-%s.sock", rt, wd);
	}

	char cmd[512] = "";
	for (int i = 1; i < argc; i++) {
		if (strlen(cmd) + strlen(argv[i]) + 2 >= sizeof cmd) {
			fprintf(stderr, "monolithctl: command too long\n");
			return 1;
		}
		if (i > 1) strcat(cmd, " ");
		strcat(cmd, argv[i]);
	}

	if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		fprintf(stderr, "monolithctl: socket path too long\n");
		return 1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	memcpy(addr.sun_path, path, strlen(path)); /* length checked above, struct is zeroed */
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		perror(path);
		return 1;
	}
	if (write(fd, cmd, strlen(cmd)) < 0) { perror("write"); return 1; }

	char out[2048];
	ssize_t r, total = 0;
	while ((r = read(fd, out + total, sizeof out - 1 - (size_t)total)) > 0) total += r;
	out[total] = '\0';
	fputs(out, stdout);
	close(fd);
	return strncmp(out, "error", 5) == 0 ? 1 : 0;
}
