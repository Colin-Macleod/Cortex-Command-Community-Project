--------------------------------------- Instructions ---------------------------------------

------- Require this in your script like so: 

-- self.buyDoorHandler = require("Activities/Utility/BuyDoorHandler");
-- self.buyDoorHandler:Initialize(Activity, bool newGame, bool verboseLogging);

-- This is a simple utility for activities to send orders to buy doors.
-- Create your items/actors beforehand and put them in a table.

-- SendCustomOrder(table order, int team) will pick a random buy door on the map of the same team
-- and send that order to it if any are available.

-- You can use GetAvailableBuyDoorsInArea(Area area, int team) to get a table of indexes that point to non-busy buy doors in that area,
-- then use SendCustomOrder(table order, int team, int index) to send to one of them (pick them randomly yourself beforehand).
-- Note that it will only give you non-busy indexes and as such may not give you all of the buy doors in that area.

-- If you manually set up your table of buy doors and want the indices to be the same in BuyDoorHandler,
-- use ReplaceBuyDoorTable(newTable). newTable should be an integer-indexed table of references to real buy doors.

-- Buy doors can be destroyed at any time, after which their memory can be reused by another object, so don't keep and use buy door objects yourself.
-- Get them by index with GetBuyDoor(index), which returns nil for a destroyed one, and use SetBuyDoorsTeam(indexTable, team) to change teams.

------- Saving/Loading

-- Saving and loading requires you to also have the SaveLoadHandler ready.
-- Simply run OnSave(instancedSaveLoadHandler) and OnLoad(instancedSaveLoadHandler) when appropriate.
-- If you also save the table you gave to ReplaceBuyDoorTable yourself, run ReplaceDeletedBuyDoors() before saving it.

--------------------------------------- Misc. Information ---------------------------------------

--




local BuyDoorHandler = {};

-- The buy doors are looked up by unique ID whenever they're used, as MovableMan:ValidMO and the like only compare addresses, which a new object can
-- have after a destroyed buy door's memory was reused. self.buyDoorTable keeps the door objects themselves (false for destroyed ones) only for saving,
-- as SaveLoadHandler saves objects rather than unique IDs, which change on load.
local function FindBuyDoor(uniqueID)
	local mo = uniqueID and MovableMan:FindObjectByUniqueID(uniqueID);
	if mo and IsActor(mo) and MovableMan:ValidMO(mo) then
		return ToActor(mo);
	end
	return nil;
end

local function GetBuyDoorUniqueIDs(buyDoorTable)
	-- Entries the SaveLoadHandler couldn't resolve on load may be strings or missing, so find the highest index rather than relying on #.
	local count = 0;
	for index in pairs(buyDoorTable) do
		if type(index) == "number" and index > count then
			count = index;
		end
	end
	local uniqueIDs = {};
	for i = 1, count do
		uniqueIDs[i] = type(buyDoorTable[i]) == "userdata" and buyDoorTable[i].UniqueID or false;
	end
	return uniqueIDs;
end

function BuyDoorHandler:Create()
	local Members = {};

	setmetatable(Members, self);
	self.__index = self;

	return Members;
end

function BuyDoorHandler:Initialize(activity, newGame, verboseLogging)

	if verboseLogging then
		self.verboseLogging = true;
	end
	
	self.Activity = activity;
	
	if newGame then
	
		-- find and save buy doors
		
		self.buyDoorTable = {};
		
		for mo in MovableMan.AddedActors do
			if mo.PresetName == "Reinforcement Door" then
				table.insert(self.buyDoorTable, ToMOSRotating(mo));
			end
		end
		self.buyDoorUniqueIDs = GetBuyDoorUniqueIDs(self.buyDoorTable);
		
	end
	
	print("INFO: BuyDoorHandler initialized!")
	
end

function BuyDoorHandler:OnLoad(saveLoadHandler)
	
	print("INFO: BuyDoorHandler loading...");
	self.buyDoorTable = saveLoadHandler:ReadSavedStringAsTable("buyDoorHandlerBuyDoorTable");
	-- Unique IDs change on load, so take them from the freshly loaded objects.
	self.buyDoorUniqueIDs = GetBuyDoorUniqueIDs(self.buyDoorTable);
	print("INFO: BuyDoorHandler loaded!");
	
end

