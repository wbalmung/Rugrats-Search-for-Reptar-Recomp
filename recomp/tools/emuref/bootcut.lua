-- Boots the game in PCSX-Redux with the native port's default test presses (Cross at f%240 100-109, Start at
-- 200-209 until vsync 7000) and logs the streamed cutscene counter (0x800C4C34 = gp+3508) and the stream header
-- length (0x800C43E0 = gp+1376) whenever they change. Compares in-engine cutscene lengths (house intro) with the port.
-- BC_DIR = output folder (forward slashes), BC_VSYNCS = vsyncs to run (default 7500).
local DIR = os.getenv('BC_DIR')
local LAST = tonumber(os.getenv('BC_VSYNCS') or '7500')
local ffi = require('ffi')
local logf = io.open(DIR .. '/bootcut.log', 'w')
local vsyncs, padState, prevC, prevL = 0, nil, -1, -1
local function setPad(b)
  if b == padState then return end
  local pad = PCSX.SIO0.slots[1].pads[1]
  if padState then pad.clearOverride(padState) end
  if b then pad.setOverride(b) end
  padState = b
end
listenVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  vsyncs = vsyncs + 1
  local B = PCSX.CONSTS.PAD.BUTTON
  local f = vsyncs % 240
  if f >= 100 and f < 110 then setPad(B.CROSS) elseif f >= 200 and f < 210 and vsyncs < 7000 then setPad(B.START) else setPad(nil) end
  local m = PCSX.getMemPtr()
  local c = ffi.cast('uint16_t*', m + 0xC4C34)[0]
  local l = ffi.cast('int16_t*', m + 0xC43E0)[0]
  if c ~= prevC or l ~= prevL then
    logf:write(string.format('vsync %d counter %d len %d\n', vsyncs, c, l)); logf:flush()
    prevC, prevL = c, l
  end
  local every = tonumber(os.getenv('BC_SHOTEVERY') or '0')   -- optional screenshots every N vsyncs (raw 16-bit)
  if every > 0 and vsyncs % every == 0 then
    local ss = PCSX.GPU.takeScreenShot()
    local g = io.open(string.format('%s/b%05d_%dx%d.raw', DIR, vsyncs, ss.width, ss.height), 'wb')
    g:write(tostring(ss.data)); g:close()
  end
  if vsyncs >= LAST then logf:write('done\n'); logf:close(); PCSX.quit(0) end
end)
