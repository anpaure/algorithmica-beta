---
title: Virtualization
weight: 8
---

An operating system kernel is written under the assumption that it controls the computer. A *virtual machine* lets several kernels keep this assumption at the same time.

The program that owns the real machine is called a *hypervisor*. It gives each guest a set of virtual CPUs, some memory, and a collection of devices, while making sure that none of the guests can take these resources away from the others.

The obvious way to implement a virtual machine is to interpret every guest instruction. It is also unnecessarily slow: two plus two means the same thing inside and outside a virtual machine. Almost all ordinary instructions can be executed directly by the physical CPU. Only operations that try to control the machine need special treatment.

## Trap and Emulate

CPUs already have [privilege levels](../interaction) that prevent normal processes from changing page tables, disabling interrupts, or talking to devices directly. It is tempting to run the guest kernel as an ordinary process and emulate every privileged instruction when it fails.

Early x86 processors made this difficult because some sensitive instructions behaved differently outside the most privileged mode without necessarily causing a trap. Hypervisors had to rewrite guest machine code or modify the guest kernel, which is known as *paravirtualization*.

Modern x86 processors have a separate virtualization mode. Intel calls it VMX and AMD calls it SVM. We will use Intel's terminology, although the two designs have the same main idea.

VMX splits execution into *root* and *non-root* modes. Both modes still have the usual rings, so the guest kernel can run in ring 0 while the hypervisor remains more privileged in root mode.

Before running a virtual CPU, the hypervisor fills a control structure with

- the guest register and control state;
- the host state to restore afterwards;
- and a list of events that should transfer control back to the hypervisor.

A *VM entry* starts the guest. Ordinary instructions then run directly until an intercepted event causes a *VM exit*. The processor saves the guest state, restores the host state, and jumps to the hypervisor. After emulating the operation or delivering an event, the hypervisor enters the guest again.

This is the main performance property of CPU virtualization. Computation between exits is mostly ordinary native computation. Exits are expensive control transfers, so a workload that constantly accesses emulated devices can behave very differently from one that spends most of its time multiplying matrices.

Which operations cause exits is configurable. A hypervisor may intercept selected control-register writes, I/O instructions, interrupts, or `cpuid`, while letting other operations run directly. Hardware also implements many formerly expensive cases without an exit. For this reason, “the cost of a virtual machine” is not one constant that can be attached to every instruction.

## Two Address Translations

Inside a normal process, the CPU translates a virtual address to a physical one using the page tables. A guest has its own page tables, but the addresses it calls physical are not real host addresses:

$$
\text{guest virtual}
\longrightarrow \text{guest physical}
\longrightarrow \text{host physical}.
$$

The second mapping is controlled by the hypervisor. Intel calls these tables *Extended Page Tables* (EPT), while AMD calls them *Nested Page Tables* (NPT).

Without hardware support, the hypervisor has to maintain a combined set of *shadow page tables* and trap whenever the guest changes its mappings. Nested paging lets the processor walk both levels itself. It also provides isolation: changing a guest page-table entry cannot map host memory that the EPT does not assign to that guest.

Two-dimensional page walks can require more memory accesses than native translation. The [TLB](/hpc/external-memory/virtual) caches the final result, so large working sets, small pages, and frequent invalidations can expose more virtualization overhead than a compact compute loop. Huge pages may help, but the guest and host page sizes and allocation policies interact.

## Virtual I/O

A guest also expects disks, network cards, timers, and interrupt controllers. There are three common ways to provide them.

**Emulation** imitates an existing physical device. It works with an unmodified guest driver, but register accesses and interrupts may require VM exits. Accurately emulating decades-old hardware is usually far more work than executing the guest instructions that use it.

**Paravirtualized devices** expose an interface designed for virtual machines. For example, virtio drivers exchange batches of descriptors with the host through shared-memory queues. The guest knows that the device is virtual, but applications above the driver do not.

**Direct assignment** gives a physical device, or one of its hardware virtual functions, to a guest. An IOMMU restricts the memory that the device can access. This avoids much of the emulation path, but makes sharing and migrating the virtual machine more difficult.

I/O-heavy virtual machines are therefore mainly a batching problem. A single packet per exit is bad for the same reason that a single byte per system call is bad; shared queues and interrupt coalescing amortize the boundary crossing.

## Scheduling

A virtual CPU is not necessarily a physical CPU. On a hosted system it is a schedulable task, and the host may stop it to run another guest. If there are more runnable virtual CPUs than physical ones, the guest can be ready to execute and still receive no CPU time.

This becomes particularly confusing with synchronization. One virtual CPU may spin while the virtual CPU holding its lock is not scheduled. Oversubscription, CPU pinning, and NUMA placement can therefore dominate the small instruction-level overheads that are usually blamed on virtualization.

## Virtual Machines and Containers

Containers virtualize a different interface. They share the host kernel and isolate groups of processes with namespaces, resource limits, capabilities, and system-call filtering. There is no guest kernel and normally no VM entry or exit on an ordinary instruction.

This makes containers lighter, but it also makes the host kernel part of their common trusted base. Virtual machines provide a hardware-shaped boundary and can run a different kernel. In practice, the two are often nested: a virtual machine isolates a tenant, and containers organize applications inside it.

When profiling a virtualized program, replace the vague question “how much does virtualization cost?” with a more useful one:

- Are there frequent VM exits?
- Are nested translations missing in the TLB?
- Is I/O emulated, paravirtualized, or assigned?
- Is the virtual CPU waiting to be scheduled?
- Is memory on the wrong NUMA node?

Virtualization adds another layer of indirection, but it does not create a new kind of performance. The same rules still apply: keep the common path direct, batch expensive transitions, and place computation close to its data.

## Further Reading

The [Intel system programming manual](https://www.intel.com/content/www/us/en/content-details/858460/intel-64-and-ia-32-architectures-software-developer-s-manual-volume-3c-system-programming-guide-part-3.html) specifies VMX operation and EPT. On Linux, the [KVM API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html) is a useful concrete example of how a user-space virtual-machine monitor enters a guest and handles its exits.
