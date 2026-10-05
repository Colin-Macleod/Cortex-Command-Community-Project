-- The trail particle can be deleted at any time and its memory reused by another object, so it's kept by UniqueID and looked up every update.
local function FindTrailParticle(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and MovableMan:IsParticle(mo) then
		return ToMOPixel(mo);
	end
	return nil;
end

function Create(self)
	self.trailPar = CreateMOPixel("Old Stock Blaster Pistol Trail Glow", "Base.rte");
	self.trailPar.Pos = self.Pos;
	self.trailPar.Vel = self.Vel * 0.1;
	self.trailPar.Lifetime = 60;
	MovableMan:AddParticle(self.trailPar);
	self.trailParUID = self.trailPar.UniqueID;
end

function Update(self)
	self.trailPar = FindTrailParticle(self.trailParUID);
	if not self.ToDelete and self.trailPar then
		self.trailPar.Pos = self.Pos - Vector(self.PrevVel.X, self.PrevVel.Y):SetMagnitude(math.min(self.PrevVel.Magnitude * rte.PxTravelledPerFrame, self.TrailLength) * 0.5);
		self.trailPar.Vel = self.PrevVel * 0.5;
		self.trailPar.Lifetime = self.Age + TimerMan.DeltaTimeMS;
	end
	
	if self.Vel:MagnitudeIsLessThan(4) then
		self.ToDelete = true;
	end
end