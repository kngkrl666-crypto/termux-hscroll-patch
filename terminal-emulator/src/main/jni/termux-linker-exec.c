/*
 * LD_PRELOAD library that intercepts execve() to route Termux binaries
 * through /system/bin/linker64, bypassing Android's SELinux W^X restriction
 * on app data directories (enforced for targetSdkVersion >= 29).
 *
 * Also remaps /bin/X and /usr/bin/X to the Termux prefix.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__LP64__)
#define SYSTEM_LINKER "/system/bin/linker64"
#else
#define SYSTEM_LINKER "/system/bin/linker"
#endif

#define TERMUX_BASE "/data/data/com.termux/"
#define TERMUX_BIN  "/data/data/com.termux/files/usr/bin/"

static int (*original_execve)(const char*, char* const[], char* const[]) = NULL;

__attribute__((constructor))
static void init(void) {
    original_execve = (int (*)(const char*, char* const[], char* const[]))dlsym(RTLD_NEXT, "execve");
}

static int is_elf(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char magic[4];
    int ok = (read(fd, magic, 4) == 4 &&
              magic[0] == 0x7f && magic[1] == 'E' &&
              magic[2] == 'L' && magic[3] == 'F');
    close(fd);
    return ok;
}

static int is_in_termux(const char* path) {
    return path && strncmp(path, TERMUX_BASE, sizeof(TERMUX_BASE) - 1) == 0;
}

/* Remap /bin/X, /usr/bin/X, /usr/lib/X to termux prefix.
 * Returns the remapped path (in buf) or the original path if no remap needed. */
static const char* remap_path(const char* path, char* buf, size_t bufsz) {
    if (!path) return path;
    const char* rest = NULL;
    if (strncmp(path, "/usr/bin/", 9) == 0)
        rest = path + 9;
    else if (strncmp(path, "/bin/", 5) == 0)
        rest = path + 5;
    else if (strncmp(path, "/usr/lib/", 9) == 0) {
        snprintf(buf, bufsz, "/data/data/com.termux/files/usr/lib/%s", path + 9);
        return buf;
    }
    if (rest) {
        snprintf(buf, bufsz, "%s%s", TERMUX_BIN, rest);
        return buf;
    }
    return path;
}

static int exec_via_linker(const char* binary, char* const argv[], char* const envp[]) {
    int argc = 0;
    while (argv && argv[argc]) argc++;

    char** new_argv = (char**)malloc((argc + 2) * sizeof(char*));
    if (!new_argv) { errno = ENOMEM; return -1; }

    new_argv[0] = (char*)SYSTEM_LINKER;
    new_argv[1] = (char*)binary;
    for (int i = 1; i <= argc; i++)
        new_argv[i + 1] = argv[i];

    int ret = original_execve(SYSTEM_LINKER, new_argv, envp);
    int saved = errno;
    free(new_argv);
    errno = saved;
    return ret;
}

/* Parse a #! shebang line and exec the interpreter via linker64. */
static int exec_script_via_linker(const char* script, char* const argv[], char* const envp[]) {
    int fd = open(script, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        errno = EACCES;
        return -1;
    }

    buf[n] = '\0';
    char* nl = strchr(buf + 2, '\n');
    if (nl) *nl = '\0';

    /* Extract interpreter path and optional argument */
    char* interp = buf + 2;
    while (*interp == ' ' || *interp == '\t') interp++;

    char* interp_arg = NULL;
    char* sp = strchr(interp, ' ');
    if (sp) {
        *sp = '\0';
        interp_arg = sp + 1;
        while (*interp_arg == ' ' || *interp_arg == '\t') interp_arg++;
        if (*interp_arg == '\0') interp_arg = NULL;
    }

    /* Remap interpreter path (e.g. /bin/bash -> termux prefix) */
    char resolved[PATH_MAX];
    interp = (char*)remap_path(interp, resolved, sizeof(resolved));

    if (!is_elf(interp)) {
        errno = EACCES;
        return -1;
    }

    /* Build argv: [LINKER, interp, [interp_arg], script, original_argv[1:], NULL] */
    int argc = 0;
    while (argv && argv[argc]) argc++;

    int extra = 3 + (interp_arg ? 1 : 0);
    char** new_argv = (char**)malloc((argc + extra) * sizeof(char*));
    if (!new_argv) { errno = ENOMEM; return -1; }

    int idx = 0;
    new_argv[idx++] = (char*)SYSTEM_LINKER;
    new_argv[idx++] = interp;
    if (interp_arg) new_argv[idx++] = interp_arg;
    new_argv[idx++] = (char*)script;
    for (int i = 1; i <= argc; i++)
        new_argv[idx++] = argv[i];

    int ret = original_execve(SYSTEM_LINKER, new_argv, envp);
    int saved = errno;
    free(new_argv);
    errno = saved;
    return ret;
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (!original_execve)
        original_execve = (int (*)(const char*, char* const[], char* const[]))dlsym(RTLD_NEXT, "execve");

    if (!pathname)
        return original_execve(pathname, argv, envp);

    /* Step 1: Remap /bin/X and /usr/bin/X to termux prefix */
    char remap_buf[PATH_MAX];
    const char* effective = remap_path(pathname, remap_buf, sizeof(remap_buf));

    /* Step 2: If not in termux data dir, pass through */
    if (!is_in_termux(effective))
        return original_execve(effective, argv, envp);

    /* Step 3: ELF binary — exec via linker64 */
    if (is_elf(effective))
        return exec_via_linker(effective, argv, envp);

    /* Step 4: Non-ELF (script etc.) — try direct exec first */
    int ret = original_execve(effective, argv, envp);

    /* Step 5: If EACCES, parse shebang and exec interpreter via linker64 */
    if (errno == EACCES)
        return exec_script_via_linker(effective, argv, envp);

    return ret;
}
