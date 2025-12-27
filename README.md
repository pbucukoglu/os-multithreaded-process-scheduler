
# Multithreaded Process Scheduler

System-level process scheduler in C using pthreads, priority queues,  
SRTF-based selection, aging, and single-CPU execution simulation.

This project simulates how an operating system scheduler coordinates CPU-bound
processes and I/O-bound blocking, using separate ready and waiting queues and a
discrete time model.

---

## Scheduling Model

- **Single CPU model**  
  Only one process can run on the simulated CPU at any given time.

- **Priority-based selection (0–10)**  
  - Lower numeric value = higher priority (0 is highest).  
  - Scheduler always picks the process with the best (lowest) priority in the ready queue.

- **SRTF tie-breaking**  
  If multiple processes have the same priority, the scheduler selects the one  
  with the **smallest remaining CPU execution time** (Shortest Remaining Time First).

- **Execution / I/O cycle**  
  Each process is defined by:
  - `arrival_time`
  - `cpu_execution_time` (total CPU time required)
  - `interval_time` (CPU burst length before blocking on I/O)
  - `io_time` (duration spent in the waiting queue per I/O)
  - `priority` (initial priority level)

  A process:
  1. Arrives at `arrival_time` and enters the ready queue.
  2. Runs for up to `interval_time` on the CPU.
  3. Either:
     - Blocks for I/O and moves to the waiting queue for `io_time`, then returns to ready queue, or  
     - Terminates once its total CPU time reaches `cpu_execution_time`.

- **Aging mechanism**  
  To prevent starvation, an aging mechanism is applied to processes waiting in the ready queue:
  - For each 100 ms spent waiting (without being dispatched), the priority is improved
    by decrementing its priority level by 1 (down to the best possible value).
  - Once dispatched, the aging timer for that process is reset.

- **Multithreaded design**
  - Main thread manages global time, arrival of processes, and scheduling decisions.
  - A dedicated **I/O manager thread** handles the waiting queue and moves processes
    back to the ready queue after their I/O completes.

---

## Input Format

The scheduler reads its configuration from a text file:

```text
[process_id] [arrival_time] [cpu_execution_time] [interval_time] [io_time] [priority]
```

Example:

```text
1 0 100 25 5 2
2 5 30 10 3 1
3 10 60 30 8 2
```

Each row describes a single process and its parameters in milliseconds.

---

## Runtime Output

During execution, the scheduler prints a trace of events to `stdout`, including:

- Process arrivals
- Moves to READY / WAITING queues
- CPU dispatch decisions (with current priority and remaining time)
- I/O blocking and completion
- Process termination

This provides a time-stamped view of how the scheduler handles contention,
I/O blocking, and priority/aging over the lifetime of all processes.

---

## Technologies & Concepts

- C programming (system-level)
- POSIX threads (`pthread`)
- Priority-based scheduling with SRTF tie-breaking
- Aging and starvation prevention
- Single-CPU execution model
- Concurrent ready / waiting queue management
- Time-based simulation using discrete clock steps

---

## Build

```bash
make
# produces ./process_scheduler
```

The provided `Makefile` builds the binary with standard warnings enabled.

---

## Run

```bash
./process_scheduler <input_file>
```

Example:

```bash
./process_scheduler inputs/processes.txt
```

---

## Project Structure

```text
src/              # scheduler implementation
inputs/           # example process definition files
Makefile
README.md
```

This project is designed as a compact, self-contained demonstration of
multithreaded scheduling logic and OS-level concepts in C.
