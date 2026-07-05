-- Real-host smoke test: insert Tenganisha on a track with real audio, render
-- 10 s through it, quit. Outer harness null-tests the render against the
-- input (expect quantization floor, ~-90 dB RMS, while in passthrough state).
--
-- Configure via env (set before launching REAPER):
--   TENGANISHA_TEST_IN   input wav (default /tmp/tenganisha-reaper/input.wav)
--   TENGANISHA_TEST_OUT  render dir (default /tmp/tenganisha-reaper)
-- Run from REAPER: Actions > Show action list > New action > Load ReaScript.
-- (Passing the .lua to the REAPER binary on the macOS command line did not
--  execute it in testing; load it through the Actions list.)

local outDir  = os.getenv("TENGANISHA_TEST_OUT") or "/tmp/tenganisha-reaper"
local wavIn   = os.getenv("TENGANISHA_TEST_IN") or (outDir .. "/input.wav")
local logPath = outDir .. "/reaper_test.log"

local function log(s)
  local f = io.open(logPath, "a")
  if f then f:write(s .. "\n") f:close() end
end

log("script started")

reaper.InsertTrackAtIndex(0, false)
local tr = reaper.GetTrack(0, 0)

reaper.SetOnlyTrackSelected(tr)
reaper.SetEditCurPos(0, false, false)
local inserted = reaper.InsertMedia(wavIn, 0)
log("media inserted: " .. tostring(inserted))

local fx = reaper.TrackFX_AddByName(tr, "VST3:Tenganisha", false, -1)
log("fx index: " .. tostring(fx))
if fx < 0 then
  log("FAIL: plugin not found")
else
  reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)      -- master mix
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)    -- custom bounds
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 10.0, true)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 44100, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", outDir, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "out", true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true) -- wav

  reaper.Main_OnCommand(42230, 0) -- render project, using recent settings, auto-close
  log("render command issued")
end

log("script done")
reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER
