function OnDetach(self, exParent)
	-- The smoker is created here rather than kept from Create, so no reference to it is held while it's owned by the magazine (where it can be deleted with it at any time).
	if not self.smokerAdded then
		self.smokerAdded = true;
		local smoker = CreateAEmitter("Laser Rifle Magazine Smoker", "Techion.rte");
		smoker.Throttle = -(self.RoundCount/self.Capacity);
		self:AddAttachable(smoker);
	end
end
