-- Target actors are kept by unique ID and looked up when needed, as they can be deleted at any time.
local function FindTargetActor(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and IsActor(mo) then
		return ToActor(mo);
	end
	return nil;
end

function Create(self)
	self.fireVel = 17;
	self.spread = math.rad(self.ShakeRange);

	self.searchRange = 100 + FrameMan.PlayerScreenWidth * 0.3;
	self.searchTimer = Timer();
	self.searchTimer:SetSimTimeLimitMS(250);
	self.lockThreshold = 2;

	self.targets = {};

	self.targetLockSound = CreateSoundContainer("Mine Activate", "Base.rte");
end

function OnFire(self)
	local rocketNumber = self.RoundInMagCount + 1;

	local rocket = CreateAEmitter("Particle Browncoat Rocket", "Browncoats.rte");
	if #self.targets > 0 then
		local targetActor = self.targets[rocketNumber] and FindTargetActor(self.targets[rocketNumber].actorUniqueID);
		if targetActor and targetActor.ID ~= rte.NoMOID then
			rocket:SetNumberValue("TargetUniqueID", targetActor.UniqueID);
			self.targets[rocketNumber].topLeft = self.targets[rocketNumber].topLeft * 1.5;
			self.targets[rocketNumber].bottomRight = self.targets[rocketNumber].bottomRight * 1.5;
		elseif rocketNumber > #self.targets then
			rocket:SetNumberValue("TargetUniqueID", self.targets[math.random(#self.targets)].actorUniqueID);
		end
	end
	rocket.Pos = self.MuzzlePos + Vector(0, (rocketNumber - self.RoundInMagCapacity * 0.5)):RadRotate(self.RotAngle);
	rocket.Vel = self.Vel + Vector(self.fireVel * RangeRand(0.9, 1.1) * self.FlipFactor, 0):RadRotate(self.RotAngle - ((self.spread * 0.5) - (rocketNumber/self.RoundInMagCapacity) * self.spread) * self.FlipFactor);
	rocket.RotAngle = rocket.Vel.AbsRadAngle;
	rocket.AngularVel = math.cos(rocket.Vel.AbsRadAngle) * 5;
	rocket.Team = self.Team;
	rocket.IgnoresTeamHits = true;
	MovableMan:AddParticle(rocket);
end

function ThreadedUpdate(self)
	local parent = self:GetRootParent();
	if IsActor(parent) then
		parent = ToActor(parent);
		local controller = parent:GetController();
		if self.Magazine then
			if self:DoneReloading() then
				self.targets = {};
			end
			if self.Magazine.RoundCount > 0 then
				if controller:IsState(Controller.AIM_SHARP) then

					if self.searchTimer:IsPastSimTimeLimit() then
						self.searchTimer:Reset();

						local searchPos = parent.ViewPoint;
						local lastTargetCount = #self.targets;
						self.targets = {};

						for actor in MovableMan.Actors do
							if #self.targets < self.RoundInMagCapacity and actor.Team ~= self.Team and actor.GetsHitByMOs then

								if (SceneMan:ShortestDistance(searchPos, actor.Pos, SceneMan.SceneWrapsX).Magnitude - actor.Radius) < self.searchRange
								and (actor.Vel.Magnitude + math.abs(actor.AngularVel) + 1)/math.sqrt(actor.Radius) < self.lockThreshold
								and SceneMan:CastObstacleRay(searchPos, SceneMan:ShortestDistance(searchPos, actor.Pos, SceneMan.SceneWrapsX), Vector(), Vector(), actor.ID, actor.Team, rte.airID, 10) < 0 then

									--Measure the approximate corners of a box that the Actor is supposedly inside of
									local topLeft = Vector(-actor.IndividualRadius, -actor.IndividualRadius);
									local bottomRight = Vector(actor.IndividualRadius, actor.IndividualRadius);
									for att in actor.Attachables do
										if IsAttachable(att) then
											local dist = SceneMan:ShortestDistance(actor.Pos, att.Pos, SceneMan.SceneWrapsX);
											local reach = dist:SetMagnitude(dist.Magnitude + math.sqrt(att.Diameter));
											if reach.X < topLeft.X then
												topLeft.X = reach.X;
											elseif reach.X > bottomRight.X then
												bottomRight.X = reach.X;
											end
											if reach.Y < topLeft.Y then
												topLeft.Y = reach.Y;
											elseif reach.Y > bottomRight.Y then
												bottomRight.Y = reach.Y;
											end
										end
									end
									table.insert(self.targets, {actorUniqueID = actor.UniqueID, topLeft = topLeft, bottomRight = bottomRight});
								end
							end
						end
						if lastTargetCount < #self.targets then
							self.targetLockSound:Play(self.Pos);
						end
					end
				else
					self.searchTimer:Reset();
				end
			else
				self:Reload();
			end
		end
		if parent:IsPlayerControlled() then
			for _, target in pairs(self.targets) do
				local targetActor = FindTargetActor(target.actorUniqueID);
				if targetActor and targetActor.ID ~= rte.NoMOID then
					local screen = ActivityMan:GetActivity():ScreenOfPlayer(ToActor(parent):GetController().Player);
					PrimitiveMan:DrawBoxPrimitive(screen, targetActor.Pos + target.topLeft, targetActor.Pos + target.bottomRight, 149);

					if self.RoundInMagCount == 0 then
						target.topLeft = target.topLeft * 0.9;
						target.bottomRight = target.bottomRight * 0.9;
					end
				end
			end
		end
		if controller:IsState(Controller.PIE_MENU_ACTIVE) then
			self.targets = {};
		end
	else
		parent = nil;
		self.targets = {};
	end
end