#include "render_workers.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
typedef CRITICAL_SECTION WorkerMutex;
typedef CONDITION_VARIABLE WorkerCondition;
typedef HANDLE WorkerThread;
static bool mutex_init(WorkerMutex *m) { return InitializeCriticalSectionAndSpinCount(m, 0) != 0; }
static void mutex_lock(WorkerMutex *m) { EnterCriticalSection(m); }
static void mutex_unlock(WorkerMutex *m) { LeaveCriticalSection(m); }
static void mutex_destroy(WorkerMutex *m) { DeleteCriticalSection(m); }
static bool condition_init(WorkerCondition *c) { InitializeConditionVariable(c); return true; }
static void condition_wait(WorkerCondition *c, WorkerMutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void condition_wake(WorkerCondition *c) { WakeConditionVariable(c); }
static void condition_wake_all(WorkerCondition *c) { WakeAllConditionVariable(c); }
static void condition_destroy(WorkerCondition *c) { (void)c; }
static void thread_join(WorkerThread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static unsigned available_cpus(void) { SYSTEM_INFO s; GetSystemInfo(&s); return s.dwNumberOfProcessors; }
#define WORKER_ENTRY unsigned __stdcall
#define WORKER_RETURN 0
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t WorkerMutex;
typedef pthread_cond_t WorkerCondition;
typedef pthread_t WorkerThread;
static bool mutex_init(WorkerMutex *m) { return pthread_mutex_init(m, NULL) == 0; }
static void mutex_lock(WorkerMutex *m) { pthread_mutex_lock(m); }
static void mutex_unlock(WorkerMutex *m) { pthread_mutex_unlock(m); }
static void mutex_destroy(WorkerMutex *m) { pthread_mutex_destroy(m); }
static bool condition_init(WorkerCondition *c) { return pthread_cond_init(c, NULL) == 0; }
static void condition_wait(WorkerCondition *c, WorkerMutex *m) { pthread_cond_wait(c, m); }
static void condition_wake(WorkerCondition *c) { pthread_cond_signal(c); }
static void condition_wake_all(WorkerCondition *c) { pthread_cond_broadcast(c); }
static void condition_destroy(WorkerCondition *c) { pthread_cond_destroy(c); }
static void thread_join(WorkerThread t) { pthread_join(t, NULL); }
#ifdef __vita__
/* newlib has no CPU count here; apps get three cores (0-2) by default. */
static unsigned available_cpus(void) { return 3; }
#else
static unsigned available_cpus(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)n : 1; }
#endif
#define WORKER_ENTRY void *
#define WORKER_RETURN NULL
#endif

#ifdef _MSC_VER
#define WORKER_LOCAL __declspec(thread)
#elif defined(__vita__)
/* The Vita toolchain only has emulated TLS (a pthread_getspecific call per
 * access). Track nesting with the participant's own flag instead: workers set
 * theirs, the submitter checks the pool's. */
#define WORKER_LOCAL
#else
#define WORKER_LOCAL _Thread_local
#endif
#ifdef __vita__
#include <stdio.h>
#include <psp2/kernel/threadmgr.h>
static volatile bool inside_callback;   /* set while the SUBMITTER runs a job */
/* PS Vita: pthread-embedded's condition variables can lose a wake-up on the
 * Vita's cores (a submitter and its workers then wait on each other forever).
 * The batch handshake therefore uses kernel semaphores, which count, plus
 * atomics for the job cursor. Watchdog breadcrumbs: per-slot stage. */
volatile const char *g_render_worker_stage[SNES_RENDER_WORKERS_MAX];
volatile unsigned g_render_worker_batches;
#else
static WORKER_LOCAL bool inside_callback;
#endif

typedef struct Worker {
  struct SnesRenderWorkers *pool;
  WorkerThread thread;
  unsigned slot;
} Worker;

struct SnesRenderWorkers {
  WorkerMutex submission, state;
  WorkerCondition work, finished;
  Worker workers[SNES_RENDER_WORKERS_MAX - 1];
  unsigned worker_count, batch_workers, pending;
  size_t next, count, grain;
  unsigned long long generation;
  bool stopping;
  SnesRenderRangeFn callback;
  void *context;
#ifdef __vita__
  SceUID start_sema, done_sema;
#endif
};

static void invoke(SnesRenderRangeFn callback, void *context, size_t begin,
                   size_t end, unsigned slot) {
#ifdef __vita__
  if (slot == 0) inside_callback = true;
  callback(context, begin, end, slot);
  if (slot == 0) inside_callback = false;
#else
  inside_callback = true;
  callback(context, begin, end, slot);
  inside_callback = false;
#endif
}

static void run_jobs(SnesRenderWorkers *pool, unsigned slot) {
#ifdef __vita__
  for (;;) {
    size_t begin = __atomic_fetch_add(&pool->next, pool->grain, __ATOMIC_ACQ_REL);
    if (begin >= pool->count) return;
    size_t end = begin + pool->grain;
    if (end > pool->count) end = pool->count;
    invoke(pool->callback, pool->context, begin, end, slot);
  }
#endif
  for (;;) {
    mutex_lock(&pool->state);
    size_t begin = pool->next;
    if (begin == pool->count) { mutex_unlock(&pool->state); return; }
    size_t take = pool->count - begin;
    if (take > pool->grain) take = pool->grain;
    size_t end = begin + take;
    pool->next = end;
    SnesRenderRangeFn callback = pool->callback;
    void *context = pool->context;
    mutex_unlock(&pool->state);
    invoke(callback, context, begin, end, slot);
  }
}

static WORKER_ENTRY worker_entry(void *opaque) {
  Worker *worker = opaque;
  SnesRenderWorkers *pool = worker->pool;
  unsigned long long seen = 0;
#ifdef __vita__
  /* Worker 1 -> core 1, worker 2 -> core 2; the submitter keeps core 0. */
  sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
      worker->slot == 1 ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2);
  fprintf(stderr, "[render_workers] worker %u started\n", worker->slot);
  for (;;) {
    g_render_worker_stage[worker->slot] = "waiting for work";
    sceKernelWaitSema(pool->start_sema, 1, NULL);
    if (__atomic_load_n(&pool->stopping, __ATOMIC_ACQUIRE)) break;
    g_render_worker_stage[worker->slot] = "drawing";
    run_jobs(pool, worker->slot);
    g_render_worker_stage[worker->slot] = "reporting done";
    sceKernelSignalSema(pool->done_sema, 1);
  }
  (void)seen;
  return WORKER_RETURN;
#endif
  mutex_lock(&pool->state);
  for (;;) {
    while (!pool->stopping && pool->generation == seen)
      condition_wait(&pool->work, &pool->state);
    if (pool->stopping) break;
    seen = pool->generation;
    if (worker->slot > pool->batch_workers) continue;
    mutex_unlock(&pool->state);
    run_jobs(pool, worker->slot);
    mutex_lock(&pool->state);
    if (--pool->pending == 0) condition_wake(&pool->finished);
  }
  mutex_unlock(&pool->state);
  return WORKER_RETURN;
}

static bool thread_start(Worker *worker) {
#ifdef _WIN32
  worker->thread = (HANDLE)_beginthreadex(NULL, 0, worker_entry, worker, 0, NULL);
  return worker->thread != NULL;
#else
#ifdef __vita__
  /* Explicit stack: the Vita's pthread default is small. */
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  bool ok = pthread_create(&worker->thread, &attr, worker_entry, worker) == 0;
  pthread_attr_destroy(&attr);
  return ok;
#else
  return pthread_create(&worker->thread, NULL, worker_entry, worker) == 0;
#endif
#endif
}

static unsigned participant_limit(unsigned requested) {
  unsigned cpus = available_cpus();
  if (!cpus) cpus = 1;
  if (!requested) {
    const char *text = getenv("SNESRECOMP_RENDER_WORKERS");
    if (text && *text >= '0' && *text <= '9') {
      char *end;
      unsigned long n = strtoul(text, &end, 10);
      if (!*end && n >= 1 && n <= SNES_RENDER_WORKERS_MAX) requested = (unsigned)n;
    }
    if (!requested) {
      requested = cpus > 1 ? cpus - 1 : 1;
      if (requested > 4) requested = 4;
    }
  }
  if (requested > cpus) requested = cpus;
  if (requested > SNES_RENDER_WORKERS_MAX) requested = SNES_RENDER_WORKERS_MAX;
  return requested;
}

