-- Determinism Chaos: a test-only Activity that pushes as much of the simulation as possible every sim update, so that any
-- nondeterminism shows up quickly in a lockstep stress test. It is itself deterministic: all randomness comes from math.random
-- and the engine's sim RNG, and it only iterates engine-ordered lists.
--
-- What it stresses:
--   * many AI actors from every faction on both teams, delivered by craft or dropped from the sky, hunting brains or walking to
--     random waypoints (AI, path finding, see rays, MOIDs, collisions, scripted weapons and devices);
--   * constant bombardment with live grenades and occasional gibbing (gibs, particles, terrain destruction and settling);
--   * real-time and sim-time timers;
--   * a chaotic double-precision value computed with math.sin/cos/exp/log/atan2/^ every update and fed into spawn positions, so
--     any difference in Lua double math between machines turns into a visible desync within seconds.

function DeterminismChaos:StartActivity(isNewGame)
	self.updateCount = 0;
	self.spawnTimer = Timer();
	self.bombTimer = Timer();
	self.gibTimer = Timer();
	self.realTimer = Timer();
	self.chaosValue = 0.123456789;
	self.maxActors = 70;

	self.techs = {};
	for _, tech in ipairs({"Base.rte", "Coalition.rte", "Imperatus.rte", "Techion.rte", "Dummy.rte", "Ronin.rte", "Browncoats.rte"}) do
		if PresetMan:GetModuleID(tech) ~= -1 then
			table.insert(self.techs, tech);
		end
	end

	for team = Activity.TEAM_1, Activity.TEAM_2 do
		self:SetTeamFunds(1000000, team);
	end
	self.humanTeam = Activity.TEAM_1;
	self.enemyTeam = Activity.TEAM_2;
end

function DeterminismChaos:EndActivity()
end

function DeterminismChaos:PauseActivity(pause)
end

-- Logistic map with a little libm noise mixed in. Chaotic, so a one-ulp difference in any of these functions grows to a completely
-- different value within a few dozen updates.
function DeterminismChaos:StepChaosValue()
	local v = self.chaosValue;
	local noise = math.sin(v * 1000) * 1e-3 + math.cos(v * 777) * 1e-3 + math.exp(-v) * 1e-4 + math.log(v + 1) * 1e-4 + math.atan2(v, 1 - v) * 1e-4 + (v ^ 1.5) * 1e-4;
	v = 3.99 * v * (1 - v) + noise;
	if v <= 0 or v >= 1 then
		v = 0.5 + noise;
	end
	self.chaosValue = v;
	return v;
end

