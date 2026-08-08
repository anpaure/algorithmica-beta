---
title: Interrupts and System Calls
weight: 9
draft: false
---

An ordinary process is deliberately not allowed to control the whole computer. It cannot install page tables, halt the processor, read another process's memory, or send commands directly to a disk controller. If it could, one broken program would be enough to break every other one.

The operating system kernel can do these things, so applications need a controlled way to ask it for help. This boundary is implemented with the same machinery that handles exceptional events: privilege levels, interrupts, and special control transfers.

### Privilege Levels

The x86 architecture defines four privilege levels called *rings*, numbered from 0 (most privileged) to 3 (least privileged). Mainstream operating systems mostly use just two of them: the kernel runs in ring 0 and applications run in ring 3, also called *user mode*.

Privilege is enforced by hardware. Page-table entries specify whether a page is accessible from user mode, and instructions that modify global processor state are privileged. Trying to execute such an instruction in user mode does not partly succeed and then ask for forgiveness: it raises an exception before changing the protected state.

The kernel is therefore not a library that a program can call with an ordinary `call` instruction. Its entry points require a control transfer that changes privilege in a way prescribed by the processor.

### A System Call

A *system call* is an intentional request from a process to the kernel. On x86-64 Linux, the system-call number is placed in `rax`, up to six arguments are placed in `rdi`, `rsi`, `rdx`, `r10`, `r8`, and `r9`, and the `syscall` instruction enters the kernel. The return value comes back in `rax`.

Here is a complete program that writes a message and terminates without using the C standard library:

```asm
global _start

section .text

_start:
  mov rax, 1        ; write(
  mov rdi, 1        ;   STDOUT_FILENO,
  mov rsi, msg      ;   "Hello, world!\n",
  mov rdx, msglen   ;   sizeof("Hello, world!\n")
  syscall           ; );

  mov rax, 60       ; exit(
  mov rdi, 0        ;   EXIT_SUCCESS
  syscall           ; );

section .rodata
  msg: db "Hello, world!", 10
  msglen: equ $ - msg
```

The numbers 1 and 60 identify `write` and `exit` in the Linux x86-64 system-call table. Other operating systems and other architectures use different numbers and calling conventions, which is why normal programs call a small libc wrapper instead of embedding them directly.

`syscall` does much more than an ordinary function call. The processor saves the return address and flags, switches to a predefined kernel entry point, and changes privilege. The entry code switches to a kernel stack and saves the registers that the kernel may need. Before doing any work, the kernel also has to validate every user-provided pointer: privilege would be rather pointless if `write` could be tricked into reading arbitrary kernel memory.

Raw Linux system-call failures are conventionally returned as small negative error codes in `rax`. The libc wrapper translates them into `-1` and stores the error number in `errno`.

Returning reverses the transition and restores user state. This is a *mode switch*, but not necessarily a *context switch*: if the call can be completed immediately, the same thread resumes without the scheduler choosing another one. A blocking call may put the thread to sleep, at which point an actual context switch is needed too.

### Exceptions

An *exception* is a synchronous event caused by the instruction currently being executed. Division by zero, an invalid opcode, a failed permission check, and a missing page-table entry are all examples. The processor stops at a precisely defined point and transfers control to a kernel handler whose address is stored in the interrupt descriptor table.

Not every exception is a program error. A page fault is also how operating systems implement several normal mechanisms:

- With *demand paging*, a newly allocated or file-backed page is populated only when it is first accessed.
- With *copy-on-write*, a page shared after `fork` is copied only when either process first tries to modify it.
- A page that was moved out of memory is brought back when it is needed again.

The first two cases may require little more than updating a page table. The last one may involve storage and take enormously longer. The instruction is restarted once the fault has been handled, so the application normally cannot tell that anything happened except by measuring the delay.

If the kernel cannot resolve an exception, it reports it to the process, usually as a signal on Unix systems. A segmentation fault is thus not the processor "crashing": it is the kernel declining to repair an invalid memory access.

### Hardware Interrupts

A hardware *interrupt* is asynchronous: it is caused by an external event rather than the current instruction. A timer expires, a network packet arrives, or a device announces that an earlier request has completed. An interrupt controller routes the event to a CPU, which temporarily stops user code or kernel code and enters the corresponding handler.

Interrupts are part of the normal operation of a computer, but doing substantial work in a handler would make the interrupted program wait unpredictably. Kernels therefore acknowledge the device, record the event, and defer most processing. Devices also coalesce events so that, for example, a burst of packets does not necessarily cause one interrupt per packet.

Interrupts can be masked for short critical sections in the kernel. They cannot remain disabled for long without increasing response latency and losing timer or device events. User programs cannot disable them at all.

### The Cost of Crossing the Boundary

A system call disrupts the normal instruction stream, saves state, executes kernel entry and exit code, and may interact with the scheduler or memory manager. Some systems also change page-table state on entry as a security measure. The useful kernel operation may be only a few instructions, but none of this surrounding work is free.

There is some overhead associated with doing system calls, so they are usually avoided. For example, all I/O is usually buffered, so that you send a single, say, 4KB piece of data to the OS.

The important optimization is *amortization*. This schematic loop makes $n$ calls:

```c
for (int i = 0; i < n; i++)
    write(fd, &buffer[i], 1);
```

while this one makes a single request for the same bytes:

```c
write(fd, buffer, n);
```

The kernel may still return after writing fewer than $n$ bytes, so production code has to repeat the call until the buffer is consumed. That does not change the principle: each request should contain enough useful work to justify crossing the boundary.

Buffering is useful beyond file I/O. Network stacks send multiple messages at once, allocators obtain memory from the kernel in large regions and split it in user space, and programs use `mmap` to access many file pages after one setup operation.

Not every service requires a system call on every use. Linux exposes a small *virtual dynamic shared object* (vDSO) inside each process. Functions such as some clock queries can read kernel-maintained data from user space and only fall back to a real system call when necessary.

The transition overhead also should not be confused with the latency of the requested operation. Waiting for a disk, a socket, or another process can dominate the entry and exit path by many orders of magnitude. Before optimizing system calls, count them and determine whether time is spent crossing the boundary, copying data, waiting for a device, or sleeping in the scheduler.

The practical rules are simple: keep privileged interfaces narrow, batch small requests, and do as much ordinary computation as possible in user mode. The hardware boundary is there for correctness and isolation; performance work is mostly about crossing it less often.
