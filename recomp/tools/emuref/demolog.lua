-- Logs movie player calls (8008b448: index), first decoded frame (8008b5f8) and ends (8008b398: decodes, ra, s5 = frames,
-- s2 = 1 when the file ended) per vsync. No input. DL_OUT=<file> DL_VSYNCS=15000 DL_FORCE=<n> (play movie n instead of demo 3).
-- Usage (Redux folder): pcsx-redux.exe -run -debugger -iso <cue> -dofile demolog.lua -interpreter -no-ui
local OUT = os.getenv('DL_OUT')
local N = tonumber(os.getenv('DL_VSYNCS') or '15000')
local f = assert(io.open(OUT, 'w'))
local vs, dec = 0, 0
bpStart = PCSX.addBreakpoint(0x8008b448, 'Exec', 4, 'mstart', function()
  local r = PCSX.getRegisters()
  local force = tonumber(os.getenv('DL_FORCE') or '')
  if force and r.GPR.n.a0 == 3 then r.GPR.n.a0 = force end
  f:write(string.format('start vsync %d index %d skippable %d\n', vs, r.GPR.n.a0, r.GPR.n.a1)); dec = 0; return true end)
bpDecode = PCSX.addBreakpoint(0x8008b5f8, 'Exec', 4, 'decode', function()
  dec = dec + 1; if dec == 1 then f:write(string.format('first decode vsync %d\n', vs)) end; return true end)
bpEnd = PCSX.addBreakpoint(0x8008b398, 'Exec', 4, 'mend', function()
  local r = PCSX.getRegisters()
  f:write(string.format('end vsync %d decodes %d ra %08x s5 %d s2 %d s3 %d\n', vs, dec, r.GPR.n.ra, r.GPR.n.s5, r.GPR.n.s2, r.GPR.n.s3)); return true end)
listenVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  vs = vs + 1
  if vs % 1000 == 0 then f:flush() end
  if vs >= N then f:close(); PCSX.quit(0) end
end)
