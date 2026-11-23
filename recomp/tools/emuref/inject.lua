-- Replays a Rugrats native-port save state in PCSX-Redux (reference check).
-- Phase A: when the game first reaches its frame pacing call (8001ceb4), copy the loader blob to 0x800F0000
--          and jump there (uploads our VRAM + GPU settings through the GPU ports).
-- Phase B: at the loader's DONE loop, copy our RAM (above the kernel), scratchpad, CPU and GTE registers,
--          set PC = 8001ceb4, run, and dump a screenshot every vblank for NSHOTS frames, then quit.
local DIR = os.getenv('INJECT_DIR')          -- scratchpad path, forward slashes
local NSHOTS = tonumber(os.getenv('INJECT_SHOTS') or '40')
local BOOTWAIT = tonumber(os.getenv('INJECT_BOOTWAIT') or '1500')
local TIMEOUT = tonumber(os.getenv('INJECT_TIMEOUT') or '6000')
local SHOTEVERY = tonumber(os.getenv('INJECT_SHOTEVERY') or '1') -- screenshot every Nth run vsync (1 = all)
local ffi = require('ffi')
local logf = io.open(DIR .. '/inject.log', 'w')
local function log(...) local t = {} for i, v in ipairs({...}) do t[#t+1] = tostring(v) end
  logf:write(table.concat(t, ' ') .. '\n'); logf:flush() end
local function readfile(p) local f = assert(io.open(p, 'rb')); local d = f:read('*a'); f:close(); return d end

local regs = {}
for l in io.lines(DIR .. '/export/regs.txt') do regs[#regs + 1] = tonumber(l, 16) end
local phase, vsyncs, shots, pending = 'boot', 0, 0, nil

local function phaseA()
  local blob = readfile(DIR .. '/inject/blob.bin')
  local mem = PCSX.getMemPtr()
  ffi.copy(mem + 0xF0000, blob, #blob)
  -- optional sound RAM (mkloader, port export since 2026-11-10): the loader DMAs it from 0x80070000 into the SPU
  local sf = io.open(DIR .. '/inject/spu_ram.bin', 'rb')
  if sf then local d = sf:read('*a'); sf:close(); ffi.copy(mem + 0x70000, d, #d); log('spu ram', #d) end
  PCSX.invalidateCache()
  PCSX.getRegisters().pc = 0x800F0000
  log('phase A at vsync', vsyncs, 'blob', #blob)
end

local function phaseB()
  local ram = readfile(DIR .. '/export/ram.bin')
  local mem = PCSX.getMemPtr()
  do local f = io.open(DIR .. '/emu/emu_ram_before.bin', 'wb'); f:write(ffi.string(mem, 0x200000)); f:close() end
  -- words to keep from the emulator (BIOS-owned links/ids inside game RAM): keep.txt = hex offsets
  local keep = {}
  local kf = io.open(DIR .. '/inject/keep.txt', 'r')
  if kf then for l in kf:lines() do local o = tonumber(l, 16); if o then keep[#keep+1] = { o, ffi.cast('uint32_t*', mem + o)[0] } end end kf:close() end
  ffi.copy(mem + 0x10000, ffi.cast('const uint8_t*', ram) + 0x10000, 0x200000 - 0x10000)
  for _, k in ipairs(keep) do ffi.cast('uint32_t*', mem + k[1])[0] = k[2] end
  log('kept', #keep, 'emulator words')
  local sp = readfile(DIR .. '/export/scratch.bin')
  ffi.copy(PCSX.getScratchPtr(), sp, 1024)
  local r = PCSX.getRegisters()
  for i = 1, 31 do PCSX.setGPR(i, regs[2 + i]) end
  PCSX.setGPR(33, regs[34])   -- hi
  PCSX.setGPR(32, regs[35])   -- lo
  for i = 0, 31 do r.CP2D.r[i] = regs[36 + i] end
  for i = 0, 31 do r.CP2C.r[i] = regs[68 + i] end
  PCSX.invalidateCache()
  r.pc = regs[1]
  log('phase B at vsync', vsyncs, string.format('pc %08x ra %08x sp %08x sr %08x', r.pc, r.GPR.n.ra, r.GPR.n.sp, r.CP0.r[12]))
end

local nframe = 0
-- INJECT_DUMPFROM=<k> INJECT_DUMPN=<n>: dump game frames k .. k+n-1 after the injection (default 1, 12)
local DUMPFROM = tonumber(os.getenv('INJECT_DUMPFROM') or '1')
local DUMPN = tonumber(os.getenv('INJECT_DUMPN') or '12')
local function frameDump()
  if phase ~= 'run' then return true end
  nframe = nframe + 1
  if nframe < DUMPFROM then return true end
  if nframe >= DUMPFROM + DUMPN then return true end
  local mem = PCSX.getMemPtr()
  local f = io.open(string.format('%s/emu/ram_f%d.bin', DIR, nframe), 'wb'); f:write(ffi.string(mem, 0x200000)); f:close()
  local r = PCSX.getRegisters()
  local t = { string.format('%08x', r.pc) }
  for i = 0, 31 do t[#t+1] = string.format('%08x', r.GPR.r[i]) end
  t[#t+1] = string.format('%08x', r.GPR.n.hi); t[#t+1] = string.format('%08x', r.GPR.n.lo)
  for i = 0, 31 do t[#t+1] = string.format('%08x', r.CP2D.r[i]) end
  for i = 0, 31 do t[#t+1] = string.format('%08x', r.CP2C.r[i]) end
  local g = io.open(string.format('%s/emu/regs_f%d.txt', DIR, nframe), 'w'); g:write(table.concat(t, '\n') .. '\n'); g:close()
  log('frame dump', nframe, 'at vsync', vsyncs)
  return true  -- true keeps the breakpoint (false deletes it); no pause here
end
-- breakpoints kept in globals so the GC never removes them
bpA = PCSX.addBreakpoint(tonumber(os.getenv('INJECT_AT') or '8001ceb4', 16), 'Exec', 4, 'injectA', function()
  if phase == 'run' then return frameDump() end
  if phase ~= 'boot' or vsyncs < BOOTWAIT then return true end
  phase = 'A'; pending = phaseA
  PCSX.pauseEmulator()
  return true
end)
local doneAddr = 0x800F0114  -- loader's DONE loop; mkloader writes the real address to inject/done.txt
do local df = io.open(DIR .. '/inject/done.txt', 'r'); if df then doneAddr = tonumber(df:read('*l'), 16) or doneAddr; df:close() end end
bpB = PCSX.addBreakpoint(doneAddr, 'Exec', 4, 'injectB', function()
  if phase ~= 'A' then return false end
  phase = 'B'; pending = phaseB
  PCSX.pauseEmulator()
  return true
end)

listenPause = PCSX.Events.createEventListener('ExecutionFlow::Pause', function()
  if pending then
    local f = pending; pending = nil
    local ok, err = pcall(f)
    if not ok then log('error', err) end
    if phase == 'B' then phase = 'run'; bpB:disable() end
    PCSX.nextTick(function() PCSX.resumeEmulator() end)
  end
end)

local padState = nil
local function setPad(b)
  if b == padState then return end
  local ok, err = pcall(function()
    local pad = PCSX.SIO0.slots[1].pads[1]
    if padState then for _, x in ipairs(type(padState) == 'table' and padState or { padState }) do pad.clearOverride(x) end end
    if b then for _, x in ipairs(type(b) == 'table' and b or { b }) do pad.setOverride(x) end end
  end)
  if not ok then log('pad error', err) end
  padState = b
end
-- INJECT_PAD=<from>:<to>:<buttons>,... : buttons held from vsync <from> to <to>-1 after the injection (1 = first run
-- vsync). Buttons = names joined by '+', e.g. up+cross. INJECT_PEEK=<hex addr>: log 3 words there every run vsync.
local padPlan = {}
for from, to, names in (os.getenv('INJECT_PAD') or ''):gmatch('(%d+):(%d+):([%a%+]+)') do
  padPlan[#padPlan + 1] = { tonumber(from), tonumber(to), names }
end
local peekAddr = tonumber(os.getenv('INJECT_PEEK') or '', 16)
local function runPad(n)
  local B = PCSX.CONSTS.PAD.BUTTON
  local set, key = {}, {}
  for _, p in ipairs(padPlan) do
    if n >= p[1] and n < p[2] then
      for nm in p[3]:gmatch('%a+') do local b = B[nm:upper()]; if b and not key[b] then key[b] = 1; set[#set + 1] = b end end
    end
  end
  table.sort(set)
  local id = table.concat(set, ',')
  if id ~= padKey then padKey = id; setPad(#set > 0 and set or nil) end
end
listenVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  vsyncs = vsyncs + 1
  if phase == 'boot' then  -- same scripted presses as the native port's test mode: Cross at f%240 100-109, Start at 200-209
    local f = vsyncs % 240
    local B = PCSX.CONSTS.PAD.BUTTON
    if f >= 100 and f < 110 then setPad(B.CROSS) elseif f >= 200 and f < 210 and vsyncs < 7000 then setPad(B.START) else setPad(nil) end
  elseif phase == 'run' then
    runPad(shots + 1)
  else
    setPad(nil)
  end
  -- INJECT_OFX=<pixels>: force GTE OFX (screen x offset) every run vsync, to look past the 4:3 screen edge
  if phase == 'run' and os.getenv('INJECT_OFX') then
    PCSX.getRegisters().CP2C.r[24] = math.floor(tonumber(os.getenv('INJECT_OFX')) * 65536)
  end
  if phase == 'run' and peekAddr then
    local w = ffi.cast('int32_t*', PCSX.getMemPtr() + bit.band(peekAddr, 0x1FFFFC))
    log(string.format('peek run %d: %d %d %d', shots + 1, w[0], w[1], w[2]))
  end
  if vsyncs % 300 == 0 then
    log('vsync', vsyncs, 'phase', phase, string.format('pc %08x', PCSX.getRegisters().pc))
    if phase ~= 'run' then
      local ok, err = pcall(function()
        local ss = PCSX.GPU.takeScreenShot()
        local f = assert(io.open(string.format('%s/emu/boot%05d_%dx%d_%s.raw', DIR, vsyncs, ss.width, ss.height, tonumber(ss.bpp)), 'wb'))
        f:write(tostring(ss.data)); f:close()
      end)
      if not ok then log('shot error', err) end
    end
  end
  if phase == 'run' then
    shots = shots + 1
    do
      local m = PCSX.getMemPtr()
      local r = PCSX.getRegisters()
      local hw = ffi.cast('uint32_t*', PCSX.getScratchPtr())
      log(string.format('run %d pc %08x ra %08x sp %08x sr %08x cause %08x', shots, r.pc, r.GPR.n.ra, r.GPR.n.sp, r.CP0.r[12], r.CP0.r[13]))
    end
    if shots % SHOTEVERY == 0 then
    local ss = PCSX.GPU.takeScreenShot()
    local f = io.open(string.format('%s/emu/s%03d_%dx%d_%d.raw', DIR, shots, ss.width, ss.height, ss.bpp == 'BPP_24' and 24 or (tonumber(ss.bpp) == 1 and 24 or 16)), 'wb')
    f:write(tostring(ss.data)); f:close()
    end
    if shots == 1 or shots == NSHOTS then
      local v = PCSX.GPU.getVRAM()
      local g = io.open(string.format('%s/emu/vram%03d.bin', DIR, shots), 'wb'); g:write(tostring(v)); g:close()
    end
    if shots >= NSHOTS then log('done', shots); PCSX.quit(0) end
  elseif vsyncs > TIMEOUT then
    log('timeout in phase', phase); PCSX.quit(2)
  end
end)
-- INJECT_BPLOG=<hex>,<hex>...: log every execution of these addresses after the injection (run vsync, a0, ra)
bpLogBps = {}  -- global: unreferenced breakpoints are garbage-collected (and stop firing)
for a in (os.getenv('INJECT_BPLOG') or ''):gmatch('%x+') do
  local addr = tonumber(a, 16)
  bpLogBps[#bpLogBps + 1] = PCSX.addBreakpoint(addr, 'Exec', 4, 'bplog', function()
    if phase ~= 'run' then return true end  -- returning nil/false would delete the breakpoint during boot
    local r = PCSX.getRegisters()
    log(string.format('bp %08x run %d a0 %d ra %08x', addr, shots + 1, r.GPR.n.a0, r.GPR.n.ra))
    return true
  end)
end
-- INJECT_WPLOG=<hex>,...: log every write to these addresses after the injection (run vsync, pc, ra)
wpBps = {}
for a in (os.getenv('INJECT_WPLOG') or ''):gmatch('%x+') do
  local addr = tonumber(a, 16)
  wpBps[#wpBps + 1] = PCSX.addBreakpoint(addr, 'Write', 2, 'wplog', function()
    if phase ~= 'run' then return true end
    local r = PCSX.getRegisters()
    log(string.format('wp %08x run %d pc %08x ra %08x', addr, shots + 1, r.pc, r.GPR.n.ra))
    return true
  end)
end
log('script loaded')
-- GTE probes: log every RTPS/RTPT (raw regs, same format as the port's RUGRATS_GTELOG) after the injection
gteLeft = tonumber(os.getenv('INJECT_GTELOG') or '0')
gteFile = io.open(DIR .. '/gte_emu.txt', 'w')
gteIn = nil
gteBps = {}
local function u(x) return bit.band(x, 0xFFFFFFFF) end
for l in io.lines(DIR .. '/gtesites.txt') do
  local a, k = l:match('(%x+) (%x+)')
  a = tonumber(a, 16); k = tonumber(k, 16)
  local name = k == 0x30 and 'rtpt' or 'rtps'
  gteBps[#gteBps + 1] = PCSX.addBreakpoint(a, 'Exec', 4, 'gteIn', function()
    if phase == 'run' and gteLeft > 0 then
      local r = PCSX.getRegisters()
      gteIn = string.format('%08x %04x %08x %04x %08x %04x tr %08x %08x %08x', u(r.CP2D.r[0]), bit.band(r.CP2D.r[1], 0xFFFF),
        u(r.CP2D.r[2]), bit.band(r.CP2D.r[3], 0xFFFF), u(r.CP2D.r[4]), bit.band(r.CP2D.r[5], 0xFFFF),
        u(r.CP2C.r[5]), u(r.CP2C.r[6]), u(r.CP2C.r[7]))
    end
    return true
  end)
  gteBps[#gteBps + 1] = PCSX.addBreakpoint(a + 4, 'Exec', 4, 'gteOut', function()
    if phase == 'run' and gteLeft > 0 and gteIn then
      local r = PCSX.getRegisters()
      gteFile:write(string.format('gtelog %s in %s out %08x %08x %08x %04x %04x %04x flag %08x\n', name, gteIn,
        u(r.CP2D.r[12]), u(r.CP2D.r[13]), u(r.CP2D.r[14]), bit.band(r.CP2D.r[17], 0xFFFF), bit.band(r.CP2D.r[18], 0xFFFF),
        bit.band(r.CP2D.r[19], 0xFFFF), u(r.CP2C.r[31])))
      gteIn = nil
      gteLeft = gteLeft - 1
      if gteLeft == 0 then gteFile:close() end
    end
    return true
  end)
end
log('gte probes', #gteBps)
