/*
 * mini-strace v0.7
 *
 * Minimal syscall tracer using ptrace(2). Runs a child process,
 * stops it on every syscall entry/exit, prints the syscall name,
 * its arguments, and the return value — with errno resolved to its
 * macro name on failure. Pointer arguments that are known to be
 * C-string paths (open, stat, execve, ...) get dereferenced and
 * printed as quoted strings instead of raw addresses.
 *
 * Supports x86-64 and ARM64 (aarch64) Linux. The two architectures
 * have completely different syscall ABIs — different register
 * names, different way to fetch registers via ptrace, and different
 * syscall numbering — so the register-fetching bits are behind
 * #ifdef __x86_64__ / __aarch64__.
 *
 * Build: make
 * Run:   ./mini-strace /bin/ls -la
 *
 * Next up:
 *   - -e trace=network / -e trace=file filters
 *   - buffer dumping for write/read (not just NUL-terminated paths)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <errno.h>

#if defined(__aarch64__)
#include <sys/uio.h>
#define NT_PRSTATUS_ 1  /* from linux/elf.h — avoids pulling in elf.h just for this */
#endif

#include "syscall_names.h"

/* Which arguments of which syscalls are NUL-terminated C-string
 * paths, as a bitmask over argument slots 0-5 (bit i set = arg i is
 * a char* path). Covers the common filesystem/exec syscalls; not
 * exhaustive — anything not in this table just prints as a raw hex
 * address, same as before. */
typedef struct {
    const char *name;
    unsigned char str_args;  /* bit i => argument i is a path string */
} string_arg_entry;

static const string_arg_entry string_arg_table[] = {
    { "open",        0x01 },
    { "openat",      0x02 },
    { "creat",       0x01 },
    { "access",      0x01 },
    { "faccessat",   0x02 },
    { "faccessat2",  0x02 },
    { "stat",        0x01 },
    { "lstat",       0x01 },
    { "newfstatat",  0x02 },
    { "statx",       0x02 },
    { "execve",      0x01 },
    { "execveat",    0x02 },
    { "unlink",      0x01 },
    { "unlinkat",    0x02 },
    { "mkdir",       0x01 },
    { "mkdirat",     0x02 },
    { "rmdir",       0x01 },
    { "chdir",       0x01 },
    { "rename",      0x03 },  /* args 0 and 1 */
    { "renameat",    0x0a },  /* args 1 and 3 */
    { "renameat2",   0x0a },
    { "readlink",    0x01 },
    { "readlinkat",  0x02 },
    { "chmod",       0x01 },
    { "fchmodat",    0x02 },
    { "chown",       0x01 },
    { "lchown",      0x01 },
    { "fchownat",    0x02 },
    { "truncate",    0x01 },
    { "statfs",      0x01 },
    { NULL,          0x00 },
};

static unsigned char string_arg_mask(const char *syscall) {
    for (int i = 0; string_arg_table[i].name != NULL; i++) {
        if (strcmp(string_arg_table[i].name, syscall) == 0)
            return string_arg_table[i].str_args;
    }
    return 0;
}

#define STR_ARG_BUF_LEN 200

/* Reads a NUL-terminated string out of the traced process's address
 * space, one machine word at a time, via PTRACE_PEEKDATA — the
 * classic way strace has always done this (predates /proc/pid/mem
 * as a read interface, and doesn't depend on /proc being mounted
 * with the right options). Falls back to printing the raw address
 * if the read fails for any reason (bad pointer, unmapped page,
 * race with the tracee tearing things down, ...) — a failed
 * dereference shouldn't crash the tracer, it should just degrade to
 * what v0.6 already did. */
