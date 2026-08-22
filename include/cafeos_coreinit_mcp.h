#ifndef ARKCHEMY_CAFEOS_COREINIT_MCP_H
#define ARKCHEMY_CAFEOS_COREINIT_MCP_H

#include <string.h>

#include "ppc_runtime.h"

/*
 * Phase 1d CafeOS runtime shim -- coreinit MCP (system product settings).
 *
 * Real signatures confirmed against devkitPro/wut's coreinit/mcp.h:
 *   MCPError MCP_Open();
 *   MCPError MCP_Close(int handle);
 *   MCPError MCP_GetSysProdSettings(int32_t handle, MCPSysProdSettings *settings);
 * MCPError is int32_t; negative is an error, non-negative a success (for
 * MCP_Open specifically, the non-negative value doubles as the handle
 * later calls pass back in).
 *
 * MCPSysProdSettings's real layout (0x46 bytes, packed) is confirmed too,
 * but the actual *values* real hardware reports (region codes, country
 * codes, serial/model numbers) are NOT independently confirmed here --
 * this fills the struct with zeroed/blank defaults rather than fabricated
 * specific region-code enum values, since a game that branches on region
 * (e.g. to pick a language) deserves an honest "unknown" over a
 * confidently wrong guess at which numeric MCPRegion value means what.
 */
static inline void ppc_import_coreinit_MCP_Open(PpcContext *ctx) {
    ctx->r[3] = 1; /* fixed, always-valid handle -- only one MCP "connection" ever exists here */
}

static inline void ppc_import_coreinit_MCP_Close(PpcContext *ctx) {
    (void)ctx;
    /* real return is MCPError; 0 well-formed as "no error" regardless of
     * exact enum naming, matches every other *Error type's OK==0 pattern
     * seen across this whole shim (FSStatus, etc). */
}

static inline void ppc_import_coreinit_MCP_GetSysProdSettings(PpcContext *ctx) {
    uint32_t settings_addr = ctx->r[4];
    for (uint32_t i = 0; i < 0x46; i++) ppc_store_u8(ctx, settings_addr + i, 0);
    /* ntsc_pal (offset 0x10, 5 bytes incl NUL) is the one field with a
     * confident, low-risk default: this project's own target is an NTSC
     * region dump (see the project's Notion plan / real game dump this
     * was built against), so "NTSC" is a reasonable, not fabricated,
     * value here specifically -- unlike the region enum fields left
     * zeroed above. */
    const char *ntsc_pal = "NTSC";
    for (uint32_t i = 0; ntsc_pal[i]; i++) ppc_store_u8(ctx, settings_addr + 0x10 + i, (uint8_t)ntsc_pal[i]);
    ctx->r[3] = 0;
}

#endif /* ARKCHEMY_CAFEOS_COREINIT_MCP_H */
