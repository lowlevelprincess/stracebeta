# mini-strace

A stripped-down clone of `strace`, built on Linux's `ptrace(2)` API

## What it does right now

Runs a target program as a child process, attaches via ptrace, and
prints every syscall it makes: name, arguments, and return value
(errno resolved to its macro name on failure). Pointer arguments that
are known to be C-string paths (`open`, `stat`, `execve`, ...) get
dereferenced and printed as quoted strings instead of raw addresses.
`write()`'s output buffer gets dumped the same way. Works on x86-64
and ARM64 Linux.

## Build

```bash
make
./mini-strace /bin/echo hello world
```

Real output, tracing `/bin/cat /tmp/somefile.txt`:

```
execve("/bin/cat", 0x7fff39fbada0, 0x7fff39fbadc0, 0x0, 0xffffffff, 0x7f45c4f4c740) = 0
access("/etc/ld.so.preload", 0x4, 0x556158984d10, 0x22, 0x7ff5eca8b000, 0x7ff5ecac1440) = -2 (ENOENT)
openat(0xffffff9c, "/etc/ld.so.cache", 0x80000, 0x0, 0x0, 0x0) = 3
openat(0xffffff9c, "/lib/x86_64-linux-gnu/libc.so.6", 0x80000, 0x0, 0x0, 0x0) = 3
openat(0xffffff9c, "/tmp/somefile.txt", 0x0, 0x0, 0xffffffff, 0x0) = 3
read(0x3, 0x7fe35154f000, 0x20000, 0x22, 0x0, 0x7fe3515c0440) = 10
write(0x1, "test data\n", 0xa, 0x22, 0x0, 0x7fe3515c0440) = 10
[mini-strace] process exited, code 0, total syscalls: 37
```

Arguments not recognized as path strings or write buffers still print
raw, per the ABI slot they came from — a `struct stat*` buffer, for
instance, is just a hex address for now. Failed calls resolve errno
to its macro name via glibc's `strerrorname_np()`.

## How it works (briefly)

The child calls `PTRACE_TRACEME` then `SIGSTOP`s itself before `execvp`.
The parent waits for that stop, then loops on `PTRACE_SYSCALL`, which
pauses the child on every syscall entry and exit, reading registers to
get the syscall number, arguments, and return value.

x86-64 and ARM64 do this completely differently, different register
names, `PTRACE_GETREGS` vs `PTRACE_GETREGSET`, different argument
registers, different syscall numbering entirely. All of that is behind
`#ifdef __x86_64__` / `#ifdef __aarch64__` in `mini_strace.c`.

`src/syscall_names.h` is generated per-architecture by
`scripts/gen_syscall_table.sh` (it asks gcc to preprocess the
platform's syscall header rather than reading a hardcoded path, since
that path differs across toolchains) and rebuilt automatically by
`make` every time, since the file isn't committed — it has to match
whatever machine you're actually building on. A few syscalls (`mmap`,
`stat`, `fstat`, ...) aren't defined as a plain number in the header —
they're defined through a level of indirection (`__NR_mmap` →
`__NR3264_mmap` → `222`), so the generator resolves that chain instead
of just grepping for digits.

Path-string arguments are read out of the child's memory one machine
word at a time via `PTRACE_PEEKDATA` — the same mechanism `strace`
itself has used since before `/proc/pid/mem` existed as a read
interface. Which argument slot is a string, for which syscall, is a
small lookup table (`string_arg_table` in `mini_strace.c`) rather than
anything automatic — there's no generic way to know a `long` argument
is "really" a `char*` without knowing the syscall's signature.

`write()`'s buffer uses the same read mechanism but a separate table
(`buffer_arg_table`), since it isn't NUL-terminated — the read stops
at a fixed byte count (the syscall's own length argument) instead of
the first zero byte, and every byte gets escaped, not just the
non-printable ones a text path would have. This only works for
"input" syscalls where the data already exists before the call runs.
`read()` is the opposite — its buffer is empty at entry and only
populated after the kernel does its thing — so dumping it needs
reading at the *exit* stop using the actual return value as the
length, which the current entry-only argument-printing code doesn't
support yet (see Todo).

## Todo / where I'm taking this

- dump read()'s buffer too — needs reading it at the exit-stop with
  the actual byte count, not entry like everything else currently
- basic filtering, e.g. only show network or file syscalls
- attach to an already-running process by PID
- maybe a summary mode at the end (counts per syscall, like strace -c)

## Requirements

Linux, x86-64 or ARM64, gcc, make, python3 (used only by the syscall
table generator).

### macOS / no native Linux

ptrace is Linux-only, so this needs a Linux VM or container either
way. There's a `Dockerfile` in the repo with everything preinstalled:

```bash
docker build -t mini-strace-dev .
docker run --rm -it --cap-add=SYS_PTRACE -v "$(pwd)":/app -w /app mini-strace-dev
# inside the container:
make
./mini-strace /bin/echo hello world
```

`--cap-add=SYS_PTRACE` is required, Docker blocks ptrace by default.
On Apple Silicon, don't add `--platform linux/amd64`: cross-arch QEMU
emulation breaks ptrace's register access (`PTRACE_GETREGS`/
`GETREGSET` start failing with EIO). Build and run the image native
to your host architecture instead.

## License

MIT