static void read_child_string(pid_t pid, unsigned long long addr, char *out, size_t out_size) {
    if (addr == 0) {
        snprintf(out, out_size, "NULL");
        return;
    }

    unsigned char raw[STR_ARG_BUF_LEN];
    size_t got = 0;

    while (got + sizeof(long) <= sizeof(raw)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + got), NULL);
        if (word == -1 && errno != 0)
            break;  /* unmapped/invalid address — stop, use what we have */

        memcpy(raw + got, &word, sizeof(long));
        got += sizeof(long);

        int has_nul = 0;
        for (size_t i = 0; i < sizeof(long); i++) {
            if (((unsigned char *)&word)[i] == '\0') { has_nul = 1; break; }
        }
        if (has_nul)
            break;
    }

    if (got == 0) {
        snprintf(out, out_size, "0x%llx", addr);
        return;
    }

    size_t len = 0;
    while (len < got && raw[len] != '\0')
        len++;
    int truncated = (len == got);  /* string may continue past our read window */

    size_t oi = 0;
    if (oi < out_size) out[oi++] = '"';
    for (size_t i = 0; i < len && oi + 5 < out_size; i++) {
        unsigned char c = raw[i];
        if (c == '"' || c == '\\') {
            out[oi++] = '\\';
            out[oi++] = (char)c;
        } else if (c == '\n') {
            out[oi++] = '\\'; out[oi++] = 'n';
        } else if (c >= 0x20 && c < 0x7f) {
            out[oi++] = (char)c;
        } else {
            oi += (size_t)snprintf(out + oi, out_size - oi, "\\x%02x", c);
        }
    }
    if (oi < out_size) out[oi++] = '"';
    if (truncated && oi + 3 < out_size) {
        memcpy(out + oi, "...", 3);
        oi += 3;
    }
    if (oi < out_size) out[oi] = '\0';
    else out[out_size - 1] = '\0';
}

#if defined(__x86_64__)

typedef struct user_regs_struct arch_regs_t;

static int get_regs(pid_t child, arch_regs_t *regs) {
    return ptrace(PTRACE_GETREGS, child, NULL, regs);
}

static long syscall_no_of(const arch_regs_t *r)  { return r->orig_rax; }
static long return_val_of(const arch_regs_t *r)  { return r->rax; }
static unsigned long long arg0(const arch_regs_t *r) { return r->rdi; }
static unsigned long long arg1(const arch_regs_t *r) { return r->rsi; }
static unsigned long long arg2(const arch_regs_t *r) { return r->rdx; }
static unsigned long long arg3(const arch_regs_t *r) { return r->r10; }
static unsigned long long arg4(const arch_regs_t *r) { return r->r8; }
static unsigned long long arg5(const arch_regs_t *r) { return r->r9; }

#elif defined(__aarch64__)

typedef struct user_regs_struct arch_regs_t;

static int get_regs(pid_t child, arch_regs_t *regs) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_GETREGSET, child, (void *)(long)NT_PRSTATUS_, &iov);
}

/* aarch64 keeps the syscall number in x8 for both entry and exit
 * stops (unlike x86-64's separate orig_rax vs rax), and the return
 * value comes back in x0 — which doubles as the first argument slot
 * on entry, so we only read it as a return value after the call. */
static long syscall_no_of(const arch_regs_t *r)  { return r->regs[8]; }
static long return_val_of(const arch_regs_t *r)  { return r->regs[0]; }
static unsigned long long arg0(const arch_regs_t *r) { return r->regs[0]; }
static unsigned long long arg1(const arch_regs_t *r) { return r->regs[1]; }
static unsigned long long arg2(const arch_regs_t *r) { return r->regs[2]; }
static unsigned long long arg3(const arch_regs_t *r) { return r->regs[3]; }
static unsigned long long arg4(const arch_regs_t *r) { return r->regs[4]; }
static unsigned long long arg5(const arch_regs_t *r) { return r->regs[5]; }

#else
#error "mini-strace only supports x86-64 and ARM64 (aarch64) Linux"
#endif