function DeterminismChaos:RandomTech()
	return self.techs[math.random(#self.techs)];
end

function DeterminismChaos:RandomX()
	-- Mix the chaotic value into positions so that Lua math differences become sim differences.
	local x = (math.random() * 0.5 + self.chaosValue * 0.5) * SceneMan.SceneWidth;
	return math.max(50, math.min(SceneMan.SceneWidth - 50, x));
end

function DeterminismChaos:CreateSoldier(team, tech)
	local soldier;
	if math.random() < 0.15 then
		soldier = RandomACrab("Actors - Mecha", tech);
	end
	if not soldier then
		soldier = RandomAHuman("Any", tech);
	end
	if not soldier then
		return nil;
	end
	if IsAHuman(soldier) then
		soldier:AddInventoryItem(RandomHDFirearm("Weapons - Primary", tech));
		soldier:AddInventoryItem(RandomHDFirearm("Weapons - Secondary", tech));
		if math.random() < 0.4 then
			soldier:AddInventoryItem(RandomHDFirearm("Tools - Diggers", tech));
		end
		if math.random() < 0.4 then
			soldier:AddInventoryItem(RandomTDExplosive("Bombs - Grenades", tech));
		end
	end
	soldier.Team = team;
	if math.random() < 0.7 then
		soldier.AIMode = Actor.AIMODE_BRAINHUNT;
	else
		soldier.AIMode = Actor.AIMODE_GOTO;
		soldier:ClearAIWaypoints();
		soldier:AddAISceneWaypoint(Vector(self:RandomX(), SceneMan.SceneHeight * 0.5));
	end
	return soldier;
end

function DeterminismChaos:SpawnWave()
	local team = math.random() < 0.6 and self.enemyTeam or self.humanTeam;
	local tech = self:RandomTech();
	if math.random() < 0.5 then
		-- Delivered by craft.
		local craft = math.random() < 0.7 and RandomACDropShip("Craft", tech) or RandomACRocket("Craft", tech);
		if not craft then
			return;
		end
		craft.Team = team;
		for i = 1, math.random(1, 3) do
			local soldier = self:CreateSoldier(team, tech);
			if soldier then
				craft:AddInventoryItem(soldier);
			end
		end
		craft.Pos = Vector(self:RandomX(), -50);
		MovableMan:AddActor(craft);
	else
		-- Dropped from the sky.
		local soldier = self:CreateSoldier(team, tech);
		if soldier then
			soldier.Pos = Vector(self:RandomX(), 0);
			soldier.Vel = Vector(RangeRand(-5, 5), RangeRand(0, 10));
			MovableMan:AddActor(soldier);
		end
	end
end

function DeterminismChaos:DropBomb()
	local bomb = RandomTDExplosive("Bombs - Grenades", self:RandomTech());
	if not bomb then
		return;
	end
	bomb.Pos = Vector(self:RandomX(), RangeRand(0, SceneMan.SceneHeight * 0.3));
	bomb.Vel = Vector(RangeRand(-10, 10), RangeRand(0, 15));
	bomb.AngularVel = RangeRand(-10, 10);
	MovableMan:AddItem(bomb);
	bomb:Activate();
end

function DeterminismChaos:GibRandomActor()
	local candidates = {};
	for actor in MovableMan.Actors do
		if not actor:IsInGroup("Brains") and not actor:IsInGroup("Craft") and (IsAHuman(actor) or IsACrab(actor)) then
			table.insert(candidates, actor);
		end
	end
	if #candidates > 0 then
		candidates[math.random(#candidates)]:GibThis();
	end
end

function DeterminismChaos:EnsureHumanBrains()
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		if self:PlayerActive(player) and self:PlayerHuman(player) and not MovableMan:IsActor(self:GetPlayerBrain(player)) then
			local brain = MovableMan:GetUnassignedBrain(self:GetTeamOfPlayer(player));
			if not brain then
				brain = CreateAHuman("Brain Robot", "Base.rte");
				brain.Team = self:GetTeamOfPlayer(player);
				brain.Pos = Vector(self:RandomX(), 0);
				MovableMan:AddActor(brain);
			end
			self:SetPlayerBrain(brain, player);
			self:SwitchToActor(brain, player, self:GetTeamOfPlayer(player));
			self:SetObservationTarget(brain.Pos, player);
		end
	end
end

function DeterminismChaos:UpdateActivity()
	if self.ActivityState == Activity.OVER then
		return;
	end
	self.updateCount = self.updateCount + 1;
	self:StepChaosValue();
	self:EnsureHumanBrains();

	if self.spawnTimer:IsPastSimMS(700) then
		self.spawnTimer:Reset();
		if MovableMan:GetTeamMOIDCount(self.enemyTeam) + MovableMan:GetTeamMOIDCount(self.humanTeam) < 1500 and self:ActorCount() < self.maxActors then
			self:SpawnWave();
		end
	end
	-- Real-time timers count sim time in deterministic mode; use one here so a regression there shows up.
	if self.bombTimer:IsPastRealMS(250) then
		self.bombTimer:Reset();
		self:DropBomb();
	end
	if self.gibTimer:IsPastSimMS(4000) then
		self.gibTimer:Reset();
		self:GibRandomActor();
	end
	-- Keep both teams rich so the AI keeps buying.
	if self.updateCount % 600 == 0 then
		self:SetTeamFunds(1000000, self.enemyTeam);
		self:SetTeamFunds(1000000, self.humanTeam);
	end
	if self.updateCount % 120 == 0 then
		for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
			if self:PlayerActive(player) and self:PlayerHuman(player) then
				FrameMan:SetScreenText("Chaos " .. self.updateCount .. "  actors " .. self:ActorCount() .. "  v " .. string.format("%.6f", self.chaosValue), self:ScreenOfPlayer(player), 0, 2500, false);
			end
		end
	end
end

function DeterminismChaos:ActorCount()
	local count = 0;
	for _ in MovableMan.Actors do
		count = count + 1;
	end
	return count;
end
