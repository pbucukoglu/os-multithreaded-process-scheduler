#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/time.h>

#define MAX_PROCESSES   100
#define AGING_INTERVAL  100   // ms (a process waiting 100 ms in READY queue gets priority--)

// Process descriptor
typedef struct {
    int pid;
    int arrival_time;
    int cpu_execution_time;
    int interval_time;
    int io_time;
    int priority;

    int remaining_time;      // Remaining total CPU time to be executed
    int current_priority;    // Dynamically updated priority (with aging)

    // Aging tracking for time spent in READY queue
    int ready_wait_accum;    // Accumulated wait time in READY (ms)
    int last_ready_check;    // Last clock value when aging was updated

    // I/O tracking
    int io_end_time;         // Time when current I/O completes

    // CPU burst state
    int dispatch_time;       // Time when the process was dispatched to CPU
    int current_burst_time;  // Duration of the currently running CPU burst
} Process;

// Singly-linked queue node
typedef struct QueueNode {
    Process *process;
    struct QueueNode *next;
} QueueNode;

// Generic queue structure (shared by READY and WAITING queues)
typedef struct {
    QueueNode *head;
    QueueNode *tail;
    int count;
    pthread_mutex_t mutex;
} Queue;

/* ===== Global State ===== */

Queue ready_queue;
Queue waiting_queue;
Process processes[MAX_PROCESSES];
int process_count = 0;

int clock_time = 0;
int simulation_running = 1;

pthread_mutex_t clock_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread-safe printf wrapper
#define PRINTF_SYNC(...)                                    \
    do {                                                    \
        pthread_mutex_lock(&output_mutex);                  \
        printf(__VA_ARGS__);                                \
        fflush(stdout);                                     \
        pthread_mutex_unlock(&output_mutex);                \
    } while (0)

/* ===== Queue helpers ===== */

void init_queue(Queue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        exit(1);
    }
}

