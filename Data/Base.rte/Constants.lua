--Misc global constants. Add whatever you think makes sense across multiple scripts. All should be under the rte table.
rte = {};

rte.NoMOID = 255;
rte.SpawnIntervalScale = 1.0;
rte.StartingFundsScale = 1.0;
rte.DiggersRate = 0.4;
rte.MetabaseArea = "MetabaseServiceArea";
-- Depends on the sim's delta time, which can change between Activities (a co-op match uses the host's), so it's worked out again whenever one starts.
local function updatePxTravelledPerFrame()
	rte.PxTravelledPerFrame = GetPPM() * TimerMan.DeltaTimeSecs;
end
updatePxTravelledPerFrame();
_AddActivityStartCallback("rte.PxTravelledPerFrame", updatePxTravelledPerFrame);

--Materials
rte.airID = 0;
rte.goldID = 2;
rte.grassID = 128;
rte.doorID = 181;