SnesRenderWorkers *SnesRenderWorkersCreate(unsigned max_participants) {
  unsigned participants = participant_limit(max_participants);
  if (participants <= 1) return NULL;
  SnesRenderWorkers *pool = calloc(1, sizeof(*pool));
  if (!pool) return NULL;
  if (!mutex_init(&pool->submission)) goto free_pool;
  if (!mutex_init(&pool->state)) goto free_submission;
  if (!condition_init(&pool->work)) goto free_state;
  if (!condition_init(&pool->finished)) goto free_work;
#ifdef __vita__
  pool->start_sema = sceKernelCreateSema("snes_rw_start", 0, 0, 64, NULL);
  pool->done_sema = sceKernelCreateSema("snes_rw_done", 0, 0, 64, NULL);
  if (pool->start_sema < 0 || pool->done_sema < 0) {
    fprintf(stderr, "[render_workers] no kernel semaphores (0x%08x 0x%08x)\n",
            (unsigned)pool->start_sema, (unsigned)pool->done_sema);
    condition_destroy(&pool->finished);
    goto free_work;
  }
#endif
  for (unsigned i = 0; i + 1 < participants; ++i) {
    Worker *worker = &pool->workers[i];
    worker->pool = pool; worker->slot = i + 1;
    if (!thread_start(worker)) { SnesRenderWorkersDestroy(pool); return NULL; }
    ++pool->worker_count;
  }
  return pool;
free_work:
  condition_destroy(&pool->work);
free_state:
  mutex_destroy(&pool->state);
free_submission:
  mutex_destroy(&pool->submission);
free_pool:
  free(pool);
  return NULL;
}

unsigned SnesRenderWorkersCount(const SnesRenderWorkers *pool) {
  return pool ? pool->worker_count + 1 : 1;
}

bool SnesRenderWorkersRun(SnesRenderWorkers *pool, size_t count, size_t grain,
                         SnesRenderRangeFn callback, void *context) {
  if (inside_callback) return false;
  if (!count) return true;
  if (!callback) return false;
  if (!grain) grain = 1;
  if (!pool) { invoke(callback, context, 0, count, 0); return true; }
  mutex_lock(&pool->submission);
  if (count <= grain) {
    invoke(callback, context, 0, count, 0);
    mutex_unlock(&pool->submission);
    return true;
  }
#ifdef __vita__
  {
    pool->callback = callback; pool->context = context;
    pool->count = count; pool->grain = grain;
    size_t extra = (count - 1) / grain;
    unsigned helpers = extra < pool->worker_count ? (unsigned)extra : pool->worker_count;
    __atomic_store_n(&pool->next, 0, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ++g_render_worker_batches;
    if (helpers) sceKernelSignalSema(pool->start_sema, (int)helpers);
    g_render_worker_stage[0] = "drawing";
    run_jobs(pool, 0);
    g_render_worker_stage[0] = "waiting for workers";
    if (helpers) {
      SceUInt timeout = 250000;
      if (sceKernelWaitSema(pool->done_sema, (int)helpers, &timeout) < 0) {
        fprintf(stderr, "[render_workers] workers late (batch %u); still waiting\n",
                g_render_worker_batches);
        sceKernelWaitSema(pool->done_sema, (int)helpers, NULL);
      }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_render_worker_stage[0] = "idle";
    pool->callback = NULL; pool->context = NULL;
    mutex_unlock(&pool->submission);
    return true;
  }
#endif
  mutex_lock(&pool->state);
  pool->callback = callback; pool->context = context;
  pool->count = count; pool->grain = grain; pool->next = 0;
  size_t extra_jobs = (count - 1) / grain;
  pool->batch_workers = extra_jobs < pool->worker_count ? (unsigned)extra_jobs : pool->worker_count;
  pool->pending = pool->batch_workers;
  ++pool->generation;
  condition_wake_all(&pool->work);
  mutex_unlock(&pool->state);
  run_jobs(pool, 0);
  mutex_lock(&pool->state);
  while (pool->pending) condition_wait(&pool->finished, &pool->state);
  pool->callback = NULL; pool->context = NULL;
  mutex_unlock(&pool->state);
  mutex_unlock(&pool->submission);
  return true;
}

void SnesRenderWorkersDestroy(SnesRenderWorkers *pool) {
  if (!pool) return;
  mutex_lock(&pool->submission);
  mutex_lock(&pool->state);
  pool->stopping = true;
  condition_wake_all(&pool->work);
  mutex_unlock(&pool->state);
#ifdef __vita__
  if (pool->worker_count) sceKernelSignalSema(pool->start_sema, (int)pool->worker_count);
#endif
  for (unsigned i = 0; i < pool->worker_count; ++i) thread_join(pool->workers[i].thread);
  mutex_unlock(&pool->submission);
  condition_destroy(&pool->finished); condition_destroy(&pool->work);
  mutex_destroy(&pool->state); mutex_destroy(&pool->submission);
#ifdef __vita__
  sceKernelDeleteSema(pool->start_sema); sceKernelDeleteSema(pool->done_sema);
#endif
  free(pool);
}
