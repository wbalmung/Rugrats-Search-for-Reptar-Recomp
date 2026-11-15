-- Saves every displayed picture that differs from the previous vsync during the boot movies (reference frames).
-- Usage (from the Redux folder): MS_DIR=<dir, forward slashes> MS_VSYNCS=2600 pcsx-redux.exe -run -iso <cue>
--   -dofile movieshots.lua -interpreter -no-ui. No input is pressed, so the logo movies play in full.
-- Output: <dir>/mNNNNN_WxH_BPP.raw (NNNNN = vsync; BPP 16 = BGR555 LE, 24 = RGB bytes) and <dir>/movieshots.txt.
local DIR = os.getenv('MS_DIR')
local N = tonumber(os.getenv('MS_VSYNCS') or '2600')
local log = assert(io.open(DIR .. '/movieshots.txt', 'w'))
local vs, last = 0, nil
local function sum(s)
  local h = 0
  for i = 1, #s, 61 do h = (h * 31 + s:byte(i)) % 2147483647 end
  return h
end
listenVsync = PCSX.Events.createEventListener('GPU::Vsync', function()
  vs = vs + 1
  local ok, err = pcall(function()
    local ss = PCSX.GPU.takeScreenShot()
    local data = tostring(ss.data)
    local h = sum(data)
    if h ~= last then
      local bpp = (ss.bpp == 'BPP_24' or tonumber(ss.bpp) == 1) and 24 or 16
      local f = assert(io.open(string.format('%s/m%05d_%dx%d_%d.raw', DIR, vs, ss.width, ss.height, bpp), 'wb'))
      f:write(data); f:close()
      log:write(string.format('vsync %d changed %dx%d bpp %d\n', vs, ss.width, ss.height, bpp))
      last = h
    end
  end)
  if not ok then log:write('error ' .. tostring(err) .. '\n') end
  if vs >= N then log:close(); PCSX.quit(0) end
end)
