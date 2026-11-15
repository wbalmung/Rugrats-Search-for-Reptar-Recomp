-- Logs on which vsync the displayed picture changes during the boot movies (reference for movie frame pacing).
-- Usage (from the Redux folder): MF_OUT=<file> MF_VSYNCS=2600 pcsx-redux.exe -run -iso <cue> -dofile movieflips.lua
--   -interpreter -no-ui. No input is pressed, so the logo movies play in full. Output: "vsync N changed" lines and
-- the display start address each vsync it changes.
local OUT = os.getenv('MF_OUT')
local N = tonumber(os.getenv('MF_VSYNCS') or '2600')
local f = assert(io.open(OUT, 'w'))
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
    local h = sum(tostring(ss.data))
    if h ~= last then f:write(string.format('vsync %d changed %dx%d\n', vs, ss.width, ss.height)); last = h end
  end)
  if not ok then f:write('error ' .. tostring(err) .. '\n') end
  if vs >= N then f:close(); PCSX.quit(0) end
end)
