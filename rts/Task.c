/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team 2001-2005
 *
 * The task manager subsystem.  Tasks execute STG code, with this
 * module providing the API which the Scheduler uses to control their
 * creation and destruction.
 * 
 * -------------------------------------------------------------------------*/

#include "PosixSource.h"
#include "Rts.h"

#include "RtsUtils.h"
#include "Task.h"
#include "Capability.h"
#include "Stats.h"
#include "Schedule.h"
#include "Hash.h"
#include "Trace.h"

#if HAVE_SIGNAL_H
#include <signal.h>
#endif

// Task lists and global counters.
// Locks required: all_tasks_mutex.
Task *all_tasks = NULL;
static nat taskCount;
static int tasksInitialized = 0;

static void   freeTask  (Task *task);
static Task * allocTask (void);
static Task * newTask   (rtsBool);

#if defined(THREADED_RTS)
static Mutex all_tasks_mutex;
#endif

/* -----------------------------------------------------------------------------
 * Remembering the current thread's Task
 * -------------------------------------------------------------------------- */

// A thread-local-storage key that we can use to get access to the
// current thread's Task structure.
#if defined(THREADED_RTS)
# if defined(MYTASK_USE_TLV)
__thread Task *my_task;
# else
ThreadLocalKey currentTaskKey;
# endif
#else
Task *my_task;
#endif

/* -----------------------------------------------------------------------------
 * Rest of the Task API
 * -------------------------------------------------------------------------- */

void
initTaskManager (void)
{
    if (!tasksInitialized) {
	taskCount = 0;
	tasksInitialized = 1;
#if defined(THREADED_RTS)
#if !defined(MYTASK_USE_TLV)
	newThreadLocalKey(&currentTaskKey);
#endif
        initMutex(&all_tasks_mutex);
#endif
    }
}

nat
freeTaskManager (void)
{
    Task *task, *next;
    nat tasksRunning = 0;

    ACQUIRE_LOCK(&all_tasks_mutex);

    for (task = all_tasks; task != NULL; task = next) {
        next = task->all_link;
        if (task->stopped) {
            freeTask(task);
        } else {
            tasksRunning++;
        }
    }

    debugTrace(DEBUG_sched, "freeing task manager, %d tasks still running",
               tasksRunning);

    all_tasks = NULL;

    RELEASE_LOCK(&all_tasks_mutex);

#if defined(THREADED_RTS) && !defined(MYTASK_USE_TLV)
    closeMutex(&all_tasks_mutex); 
    freeThreadLocalKey(&currentTaskKey);
#endif

    tasksInitialized = 0;

    return tasksRunning;
}

static Task *
allocTask (void)
{
    Task *task;

    task = myTask();
    if (task != NULL) {
        return task;
    } else {
        task = newTask(rtsFalse);
#if defined(THREADED_RTS)
        task->id = osThreadId();
#endif
        setMyTask(task);
        return task;
    }
}

static void
freeTask (Task *task)
{
    InCall *incall, *next;

    // We only free resources if the Task is not in use.  A
    // Task may still be in use if we have a Haskell thread in
    // a foreign call while we are attempting to shut down the
    // RTS (see conc059).
#if defined(THREADED_RTS)
    closeCondition(&task->cond);
    closeMutex(&task->lock);
#endif

    for (incall = task->incall; incall != NULL; incall = next) {
        next = incall->prev_stack;
        stgFree(incall);
    }
    for (incall = task->spare_incalls; incall != NULL; incall = next) {
        next = incall->next;
        stgFree(incall);
    }

    stgFree(task);
}

