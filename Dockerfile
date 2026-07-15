# Dev/build environment for mini-strace. This exists purely because
# ptrace is Linux-only and I'm developing on macOS — build and run
# happen in here, editing still happens on the host.
#
# Build:  docker build -t mini-strace-dev .
# Run:    docker run --rm -it --cap-add=SYS_PTRACE -v "$(pwd)":/app -w /app mini-strace-dev
#
# Note for Apple Silicon: don't add --platform linux/amd64 — running
# the "wrong" architecture through QEMU emulation breaks ptrace's
# register access (PTRACE_GETREGS/GETREGSET starts failing with
# EIO). Build the image natively for whatever the host actually is.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    make \
    python3 \
    libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
