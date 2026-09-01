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
 * instance). Arkchemy's actual, full recompiled game is far too large
 * (8.5M+ lines) to safely compile as that same single-translation-unit
 * shape (a real attempt exhausted a real, deliberate 5.5GB compile-time
 * memory safety cap) -- it needs to be split across many independently-
 * compiled `.c` files instead, purely to bound each individual `gcc`
 * invocation's own real memory use. But splitting across multiple
 * files means each one, if it `#include`s a shim header directly,
 * would get its *own*, separate, `static`-scoped (internal-linkage)
 * copy of that header's state -- e.g. every file's own private copy of
 * `g_arkchemy_gx2`, meaning a real `GX2Init` call recorded in one file's
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
#include "cafeos_vpad.h"
#endif

ppc_unhandled_log_fn g_ppc_unhandled_log = NULL;
/* g_ppc_debug_watch is now a weak default in ppc_runtime.h itself -- see
   its own comment there for why (ppc_store_u32 now references it
   unconditionally, unlike before). */

ppc_fs_open_log_fn g_ppc_fs_open_log = NULL;

int32_t g_ppc_fs_last_error = ARKCHEMY_FS_STATUS_OK;
FILE *g_ppc_fs_files[ARKCHEMY_FS_MAX_HANDLES];
DIR *g_ppc_fs_dirs[ARKCHEMY_FS_MAX_DIR_HANDLES];
int g_ppc_im_dim_enabled = 1; /* real hardware default is dimming enabled */

#ifdef __SWITCH__
/* Real bug found 2026-08-24: these still pointed at "switch/Arkchemy",
 * the pre-rename directory. The shipped .nro is Jouster and its SD
 * folder is /switch/Jouster, which on the owner's card is fully
 * populated with the game's real extracted content (movies/, character/,
 * level/, item/, ...). So every asset lookup resolved to a directory
 * that does not exist and cleanly "failed to find" the file -- which is
 * exactly why the Bink test has reported
 *   BinkOpen("movies/bash.mov") returned NULL
 * on every single run, and why nothing else has ever loaded either. Not
 * a decoder or filesystem fault at all, just a stale path left behind by
 * the Arkchemy -> Arkchemy/Jouster rename.
 *
 * Kept as a variable rather than a literal at the use site so a future
 * build can point it elsewhere without another rebuild of the shims. */
char g_arkchemy_fs_content_root[256] = "sdmc:/switch/Jouster/content";
char g_arkchemy_fs_save_root[256] = "sdmc:/switch/Jouster/save";
#else
char g_arkchemy_fs_content_root[256] = "content";
char g_arkchemy_fs_save_root[256] = "save";
#endif

ArkchemyThreadEntry g_arkchemy_threads[ARKCHEMY_THREAD_TABLE_SIZE];
pthread_mutex_t g_arkchemy_thread_table_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_arkchemy_current_thread_key;
pthread_once_t g_arkchemy_thread_tls_once = PTHREAD_ONCE_INIT;

ArkchemyMemHeap g_arkchemy_mem_heaps[ARKCHEMY_MEM_MAX_HEAPS];
ArkchemyAllocSite g_arkchemy_alloc_sites[ARKCHEMY_ALLOC_SITE_SLOTS];
ppc_mem_alloc_fail_log_fn g_ppc_mem_alloc_fail_log = NULL;
uint32_t g_arkchemy_base_heap_handle[3];
uint64_t g_arkchemy_mem_alloc_fail_total = 0;
uint64_t g_arkchemy_mem_free_total = 0;
uint64_t g_arkchemy_mem_reuse_total = 0;

ArkchemyMutexEntry g_arkchemy_mutexes[ARKCHEMY_SYNC_TABLE_SIZE];
pthread_mutex_t g_arkchemy_mutex_table_lock = PTHREAD_MUTEX_INITIALIZER;
ArkchemyEventEntry g_arkchemy_events[ARKCHEMY_SYNC_TABLE_SIZE];
pthread_mutex_t g_arkchemy_event_table_lock = PTHREAD_MUTEX_INITIALIZER;
ArkchemySemEntry g_arkchemy_sems[ARKCHEMY_SYNC_TABLE_SIZE];
pthread_mutex_t g_arkchemy_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;

/* Aliasing fallbacks, used only after a sync table fills. Wrong semantics
 * on purpose, and counted so the wrongness is visible -- see
 * arkchemy_mutex_get in cafeos_coreinit_sync.h. */
pthread_mutex_t g_arkchemy_mutex_fallback = PTHREAD_MUTEX_INITIALIZER;
ArkchemyEventEntry g_arkchemy_event_fallback = { 0, 1, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 1, 0, 0 };
ArkchemySemEntry g_arkchemy_sem_fallback = { 0, 1, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 };

unsigned g_arkchemy_sync_used[3];
unsigned g_arkchemy_sync_exhausted[3];
unsigned g_arkchemy_event_signals;
unsigned g_arkchemy_event_wakes;
unsigned g_arkchemy_event_timeouts;
uint32_t g_arkchemy_event_last_signal;
uint32_t g_arkchemy_event_last_wait;

int g_ax_initialized = 0;
int g_arkchemy_ax_voice_used[ARKCHEMY_AXVOICE_MAX];

#ifdef __SWITCH__
ArkchemyGx2State g_arkchemy_gx2;
ArkchemyVpadState g_arkchemy_vpad;
#endif