static Task*
newTask (rtsBool worker)
{
#if defined(THREADED_RTS)
    Ticks currentElapsedTime, currentUserTime;
#endif
    Task *task;

#define ROUND_TO_CACHE_LINE(x) ((((x)+63) / 64) * 64)
    task = stgMallocBytes(ROUND_TO_CACHE_LINE(sizeof(Task)), "newTask");
    
    task->cap           = NULL;
    task->worker        = worker;
    task->stopped       = rtsFalse;
    task->running_finalizers = rtsFalse;
    task->n_spare_incalls = 0;
    task->spare_incalls = NULL;
    task->incall        = NULL;
    
#if defined(THREADED_RTS)
    initCondition(&task->cond);
    initMutex(&task->lock);
    task->wakeup = rtsFalse;
#endif

#if defined(THREADED_RTS)
    currentUserTime = getThreadCPUTime();
    currentElapsedTime = getProcessElapsedTime();
    task->mut_time = 0;
    task->mut_etime = 0;
    task->gc_time = 0;
    task->gc_etime = 0;
    task->muttimestart = currentUserTime;
    task->elapsedtimestart = currentElapsedTime;
#endif

    task->next = NULL;

    ACQUIRE_LOCK(&all_tasks_mutex);

    task->all_link = all_tasks;
    all_tasks = task;

    taskCount++;

    RELEASE_LOCK(&all_tasks_mutex);

    return task;
}

// avoid the spare_incalls list growing unboundedly
#define MAX_SPARE_INCALLS 8

static void
newInCall (Task *task)
{
    InCall *incall;
    
    if (task->spare_incalls != NULL) {
        incall = task->spare_incalls;
        task->spare_incalls = incall->next;
        task->n_spare_incalls--;
    } else {
        incall = stgMallocBytes((sizeof(InCall)), "newBoundTask");
    }

    incall->tso = NULL;
    incall->task = task;
    incall->suspended_tso = NULL;
    incall->suspended_cap = NULL;
    incall->stat          = NoStatus;
    incall->ret           = NULL;
    incall->next = NULL;
    incall->prev = NULL;
    incall->prev_stack = task->incall;
    task->incall = incall;
}

static void
endInCall (Task *task)
{
    InCall *incall;

    incall = task->incall;
    incall->tso = NULL;
    task->incall = task->incall->prev_stack;

    if (task->n_spare_incalls >= MAX_SPARE_INCALLS) {
        stgFree(incall);
    } else {
        incall->next = task->spare_incalls;
        task->spare_incalls = incall;
        task->n_spare_incalls++;
    }
}


Task *
newBoundTask (void)
{
    Task *task;

    if (!tasksInitialized) {
        errorBelch("newBoundTask: RTS is not initialised; call hs_init() first");
        stg_exit(EXIT_FAILURE);
    }

    task = allocTask();

    task->stopped = rtsFalse;

    newInCall(task);

    debugTrace(DEBUG_sched, "new task (taskCount: %d)", taskCount);
    return task;
}

void
boundTaskExiting (Task *task)
{
#if defined(THREADED_RTS)
    ASSERT(osThreadId() == task->id);
#endif
    ASSERT(myTask() == task);

    endInCall(task);

    // Set task->stopped, but only if this is the last call (#4850).
    // Remember that we might have a worker Task that makes a foreign
    // call and then a callback, so it can transform into a bound
    // Task for the duration of the callback.
    if (task->incall == NULL) {
        task->stopped = rtsTrue;
    }

    debugTrace(DEBUG_sched, "task exiting");
}


#ifdef THREADED_RTS
#define TASK_ID(t) (t)->id
#else
#define TASK_ID(t) (t)
#endif

void
discardTasksExcept (Task *keep)
{
    Task *task, *next;

    // Wipe the task list, except the current Task.
    ACQUIRE_LOCK(&all_tasks_mutex);
    for (task = all_tasks; task != NULL; task=next) {
        next = task->all_link;
        if (task != keep) {
            debugTrace(DEBUG_sched, "discarding task %ld", (long)TASK_ID(task));
            freeTask(task);
        }
    }
    all_tasks = keep;
    keep->all_link = NULL;
    RELEASE_LOCK(&all_tasks_mutex);
}

