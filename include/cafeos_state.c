/*
 * Real, single, external-linkage home for every cafeos_*.h shim
 * header's own persistent state (the `static <Type> g_<name>` globals
 * each one previously defined directly in the header itself).
 *
 * Real problem this solves, found the hard way (a real, project-first
 * full-game build attempt): every one of this project's `.h` shim
 * headers is intentionally header-only (`static inline` functions +
 * `static` state), correct and sufficient for every project that
 * `#include`s them from exactly one real translation unit (both
 * switch/native/ and switch/gx2_test/ do -- their own single main.c is
 * the only file that ever includes these headers, so each header's
 * `static` state naturally has exactly one, real, correctly-shared
 * instance). Bramble's actual, full recompiled game is far too large
 * (8.5M+ lines) to safely compile as that same single-translation-unit
 * shape (a real attempt exhausted a real, deliberate 5.5GB compile-time
 * memory safety cap) -- it needs to be split across many independently-
 * compiled `.c` files instead, purely to bound each individual `gcc`
 * invocation's own real memory use. But splitting across multiple
 * files means each one, if it `#include`s a shim header directly,
 * would get its *own*, separate, `static`-scoped (internal-linkage)
 * copy of that header's state -- e.g. every file's own private copy of
 * `g_bramble_gx2`, meaning a real `GX2Init` call recorded in one file's
 * copy would be genuinely invisible to a `GX2ClearColor` call compiled
 * into a different file, silently breaking real cross-call state
 * coherence throughout the whole game.
 *
 * Real fix: every one of those state variables is declared `extern` in
 * its own real header now (see each cafeos_*.h's own updated
 * declaration) instead of `static` -- every translation unit that
 * includes the header shares the exact same one, real, externally-
 * linked instance, defined exactly once, here. This file must be
 * compiled and linked into every real project that uses any of these
 * headers (switch/native/, switch/gx2_test/, switch/game/ -- their own
 * Makefiles were updated alongside this file to do so), even ones that
 * only ever needed a single translation unit before -- a real, small,
 * mechanical addition to each, not a behavior change for them.
 */
#include "ppc_runtime.h"
#include "cafeos_coreinit_fs.h"
#include "cafeos_coreinit_im.h"
#include "cafeos_coreinit_mem.h"
#include "cafeos_coreinit_sync.h"
#include "cafeos_coreinit_thread.h"
#include "cafeos_snd_core.h"
#ifdef __SWITCH__
#include "cafeos_gx2.h"
#endif

int32_t g_ppc_fs_last_error = BRAMBLE_FS_STATUS_OK;
FILE *g_ppc_fs_files[BRAMBLE_FS_MAX_HANDLES];
DIR *g_ppc_fs_dirs[BRAMBLE_FS_MAX_DIR_HANDLES];
int g_ppc_im_dim_enabled = 1; /* real hardware default is dimming enabled */

#ifdef __SWITCH__
char g_bramble_fs_content_root[256] = "sdmc:/switch/Bramble/content";
char g_bramble_fs_save_root[256] = "sdmc:/switch/Bramble/save";
#else
char g_bramble_fs_content_root[256] = "content";
char g_bramble_fs_save_root[256] = "save";
#endif

BrambleThreadEntry g_bramble_threads[BRAMBLE_THREAD_TABLE_SIZE];
pthread_mutex_t g_bramble_thread_table_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_bramble_current_thread_key;
pthread_once_t g_bramble_thread_tls_once = PTHREAD_ONCE_INIT;

BrambleMemHeap g_bramble_mem_heaps[BRAMBLE_MEM_MAX_HEAPS];
uint32_t g_bramble_base_heap_handle[3];

BrambleMutexEntry g_bramble_mutexes[BRAMBLE_SYNC_TABLE_SIZE];
pthread_mutex_t g_bramble_mutex_table_lock = PTHREAD_MUTEX_INITIALIZER;
BrambleEventEntry g_bramble_events[BRAMBLE_SYNC_TABLE_SIZE];
pthread_mutex_t g_bramble_event_table_lock = PTHREAD_MUTEX_INITIALIZER;
BrambleSemEntry g_bramble_sems[BRAMBLE_SYNC_TABLE_SIZE];
pthread_mutex_t g_bramble_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;

int g_ax_initialized = 0;
int g_bramble_ax_voice_used[BRAMBLE_AXVOICE_MAX];

#ifdef __SWITCH__
BrambleGx2State g_bramble_gx2;
#endif
