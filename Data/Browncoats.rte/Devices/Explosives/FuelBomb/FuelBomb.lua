-- Fuel targets are kept by unique ID and looked up every update, as they can be deleted at any time.
local function FindFuelTarget(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and IsMOSRotating(mo) and mo.ID ~= rte.NoMOID and not mo.ToDelete then
		return ToMOSRotating(mo);
	end
	return nil;
end

-- Fuel particles are kept by unique ID too, as they can be deleted and their memory reused by another object at any time.
local function FindFuelParticle(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and MovableMan:IsParticle(mo) and IsMOPixel(mo) then
		return ToMOPixel(mo);
	end
	return nil;
end

function Create(self)
	self.explodeTimer = Timer();
	self.partList = {};

	self.explodeTime = 1500;
	self.numOfParticles = 40;
	self.particlesPerOil = 5;

	self:EraseFromTerrain();

	for i = 1, self.numOfParticles do
		local fuel = CreateMOPixel("Browncoat Fuel Bomb Fuel");
		fuel.Pos = self.Pos;
		fuel.Vel = Vector(math.random(20), 0):RadRotate(math.pi * 2 * math.random()) + self.Vel;
		MovableMan:AddParticle(fuel);
		self.partList[i] = {uniqueID = fuel.UniqueID, queue = math.abs(fuel.Vel.X - self.Vel.X) * TimerMan.DeltaTimeMS};

		if i < self.numOfParticles * 0.5 then
			local part = CreateMOSParticle("Oil Spray Particle");
			part.Pos = self.Pos;
			part.Lifetime = part.Lifetime*RangeRand(0.5, 1.5);
			part.Vel = fuel.Vel;
			MovableMan:AddParticle(part);
		end
	end
	self.soundCount = 0;
end

function ThreadedUpdate(self)
	self.ToSettle = false;
	self.ToDelete = false;

	if self.explodeTimer:IsPastSimMS(self.explodeTime) then
		if self.soundCount <= 0 then
			self.soundCount = self.soundCount + 1;
			local sfx = CreateMOSRotating("Browncoat Fuel Bomb Ignition");
			sfx.Pos = self.Pos;
			sfx:GibThis();
			MovableMan:AddParticle(sfx);
		end

		local partsLeft = 0;
		for i = 1, #self.partList do
			local fuel = self.partList[i] and FindFuelParticle(self.partList[i].uniqueID);
			if fuel and fuel.PresetName == "Browncoat Fuel Bomb Fuel" then
				if self.explodeTimer:IsPastSimMS(self.explodeTime + self.partList[i].queue) then
					local fire = CreatePEmitter("Flame ".. math.random(2) .." Hurt");
					local target = FindFuelTarget(self.partList[i].targetUniqueID);
					if target then
						fire.Pos = target.Pos + self.partList[i].stickOffset;
						fire.Vel = Vector(-self.partList[i].stickOffset.X, -self.partList[i].stickOffset.Y):SetMagnitude(3);
					else
						fire.Pos = Vector(fuel.Pos.X, fuel.Pos.Y);
						fire.Vel = self.Vel;
						fire.Lifetime = math.random(1500, 3000);
					end

					MovableMan:AddParticle(fire);
					for j = 1, self.particlesPerOil do
						local firePar;
						if j > self.particlesPerOil * 0.5 then
							firePar = CreateMOPixel("Ground Fire Burn Particle");
							firePar.Vel = self.Vel + Vector(RangeRand(-20, 20), -math.random(-10, 30));
						else
							firePar = CreateMOSParticle("Flame Smoke 2");
							firePar.Vel = self.Vel + Vector(math.random() * j, 0):RadRotate(math.random() * math.pi * 2);
							firePar.Lifetime = math.random(500, 1000);
							firePar.GlobalAccScalar = RangeRand(-0.6, -0.1);
						end
						firePar.Pos = Vector(fuel.Pos.X, fuel.Pos.Y);
						MovableMan:AddParticle(firePar);
					end

					fuel.ToDelete = true;
				else
					partsLeft = partsLeft + 1;
				end
			end
		end

		if partsLeft == 0 then
			self.ToDelete = true;
		end
	else
		--Look for targets to douse with fuel
		for i = 1, #self.partList do
			local fuel = self.partList[i] and FindFuelParticle(self.partList[i].uniqueID);
			if fuel and fuel.PresetName == "Browncoat Fuel Bomb Fuel" then
				local target = FindFuelTarget(self.partList[i].targetUniqueID);
				if target then
					if math.random() < 0.01 then
						fuel.Vel = target.Vel;
						fuel.Pos = target.Pos + Vector(self.partList[i].stickOffset.X, self.partList[i].stickOffset.Y):RadRotate(target.RotAngle - self.partList[i].targetStickAngle);
					end
				else
					self.partList[i].targetUniqueID = nil;
					local velNum = math.ceil(math.sqrt(fuel.Vel.Magnitude + 1));

					local mocheck = SceneMan:CastMORay(fuel.Pos, Vector(velNum, 0):RadRotate(fuel.Vel.AbsRadAngle), fuel.ID, -2, rte.airID, true, 1);
					if mocheck ~= rte.NoMOID then
						local mo = MovableMan:GetMOFromID(MovableMan:GetMOFromID(mocheck).ID);
						if mo and mo.PresetName ~= self.PresetName then

							self.partList[i].targetUniqueID = mo.UniqueID;

							self.partList[i].targetStickAngle = mo.RotAngle;

							self.partList[i].stickOffset = SceneMan:ShortestDistance(mo.Pos, fuel.Pos, SceneMan.SceneWrapsX) * 0.8;
						end
					end
				end
			else
				self.partList[i] = nil;
			end
		end
	end
end