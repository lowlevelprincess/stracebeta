# mini-strace

A stripped-down clone of `strace`, built on Linux's `ptrace(2)` API.

## What it does right now

Runs a target program as a child process, attaches via ptrace, and
prints every syscall it makes along with the return value. just raw numbers.

## Build

```bash
make
./mini-strace /bin/echo hello world
```

Output looks something like:

```
syscall(  0) = 0
syscall(257) = 3
syscall( 12) = 94208
...
[mini-strace] process exited, code 0, total syscalls: 42
```

## How it works 

The child calls `PTRACE_TRACEME` then `SIGSTOP`s itself before `execvp`.
The parent waits for that stop, then loops on `PTRACE_SYSCALL`, which
pauses the child on every syscall entry and exit. `PTRACE_GETREGS` grabs
`orig_rax` (syscall number) on entry and `rax` (return value) on exit.


## Todo 

- syscall name table instead of raw numbers (there's like 300+ of them,
  probably going to generate this from unistd_64.h rather than typing
  it by hand)
- decode arguments using the ABI registers (rdi, rsi, rdx, r10, r8, r9)
- follow pointer args and print what they point to (strings, buffers) —
  this needs reading child memory via /proc/pid/mem or PEEKDATA
- turn raw errno numbers into names (ENOENT instead of 2)
- basic filtering, e.g. only show network or file syscalls
- attach to an already-running process by PID
- maybe a summary mode at the end (counts per syscall, like strace -c)

## Requirements

Linux x86-64, gcc, make. 
