#define _GNU_SOURCE

#include <dlfcn.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sysprof-capture.h>
#include <unistd.h>
#include <libunwind.h>

#define CAPTURE_MAX_STACK_DEPTH 32

typedef void *(* RealMalloc)        (size_t);
typedef void  (* RealFree)          (void *);
typedef void *(* RealCalloc)        (size_t, size_t);
typedef void *(* RealRealloc)       (void *, size_t);
typedef void *(* RealAlignedAlloc)  (size_t, size_t);
typedef int   (* RealPosixMemalign) (void **, size_t, size_t);
typedef void *(* RealMemalign)      (size_t, size_t);

typedef struct
{
  char buf[4092];
  int  off;
} ScratchAlloc;

static void  hook_memtable   (void);
static void *scratch_malloc  (size_t);
static void *scratch_realloc (void *, size_t);
static void *scratch_calloc  (size_t, size_t);
static void  scratch_free    (void *);

static G_LOCK_DEFINE (writer);
static SysprofCaptureWriter *writer;
static int hooked;
static int pid;
static ScratchAlloc scratch;
static RealCalloc real_calloc = scratch_calloc;
static RealFree real_free = scratch_free;
static RealMalloc real_malloc = scratch_malloc;
static RealRealloc real_realloc = scratch_realloc;
static RealAlignedAlloc real_aligned_alloc;
static RealPosixMemalign real_posix_memalign;
static RealMemalign real_memalign;

static void *
scratch_malloc (size_t size)
{
  hook_memtable ();
  return real_malloc (size);
}

static void *
scratch_realloc (void   *ptr,
                 size_t  size)
{
  hook_memtable ();
  return real_realloc (ptr, size);
}

static void *
scratch_calloc (size_t nmemb,
                size_t size)
{
  void *ret;

  /* re-entrant, but forces early hook in case calloc is
   * called before any of our other hooks.
   */
  if (!hooked)
    hook_memtable ();

  size *= nmemb;
  ret = &scratch.buf[scratch.off];
  scratch.off += size;

  return ret;
}

static void
scratch_free (void *ptr)
{
  if ((char *)ptr >= scratch.buf && (char *)ptr < scratch.buf + scratch.off)
    return;
}

static void
flush_writer (void)
{
  G_LOCK (writer);
  sysprof_capture_writer_flush (writer);
  G_UNLOCK (writer);
}

static void
hook_memtable (void)
{
  const gchar *env;

  if (hooked)
    return;

  hooked = 1;

  real_calloc = dlsym (RTLD_NEXT, "calloc");
  real_free = dlsym (RTLD_NEXT, "free");
  real_malloc = dlsym (RTLD_NEXT, "malloc");
  real_realloc = dlsym (RTLD_NEXT, "realloc");
  real_aligned_alloc = dlsym (RTLD_NEXT, "aligned_alloc");
  real_posix_memalign = dlsym (RTLD_NEXT, "posix_memalign");
  real_memalign = dlsym (RTLD_NEXT, "memalign");

  unsetenv ("LD_PRELOAD");

  pid = getpid ();

  /* TODO: We want an API that let's us create a new writer
   * per-thread instead of something like this (or using an
   * environment variable). That will require a control channel
   * to sysprof to request new writer/muxed APIs.
   */

  env = getenv ("MEMORY_TRACE_FD");

  if (env != NULL)
    {
      int fd = atoi (env);

      if (fd > 0)
        writer = sysprof_capture_writer_new_from_fd (fd, 0);
    }

  if (writer == NULL)
    writer = sysprof_capture_writer_new ("memory.syscap", 0);

  atexit (flush_writer);
}

#define gettid() syscall(__NR_gettid, 0)

static inline void
track_malloc (void   *ptr,
              size_t  size)
{
  SysprofCaptureAddress addrs[CAPTURE_MAX_STACK_DEPTH];
  unw_context_t uc;
  unw_cursor_t cursor;
  unw_word_t ip;
  guint n_addrs = 0;

  if G_UNLIKELY (!writer)
    return;

  /* Get a stacktrace for the current allocation, but walk past our
   * current function because we don't care about that stack frame.
   */
  unw_getcontext (&uc);
  unw_init_local (&cursor, &uc);
  if (unw_step (&cursor) > 0)
    {
      while (n_addrs < G_N_ELEMENTS (addrs) && unw_step (&cursor) > 0)
        {
          unw_get_reg (&cursor, UNW_REG_IP, &ip);
          addrs[n_addrs++] = ip;
        }
    }

  G_LOCK (writer);

  sysprof_capture_writer_add_memory_alloc (writer,
                                           SYSPROF_CAPTURE_CURRENT_TIME,
                                           sched_getcpu (),
                                           pid,
                                           gettid(),
                                           GPOINTER_TO_SIZE (ptr),
                                           size,
                                           addrs,
                                           n_addrs);
  G_UNLOCK (writer);
}

static inline void
track_free (void *ptr)
{
  if G_UNLIKELY (!writer)
    return;

  G_LOCK (writer);
  sysprof_capture_writer_add_memory_free (writer,
                                          SYSPROF_CAPTURE_CURRENT_TIME,
                                          sched_getcpu (),
                                          pid,
                                          gettid(),
                                          GPOINTER_TO_SIZE (ptr));
  G_UNLOCK (writer);
}

void *
malloc (size_t size)
{
  void *ret = real_malloc (size);
  track_malloc (ret, size);
  return ret;
}

void *
calloc (size_t nmemb,
        size_t size)
{
  void *ret = real_calloc (nmemb, size);
  track_malloc (ret, size);
  return ret;
}

void
free (void *ptr)
{
  real_free (ptr);
  track_free (ptr);
}

void *
aligned_alloc (size_t alignment,
               size_t size)
{
  void *ret = real_aligned_alloc (alignment, size);
  track_malloc (ret, size);
  return ret;
}

int
posix_memalign (void   **memptr,
                size_t   alignment,
                size_t   size)
{
  int ret = real_posix_memalign (memptr, alignment, size);
  track_malloc (*memptr, size);
  return ret;
}

void *
memalign (size_t alignment,
          size_t size)
{
  void *ret = real_memalign (alignment, size);
  track_malloc (ret, size);
  return ret;
}
