#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "codegen.h"
#include "disassembler.h"
#include "elf_loader.h"
#include "func_recovery.h"

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool force_stripped = false;
    bool extern_globals = false;
    std::string input_path, output_path, entry_alias;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--stripped") {
            force_stripped = true;
        } else if (args[i] == "--extern-globals") {
            extern_globals = true;
        } else if (args[i] == "-o" && i + 1 < args.size()) {
            output_path = args[++i];
        } else if (args[i] == "--entry-alias" && i + 1 < args.size()) {
            entry_alias = args[++i];
        } else if (input_path.empty()) {
            input_path = args[i];
        }
    }
    if (input_path.empty() || output_path.empty()) {
        std::cerr << "usage: recomp [--stripped] [--entry-alias NAME] [--extern-globals] <input.elf> -o <output.c>\n";
        std::cerr << "  --stripped:          ignore any symbol table and recover function\n";
        std::cerr << "                       boundaries via control-flow analysis instead\n";
        std::cerr << "                       (see func_recovery.h)\n";
        std::cerr << "  --entry-alias NAME:  rename whichever function starts at the ELF\n";
        std::cerr << "                       entry point to NAME -- heuristic recovery names\n";
        std::cerr << "                       functions after their address, which shifts if\n";
        std::cerr << "                       the binary is relinked; callers of the generated\n";
        std::cerr << "                       C that want a stable entry symbol should use this\n";
        std::cerr << "  --extern-globals:    for linking multiple recompiled objects together --\n";
        std::cerr << "                       every object normally defines its own\n";
        std::cerr << "                       ppc_init_globals/ppc_dispatch, which collide at\n";
        std::cerr << "                       link time if more than one object's output is\n";
        std::cerr << "                       ever compiled into the same program. Pass this on\n";
        std::cerr << "                       every object except one \"primary\" one so only that\n";
        std::cerr << "                       one defines them; refuses to run if this object has\n";
        std::cerr << "                       any global/static data of its own, since that data\n";
        std::cerr << "                       would then never actually get initialized by\n";
        std::cerr << "                       anyone. Indirect (mtctr/bctrl) calls into this\n";
        std::cerr << "                       object's functions from code in a *different*\n";
        std::cerr << "                       object are also not supported yet -- only the\n";
        std::cerr << "                       primary object's own functions are reachable that\n";
        std::cerr << "                       way, since dispatch tables aren't merged across\n";
        std::cerr << "                       objects. Direct calls (bl) across objects work fine\n";
        std::cerr << "                       regardless.\n";
        return 1;
    }

    recomp::ElfImage img;
    std::string err;
    if (!recomp::load_elf(input_path, img, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    recomp::assign_global_addrs(img);
    recomp::find_import_trampolines(img);
    recomp::resolve_data_imports(img);

    if (extern_globals && !img.global_section_base.empty()) {
        std::cerr << "error: --extern-globals was passed, but this object has its own "
                      "global/static data (would never get initialized -- see --help)\n";
        return 1;
    }

    if (force_stripped || img.functions.empty()) {
        img.functions = recomp::recover_functions_heuristic(img.text, img.text_addr, img.entry, err);
        if (img.functions.empty()) {
            std::cerr << "error: heuristic function recovery found nothing: " << err << "\n";
            return 1;
        }
        std::cerr << "no usable symbol table -- recovered " << img.functions.size()
                   << " function(s) via control-flow analysis\n";
    }

    if (!entry_alias.empty()) {
        for (auto &fn : img.functions) {
            if (fn.addr == img.entry) fn.name = entry_alias;
        }
    }

    std::map<uint32_t, std::string> addr_to_name;
    for (const auto &fn : img.functions) addr_to_name[fn.addr] = fn.name;

    std::ostringstream body;
    int unhandled_total = 0;

    // Real GHS-linked binaries have symbol-table entries that share a
    // name -- confirmed against the real Skylanders: Spyro's Adventure
    // binary: several "__ghs_thunk__0x<offset>__<mangled target>" adjustor
    // thunks appear multiple times, generating a real "redefinition of
    // ppc_<name>" compile error otherwise. Skipping a name already
    // defined is safe: identically-named autogenerated symbols disassemble
    // to identical code either way (that's what makes their generated
    // name identical in the first place), so keeping just the first body
    // is correct even when the duplicate happens to sit at a different
    // address -- ppc_dispatch's separate by-address dedup (see below)
    // still routes every such address to this one shared body.
    std::set<std::string> defined_names;
    for (const auto &fn : img.functions) {
        if (!defined_names.insert(fn.name).second) continue;
        uint32_t start_off = fn.addr - img.text_addr;
        if (start_off + fn.size > img.text.size()) {
            std::cerr << "error: function '" << fn.name << "' extends past .text\n";
            return 1;
        }

        recomp::DisasmResult insns;
        if (!recomp::disassemble_range(&img.text[start_off], fn.size, fn.addr, insns, err)) {
            std::cerr << "error: disassembling '" << fn.name << "': " << err << "\n";
            return 1;
        }

        auto unhandled = recomp::generate_function_c(img, fn, insns, addr_to_name, body);
        unhandled_total += (int)unhandled.size();
        std::cerr << "recompiled " << fn.name << " (" << insns.size() << " instructions, " << unhandled.size()
                   << " unhandled)\n";
    }

    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "error: cannot write '" << output_path << "'\n";
        return 1;
    }
    out << "/* Generated by Bramble's recomp tool. Do not edit by hand. */\n";
    out << "#include \"ppc_runtime.h\"\n\n";
    std::set<std::string> declared;
    for (const auto &fn : img.functions) {
        out << "void ppc_" << fn.name << "(PpcContext *ctx);\n";
        declared.insert(fn.name);
    }
    // Functions called (via `bl`) but not defined in this translation unit
    // still need a prototype for the generated C to compile on its own --
    // real multi-object-file code will call across file boundaries
    // constantly, and C (unlike the assembly this was recompiled from)
    // doesn't let a call site get away with an undeclared callee. The
    // actual definition is expected to come from another recompiled
    // object's output (same `ppc_<name>` naming) linked in alongside this
    // one, or from hand-written C filling in a not-yet-recompiled function.
    for (const auto &entry : img.call_relocs) {
        const std::string &name = entry.second;
        if (declared.insert(name).second) {
            out << "void ppc_" << name << "(PpcContext *ctx); /* external -- defined in another translation unit */\n";
        }
    }
    // RPL cross-library imports (real Cafe OS system calls, e.g.
    // coreinit's FSFlushFile) resolved through find_import_trampolines --
    // these have no recompiled implementation at all yet. Declaring them
    // here makes the generated C compile and clearly documents exactly
    // which system functions a real CafeOS runtime shim (Phase 1d) needs
    // to provide, instead of leaving a #error at every call site.
    std::set<std::string> declared_imports;
    for (const auto &entry : img.import_trampolines) {
        const std::string key = entry.second.library + "_" + entry.second.function;
        if (declared_imports.insert(key).second) {
            out << "void ppc_import_" << key << "(PpcContext *ctx); /* RPL import: " << entry.second.library << "."
                << entry.second.function << " -- not implemented, needs the CafeOS runtime shim */\n";
        }
    }
    // Real Cafe OS "data imports" (see DataImport's own comment) --
    // routed through the exact same ppc_import_<library>_<function>
    // naming convention as the trampoline imports above, so a shim
    // header implementing one automatically covers both real call
    // shapes (a local `bl` to a `.fimport_*` trampoline, or an
    // indirect `bctrl` through a real `.dimport_*` function-pointer
    // slot) with the same one function.
    for (const auto &entry : img.data_imports) {
        const std::string key = entry.second.target.library + "_" + entry.second.target.function;
        if (declared_imports.insert(key).second) {
            out << "void ppc_import_" << key << "(PpcContext *ctx); /* Cafe OS data import: "
                << entry.second.target.library << "." << entry.second.target.function
                << " -- not implemented, needs the CafeOS runtime shim */\n";
        }
    }
    out << "void ppc_dispatch(PpcContext *ctx, uint32_t addr);\n";
    out << "\n";

    if (extern_globals) {
        out << "/* --extern-globals: ppc_init_globals/ppc_dispatch are defined in the "
               "primary translation unit, not here. */\n\n";
    } else {
        // Global/static variables and read-only data (.data/.bss/.rodata*)
        // live at synthetic addresses in ctx->mem (see
        // ElfImage::global_section_base) -- each section's initial content
        // has to actually be copied in before any recompiled function runs,
        // the same job a real ELF loader does. Callers must invoke this
        // once before calling any ppc_<function>.
        out << "void ppc_init_globals(PpcContext *ctx) {\n";
        for (const auto &entry : img.global_section_base) {
            auto bytes_it = img.section_bytes.find(entry.first);
            if (bytes_it == img.section_bytes.end()) continue;  // e.g. .bss: no file content, mem is already zeroed
            const std::vector<uint8_t> &bytes = bytes_it->second;
            for (size_t i = 0; i < bytes.size(); i++) {
                if (bytes[i] == 0) continue;  // ctx->mem starts zeroed; skip no-op stores
                out << "  ppc_store_u8(ctx, " << (entry.second + (uint32_t)i) << "u, " << (unsigned)bytes[i]
                    << ");\n";
            }
        }
        // Real Cafe OS data imports (see DataImport's own comment) -- on
        // real hardware, the RPL loader itself writes each of these
        // real function pointers into its slot before the game's own
        // code ever runs; this is that same real job, writing this
        // project's own real, reserved dispatchable fake_addr instead
        // of the real function's own real address (which doesn't exist
        // here -- there's no real linked .fimport_*-style local copy of
        // it, only a shim).
        for (const auto &entry : img.data_imports) {
            out << "  ppc_store_u32(ctx, " << entry.first << "u, " << entry.second.fake_addr << "u); /* data import: "
                << entry.second.target.library << "." << entry.second.target.function << " */\n";
        }
        out << "}\n\n";

        // Real, previously-missing piece found 2026-08-20 chasing a real
        // hang: GHS emits one real "__sti_<len>_<sourcefile>_<hash>"
        // function per translation unit that has C++ global/static
        // objects needing construction (the real Skylanders binary has
        // 114 of them) -- real Cafe OS's own process startup calls every
        // one of these before the game's actual entry point runs, the
        // same job libc's __libc_csu_init/.init_array walk does on other
        // platforms. This project's own pipeline never did that at all:
        // main.c called the game's entry point directly, so every real
        // C++ static/global object across the whole binary was reaching
        // gameplay code completely unconstructed -- confirmed as a real,
        // concrete gap by hardware-tracing one specific real symptom (a
        // lazily-cached heap handle inside Core::igMemoryContext reading
        // back 0 with no code anywhere in the whole 19,622-function
        // binary ever found to write it) back to this.
        //
        // UPDATE (2026-08-21): sorting by address turns out to be not
        // just "a reasonable approximation" but the real, confirmed-
        // correct order. Found the real ordering table itself: all 114
        // real __sti_ function addresses appear exactly once each, in
        // one single unbroken run, embedded directly in the real
        // .rodata section (this game's own build has no .init_array/
        // .ctors section at all -- GHS/RPL stores the constructor table
        // as plain data instead, which the real Cafe OS RPL loader walks
        // at load time). Extracted that real table and diff'd it against
        // plain address-ascending order: zero of 114 entries differ.
        // This was checked, not assumed, while chasing the real
        // igStringPoolContainer::mallocString stall -- rules out
        // static-initializer *ordering* as that bug's cause with hard
        // evidence, closing off a real, previously-open theory rather
        // than leaving it as an unverified guess.
        out << "void ppc_run_static_initializers(PpcContext *ctx) {\n";
        {
            std::vector<const recomp::ElfFunction *> sti_fns;
            for (const auto &fn : img.functions) {
                if (fn.name.rfind("__sti_", 0) == 0) sti_fns.push_back(&fn);
            }
            std::sort(sti_fns.begin(), sti_fns.end(),
                      [](const recomp::ElfFunction *a, const recomp::ElfFunction *b) { return a->addr < b->addr; });
            // See g_ppc_static_init_index's own comment in ppc_runtime.h --
            // set right before each call so a hang inside one of these is
            // pinpointable by index, not just by whichever tiny, widely-
            // shared linker helper g_ppc_current_pc happens to still show.
            uint32_t idx = 0;
            for (const recomp::ElfFunction *fn : sti_fns) {
                out << "  g_ppc_static_init_index = " << idx << "u; /* " << fn->name << " */\n";
                out << "  ppc_" << fn->name << "(ctx);\n";
                idx++;
            }
        }
        out << "}\n\n";

        // Indirect-call dispatch table (mtctr/bctrl -- function pointers,
        // vtables, callback tables): the target address is only known at
        // runtime, so it can't be resolved to a direct C call the way `bl`
        // is. Function addresses live in the same address space as
        // ElfFunction::addr (relocatable-object .text offsets, or real
        // linked addresses), so a plain switch over that address is enough
        // to route to the matching generated function -- within this
        // object only; see --extern-globals's help text for the
        // cross-object limitation.
        out << "void ppc_dispatch(PpcContext *ctx, uint32_t addr) {\n";
        out << "  switch (addr) {\n";
        // Multiple distinct symbols legitimately share one address in real
        // GHS-linked binaries -- identical-code-folding of byte-identical
        // functions (e.g. many empty destructors), and GHS adjustor
        // thunks aliasing their target. Confirmed against the real
        // Skylanders: Spyro's Adventure binary: gcc rejected the
        // generated switch with "duplicate case value" until this
        // dedup was added. Same address means same underlying code, so
        // arbitrarily keeping the first name seen is correct -- every
        // alias would disassemble to the exact same instructions anyway.
        std::set<uint32_t> dispatched_addrs;
        for (const auto &fn : img.functions) {
            if (!dispatched_addrs.insert(fn.addr).second) continue;
            out << "    case " << fn.addr << "u: ppc_" << fn.name << "(ctx); return;\n";
        }
        // Real Cafe OS data imports (see DataImport's own comment) -- a
        // real bctrl through a slot ppc_init_globals just initialized to
        // one of these fake_addr values routes here, same as any other
        // indirect call. Multiple real slots can resolve to the exact
        // same fake_addr (the same real function imported via more than
        // one real symbol) -- dedup the same way real function aliasing
        // above already has to.
        for (const auto &entry : img.data_imports) {
            if (!dispatched_addrs.insert(entry.second.fake_addr).second) continue;
            out << "    case " << entry.second.fake_addr << "u: ppc_import_" << entry.second.target.library << "_"
                << entry.second.target.function << "(ctx); return;\n";
        }
        out << "  }\n";
        out << "}\n\n";
    }

    out << body.str();

    if (unhandled_total > 0) {
        std::cerr << unhandled_total << " unhandled instruction(s) -- generated code will not compile.\n";
        return 2;
    }

    std::cerr << "wrote " << output_path << " (" << img.functions.size() << " function(s))\n";
    return 0;
}
