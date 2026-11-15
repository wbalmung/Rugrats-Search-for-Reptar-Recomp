-- Logs, per vsync, when the boot movie player starts decoding a frame (8008b5f8, first DecDCTout of a frame),
-- each strip upload (8008b7a0, LoadImage in the DMA1 callback) and each display flip call (8009cb10, PutDispEnv's
-- GP1 05 write site: return address 8009cb18). Reference for movie frame timing vs our port.
-- Usage (from the Redux folder): MO_OUT=<file> MO_VSYNCS=1700 pcsx-redux.exe -run -iso <cue> -dofile movieorder.lua
--   -interpreter -no-ui
local OUT = os.getenv('MO_OUT')
local N = tonumber(os.getenv('MO_VSYNCS') or '1700')
local f = assert(io.open(OUT, 'w'))
local vs = 0
local function cyc() return tonumber(PCSX.getCPUCycles and PCSX.getCPUCycles() or 0) end
bpDecode = PCSX.addBreakpoint(0x8008b5f8, 'Exec', 4, 'decode', function()
  f:write(string.format('decode vsync %d\n', vs)); return true end)
bpUpload = PCSX.addBreakpoint(0x8008b7a0, 'Exec', 4, 'upload', function()
  f:write(string.format('upload vsync %d\n', vs)); return true end)
bpFlip = PCSX.addBreakpoint(0x8009cb10, 'Exec', 4, 'flip', function()
  f:write(string.format('flip vsync %d\n', vs)); return true end)
local ffi = require('ffi')
local mem = PCSX.getMemPtr()
local function flag() return mem[0x0C3F04] end   -- gp+132: bit1 = flip requested, bit0 = display switch next callback
bpCb = PCSX.addBreakpoint(0x8001d0d8, 'Exec', 4, 'vbcb', function()
  f:write(string.format('vbcb vsync %d flag %02x\n', vs, flag())); return true end)
bpWait = PCSX.addBreakpoint(0x8001d440, 'Exec', 4, 'wait', function()
  f:write(string.format('wait vsync %d flag %02x\n', vs, flag())); return true end)
bpFend = PCSX.addBreakpoint(0x8001d3e8, 'Exec', 4, 'fend', function()
  f:write(string.format('fend vsync %d flag %02x\n', vs, flag())); return true end)
listenVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  vs = vs + 1
  if vs >= N then f:close(); PCSX.quit(0) end
end)
