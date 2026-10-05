-- Explosives can be deleted at any time and their memory reused by other objects, so they're kept by UniqueID and looked up again before use.
local function FindExplosive(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and MovableMan:IsParticle(mo) then
		return ToMOSRotating(mo);
	end
	return nil;
end

function Create(self)
	self.disarmTicks = 3;
	self.disarmRange = GetPPM() * 4;
	
	self.delayTimer = Timer();
	self.overallTimer = Timer();
	self.scanTimer = Timer();
	
	self.tickDuration = 1000;
	self.disarmTime = self.disarmTicks * self.tickDuration;
	self.actionPhase = 0;
	self.blink = false;
	self.MuzzleOffset = Vector(self.disarmRange * 0.5, 0);
	self.targetTable = {};

	self.errorSound = CreateSoundContainer("Error", "Base.rte");
	self.safeSound = CreateSoundContainer("Disarmer Blip Safe", "Base.rte");
	self.dangerSound = CreateSoundContainer("Disarmer Blip Danger", "Base.rte");
end

function Update(self)
	if self.Magazine then
		if self:IsActivated() then
			self.Magazine.RoundCount = math.ceil(self.disarmTime - self.overallTimer.ElapsedSimTimeMS);
			if self.delayTimer:IsPastSimMS(self.tickDuration) then
				self.delayTimer:Reset();
				self.blink = false;
				local targetCount = 0;
				self.actionPhase = self.actionPhase + 1;
				for i = 1, #self.targetTable do
					local target = FindExplosive(self.targetTable[i]);
					if target and SceneMan:ShortestDistance(self.MuzzlePos, target.Pos, SceneMan.SceneWrapsX):MagnitudeIsLessThan(self.disarmRange + 5) then
						targetCount = targetCount + 1;
						local detectPar = CreateMOPixel("Disarmer Detection Particle ".. (self.actionPhase == self.disarmTicks and "Safe" or "Neutral"));
						detectPar.Pos = target.Pos;
						MovableMan:AddParticle(detectPar);

						if self.actionPhase == self.disarmTicks then
							local itemName = string.gsub(target:GetModuleAndPresetName(), " Active", "");
							local disarmedItem = CreateTDExplosive(itemName);
							disarmedItem.Pos = target.Pos;
							disarmedItem.RotAngle = target.RotAngle;
							MovableMan:AddParticle(disarmedItem);
							target.Sharpness = 1;
						end
					else
						self.targetTable[i] = nil;
					end
				end
				if targetCount == 0 then
					self.actionPhase = 0;
					self.overallTimer:Reset();
					self.errorSound:Play(self.Pos);
				elseif self.actionPhase == self.disarmTicks then
					self.BaseReloadTime = 1000 + (500 * targetCount);
					self:Reload();
					-- The disarmed targets are about to be deleted, so forget them.
					self.targetTable = {};
				end
			end
			if self.actionPhase > 0 and self.actionPhase < self.disarmTicks then
				if self.blink == false then
					self.blink = true;
					local soundfx = CreateAEmitter("Disarmer Sound Blip");
					soundfx.Pos = self.Pos;
					MovableMan:AddParticle(soundfx);
				end
			elseif self.actionPhase == self.disarmTicks then
				local soundfx = CreateAEmitter("Disarmer Sound Disarm");
				soundfx.Pos = self.Pos;
				MovableMan:AddParticle(soundfx);
			end
		else
			self.delayTimer:Reset();
			self.overallTimer:Reset();
			self.Magazine.RoundCount = self.disarmTime;
			self.actionPhase = 0;

			if self:GetParent() and self.scanTimer:IsPastSimMS(self.tickDuration * 0.5) then
				self.targetTable = {};
				self.scanTimer:Reset();
				local alarm = false;
				local alarmSound = self.safeSound;
				local disarmTables = {AntiPersonnelMineTable, RemoteExplosiveTableA, TimedExplosiveTable};
				for _, bombTable in pairs(disarmTables) do
					if bombTable then
						for _, explosiveEntry in pairs(bombTable) do
							-- The explosive tables hold UniqueIDs, but scripts that still store the explosive itself are handled as before.
							local explosive;
							if type(explosiveEntry) == "number" then
								explosive = FindExplosive(explosiveEntry);
							elseif MovableMan:IsParticle(explosiveEntry) then
								explosive = explosiveEntry;
							end
							if explosive and SceneMan:ShortestDistance(self.MuzzlePos, explosive.Pos, SceneMan.SceneWrapsX):MagnitudeIsLessThan(self.disarmRange) then
								alarm = true;
								local isFriendly = explosive.Team == self.Team;
								alarmSound = isFriendly and alarmSound or self.dangerSound;
								local detectPar = CreateMOPixel("Disarmer Detection Particle ".. (isFriendly and "Safe" or "Danger"));
								detectPar.Pos = explosive.Pos;
								MovableMan:AddParticle(detectPar);
								table.insert(self.targetTable, explosive.UniqueID);
							end
						end
					end
				end
				if alarm and not self:IsActivated() then
					alarmSound:Play(self.Pos);
				end
			end
		end
	end
end