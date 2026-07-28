#include "task.h"
#include "types.h"

static int8_t current_task = 0;
static struct task tasks[MAX_TASKS] = { 0 };

void task_init(void)
{
    for (uint8_t i=0; i<MAX_TASKS; i++) {
        tasks[i].state = TASK_FREE;
    }
    tasks[0].state = TASK_READY;
    current_task = 0;
}

int task_create(void (*entry)(void)) {
    int8_t free_task = -1;
    for (uint8_t i=0; i<MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) {
            free_task = i;
            break;
        }
    } 

    if (free_task == -1) {
        return free_task;
    }

    struct task *t = &tasks[free_task];

    uint32_t *sp = (uint32_t *)(t->stack + TASK_STACK_SIZE);
    
    *--sp = (uint32_t)entry;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->esp = (uint32_t)sp;

    t->state = TASK_READY;

    return free_task;
}

int task_current(void)
{
    return current_task;
}

struct task *task_slot(int i)
{
    return &tasks[i];
}

void task_yield(void)
{
    int next = task_next(tasks, MAX_TASKS, current_task);
    if (next < -1) {
        return;
    }
    
    uint8_t prev = current_task;
    current_task = next;
    task_switch(&(tasks[prev].esp), tasks[next].esp);  
}

int task_next(const struct task *table, int n, int current)
{
    for (uint8_t k=1; k<n; k++) {
        uint8_t i = (current + k) % n;
        if (table[i].state == TASK_READY) {
            return i;
        }
    }

    return -1;
}