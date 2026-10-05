function Create(self)
	self.turnStrength = 5;
	self.lifeTimer = Timer();
	self.targetSound = CreateSoundContainer("Explosive Device Detonate", "Base.rte");

	if self:NumberValueExists("TargetUniqueID") then
		local mo = MovableMan:FindObjectByUniqueID(self:GetNumberValue("TargetUniqueID"));
		if mo and IsMOSRotating(mo) then
			-- Kept by unique ID and looked up every update, as the target can be deleted at any time.
			self.targetUniqueID = mo.UniqueID;
			self.targetSound:Play(self.Pos);
		end
	end
	
	self.lifeTimer:SetSimTimeLimitMS(self.Lifetime - math.ceil(TimerMan.DeltaTimeMS));
end

function Update(self)
	self.GlobalAccScalar = 1/math.sqrt(1 + math.abs(self.Vel.X) * 0.1);
	local target = self.targetUniqueID and MovableMan:FindObjectByUniqueID(self.targetUniqueID);
	if target and not target.ToDelete and target.ID ~= rte.NoMOID then
		local targetDist = SceneMan:ShortestDistance(self.Pos, target.Pos, SceneMan.SceneWrapsX);
		if targetDist:MagnitudeIsLessThan(self.Diameter) then
			self:GibThis();
		else
			local targetVel = targetDist:SetMagnitude(self.turnStrength);

			local turnAngle = self.Vel.AbsRadAngle - targetVel.AbsRadAngle;
			turnAngle = turnAngle > math.pi and turnAngle - (math.pi * 2) or (turnAngle < -math.pi and turnAngle + (math.pi * 2) or turnAngle);

			self.Vel = (self.Vel + targetVel):SetMagnitude(self.Vel.Magnitude);
			self.AngularVel = self.AngularVel * 0.5 - (turnAngle * self.turnStrength);
		end
	end

	if self.lifeTimer:IsPastSimTimeLimit() then
		self:GibThis();
	end
end