// Thread-safe enqueue (generic use)
void enqueue_raw(Queue *q, Process *p) {
    QueueNode *node = (QueueNode *)malloc(sizeof(QueueNode));
    if (node == NULL) {
        perror("malloc");
        exit(1);
    }
    node->process = p;
    node->next = NULL;

    if (pthread_mutex_lock(&q->mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }

    if (q->tail == NULL) {
        q->head = node;
        q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    q->count++;

    if (pthread_mutex_unlock(&q->mutex) != 0) {
        perror("pthread_mutex_unlock");
        exit(1);
    }
}

// Returns whether queue is empty (READY / WAITING)
int is_queue_empty(Queue *q) {
    if (pthread_mutex_lock(&q->mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }
    int empty = (q->head == NULL);
    if (pthread_mutex_unlock(&q->mutex) != 0) {
        perror("pthread_mutex_unlock");
        exit(1);
    }
    return empty;
}

// Enqueue into READY queue and initialize aging timestamps
void enqueue_ready(Process *p, int now) {
    p->last_ready_check = now;
    enqueue_raw(&ready_queue, p);
}

// Enqueue into WAITING queue
void enqueue_waiting(Process *p) {
    enqueue_raw(&waiting_queue, p);
}

// Priority + SRTF dequeue:
// Lower current_priority wins; on tie, smaller remaining_time wins
Process* dequeue_priority_srtf(Queue *q) {
    if (pthread_mutex_lock(&q->mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }

    if (q->head == NULL) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    QueueNode *cur = q->head;
    QueueNode *prev = NULL;
    QueueNode *best = q->head;
    QueueNode *best_prev = NULL;

    while (cur != NULL) {
        Process *cp = cur->process;
        Process *bp = best->process;

        if (cp->current_priority < bp->current_priority ||
            (cp->current_priority == bp->current_priority &&
             cp->remaining_time < bp->remaining_time)) {
            best = cur;
            best_prev = prev;
        }

        prev = cur;
        cur  = cur->next;
    }

    // Remove selected node
    if (best_prev == NULL)
        q->head = best->next;
    else
        best_prev->next = best->next;

    if (best == q->tail)
        q->tail = best_prev;

    Process *p = best->process;
    q->count--;
    free(best);

    pthread_mutex_unlock(&q->mutex);
    return p;
}

/* ===== Aging mechanism ===== */

void apply_aging(int current_time) {
    if (pthread_mutex_lock(&ready_queue.mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(1);
    }

    QueueNode *node = ready_queue.head;
    while (node != NULL) {
        Process *p = node->process;

        int delta = current_time - p->last_ready_check;
        if (delta > 0) {
            p->ready_wait_accum += delta;
            p->last_ready_check = current_time;

            // For every 100 ms waiting in READY, improve priority (down to 0)
            while (p->ready_wait_accum >= AGING_INTERVAL &&
                   p->current_priority > 0) {
                p->current_priority--;
                p->ready_wait_accum -= AGING_INTERVAL;
            }
        }

        node = node->next;
    }

    pthread_mutex_unlock(&ready_queue.mutex);
}

/* ===== I/O manager thread ===== */

void* io_manager(void *arg) {
    (void)arg;  // unused

    while (simulation_running) {
        // Read global clock
        pthread_mutex_lock(&clock_mutex);
        int now = clock_time;
        pthread_mutex_unlock(&clock_mutex);

        // Check WAITING queue for processes whose I/O has completed
        pthread_mutex_lock(&waiting_queue.mutex);

        QueueNode *cur = waiting_queue.head;
        QueueNode *prev = NULL;

        while (cur != NULL) {
            Process *p = cur->process;
            QueueNode *next = cur->next;

            if (p->io_end_time <= now) {
                // Remove from WAITING
                if (prev == NULL)
                    waiting_queue.head = next;
                else
                    prev->next = next;

                if (cur == waiting_queue.tail)
                    waiting_queue.tail = prev;

                waiting_queue.count--;

                PRINTF_SYNC("[Clock: %d] PID %d finished I/O\n", now, p->pid);
                PRINTF_SYNC("[Clock: %d] PID %d moved to READY queue\n", now, p->pid);

                // Re-insert into READY (reset aging counters)
                p->ready_wait_accum = 0;
                p->last_ready_check = now;
                enqueue_ready(p, now);

                free(cur);
                cur = next;
            } else {
                prev = cur;
                cur  = next;
            }
        }

        pthread_mutex_unlock(&waiting_queue.mutex);

        // No active sleep here; main loop advances the clock.
        // (Optionally, a small usleep could be added.)
    }

    return NULL;
}

/* ===== Input reader ===== */

int read_processes(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen");
        return -1;
    }

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strlen(line) <= 1)
            continue;

        Process p;
        if (sscanf(line, "%d %d %d %d %d %d",
                   &p.pid, &p.arrival_time, &p.cpu_execution_time,
                   &p.interval_time, &p.io_time, &p.priority) == 6) {

            p.remaining_time      = 0;
            p.current_priority    = p.priority;
            p.ready_wait_accum    = 0;
            p.last_ready_check    = 0;
            p.io_end_time         = 0;
            p.dispatch_time       = 0;
            p.current_burst_time  = 0;

            processes[count] = p;
            count++;
            if (count >= MAX_PROCESSES)
                break;
        }
    }

    fclose(f);
    return count;
}

/* ===== main simulation loop ===== */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    process_count = read_processes(argv[1]);
    if (process_count <= 0) {
        fprintf(stderr, "No processes found in file\n");
        return 1;
    }

    init_queue(&ready_queue);
    init_queue(&waiting_queue);

    pthread_t io_thread;
    if (pthread_create(&io_thread, NULL, io_manager, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Compute a reasonable upper bound for simulation time
    int max_time = 0;
    for (int i = 0; i < process_count; i++) {
        int ft = processes[i].arrival_time + processes[i].cpu_execution_time;
        int bursts = (processes[i].cpu_execution_time +
                      processes[i].interval_time - 1) / processes[i].interval_time;
        ft += bursts * processes[i].io_time;
        if (ft > max_time)
            max_time = ft;
    }
    max_time += 2000;

    Process *current_process = NULL;
    int process_finish_time  = -1;
    int terminated_count     = 0;

    while ((terminated_count < process_count ||
            current_process != NULL ||
            !is_queue_empty(&ready_queue) ||
            !is_queue_empty(&waiting_queue)) &&
           clock_time <= max_time) {

        // Process arrivals at current time
        for (int i = 0; i < process_count; i++) {
            if (processes[i].arrival_time == clock_time) {
                Process *p = &processes[i];

                PRINTF_SYNC("[Clock: %d] PID %d arrived\n", clock_time, p->pid);
                PRINTF_SYNC("[Clock: %d] PID %d moved to READY queue\n", clock_time, p->pid);

                p->remaining_time      = p->cpu_execution_time;
                p->current_priority    = p->priority;
                p->ready_wait_accum    = 0;
                p->last_ready_check    = clock_time;

                enqueue_ready(p, clock_time);
            }
        }

        // Apply aging to processes in READY queue
        apply_aging(clock_time);

        // Check CPU burst completion
        if (current_process != NULL && clock_time >= process_finish_time) {
            int burst_used = current_process->current_burst_time;
            current_process->remaining_time -= burst_used;

            if (current_process->remaining_time <= 0) {
                PRINTF_SYNC("[Clock: %d] PID %d TERMINATED\n",
                            clock_time, current_process->pid);
                terminated_count++;
                current_process = NULL;
                process_finish_time = -1;
            } else {
                PRINTF_SYNC("[Clock: %d] PID %d blocked for I/O for %d ms\n",
                            clock_time, current_process->pid,
                            current_process->io_time);

                current_process->io_end_time = clock_time + current_process->io_time;
                enqueue_waiting(current_process);

                current_process = NULL;
                process_finish_time = -1;
            }
        }

        // If CPU is idle, dispatch next process from READY
        if (current_process == NULL && !is_queue_empty(&ready_queue)) {
            current_process = dequeue_priority_srtf(&ready_queue);
            if (current_process != NULL) {
                int burst_time = current_process->interval_time;
                if (burst_time > current_process->remaining_time)
                    burst_time = current_process->remaining_time;

                // Reset aging when moving from READY → RUNNING
                current_process->ready_wait_accum = 0;
                current_process->last_ready_check = clock_time;

                PRINTF_SYNC("[Clock: %d] Scheduler dispatched PID %d (Pr: %d, Rm: %d) for %d ms burst\n",
                            clock_time,
                            current_process->pid,
                            current_process->current_priority,
                            current_process->remaining_time,
                            burst_time);

                current_process->dispatch_time      = clock_time;
                current_process->current_burst_time = burst_time;

                process_finish_time = clock_time + burst_time;
            }
        }

        // Advance simulated clock
        pthread_mutex_lock(&clock_mutex);
        clock_time++;
        pthread_mutex_unlock(&clock_mutex);

        // Allow I/O manager thread to run (1 ms real-time)
        usleep(1000);
    }

    simulation_running = 0;
    pthread_join(io_thread, NULL);

    return 0;
}
