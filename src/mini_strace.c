/*
 * mini-strace v0.1
 *
 * Minimal syscall tracer using ptrace(2). Runs a child process,
 * stops it on every syscall entry/exit, prints the syscall number
 * and return value.
 *
 * Build: make
 * Run:   ./mini-strace /bin/ls -la
 *
 * Next up:
 *   - decode args per x86-64 ABI (rdi, rsi, rdx, r10, r8, r9)
 *   - errno names instead of raw numbers
 *   - -e trace=network / -e trace=file filters
 *   - dereference pointer args (e.g. the buffer in write/open)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <errno.h>

#include "syscall_names.h"

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

    /* wait for the initial SIGSTOP from raise() above */
    waitpid(child, &status, 0);

    ptrace(PTRACE_SETOPTIONS, child, NULL, PTRACE_O_TRACESYSGOOD);

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) == -1) {
            perror("ptrace(SYSCALL)");
            break;
        }

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

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, child, NULL, &regs) == -1) {
            perror("ptrace(GETREGS)");
            break;
        }

        if (!in_syscall) {
            /* syscall entry */
            syscall_no = regs.orig_rax;
            printf("%s(%ld) ", syscall_name(syscall_no), syscall_no);
            fflush(stdout);
            call_count++;
            in_syscall = 1;
        } else {
            /* syscall exit — regs.rax now holds the return value */
            long ret = regs.rax;
            if (ret < 0)
                printf("= %ld (errno %ld)\n", ret, -ret);
            else
                printf("= %ld\n", ret);
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
