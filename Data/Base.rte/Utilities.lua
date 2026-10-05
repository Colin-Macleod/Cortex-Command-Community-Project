-- pairs() visits keys that are objects (Vectors, Boxes, MovableObjects...) in memory address order, which differs from one machine to the next, so
-- anything order-dependent done in such a loop (spawning, random numbers, picking the first of equals) desyncs multiplayer.
-- SortedPairs(t, sortKey) visits the keys in an order that only depends on their values. sortKey turns an object key into a number or an array of
-- numbers; number and string keys need none. Use SortKeyVector, SortKeyBox or SortKeyMO, or your own. Keys must be distinct under the ordering.
-- A sortKey can return nil for keys that should be skipped (SortKeyMO does for objects that no longer exist), as there's no ordering them deterministically.

function SortKeyVector(vector)
	return {vector.X, vector.Y};
end

function SortKeyBox(box)
	return {box.Corner.X, box.Corner.Y, box.Width, box.Height};
end

-- By UniqueID. Objects that no longer exist are skipped (they'd all tie, and table.sort would order them by memory address), and aren't touched beyond MovableMan:ValidMO.
-- Prefer keeping unique IDs over keeping objects: ValidMO only compares addresses, so an object that was deleted and whose memory went to a new one is taken for that one.
function SortKeyMO(movableObject)
	if MovableMan:ValidMO(movableObject) then
		return movableObject.UniqueID;
	end
	return nil;
end

local function SortedPairsKey(key, sortKey)
	local keyType = type(key);
	if keyType == "number" then
		return {1, key};
	elseif keyType == "string" then
		return {2, key};
	elseif sortKey then
		local value = sortKey(key);
		if value == nil then
			return nil;
		elseif type(value) == "table" then
			local result = {3};
			for i = 1, #value do
				result[i + 1] = value[i];
			end
			return result;
		end
		return {3, value};
	end
	error("SortedPairs needs a sortKey function for keys of type " .. keyType);
end

local function SortedPairsLess(a, b)
	for i = 1, math.max(#a, #b) do
		if a[i] ~= b[i] then
			if a[i] == nil then
				return true;
			elseif b[i] == nil then
				return false;
			end
			return a[i] < b[i];
		end
	end
	return false;
end

function SortedPairs(t, sortKey)
	local entries = {};
	for key in pairs(t) do
		local keySortKey = SortedPairsKey(key, sortKey);
		if keySortKey ~= nil then
			entries[#entries + 1] = {key = key, sortKey = keySortKey};
		end
	end
	table.sort(entries, function(a, b) return SortedPairsLess(a.sortKey, b.sortKey); end);
	local i = 0;
	return function()
		i = i + 1;
		local entry = entries[i];
		if entry then
			return entry.key, t[entry.key];
		end
	end;
end

function FindStartPositionWithShortestPathToEndPosition(startPositions, endPosition, team, jumpHeight, digStrength)
	if startPositions == nil or type(startPositions) ~= "table" then
		print("FindShortestPathAsync Error: A table of start positions is required.");
		return;
	end
	if endPosition == nil or type(endPosition) ~= "userdata" or endPosition.ClassName ~= "Vector" then
		print("FindShortestPathAsync Error: An end position is required.");
		return;
	end
	if team == nil then
		print("FindShortestPathAsync Error: A team is required, -1 is allowed.");
		return;
	end
	
	jumpHeight = jumpHeight or GetPathFindingFlyingJumpHeight();
	digStrength = digStrength or GetPathFindingDefaultDigStrength();
	
	local closestStartPositionKey;
	local closestStartPosition;
	local totalCostToClosestStartPosition = 1/0;
	local pathRequestsCompleted = 0;
	local totalPathingRequests = 0;
	-- Sorted, so path requests (and so their results, and which of equally short paths wins) go in the same order on every machine.
	for startPositionKey, startPosition in SortedPairs(startPositions, SortKeyMO) do
		SceneMan.Scene:CalculatePathAsync(
			function(pathRequest)
				if pathRequest.TotalCost < totalCostToClosestStartPosition then
					closestStartPositionKey = startPositionKey;
					closestStartPosition = startPosition;
					totalCostToClosestStartPosition = pathRequest.TotalCost;
				end
				pathRequestsCompleted = pathRequestsCompleted + 1;
			end, 
			startPosition, endPosition, jumpHeight, digStrength, team
		);
		totalPathingRequests = totalPathingRequests + 1;
	end
	
	while pathRequestsCompleted < totalPathingRequests do
		if totalCostToClosestStartPosition == 0 then
			break;
		end
		coroutine.yield();
	end
	
	return {key = closestStartPositionKey, position = closestStartPosition, totalCost = totalCostToClosestStartPosition};
end