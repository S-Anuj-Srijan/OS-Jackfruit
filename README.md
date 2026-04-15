# OS-Jackfruit: Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## 1. Team Information
- **Team Members:** [Fill Your Name]
- **SRN:** [Fill Your SRN]

## 2. Build, Load, and Run Instructions

### A. Environment Prerequisites
This project must be compiled and tested on an **Ubuntu 22.04 or 24.04** VM with **Secure Boot OFF**. WSL is not supported.

Install essential kernel headers and build tools:
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### B. Building The Project
Navigate to the central project environment:
```bash
cd boilerplate
make
```

### C. Kernel Space Monitor
Deploy the monitoring Kernel Module (`monitor.ko`) via `insmod`:
```bash
sudo insmod monitor.ko
# Verify tracking block interface created
ls -l /dev/container_monitor
```

### D. Preparing The System Run Context
Download and define the standard Alpine Base filesystem configuration:
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Prepare two test container root filesystems from the base template
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### E. Execution Lifecycle 
**Terminal 1 (Start the Long-Running Supervisor background logic):**
```bash
sudo ./engine supervisor ./rootfs-base
```

**Terminal 2 (Deploy and Control Test Environments):**
```bash
# Start containers referencing respective filesystems configuring specific soft and hard memory bounds
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# Visualize background system tracking:
sudo ./engine ps

# Print asynchronous log pipes:
sudo ./engine logs alpha

# Clean environments via clean signal stop payloads:
sudo ./engine stop alpha
sudo ./engine stop beta
```

### F. Teardown
```bash
# Invalidate local processes and unload kernel footprint
sudo rmmod monitor
make clean
```

---

## 3. Demo with Screenshots

*Before submitting your repository, execute the instructions above and paste in the correct screenshots here!*

| #   | What to Demonstrate         | What the Screenshot Must Show | Image |
| --- | --------------------------- | ------------------------------------------------|-------|
| 1   | Multi-container supervision | Two or more containers running under one supervisor process | `![supervision_demo](img.png)` |
| 2   | Metadata tracking           | Output of the `ps` command showing tracked container metadata | `![metadata_ps](img.png)` |
| 3   | Bounded-buffer logging      | Log file contents captured through the logging pipeline | `![logger](img.png)` |
| 4   | CLI and IPC                 | CLI command being issued / responding from the Socket | `![cli_ipc](img.png)` |
| 5   | Soft-limit warning          | `dmesg` or log output showing a soft-limit warning event | `![soft_limit](img.png)` |
| 6   | Hard-limit enforcement      | `dmesg` showing a container being killed after limit | `![hard_limit](img.png)` |
| 7   | Scheduling experiment       | Terminal measurements from the scheduling experiment setup | `![scheduler](img.png)` |
| 8   | Clean teardown              | Evidence containers are reaped, and `rmmod` resolves | `![teardown](img.png)` |

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms
The runtime heavily leverages `CLONE_NEWPID`, `CLONE_NEWNS`, and `CLONE_NEWUTS` inside the `clone()` syscall to deploy strict isolation bounds across standard Host interaction interfaces. The local `chroot` and bind-mount algorithms pivot the direct kernel `root` structure away from native libraries confining `/` specifically to generated folders. The system lacks network bridging implementations natively, indicating internal network sockets remain natively joined to the Host.

### 2. Supervisor and Process Lifecycle
Instead of invoking `exec` globally leaving orphaned executions blocking IO contexts, the process creates an overarching Supervisor Daemon mapping a persistent socket event polling system. Container processes (`forks`/`clones`) run completely abstract background computations mapping their states. A global `SIGCHLD` processing loop or recurring `waitpid(WNOHANG)` traps unexpectedly or cleanly exiting routines silently destroying leaked memory states effortlessly without blocking. 

### 3. IPC, Threads, and Synchronization
Logging IPC passes naturally through `pipe()` STDOUT rerouting arrays utilizing multi-threaded consumers securely synchronized under condition arrays `not_empty` and `not_full` via mutex definitions avoiding data overlaps. CLI control IPC utilizes synchronous Unix-domain endpoints mapping state variables rapidly. Spinlocks mapped into `timer_callback` loops bypass latency overhead normally generated via kernel IRQ interactions preserving core timer functionality.

### 4. Memory Management and Enforcement
The native user-space runtime fails to properly calculate real memory mapping environments; therefore the kernel-module actively reads RSS (`Resident Set Size`) page counts directly. Soft limits enforce passive notification triggers logging standard limits exceeded configurations allowing basic diagnostic notifications avoiding premature executions errors. Overstretching bounds reaches Hard-limits which execute non-reversible `SIGKILL` handlers enforcing native architectural stability limits cleanly over memory hogs directly utilizing standard linked-list algorithms securely mapping PIDs locally.

### 5. Scheduling Behavior
The default Completely Fair Scheduler generates equal processor interactions mapping CPU vs I/O bursts equally natively. The experiments successfully execute CLI configurations altering default `NICE` parameters shifting CFS priorities negatively mapping CPU background boundaries efficiently. I/O Pulse processes typically halt executing resources triggering thread wait signals returning native system executions cleanly toward more heavy logic boundaries.

---

## 5. Design Decisions and Tradeoffs

- **Supervisor Architecture**
  - *Choice:* Combined `waitpid` looping routines beside atomic Socket polling architectures securely defining metadata.
  - *Tradeoff:* Requires strict lock mutex handling across multiple dynamic thread iterations mapping changes constantly natively.
  - *Justification:* Prevents memory access errors entirely natively synchronizing variables gracefully across execution boundaries.

- **Bounded Buffers & Pipeline Routing**
  - *Choice:* Mapped local file writing inside singular pipeline threads reading locally produced condition triggers.
  - *Tradeoff:* Single failure logs blocks all incoming parallel pipeline components passively.
  - *Justification:* Drastically prevents IO thread contention logic saving kernel interactions dynamically securely parsing components dynamically.

- **Kernel Timer Monitoring Loops**
  - *Choice:* Reused recursive OS native `timer_setup` environments scanning memory limits actively at 1-sec arrays defined by Spinlocks.
  - *Tradeoff:* Spinlock blocks interrupts dynamically natively locking out external logic locally generating small system stalls when evaluating massive container numbers.
  - *Justification:* Softirq structures require explicitly atomic processing bounds preventing traditional sleep conditions completely without complex tasklet queues avoiding data overlap routines natively.

---

## 6. Scheduler Experiment Results

The scheduler execution metrics mapping generic system operations running test workload binaries inside containers identically natively mapping completely fair scheduling algorithms correctly:

| Workload Condition | Expected Execution Outcome | Scheduler Evaluation Reason |
|---------|---------|---------|
| Parallel CPU Hog vs I/O Pulse routines (`nice 0`) | The `cpu_hog` demands 99% logic mapping loops executing frequently over background routines smoothly. | Standard CFS prioritizes heavy calculation bounds efficiently natively pushing IO execution gracefully avoiding halts natively. |
| Altered Priority CPU logic (`nice 19`) | The kernel CFS pushes logic execution loops down actively giving alternative backgrounds active system execution. | System natively delays system operations providing lower `TIME` iterations over priority applications natively enforcing priority thresholds gracefully. |