void
taskTimeStamp (Task *task USED_IF_THREADS)
{
#if defined(THREADED_RTS)
    Ticks currentElapsedTime, currentUserTime;

    currentUserTime = getThreadCPUTime();
    currentElapsedTime = getProcessElapsedTime();

    task->mut_time =
	currentUserTime - task->muttimestart - task->gc_time;
    task->mut_etime = 
        currentElapsedTime - task->elapsedtimestart - task->gc_etime;

    if (task->gc_time   < 0) { task->gc_time   = 0; }
    if (task->gc_etime  < 0) { task->gc_etime  = 0; }
    if (task->mut_time  < 0) { task->mut_time  = 0; }
    if (task->mut_etime < 0) { task->mut_etime = 0; }
#endif
}

void
taskDoneGC (Task *task, Ticks cpu_time, Ticks elapsed_time)
{
    task->gc_time  += cpu_time;
    task->gc_etime += elapsed_time;
}

#if defined(THREADED_RTS)

void
workerTaskStop (Task *task)
{
    DEBUG_ONLY( OSThreadId id );
    DEBUG_ONLY( id = osThreadId() );
    ASSERT(task->id == id);
    ASSERT(myTask() == task);

    task->cap = NULL;
    taskTimeStamp(task);
    task->stopped = rtsTrue;
}

#endif

#ifdef DEBUG

static void *taskId(Task *task)
{
#ifdef THREADED_RTS
    return (void *)task->id;
#else
    return (void *)task;
#endif
}

#endif

#if defined(THREADED_RTS)

static void OSThreadProcAttr
workerStart(Task *task)
{
    Capability *cap;

    // See startWorkerTask().
    ACQUIRE_LOCK(&task->lock);
    cap = task->cap;
    RELEASE_LOCK(&task->lock);

    if (RtsFlags.ParFlags.setAffinity) {
        setThreadAffinity(cap->no, n_capabilities);
    }

    // set the thread-local pointer to the Task:
    setMyTask(task);

    newInCall(task);

    scheduleWorker(cap,task);
}

void
startWorkerTask (Capability *cap)
{
  int r;
  OSThreadId tid;
  Task *task;

  // A worker always gets a fresh Task structure.
  task = newTask(rtsTrue);

  // The lock here is to synchronise with taskStart(), to make sure
  // that we have finished setting up the Task structure before the
  // worker thread reads it.
  ACQUIRE_LOCK(&task->lock);

  task->cap = cap;

  // Give the capability directly to the worker; we can't let anyone
  // else get in, because the new worker Task has nowhere to go to
  // sleep so that it could be woken up again.
  ASSERT_LOCK_HELD(&cap->lock);
  cap->running_task = task;

  r = createOSThread(&tid, (OSThreadProc*)workerStart, task);
  if (r != 0) {
    sysErrorBelch("failed to create OS thread");
    stg_exit(EXIT_FAILURE);
  }

  debugTrace(DEBUG_sched, "new worker task (taskCount: %d)", taskCount);

  task->id = tid;

  // ok, finished with the Task struct.
  RELEASE_LOCK(&task->lock);
}

void
interruptWorkerTask (Task *task)
{
  ASSERT(osThreadId() != task->id);    // seppuku not allowed
  ASSERT(task->incall->suspended_tso); // use this only for FFI calls
  interruptOSThread(task->id);
  debugTrace(DEBUG_sched, "interrupted worker task %p", taskId(task));
}

#endif /* THREADED_RTS */

#ifdef DEBUG

void printAllTasks(void);

void
printAllTasks(void)
{
    Task *task;
    for (task = all_tasks; task != NULL; task = task->all_link) {
	debugBelch("task %p is %s, ", taskId(task), task->stopped ? "stopped" : "alive");
	if (!task->stopped) {
	    if (task->cap) {
		debugBelch("on capability %d, ", task->cap->no);
	    }
	    if (task->incall->tso) {
	      debugBelch("bound to thread %lu",
                         (unsigned long)task->incall->tso->id);
	    } else {
		debugBelch("worker");
	    }
	}
	debugBelch("\n");
    }
}		       

#endif