function BuyDoorHandler:OnSave(saveLoadHandler)
	
	print("INFO: BuyDoorHandler saving...");
	self:ReplaceDeletedBuyDoors();
	saveLoadHandler:SaveTableAsString("buyDoorHandlerBuyDoorTable", self.buyDoorTable);
	print("INFO: BuyDoorHandler saved!");
	
end

-- Replaces the destroyed buy doors in the buy door table with false, as saving one would read its (possibly reused) memory.
function BuyDoorHandler:ReplaceDeletedBuyDoors()

	for i = 1, #self.buyDoorUniqueIDs do
		if not FindBuyDoor(self.buyDoorUniqueIDs[i]) then
			self.buyDoorTable[i] = false;
		end
	end

end

function BuyDoorHandler:ReplaceBuyDoorTable(newTable)

	if newTable then
		self.buyDoorTable = newTable;
		self.buyDoorUniqueIDs = GetBuyDoorUniqueIDs(newTable);
		return true;
	end
	
	return false;

end

-- Gets the buy door at an index of the buy door table, or nil if it has been destroyed.
function BuyDoorHandler:GetBuyDoor(index)

	return FindBuyDoor(index and self.buyDoorUniqueIDs[index]);

end

-- Sets the team of the buy doors at the indexes that are the keys of indexTable (the values don't matter), skipping destroyed ones.
function BuyDoorHandler:SetBuyDoorsTeam(indexTable, team)

	for index in pairs(indexTable) do
		local buyDoor = self:GetBuyDoor(index);
		if buyDoor then
			buyDoor.Team = team;
		end
	end

end

function BuyDoorHandler:IsBuyDoorBusy(specificIndex)

	local buyDoor = self:GetBuyDoor(specificIndex);
	return not buyDoor or buyDoor:NumberValueExists("BuyDoor_Unusable");
	
end

function BuyDoorHandler:GetAvailableBuyDoorsInArea(area, team)

	local usableIndexesTable = {};
	for i = 1, #self.buyDoorUniqueIDs do
		local buyDoor = self:GetBuyDoor(i);
		if buyDoor and not buyDoor:NumberValueExists("BuyDoor_Unusable") and (not team or buyDoor.Team == team) then
			if area:IsInside(buyDoor.Pos) then
				table.insert(usableIndexesTable, i);
			end
		end
	end
	
	if #usableIndexesTable > 0 then
		return usableIndexesTable;
	else
		return false;
	end
	
end

function BuyDoorHandler:ChangeCooldownTime(index, newTime)

	if index and newTime then
		local buyDoor = self:GetBuyDoor(index);
		if buyDoor then
			buyDoor:SendMessage("BuyDoor_ChangeCooldownTime", newTime);
		else
			print("ERROR: BuyDoorHandler was asked to change the cooldown time of an index that didn't exist!");
			return false;
		end
	else
		print("ERROR: BuyDoorHandler was asked to change a cooldown time, but was not given an index or a new time!");
		return false;
	end
	
	return true;

end
function BuyDoorHandler:SendCustomOrder(order, team, specificIndex)
	
	if specificIndex then
		local buyDoor = self:GetBuyDoor(specificIndex);
		if buyDoor and (not buyDoor:NumberValueExists("BuyDoor_Unusable")) and buyDoor:IsInventoryEmpty() then
			for k, item in pairs(order) do
				buyDoor:AddInventoryItem(item);
			end
			buyDoor:SendMessage("BuyDoor_CustomTableOrder");
		else
			--print("Buy Door Handler was asked to send a custom order to a busy specific index!");
			return false;
		end
	else
		-- we trust the buy door to tell us if it can be used or not
		-- it's either busy or has enemies nearby if it can't
		local usableIndexesTable = {};
		for i = 1, #self.buyDoorUniqueIDs do
			local buyDoor = self:GetBuyDoor(i);
			if buyDoor and buyDoor:IsInventoryEmpty() and not buyDoor:NumberValueExists("BuyDoor_Unusable") and (not team or buyDoor.Team == team) then
				table.insert(usableIndexesTable, i);
			end
		end
		if #usableIndexesTable > 0 then
			self:GetBuyDoor(usableIndexesTable[math.random(1, #usableIndexesTable)]):SendMessage("BuyDoor_CustomTableOrder", order);
		else
			--print("Buy Door Handler could not find any non-busy Buy Doors to send a custom order to!");
			return false;
		end
	end

	return true;

end


return BuyDoorHandler:Create();
