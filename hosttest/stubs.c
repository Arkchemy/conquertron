/* Stubs for symbols the igTlsfWrapper translation unit references but the
   allocator itself never reaches -- rapidxml, igMemory, the metaobject
   registrar. Each aborts loudly rather than returning a plausible value, so if
   a test ever does reach one the harness says so instead of quietly lying. */
#include <stdio.h>
#include <stdlib.h>
#include "ppc_runtime.h"

static void unreachable(const char *name)
{
    fprintf(stderr, "harness: reached %s, which the allocator should never call\n", name);
    abort();
}

#define STUB(sym) void sym(PpcContext *ctx) { (void)ctx; unreachable(#sym); }

STUB(ppc_dispatch_unused_)
void ppc_dispatch(PpcContext *ctx, uint32_t addr)
{ (void)ctx; fprintf(stderr, "harness: ppc_dispatch(0x%x)\n", addr); abort(); }

STUB(ppc___CPR150__igArkRegister__4CoreFPPQ2_J15J12igMetaObjectPQ2_J15JJ31JPFv_PQ2_J15J22__internalFunctionListPFv_PQ2_J15JJ31JPCciPvPFv_vPCPFv_v)
STUB(ppc_fprintf)
STUB(ppc_getMemoryPool__Q2_4Core8igObjectCFv)
STUB(ppc_igFree__Q2_4Core8igMemorySFPv)
STUB(ppc_igGetMemoryPool__4CoreFi)
STUB(ppc_igMallocFromPool__Q2_4Core8igMemorySFUiPQ2_4Core12igMemoryPool)
STUB(ppc_igObject_Release__4CoreFPCQ2_4Core8igObject)
STUB(ppc_instantiateFromPool__Q2_4Core6igFileSFPQ2_4Core12igMemoryPool)
STUB(ppc_longjmp)
STUB(ppc_malloc)
STUB(ppc___nw__FUi)
STUB(ppc_parse_error_handler__8rapidxmlFPCcPv)
STUB(ppc_printf)
STUB(ppc_setjmp)
STUB(ppc_strcspn)
STUB(ppc_strspn)
