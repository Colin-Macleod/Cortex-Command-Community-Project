function Create(self)
	self.speed = self.Vel.Magnitude;
	if self:NumberValueExists("CollectTargetUniqueID") then
		local mo = MovableMan:FindObjectByUniqueID(self:GetNumberValue("CollectTargetUniqueID"));
		if mo then
			self.target = mo;
			self.targetUniqueID = mo.UniqueID;
			self.Sharpness = 0;
		end
	else
		self.ToDelete = true;
	end
end

function Update(self)
	-- The target can be deleted at any time, so look it up again by its unique ID.
	self.target = self.targetUniqueID and MovableMan:FindObjectByUniqueID(self.targetUniqueID) or nil;
	if self.target and self.target.ID ~= rte.NoMOID then
		self:NotResting();
		local targetPos = IsHDFirearm(self.target) and ToHDFirearm(self.target).MuzzlePos or self.target.Pos;
		local dist = SceneMan:ShortestDistance(self.Pos, targetPos, SceneMan.SceneWrapsX);
		if dist:MagnitudeIsGreaterThan(self.speed) then
			self.Vel = Vector(dist.X, dist.Y):SetMagnitude(self.speed) - (SceneMan.GlobalAcc * TimerMan.DeltaTimeSecs) * self.GlobalAccScalar;
		else
			self.ToDelete = true;
		end
	else
		self.target = nil;
		self.PinStrength = 0;
	end
	
	if self.PinStrength > 0 then
		self.Pos = self.Pos + self.Vel * rte.PxTravelledPerFrame;
	end
end