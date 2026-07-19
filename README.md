# stracebeta

A stripped-down clone of `strace`, built on Linux's `ptrace(2)` API.

## What it does

Traces a target program and prints every syscall: name, arguments,
return value. Failed calls show the errno name (`ENOENT`, not `2`).
`open`/`stat`/`execve` and friends show their path argument as an actual string
instead of a pointer, and `read`/`write` show the bytes they're
moving:

```
execve("/bin/cat", 0x7fff39fbada0, 0x7fff39fbadc0, 0x0, 0xffffffff, 0x7f45c4f4c740) = 0
access("/etc/ld.so.preload", 0x4, 0x556158984d10, 0x22, 0x7ff5eca8b000, 0x7ff5ecac1440) = -2 (ENOENT)
openat(0xffffff9c, "/etc/ld.so.cache", 0x80000, 0x0, 0x0, 0x0) = 3
openat(0xffffff9c, "/tmp/somefile.txt", 0x0, 0x0, 0xffffffff, 0x0) = 3
read(0x3, "test data content\n", 0x20000, 0x22, 0x0, 0x7fe3515c0440) = 18
write(0x1, "test data content\n", 0x12, 0x22, 0x0, 0x7fe3515c0440) = 18
read(0x3, 0x7fa474eae000, 0x20000, 0x22, 0x0, 0x7fa474f1f440) = 0
[mini-strace] process exited, code 0, total syscalls: 37
```


## Build & run

```bash
make
./mini-strace /bin/echo hello world
```

On macOS:

```bash
docker compose run --rm dev
make
./mini-strace /bin/echo hello world
```

## How it works

The child does `PTRACE_TRACEME` then `SIGSTOP`s itself before
`execvp`. The parent waits for that, then loops on `PTRACE_SYSCALL`,
which stops the child at every syscall entry and exit and lets the
tracer read its registers in between.

`scripts/gen_syscall_table.sh` asks gcc to preprocess the platform's syscall header and pulls the
`__NR_*` defines out of that, which sidesteps having to hardcode a
header path . A few syscalls — `mmap`, `stat`, `fstat` aren't a plain number in the
header, they're defined through a level of indirection (`__NR_mmap`
-> `__NR3264_mmap` -> `222`), so the generator follows that chain. The
table gets regenerated on every `make`, since it has to match
whatever machine you're actually building on and isn't committed.

Path arguments and buffer contents get pulled out of the child's
memory one word at a time via `PTRACE_PEEKDATA`, the same mechanism
`strace` used long before `/proc/pid/mem` existed as a read interface.

`read()` needed a different approach than `write()`. `write()`'s
buffer already has real data at the entry-stop, so it prints
immediately like everything else. `read()`'s buffer is empty at that
point, the kernel hasn't run yet, so for that one the tracer holds
off printing anything at entry, waits for the exit-stop, and uses the
return value (the actual byte count, usually less than what was
requested) as the dump length.

## Todo

- `-e trace=network` / `-e trace=file` style filters
- attach to an already-running process by PID
- summary mode at the end, counts per syscall (like `strace -c`)

## Requirements

Linux, x86-64 or ARM64, gcc, make, python3 (only used by the syscall
table generator).
