"""Step 3: static recompiler. R3000A machine code -> C.

usage: python -I recomp.py <extract_dir> <out_c_dir>

Model: every discovered function entry becomes `void f_XXXXXXXX(void)`.
CPU registers live in the global `cpu` struct (runtime/cpu.h).
jal -> direct C call, jr ra -> return, jalr / unknown jr -> rt_dispatch().
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Ins  # noqa: E402
from analyze import find_bios_stubs  # noqa: E402

FUNCS_PER_FILE = 150

# guest functions with a native hook (runtime/hooks.c): address -> purpose
HOOKS = {
    0x8009af48: "VSync",
    0x8009c264: "DrawSync",
    0x8001d0d8: "frame callback (clears transition flag)",
    0x8001c408: "transition step",
    0x8001ce48: "set game time step (a0 = vblanks elapsed)",
    0x8001d3c4: "poll: buffer flip done? (flip normally happens in the vblank callback)",
    0x8001ceb4: "frame pacing: wait until >= a1 vblanks since a0, clamp to a2",
    0x8008b398: "movie player cleanup (debug log)",
    0x8009fd08: "ReadGeomScreen (widescreen: squeeze portal corners before the CPU projection in 80033278)",
    0x80031834: "object visibility test (native; widescreen: squeeze the CPU-projected centre/radius)",
    0x8001eb4c: "object camera matrix, returns its camera z (object draws: widescreen near-cull relaxed for the side bands)",
    0x80032cf4: "room geometry chunk visibility test (bbox corners through the GTE vs rect): RUGRATS_CHUNKLOG debug log",
    0x800102dc: "mesh quad face setup (t9 > 0 = draw): faces of near-cull-passed objects with a corner at the lens are skipped",
    0x80010000: "model draw (a0 = model): back-to-back double-sided panels (signs) drawn one-sided (pair cull)",
    0x80010174: "mesh triangle face setup (t9 > 0 = draw): same as 800102dc",
    0x800112dc: "room tri face setup (t9 > 0 = draw): faces reaching behind the camera culled by mirrored outcodes pass when in view",
    0x800113c8: "room quad face setup (t9 > 0 = draw): same as 800112dc",
    0x80033278: "room/portal visibility test: parent rect widened by RUGRATS_PORTALPAD px during the call (door jamb pop-in)",
    0x8008b448: "movie player entry (a0 = movie index; RUGRATS_MOVIE debug override)",
    0x80030010: "still screen for N seconds (boot: legal screen; skip_intro)",
    0x8008b230: "THQ logo movie wrapper (skip_intro at boot)",
    0x8008b27c: "Nickelodeon logo movie wrapper (skip_intro at boot)",
    0x8008b0b4: "n-Space logo movie wrapper (skip_intro at boot)",
    0x80018c38: "level/menu run loop (a0 = mode; returns the next database number); bench logging",
    0x8006ee14: "called by the main loop right after 80018c38 returns (s4 = next database); RUGRATS_LEVEL override",
    0x8002953c: "gameplay camera update (cam, player): right-stick camera puts the game's own eye back",
    0x8005be24: "player movement (a0 = player): golf aim speed / Aisle 7 pie gallery aim rescale its per-frame aim nudge",
    0x8003f934: "menu/HUD model draw (button icons etc.): widescreen keeps the model's 4:3 screen position",
    0x8002407c: "project N points through the GTE (from 8008f718 = HUD quad, e.g. golf swing meter frame): no widescreen squeeze for the HUD",
    0x8009b0c0: "libetc v_wait(target vblank count, timeout): native wait (the guest loop timed out early)",
    0x80072bf0: "timer list (1/30 s callbacks): native pass, callbacks see the time since they last fired; hub label redraw",
    0x80031a28: "arrow over a character's head: blink by a 30 Hz real-time counter instead of the frame counter",
    0x80075698: "eye blink state machine: steps at the 30 Hz game rate (high fps blinked 4x fast / often)",
    0x80049eec: "hub level-name label timer callback (draws the text, then 80050fd0 sparkles)",
    0x80050fd0: "portal sparkle spawn: skipped while the label is only being redrawn",
    0x800211e8: "LookAt(eye*, target*, matrix*): right-stick camera orbits the eye (only from 8001e838 via 80029df0)",
    0x800818e0: "per-level idle voice line (chatter) timer, gp+0xe40: chatter option stretches / stops it",
    0x8008da4c: "play XA voice line (a0 = id): Restored Lines swaps idle chatter picks for never-used lines",
    0x8007a9d8: "golf hole finished (a0 = player): Golf Reactions plays the score / fail line first, then runs it",
    0x8005fcb8: "golf stroke counted at ball launch (a0 = player): Golf Reactions arms a ball-rolling line",
    0x8007db6c: "Circus trike on the target (queues event 57, called every frame): Restored Lines holds it for Phil's line 762",
    0x8007db90: "Circus trike missed the target (queues event 56): skipped while a hit is held for line 762",
    0x80010e14: "room geometry renderer: GTE depth kind DCLS_ROOM for the hi-res depth check (depthfix)",
    0x80031b84: "world object draw: GTE depth kind DCLS_OBJ (depthfix)",
    0x8003205c: "world object draw (multi-part): GTE depth kind DCLS_OBJ (depthfix)",
    0x80030ce0: "room water surface draw: GTE depth kind DCLS_OBJ, no model-sort bits (depthfix: water under a ledge)",
    0x800120fc: "room triangle 2D split (edge midpoints): midpoints get their ends' depth (depthfix)",
    0x80011854: "room quad 2D split (edge midpoints + centre): midpoints get their ends' depth (depthfix)",
    0x80011a28: "room quad split texture coordinates: exact fractions of split points (perspective textures)",
    0x800a99a8: "libsnd SsUtKeyOnV: sfx debug log (RUGRATS_SFXLOG)",
    0x8006f26c: "sound effect play (main): Reptar bar roar option (sound 0x1c from 800517b0), sfx debug log",
    0x8006f2fc: "sound effect wrapper: sfx debug log",
    0x8007412c: "streamed cutscene packet fetch: skip empty padding packets (OPEN.BIN), Smooth Cutscenes blend, debug log (RUGRATS_CUTLOG)",
    0x80019c88: "cutscene stream CD callback: arrival time of each packet (Smooth Cutscenes)",
    0x8005e134: "player walk step: 30 fps step x ticks/10 with carried remainder (soap walk speed at high fps)",
    0x8005e1f4: "player walk state body (state 2): Wall Bonk option turns its bonk request (state 9) into idle",
    0x80072d38: "proximity checker (5 objects per frame, in/out-of-range callbacks): runs at 30 Hz game time (door close sound)",
    0x8008864c: "spawn particle from descriptor a0 (feather rate match, RUGRATS_PARTLOG)",
    0x80057214: "set animation speed obj+0xe4 (clamp): Walk After Shooting option resets the walk to 1x after a shot",
    0x800631d8: "player Triangle target finder (debug log RUGRATS_ENTLOG)",
    0x8004f78c: "level entrance Triangle handler (obj, player): obj+108 = db (debug log RUGRATS_ENTLOG)",
    0x800781f8: "queue 'leave level for db a1' message: After Last Piece option drops the go-to-final-level (db 7) one",
    0x80093048: "puzzle message screen (fade in, wait X, fade out): after 'You did it!' (ra 800949d0) show 'Final level unlocked!'",
    0x8006f3a4: "sound effect wrapper: sfx debug log",
    0x8006f438: "sound effect wrapper: sfx debug log",
    0x8006f644: "sound effect play (positional?): sfx debug log",
    0x80070064: "sound effect wrapper: sfx debug log",
    0x8007013c: "sound effect wrapper: sfx debug log",
    0x80064a34: "movement collision probe (obj, mode): player probe sees >= 10 ticks (PS1 look-ahead; high fps ledge hop loop)",
    0x80077864: "damage event (a0 = msg: +4 obj, +0x10 other obj, +0x14 amount -> obj+0x1a4): player hit cooldown",
    0x8004e2ac: "spinning prop timer callback (yaw += obj+0x2c.. x vblanks): kitchen ceiling fan still in Let There Be Light (db 12)",
    0x80056834: "anim update (obj): marks timer-driven characters for in-between smoothing (Chuckie, RUGRATS_AISMOOTH)",
}

# Load instructions whose loaded value goes through a native function (runtime/hooks.c):
# rt = rt_load_XXXXXXXX(value). Used where one read of the frame's ticks must see the PS1's minimum step.
# Screen position copies (gpu.c subpx_tag): lw / sw instructions that carry a projected point from the vertex
# buffer into a packet, recorded with RECOMP_TAGALL=1 + RUGRATS_TAGLOG=1 (see recomp/README.md). Only these get the
# tag bookkeeping (on every lw / sw it cost ~8% frame time).
TAG_ALL = os.environ.get("RECOMP_TAGALL") == "1"
TAG_SITES = set()
_ts = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tagsites.txt")
if os.path.exists(_ts):
    for _l in open(_ts):
        _l = _l.split("#")[0].strip()
        if _l: TAG_SITES.add(int(_l, 16))

# The room renderer (mesh transform, near-polygon splits, packet building) gets full tracking: every lw / sw and
# register moves too, so split points copied around by moves keep their tags (gpu.c subpx_split).
TAG_RANGE = (0x80010e14, 0x80012820)   # f_80010e14 (calls the splits) up to the split routines
def tag_here(a):
    return TAG_ALL or a in TAG_SITES or TAG_RANGE[0] <= a < TAG_RANGE[1]

LOAD_HOOKS = {
    0x8005c7ec: "player movement 8005be24: ticks for the turn step (high fps: turn target spun instead of snapping)",
}

# `addiu rt, rs, imm` instructions whose constant step is replaced by a native function (runtime/hooks.c):
# rt = rs + rt_step_XXXXXXXX(imm). Used for per-game-frame steps that must follow real time at high fps.
STEP_HOOKS = {
    0x80058b34: "power meter (80058a78) falling step -6 per game frame",
    0x80058b64: "power meter rising step +6 per game frame",
    0x80058ba8: "power meter second phase step +6 per game frame",
}


# Save states (runtime/savestate.c) resume a gameplay frame by re-entering the guest call chain at these return
# addresses, so each must be a function entry. They are guest addresses (fixed by the disc): keep them in every
# future build or old save states can't be loaded. Chain (callee -> caller): pacing 8001ceb4 returns to 8001d0a8
# (in 8001d00c), which returns to 8001d400 (8001d3e8), 80019038 (level loop 80018c38), 80018208 (main loop
# 80017c0c), 80018828 (main 800185f8), 8009788c (boot entry 800977e8).
RESUME_POINTS = [0x8001d0a8, 0x8001d400, 0x80019038, 0x80018208, 0x80018828, 0x8009788c]


class Program:
    def __init__(self, extract_dir):
        info = json.load(open(os.path.join(extract_dir, "disc.json")))
        exe = open(os.path.join(extract_dir, "game.exe.bin"), "rb").read()
        self.info = info
        self.base = info["text_addr"]
        self.image = exe[0x800:0x800 + info["text_size"]]
        n = len(self.image) // 4
        self.words = struct.unpack("<%dI" % n, self.image[:n * 4])
        self.end = self.base + n * 4
        self._ins = {}

    def in_text(self, a):
        return self.base <= a < self.end and a % 4 == 0

    def ins(self, a):
        x = self._ins.get(a)
        if x is None:
            x = Ins(a, self.words[(a - self.base) // 4])
            self._ins[a] = x
        return x

    def word(self, a):
        return self.words[(a - self.base) // 4]

    # ---- jump tables -------------------------------------------------
    def jump_table(self, jr):
        """Recover targets of `jr rX` fed by `lw rX, lo(rY)` with rY = lui hi + index."""
        # forward pass over the preceding window: track constants and const+index sums
        const = {}   # reg -> int
        based = {}   # reg -> table base (const + some index)
        table = None
        bound = None
        start = jr.addr - 4 * 64
        for a in range(start, jr.addr, 4):
            if not self.in_text(a):
                continue
            x = self.ins(a)
            n = x.name
            if n == "lui":
                const[x.rt] = x.imm << 16
                based.pop(x.rt, None)
                continue
            if n in ("addiu", "ori") and x.rs in const:
                v = const[x.rs] + (x.simm if n == "addiu" else x.imm)
                const[x.rt] = v & 0xFFFFFFFF
                based.pop(x.rt, None)
                continue
            if n == "addiu" and x.rs in based:
                based[x.rt] = (based[x.rs] + x.simm) & 0xFFFFFFFF
                const.pop(x.rt, None)
                continue
            if n == "addu":
                if x.rs in const and x.rt not in const:
                    based[x.rd] = const[x.rs]
                elif x.rt in const and x.rs not in const:
                    based[x.rd] = const[x.rt]
                else:
                    based.pop(x.rd, None)
                const.pop(x.rd, None)
                continue
            if n == "lw":
                if x.rs in based:
                    cand = (based[x.rs] + x.simm) & 0xFFFFFFFF
                    if x.rt == jr.rs:
                        table = cand
                const.pop(x.rt, None)
                based.pop(x.rt, None)
                continue
            if n == "sltiu":
                bound = x.imm
            # any other write clobbers tracking for its destination
            if n in ("sll", "srl", "sra", "sllv", "srlv", "srav", "and", "or", "xor", "nor",
                     "slt", "sltu", "subu", "sub", "add", "mfhi", "mflo"):
                const.pop(x.rd, None)
                based.pop(x.rd, None)
            elif n in ("andi", "xori", "slti", "sltiu", "lb", "lbu", "lh", "lhu", "addi", "addiu", "ori"):
                const.pop(x.rt, None)
                based.pop(x.rt, None)
        if table is None:
            return None
        targets = []
        t = table
        limit = bound if bound else 512
        while len(targets) < limit and self.in_text(t):
            w = self.word(t)
            if not self.in_text(w) or abs(w - jr.addr) > 0x10000:
                break
            targets.append(w)
            t += 4
        return targets or None

    # ---- discovery -----------------------------------------------------
    def explore(self, entry, entries):
        """Recursive descent of one function. Returns (body set, jr-table map, new_entries)."""
        body = set()
        tables = {}
        work = [entry]
        new_entries = set()
        while True:
            self._explore_work(entry, entries, body, tables, work, new_entries)
            # a branch that lands on a delay slot continues sequentially after it
            extra = []
            for a in body:
                x = self.ins(a)
                tgts = []
                if x.is_branch():
                    tgts.append(x.branch_target())
                elif x.name == "j":
                    tgts.append(x.target)
                for t in tgts:
                    if t in body and self.in_text(t - 4) and self.ins(t - 4).has_delay() \
                            and (t + 4) not in body and self.in_text(t + 4) and (t + 4) not in entries:
                        extra.append(t + 4)
            for tg in tables.values():
                for t in tg:
                    if t in body and self.in_text(t - 4) and self.ins(t - 4).has_delay() \
                            and (t + 4) not in body and (t + 4) not in entries:
                        extra.append(t + 4)
            if not extra:
                break
            work.extend(extra)
        return body, tables, new_entries

    def _explore_work(self, entry, entries, body, tables, work, new_entries):
        while work:
            a = work.pop()
            while self.in_text(a) and a not in body:
                if a != entry and a in entries:  # fell/branched into another function -> tail call
                    break
                x = self.ins(a)
                if x.name == "invalid":
                    body.add(a)
                    break
                body.add(a)
                if x.has_delay():
                    if self.in_text(a + 4):
                        body.add(a + 4)
                    if x.is_branch():
                        t = x.branch_target()
                        if self.in_text(t):
                            if t in entries and t != entry:
                                pass
                            else:
                                work.append(t)
                        if x.name in ("bltzal", "bgezal") and self.in_text(t):
                            new_entries.add(t)
                        # unconditional `beq zero,zero` stops the fallthrough
                        if x.name == "beq" and x.rs == 0 and x.rt == 0:
                            break
                        a += 8
                        continue
                    if x.name == "j":
                        if self.in_text(x.target) and x.target not in entries:
                            work.append(x.target)
                        break
                    if x.name == "jal":
                        a += 8
                        continue
                    if x.name == "jalr":
                        a += 8
                        continue
                    if x.name == "jr":
                        if x.rs != 31:
                            tgts = self.jump_table(x)
                            if tgts:
                                tables[a] = tgts
                                work.extend(t for t in tgts if t not in entries or t == entry)
                        break
                if x.name == "break":
                    break
                a += 4

    def discover(self, analysis):
        entries = set(analysis["entries"])
        entries |= set(RESUME_POINTS) if not os.environ.get("RUGRATS_NO_RESUME") else set()
        stubs = find_bios_stubs([self.ins(a) for a in range(self.base, self.end, 4)])
        self.stubs = stubs
        entries |= set(stubs)
        # setjmp: "sw ra,0(a0); sw sp,4(a0)". Code after each call to it is re-entered by
        # longjmp / the BIOS custom exception exit, so it becomes its own entry point.
        self.setjmps = set()
        for e in list(entries):
            if self.in_text(e + 12):
                a = self.ins(e)
                if a.name == "sw" and a.rs == 4 and a.rt == 31 and a.simm == 0 and any(
                        self.ins(e + k).name == "sw" and self.ins(e + k).rs == 4 and self.ins(e + k).rt == 29
                        for k in (4, 8, 12)):
                    self.setjmps.add(e)
        for a in range(self.base, self.end, 4):
            x = self.ins(a)
            if x.name == "jal" and x.target in self.setjmps:
                entries.add(a + 8)
        ptr_cands = set(analysis["data_ptrs"]) | set(analysis["code_ptrs"])
        code_ptr_set = set(analysis["code_ptrs"])
        funcs = {}
        while True:
            for e in sorted(entries - set(funcs)):
                body, tables, ne = self.explore(e, entries)
                funcs[e] = (body, tables)
                entries |= ne
            # targets jumped to from multiple functions (shared tails) become entries
            all_bodies = collections.Counter()
            for body, _ in funcs.values():
                all_bodies.update(body)
            covered = set(all_bodies)
            added = set()
            for p in ptr_cands:
                if p in entries or (p in covered and p not in code_ptr_set):
                    continue
                x = self.ins(p)
                if x.name == "invalid":
                    continue
                prev = self.ins(p - 4) if self.in_text(p - 4) else None
                # plausible function start: after a return/jump delay slot, or stack frame setup
                pp = self.ins(p - 8) if self.in_text(p - 8) else None
                looks = (x.name == "addiu" and x.rs == 29 and x.rt == 29 and x.simm < 0) or \
                        (pp is not None and pp.name in ("jr", "j")) or \
                        (prev is not None and prev.word == 0 and pp is not None and pp.word == 0)
                # addresses built in code (lui/addiu) are taken on purpose: always an entry
                if looks or p in code_ptr_set:
                    added.add(p)
            # branch targets that land inside *another* function's entry range are fine (tail call);
            # j targets that are not entries but are inside no body get explored as part of caller.
            if not added - entries:
                if entries - set(funcs):
                    continue  # entries found during this pass still need exploring
                break
            entries |= added
            # re-explore functions whose body overlaps a new entry so they tail-call instead
            for e in list(funcs):
                if funcs[e][0] & added:
                    del funcs[e]
        self.entries = entries
        self.funcs = funcs
        return funcs


# ---------------------------------------------------------------------------
# C emission

def R(i):
    return "0" if i == 0 else f"R[{i}]"


def W(i):
    return f"R[{i}]"


class Emitter:
    def __init__(self, prog):
        self.p = prog
        self.unknown_jr = []

    def emit_ins(self, x, out, ind="    "):
        n, rs, rt, rd = x.name, x.rs, x.rt, x.rd

        def put(s):
            out.append(ind + s)

        def setr(r, expr):
            if r != 0:
                put(f"{W(r)} = {expr};")

        if n == "sll":
            if x.word == 0:
                return
            setr(rd, f"{R(rt)} << {x.sa}")
        elif n == "srl":
            setr(rd, f"{R(rt)} >> {x.sa}")
        elif n == "sra":
            setr(rd, f"(uint32_t)((int32_t){R(rt)} >> {x.sa})")
        elif n == "sllv":
            setr(rd, f"{R(rt)} << ({R(rs)} & 31)")
        elif n == "srlv":
            setr(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        elif n == "srav":
            setr(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))")
        elif n == "syscall":
            put(f"rt_syscall(0x{x.addr:08x}u);")
        elif n == "break":
            put(f"rt_break(0x{x.addr:08x}u);")
        elif n == "mfhi":
            setr(rd, "cpu.hi")
        elif n == "mthi":
            put(f"cpu.hi = {R(rs)};")
        elif n == "mflo":
            setr(rd, "cpu.lo")
        elif n == "mtlo":
            put(f"cpu.lo = {R(rs)};")
        elif n == "mult":
            put(f"{{ int64_t m = (int64_t)(int32_t){R(rs)} * (int32_t){R(rt)}; "
                f"cpu.lo = (uint32_t)m; cpu.hi = (uint32_t)(m >> 32); }}")
        elif n == "multu":
            put(f"{{ uint64_t m = (uint64_t){R(rs)} * {R(rt)}; "
                f"cpu.lo = (uint32_t)m; cpu.hi = (uint32_t)(m >> 32); }}")
        elif n == "div":
            put(f"rt_div({R(rs)}, {R(rt)});")
        elif n == "divu":
            put(f"rt_divu({R(rs)}, {R(rt)});")
        elif n in ("add", "addu"):
            setr(rd, f"{R(rs)} + {R(rt)}")
            if rd and (rs == 0 or rt == 0) and TAG_RANGE[0] <= x.addr < TAG_RANGE[1]:   # move: keep the tag source
                put(f"tag_ld[{rd}] = tag_ld[{rs or rt}];")
        elif n in ("sub", "subu"):
            setr(rd, f"{R(rs)} - {R(rt)}")
        elif n == "and":
            setr(rd, f"{R(rs)} & {R(rt)}")
        elif n == "or":
            setr(rd, f"{R(rs)} | {R(rt)}")
            if rd and (rs == 0 or rt == 0) and TAG_RANGE[0] <= x.addr < TAG_RANGE[1]:   # move: keep the tag source
                put(f"tag_ld[{rd}] = tag_ld[{rs or rt}];")
        elif n == "xor":
            setr(rd, f"{R(rs)} ^ {R(rt)}")
        elif n == "nor":
            setr(rd, f"~({R(rs)} | {R(rt)})")
        elif n == "slt":
            setr(rd, f"(int32_t){R(rs)} < (int32_t){R(rt)}")
        elif n == "sltu":
            setr(rd, f"{R(rs)} < {R(rt)}")
        elif n == "addiu" and x.addr in STEP_HOOKS:
            setr(rt, f"{R(rs)} + (uint32_t)rt_step_{x.addr:08x}({x.simm})")
        elif n in ("addi", "addiu"):
            setr(rt, f"{R(rs)} + {x.simm & 0xFFFFFFFF:#x}u")
        elif n == "slti":
            setr(rt, f"(int32_t){R(rs)} < {x.simm}")
        elif n == "sltiu":
            setr(rt, f"{R(rs)} < {x.simm & 0xFFFFFFFF:#x}u")
        elif n == "andi":
            setr(rt, f"{R(rs)} & {x.imm:#x}u")
        elif n == "ori":
            setr(rt, f"{R(rs)} | {x.imm:#x}u")
        elif n == "xori":
            setr(rt, f"{R(rs)} ^ {x.imm:#x}u")
        elif n == "lui":
            setr(rt, f"{x.imm << 16:#x}u")
        elif n in ("lb", "lh", "lw", "lbu", "lhu"):
            addr = f"{R(rs)} + {x.simm & 0xFFFFFFFF:#x}u"
            fn = {"lb": "(uint32_t)(int8_t)mem_r8", "lbu": "mem_r8", "lh": "(uint32_t)(int16_t)mem_r16",
                  "lhu": "mem_r16", "lw": "mem_r32"}[n]
            if rt == 0:
                put(f"(void){fn}({addr});")
            elif x.addr in LOAD_HOOKS:
                setr(rt, f"rt_load_{x.addr:08x}({fn}({addr}))")
            elif n == "lw" and tag_here(x.addr):   # remember where it came from (gte.c tag_ld)
                pc = f" tag_lpc[{rt}] = {x.addr:#x}u;" if TAG_ALL else ""
                put(f"{{ const uint32_t a_ = {addr}; {W(rt)} = mem_r32(a_); tag_ld[{rt}] = a_;{pc} }}")
            else:
                setr(rt, f"{fn}({addr})")
        elif n in ("lwl", "lwr"):
            addr = f"{R(rs)} + {x.simm & 0xFFFFFFFF:#x}u"
            if rt:
                setr(rt, f"mem_{n}({R(rt)}, {addr})")
        elif n in ("sb", "sh", "sw"):
            addr = f"{R(rs)} + {x.simm & 0xFFFFFFFF:#x}u"
            if os.environ.get("RECOMP_WATCH") == "1":   # debug: log stores to RUGRATS_WATCH (gpu.c rt_watch)
                put(f"rt_watch({addr}, {x.addr:#x}u, {R(rt)}, {rt}, tag_ld[{rt}]);")
            fn = {"sb": "mem_w8", "sh": "mem_w16", "sw": "mem_w32"}[n]
            if n == "sw" and rt and TAG_ALL:   # profiling: log which lw / sw pairs copy tagged points
                put(f"{{ const uint32_t a_ = {addr}; mem_w32(a_, {R(rt)}); if (tag_src(tag_ld[{rt}]) && subpx_tag_cp(a_, tag_ld[{rt}], {R(rt)})) tag_site(tag_lpc[{rt}], {x.addr:#x}u); }}")
            elif n == "sw" and rt and tag_here(x.addr):   # a copied screen position keeps its tag (gpu.c subpx_tag)
                put(f"{{ const uint32_t a_ = {addr}; mem_w32(a_, {R(rt)}); if (tag_src(tag_ld[{rt}])) subpx_tag_cp(a_, tag_ld[{rt}], {R(rt)}); }}")
            else:
                put(f"{fn}({addr}, {R(rt)});")
        elif n in ("swl", "swr"):
            addr = f"{R(rs)} + {x.simm & 0xFFFFFFFF:#x}u"
            put(f"mem_{n}({addr}, {R(rt)});")
        elif n == "lwc2" and 12 <= rt <= 15:   # screen position back into the GTE: keep its tag
            put(f"gte_lwc2_sxy({rt}, {R(rs)} + {x.simm & 0xFFFFFFFF:#x}u);")
        elif n == "lwc2":
            put(f"gte_write({rt}, mem_r32({R(rs)} + {x.simm & 0xFFFFFFFF:#x}u));")
        elif n == "swc2" and 12 <= rt <= 15:   # screen position: tag its RAM word (gte.c gte_swc2_sxy)
            put(f"gte_swc2_sxy({R(rs)} + {x.simm & 0xFFFFFFFF:#x}u, {rt});")
        elif n == "swc2":
            put(f"mem_w32({R(rs)} + {x.simm & 0xFFFFFFFF:#x}u, gte_read({rt}));")
        elif n == "mfc2" and 12 <= rd <= 15 and rt:   # screen position into a register: keep its tag
            setr(rt, f"gte_read({rd})")
            put(f"gte_mfc2_sxy({rt}, {rd});")
        elif n == "mfc2":
            setr(rt, f"gte_read({rd})")
        elif n == "cfc2":
            setr(rt, f"gte_read({rd + 32})")
        elif n == "mtc2":
            if 12 <= rd <= 15 and rt:   # screen position: restore its sub-pixel tag (gte.c gte_mtc2_sxy)
                put(f"gte_mtc2_sxy({rd}, {R(rt)}, tag_ld[{rt}]);")
            else:
                put(f"gte_write({rd}, {R(rt)});")
        elif n == "ctc2":
            put(f"gte_write({rd + 32}, {R(rt)});")
        elif n == "cop2":
            put(f"gte_op(0x{x.word & 0x1FFFFFF:07x}u);")
        elif n == "mfc0":
            setr(rt, f"cop0_read({rd})")
        elif n == "mtc0":
            put(f"cop0_write({rd}, {R(rt)});")
        elif n == "rfe":
            put("cop0_rfe();")
        elif n == "cop0op":
            put(f"/* cop0 op {x.word:08x} */")
        elif n == "invalid" or x.has_delay():
            # (a control instruction only reaches here when it sits in a delay slot)
            put(f"rt_invalid(0x{x.addr:08x}u);")
        else:
            raise NotImplementedError(n)

    def cond(self, x):
        n, rs, rt = x.name, x.rs, x.rt
        return {
            "beq": f"{R(rs)} == {R(rt)}", "bne": f"{R(rs)} != {R(rt)}",
            "blez": f"(int32_t){R(rs)} <= 0", "bgtz": f"(int32_t){R(rs)} > 0",
            "bltz": f"(int32_t){R(rs)} < 0", "bgez": f"(int32_t){R(rs)} >= 0",
            "bltzal": f"(int32_t){R(rs)} < 0", "bgezal": f"(int32_t){R(rs)} >= 0",
        }[n]

    def jump(self, func_entry, body, target, out, ind):
        """Goto inside function, or tail call."""
        if target in body and (target != func_entry or True) and target in self.labels:
            if target <= self.cur:
                out.append(ind + f"RT_POLL(0x{target:08x}u);")
            out.append(ind + f"goto L_{target:08x};")
        elif self.p.in_text(target) and target in self.p.entries:
            out.append(ind + f"f_{target:08x}(); return;")
        else:
            out.append(ind + f"rt_dispatch(0x{target:08x}u); return;")

    def emit_function(self, entry, body, tables):
        p = self.p
        out = [f"void f_{entry:08x}(void) {{"]
        if entry in HOOKS:
            # native hook runs first; returning nonzero skips the guest function (it set v0 itself)
            out.append(f"    if (rt_hook_{entry:08x}()) return;")
        # labels: entry + every branch/jump/table target inside body
        labels = {entry}
        for a in body:
            x = p.ins(a)
            if x.is_branch():
                labels.add(x.branch_target())
            elif x.name == "j":
                labels.add(x.target)
            elif x.has_delay() or x.name == "jal":
                pass
        for tg in tables.values():
            labels.update(tg)
        labels &= body
        self.labels = labels
        order = sorted(body)
        # contiguous runs; insert an explicit jump when a run does not fall through
        out.append("    RT_ENTER();")
        if order and order[0] != entry:
            # the body also holds code below the entry (an entry inside a loop: the loop start is explored
            # from the back branch); code is emitted in address order, so start at the entry explicitly
            out.append(f"    goto L_{entry:08x};")
        prev_end = None
        skip = set()
        i = 0
        while i < len(order):
            a = order[i]
            self.cur = a
            if a in skip:
                i += 1
                continue
            if prev_end is not None and a != prev_end:
                if prev_end in p.entries:
                    out.append(f"    f_{prev_end:08x}(); return;")
                else:
                    out.append(f"    rt_fallthrough_error(0x{prev_end:08x}u); return;")
            if a in labels:
                out.append(f"L_{a:08x}:")
            x = p.ins(a)
            prev_end = a + 4
            if x.has_delay():
                d = p.ins(a + 4) if (a + 4) in body else None
                skip.add(a + 4)
                prev_end = a + 8
                if (a + 4) in labels:
                    # branch into a delay slot: emit the slot standalone too
                    out.append(f"    goto L_{a:08x}_after;")
                    out.append(f"L_{a + 4:08x}:")
                    if d:
                        self.emit_ins(d, out)
                    out.append(f"    goto L_{a + 8:08x};")
                    labels.add(a + 8)
                    out.append(f"L_{a:08x}_after:;")
                self.emit_control(entry, body, x, d, out)
            else:
                self.emit_ins(x, out)
                if x.name in ("break", "invalid"):
                    out.append("    return;")
                    prev_end = None
            i += 1
        if prev_end is not None:
            out.append(f"    f_{prev_end:08x}(); return;" if prev_end in p.entries
                       else f"    rt_fallthrough_error(0x{prev_end:08x}u);")
        out.append("}")
        # labels that were added late (a+8 after delay-slot targets) must exist
        text = "\n".join(out)
        for lab in labels:
            if f"L_{lab:08x}:" not in text:
                # jump target outside emitted code -> stub label that tail calls
                text = text[:-1] + f"L_{lab:08x}: f_{lab:08x}(); return;\n}}" if lab in p.entries else \
                    text[:-1] + f"L_{lab:08x}: rt_dispatch(0x{lab:08x}u); return;\n}}"
        return text

    def emit_control(self, entry, body, x, d, out):
        n = x.name
        ind = "    "
        dly = []
        if d is not None and d.has_delay():
            # control transfer in a delay slot: never valid in real code paths
            out.append(f"    rt_invalid(0x{x.addr:08x}u); return;")
            return
        if d is not None:
            self.emit_ins(d, dly)
        if x.is_branch():
            t = x.branch_target()
            out.append(f"    {{ int c = {self.cond(x)};")
            if n in ("bltzal", "bgezal"):
                out.append(f"    R[31] = 0x{x.addr + 8:08x}u;")
            out.extend("  " + s for s in dly)
            if n in ("bltzal", "bgezal"):
                out.append(f"      if (c) f_{t:08x}(); }}")
                return
            j = []
            self.jump(entry, body, t, j, "      ")
            if x.name == "beq" and x.rs == 0 and x.rt == 0:
                out.extend(j)
                out.append("    }")
            else:
                out.append("      if (c) {")
                out.extend("  " + s for s in j)
                out.append("      } }")
        elif n == "j":
            out.extend(dly)
            self.jump(entry, body, x.target, out, ind)
        elif n == "jal":
            out.extend(dly)
            out.append(f"    R[31] = 0x{x.addr + 8:08x}u;")
            if x.target in self.p.stubs:
                tbl, fn = self.p.stubs[x.target]
                out.append(f"    bios_call(0x{tbl:02x}, 0x{fn:02x});")
            elif x.target in self.p.entries:
                out.append(f"    f_{x.target:08x}();")
            else:
                out.append(f"    rt_dispatch(0x{x.target:08x}u);")
        elif n == "jalr":
            out.append(f"    {{ uint32_t t = {R(x.rs)};")
            out.extend("  " + s for s in dly)
            if x.rd:
                out.append(f"      R[{x.rd}] = 0x{x.addr + 8:08x}u;")
            out.append("      rt_dispatch(t); }")
        elif n == "jr":
            if x.rs == 31:
                out.extend(dly)
                out.append("    RT_RETURN(R[31]); return;")
            else:
                out.append(f"    {{ uint32_t t = {R(x.rs)};")
                out.extend("  " + s for s in dly)
                tg = self.p.funcs[entry][1].get(x.addr)
                if tg:
                    out.append("      switch (t) {")
                    for t in sorted(set(tg)):
                        if t in self.labels:
                            out.append(f"      case 0x{t:08x}u: goto L_{t:08x};")
                    out.append("      }")
                out.append("      rt_dispatch(t); return; }")


def main():
    extract_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    analysis = json.load(open(os.path.join(extract_dir, "analysis.json")))
    p = Program(extract_dir)
    funcs = p.discover(analysis)
    em = Emitter(p)
    entries = sorted(funcs)
    code_addrs = set()
    for b, _ in funcs.values():
        code_addrs |= b
    print(f"functions: {len(funcs)}  code words: {len(code_addrs)}  "
          f"jump tables: {sum(len(t) for _, t in funcs.values())}")

    hdr = ["#pragma once", "#include \"cpu.h\""]
    hdr += [f"void f_{e:08x}(void);" for e in entries]
    hdr += [f"int rt_hook_{h:08x}(void); /* {name} */" for h, name in HOOKS.items()]
    hdr += [f"int32_t rt_step_{h:08x}(int32_t imm); /* {name} */" for h, name in STEP_HOOKS.items()]
    hdr += [f"uint32_t rt_load_{h:08x}(uint32_t v); /* {name} */" for h, name in LOAD_HOOKS.items()]
    open(os.path.join(out_dir, "recomp_funcs.h"), "w").write("\n".join(hdr) + "\n")

    nfile = 0
    for i in range(0, len(entries), FUNCS_PER_FILE):
        chunk = entries[i:i + FUNCS_PER_FILE]
        src = ["#include \"recomp_funcs.h\"", "#define R cpu.r", ""]
        for e in chunk:
            body, tables = funcs[e]
            src.append(em.emit_function(e, body, tables))
            src.append("")
        open(os.path.join(out_dir, f"recomp_{nfile:03d}.c"), "w").write("\n".join(src))
        nfile += 1

    # dispatch table (sorted for binary search)
    tab = ["#include \"recomp_funcs.h\"", "const rt_func_entry rt_func_table[] = {"]
    tab += [f"  {{0x{e:08x}u, f_{e:08x}}}," for e in entries]
    tab += ["};", f"const int rt_func_count = {len(entries)};"]
    open(os.path.join(out_dir, "recomp_table.c"), "w").write("\n".join(tab) + "\n")

    # the original EXE image (text+data), copied into RAM at boot
    exe = open(os.path.join(extract_dir, "game.exe.bin"), "rb").read()
    lines = ["const unsigned int game_exe_size = %d;" % len(exe), "const unsigned char game_exe[] = {"]
    for i in range(0, len(exe), 32):
        lines.append(",".join(str(b) for b in exe[i:i + 32]) + ",")
    lines.append("};")
    open(os.path.join(out_dir, "recomp_exe.c"), "w").write("\n".join(lines) + "\n")

    # metadata
    meta = {"text_addr": p.base, "pc0": p.info["pc0"], "gp0": p.info["gp0"], "sp": p.info["sp"],
            "functions": len(entries), "files": nfile}
    json.dump(meta, open(os.path.join(out_dir, "recomp_meta.json"), "w"), indent=1)
    print(f"wrote {nfile} C files to {out_dir}")


if __name__ == "__main__":
    main()
