/*
 * mini-strace v0.5
 *
 * Minimal syscall tracer using ptrace(2). Runs a child process,
 * stops it on every syscall entry/exit, prints the syscall name,
 * its raw arguments, and the return value — with errno resolved
 * to its macro name on failure.
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
 *   - dereference pointer args (e.g. the buffer in write/open) —
 *     right now pointers just print as raw hex addresses
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
            /* syscall entry — print the syscall name and its raw
             * argument registers, per the platform's syscall ABI */
            syscall_no = syscall_no_of(&regs);
            printf("%s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) ",
                   syscall_name(syscall_no),
                   arg0(&regs), arg1(&regs), arg2(&regs),
                   arg3(&regs), arg4(&regs), arg5(&regs));
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
