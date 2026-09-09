# OSWaitEvent parked without delivering queued FS completions

**Status:** fixed 2026-09-09 in `include/cafeos_coreinit_sync.h`.
**Scope:** any guest that waits on an event for an async file read before it
has started a second thread.
**Severity:** hard deadlock. One thread, one queued completion, no crash.

## The shape

The FS shim does not complete `FSReadFileWithPosAsync` inline. It cannot:
Cafe OS delivers the callback later, on its own I/O thread, and the game is
written against that order — `igCafeStorageDevice::read` stores into its own
object *after* the call returns, and running the callback inside the call let
`setStatus` and `igMemoryPool::free` land first. So completions are queued and
delivered at the next import call, which is always between guest instructions
and after the initiating function has returned.

`OSWaitEventWithTimeout` pumped that queue before parking. **`OSWaitEvent` did
not.** That was survivable only because two worker threads sit in
`jqWorkerSleep` calling the timeout variant forever, so somebody always ran
the pump.

## Why it surfaced now

The 2026-09-09 relocation fix made `igArkCore::init` load the registry for the
first time. That path runs *before any guest thread exists*:

```
igArkCore::init
  -> igRegistry::read(igFile*)
  -> igPhysicalStorageDevice::update
       FSReadFileWithPosAsync   -- shim reads the file, queues the completion
  -> igCafeSignal::wait
       2156878: addi r3, r3, 0x10
       215687c: b OSWaitEvent   -- parks
```

Measured, from the run that found it:

```
fs: open=1  ard=1/542  fsz=542  buf=0x4530c40
    head=[0x3c726f6f,0x743e0a09,...]   = "<root>\n\t<Core\n\t\t"
asyncq: q=1 done=0 drop=0 pend=1
cb: ok=0 skip=0
evt: sig=0 wake=0 tmo=0
threads: created=0 started=0
```

The read had already succeeded — `alchemy.xml`'s 542 bytes were sitting in the
guest buffer. The completion that would raise the signal was one entry deep in
a queue that nothing was left alive to drain. Guest calls froze at 159,416
with `last_pc = igCafeSignal::wait` and stayed there for the remaining 25,000
frames.

`threads: created=0` is the whole story in one counter: the design assumed a
parked worker would always be there to pump, and at this point in boot there
is not one.

## The fix

`arkchemy_fs_pump_completions(ctx)` at the top of `OSWaitEvent`, before
`arkchemy_event_get`. The ordering is not cosmetic: the completion callback
runs guest code that signals this very event, and `ArkchemyEventEntry::lock`
is not recursive, so pumping while holding it would swap one deadlock for
another.

## The general rule

**Every blocking import is a place the pump must run**, not just the ones that
happen to have a worker parked in them. A shim that blocks the calling thread
and does not first drain deferred work is a deadlock waiting for the right
call order. The remaining blocking imports were checked; the mutex and
condition paths already pump.
