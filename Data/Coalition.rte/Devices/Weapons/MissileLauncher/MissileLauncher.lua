function Create(self)
	self.fireVel = 50;
	self.checkTimer = Timer();

	self.targetLostTimer = Timer();
	self.targetLostTimer:SetSimTimeLimitMS(3000);

	self.laserLength = self.SharpLength + math.sqrt(FrameMan.PlayerScreenWidth^2 + FrameMan.PlayerScreenHeight^2) * 0.5;
	self.laserPointerOffset = Vector(0, -3);

	self.markerRotAngle = math.random() * math.pi;
	self.markerTurnSpeed = 10;
	self.markerColor = 13;
	self.markerSize = 0;

	self.lockThreshold = 12;

	self.arrow = CreateMOSRotating("Grapple Gun Guide Arrow");
	self.detectSound = CreateSoundContainer("Mine Activate", "Base.rte");
	self.missile = CreateAEmitter("Particle Coalition Missile Launcher", "Coalition.rte");
end

-- The locked target, looked up by unique ID every update, as it can be deleted at any time.
local function GetLockedTarget(self)
	local target = self.targetID and MovableMan:FindObjectByUniqueID(self.targetID);
	if target and IsMOSRotating(target) and not target.ToDelete then
		return IsACrab(target) and ToACrab(target) or ToMOSRotating(target);
	end
	self.targetID = nil;
	return nil;
end

function ThreadedUpdate(self)
	local parent = self:GetRootParent();
	local sharpAimProgress = 0;
	if parent and IsActor(parent) then
		parent = ToActor(parent);
		sharpAimProgress = parent.SharpAimProgress;
		local controller = parent:GetController();
		local screen = ActivityMan:GetActivity():ScreenOfPlayer(controller.Player);
		local playerControlled = parent:IsPlayerControlled();
		local markerSize = (self.markerSize * 0.95) + (self.markerSize * 0.1) * math.sin(self.markerRotAngle/(self.markerTurnSpeed + math.sqrt(self.markerSize)));
		if not self:IsReloading() then
			if controller:IsState(Controller.AIM_SHARP) then
				local startPos = self.Pos + Vector(self.laserPointerOffset.X * self.FlipFactor, self.laserPointerOffset.Y):RadRotate(self.RotAngle);
				local hitPos = Vector();
				local skipPx = 10;
				local trace = Vector(self.laserLength * self.FlipFactor, 0):RadRotate(self.RotAngle);
				local obstRay = SceneMan:CastObstacleRay(startPos, trace, hitPos, Vector(), parent.ID, self.Team, rte.airID, skipPx);
				if obstRay >= 0 then
					obstRay = obstRay - skipPx + SceneMan:CastObstacleRay(hitPos - trace:SetMagnitude(skipPx), trace, hitPos, Vector(), parent.ID, parent.Team, rte.airID, 1);
					local endPos = startPos + trace:SetMagnitude(obstRay);
					local moCheck = SceneMan:GetMOIDPixel(hitPos.X, hitPos.Y);
					if moCheck ~= rte.NoMOID then
						local mo = ToMOSRotating(MovableMan:GetMOFromID(MovableMan:GetMOFromID(moCheck).RootID));
						if mo and mo.ClassName ~= "ADoor" and mo.Team ~= parent.Team and mo.GetsHitByMOs then
							local movement = (mo.Vel.Magnitude + math.abs(mo.AngularVel) + 0.1) * math.sqrt(mo.Radius);
							if movement > self.lockThreshold then

								self.targetLostTimer:Reset();
								if self.targetID ~= mo.UniqueID then
									self.detectSound:Play(self.Pos);
								end
								self.targetID = mo.UniqueID;
								self.markerSize = mo.Radius;
							end
						end
					end
					if playerControlled then
						PrimitiveMan:DrawLinePrimitive(screen, startPos, endPos, self.markerColor);
					end
				end
			end
		elseif self.markerSize > 0 then
			self.markerSize = (self.markerSize * 0.9) - 1;
		end
		local target = GetLockedTarget(self);
		if target and target.ID ~= rte.NoMOID and not self.targetLostTimer:IsPastSimTimeLimit() and self.markerSize > 0 then
			if playerControlled then
				local crosshairPos = target.Pos;
				if IsACrab(target) and target.Turret then
					crosshairPos = target.Pos + SceneMan:ShortestDistance(target.Pos, target.Turret.Pos, SceneMan.SceneWrapsX) * 0.5;
				end
				local crossVecX = Vector(markerSize, 0):DegRotate(self.markerRotAngle);
				local crossVecY = Vector(0, markerSize):DegRotate(self.markerRotAngle);

				local frame = self.markerSize > 50 and 1 or 0;

				PrimitiveMan:DrawBitmapPrimitive(screen, crosshairPos - crossVecX, self.arrow, crossVecX.AbsRadAngle, frame);
				PrimitiveMan:DrawBitmapPrimitive(screen, crosshairPos + crossVecX, self.arrow, crossVecX.AbsRadAngle + math.pi, frame);

				PrimitiveMan:DrawBitmapPrimitive(screen, crosshairPos - crossVecY, self.arrow, crossVecY.AbsRadAngle, frame);
				PrimitiveMan:DrawBitmapPrimitive(screen, crosshairPos + crossVecY, self.arrow, crossVecY.AbsRadAngle + math.pi, frame);

				self.markerRotAngle = self.markerRotAngle + (self.markerTurnSpeed/math.sqrt(self.markerSize) * self.FlipFactor);
			end
		else
			self.targetID = nil;
		end
	end
	if self.FiredFrame then
		local missile = self.missile:Clone();
		missile.Pos = self.MuzzlePos;
		local shake = math.rad(self.ShakeRange * (1 - sharpAimProgress) + self.SharpShakeRange * sharpAimProgress) * RangeRand(-1, 1);
		local fireVector = Vector(self.fireVel * self.FlipFactor, 0):RadRotate(self.RotAngle + shake);
		missile.Vel = self.Vel + fireVector + Vector(0, -math.abs(math.cos(fireVector.AbsRadAngle)));
		missile.RotAngle = missile.Vel.AbsRadAngle;
		missile.AngularVel = math.cos(missile.Vel.AbsRadAngle) * 10 + shake;
		missile.Team = self.Team;
		missile.IgnoresTeamHits = true;

		local target = GetLockedTarget(self);
		if target then
			missile:SetNumberValue("TargetID", target.ID);
		end
		MovableMan:AddParticle(missile);
	end
end