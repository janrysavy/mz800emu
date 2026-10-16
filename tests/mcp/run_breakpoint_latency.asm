; Synthetic host-wakeup regression fixture; no emulated I/O or game assets.
; Assemble with sjasmplus --raw=run_breakpoint_latency.bin this-file.asm.
; Starting at loop, each resumed execution reaches loop again after exactly
;14 Z80 T-states and two instruction fetches. All registers except R/WZ stay
; unchanged; DI and normal RAM are supplied by the normal MZF bootstrap.
    ORG $1200
loop:
    NOP
    JP loop