static void run_tracee(char **argv) {
    /* let the parent trace us */
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
        perror("ptrace(TRACEME)");
        exit(1);
    }
    /* stop ourselves before exec so the tracer has time to catch up */
    raise(SIGSTOP);

    execvp(argv[0], argv);
    /* only reached if execvp failed */
    perror("execvp");
    exit(1);
}

static void run_tracer(pid_t child) {
    int status;
    int in_syscall = 0;      /* 0 = waiting for entry, 1 = waiting for exit */
    long syscall_no = -1;
    long call_count = 0;
    int pending_signal = 0;  /* signal to re-deliver to the child, if any */

    /* wait for the initial SIGSTOP from raise() above */
    waitpid(child, &status, 0);

    ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACESYSGOOD);

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, NULL, (void *)(long)pending_signal) == -1) {
            perror("ptrace(SYSCALL)");
            break;
        }
        pending_signal = 0;

        waitpid(child, &status, 0);

        if (WIFEXITED(status)) {
            fprintf(stderr, "\n[mini-strace] process exited, code %d, total syscalls: %ld\n",
                    WEXITSTATUS(status), call_count);
            break;
        }
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "\n[mini-strace] process killed by signal %d\n", WTERMSIG(status));
            break;
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }

        /* PTRACE_SYSCALL stops the child both on real syscall
         * boundaries *and* whenever a signal is about to be
         * delivered to it. PTRACE_O_TRACESYSGOOD (set above) makes
         * genuine syscall-stops report SIGTRAP with the high bit
         * set (SIGTRAP | 0x80) so we can tell the two apart —
         * without this check, a signal landing mid-trace desyncs
         * the entry/exit toggle and every arg/return value after
         * that point is garbage. */
        int stopsig = WSTOPSIG(status);
        if (stopsig != (SIGTRAP | 0x80)) {
            /* not a syscall-stop — remember to hand the signal back
             * to the child on the next PTRACE_SYSCALL so it doesn't
             * just get silently eaten */
            if (stopsig != SIGTRAP)
                pending_signal = stopsig;
            continue;
        }

        arch_regs_t regs;
        if (get_regs(child, &regs) == -1) {
            perror("ptrace(GETREGS)");
            break;
        }

        if (!in_syscall) {
            /* syscall entry — print the syscall name and its
             * arguments. Known path-string arguments (per
             * string_arg_table) get dereferenced from the child's
             * memory and printed quoted; everything else prints as
             * a raw hex address/value, same as before. */
            syscall_no = syscall_no_of(&regs);
            const char *name = syscall_name(syscall_no);
            unsigned long long raw_args[6] = {
                arg0(&regs), arg1(&regs), arg2(&regs),
                arg3(&regs), arg4(&regs), arg5(&regs),
            };
            unsigned char str_mask = string_arg_mask(name);

            char argbuf[6][STR_ARG_BUF_LEN];
            for (int i = 0; i < 6; i++) {
                if (str_mask & (1 << i))
                    read_child_string(child, raw_args[i], argbuf[i], sizeof(argbuf[i]));
                else
                    snprintf(argbuf[i], sizeof(argbuf[i]), "0x%llx", raw_args[i]);
            }

            printf("%s(%s, %s, %s, %s, %s, %s) ",
                   name, argbuf[0], argbuf[1], argbuf[2],
                   argbuf[3], argbuf[4], argbuf[5]);
            fflush(stdout);
            call_count++;
            in_syscall = 1;
        } else {
            /* syscall exit — return value in whichever register the
             * platform uses for it */
            long ret = return_val_of(&regs);
            if (ret < 0) {
                const char *ename = strerrorname_np((int)(-ret));
                printf("= %ld (%s)\n", ret, ename ? ename : "unknown errno");
            } else {
                printf("= %ld\n", ret);
            }
            in_syscall = 0;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
        fprintf(stderr, "example: %s /bin/echo hello\n", argv[0]);
        return 1;
    }

    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        run_tracee(&argv[1]);
    } else {
        run_tracer(child);
    }

    return 0;
}
