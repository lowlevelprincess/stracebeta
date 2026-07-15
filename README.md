# mini-strace

A stripped-down clone of `strace`, built on Linux's `ptrace(2)` API

## What it does right now

Runs a target program as a child process, attaches via ptrace, and
prints every syscall it makes: name, raw arguments, and return value
(errno resolved to its macro name on failure). Works on x86-64 and
ARM64 Linux.

## Build

```bash
make
./mini-strace /bin/echo hello world
```

Real output, tracing `/bin/echo hello world` on ARM64:

```
execve(0xffffc035c82c, 0xffffc035c330, 0xffffc035c350, ...) = 0
openat(0xffffffffffffff9c, 0xffffbc1a4140, 0x80000, 0x0, 0x0, 0x0) = 3
read(0x3, 0xfffffc791af0, 0x340, 0x0, 0x0, 0x0) = 832
mmap(0x0, 0x1cdf90, 0x0, 0x822, 0xffffffffffffffff, 0x0) = 281473835458560
close(0x3, 0xffffbbfa0040, 0x6, 0x6474e553, 0xffffbbfa0040, 0x6) = 0
write(0x1, 0xaaaadc1692f0, 0xc, 0x1, 0xfffffc793845, 0xaaaadc1692fb) hello world
= 12
exit_group(0x0, 0x0, 0xffffbc1a5620, 0x20, 0x0, 0x0)

[mini-strace] process exited, code 0, total syscalls: 35
```

(trimmed — full run is ~35 syscalls, mostly the dynamic linker loading
libc before `echo` itself ever runs). Note `write`'s return value of
12, exactly the byte length of `"hello world\n"`.

Arguments are printed raw, per the ABI slot they came from. Failed calls resolve errno to its macro name
via glibc's `strerrorname_np()`.

## How it works (briefly)

The child calls `PTRACE_TRACEME` then `SIGSTOP`s itself before `execvp`.
The parent waits for that stop, then loops on `PTRACE_SYSCALL`, which
pauses the child on every syscall entry and exit, reading registers to
get the syscall number, arguments, and return value.

x86-64 and ARM64 do this completely differently, different register
names, `PTRACE_GETREGS` vs `PTRACE_GETREGSET`, different argument
registers, different syscall numbering entirely. All of that is behind
`#ifdef __x86_64__` / `#ifdef __aarch64__` in `mini_strace.c`.

`src/syscall_names.h` is generated
per-architecture by `scripts/gen_syscall_table.sh` (it asks gcc to
preprocess the platform's syscall header rather than reading a
hardcoded path, since that path differs across toolchains) and
rebuilt automatically by `make` every time, since the file isn't
committed — it has to match whatever machine you're actually building
on. A few syscalls (`mmap`, `stat`, `fstat`, ...) aren't defined as a
plain number in the header — they're defined through a level of
indirection (`__NR_mmap` → `__NR3264_mmap` → `222`), so the generator
resolves that chain instead of just grepping for digits.

## Todo / where I'm taking this

- dereference pointer args and print what they point to (strings, buffers) —
  needs reading child memory via /proc/pid/mem or PEEKDATA
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

`--cap-add=SYS_PTRACE` is required — Docker blocks ptrace by default.
On Apple Silicon, don't add `--platform linux/amd64`: cross-arch QEMU
emulation breaks ptrace's register access (`PTRACE_GETREGS`/
`GETREGSET` start failing with EIO). Build and run the image native
to your host architecture instead — this code supports both x86-64
and ARM64 anyway, so there's no reason to emulate.
