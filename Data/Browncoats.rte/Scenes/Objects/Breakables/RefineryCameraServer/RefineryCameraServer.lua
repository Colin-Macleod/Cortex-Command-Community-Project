function Create(self)

	self.Activity = ToGameActivity(ActivityMan:GetActivity());

end

-- The engine calls Destroy (there's no OnDestroy callback) whenever this object is deleted, also when the scene is cleared, so only report it when it was destroyed during play.
function Destroy(self)

	if self.ToDelete then
		local activity = ActivityMan:GetActivity();
		if activity and activity.ActivityState == Activity.RUNNING then
			activity:SendMessage("Refinery_S4CameraServerBroken");
		end
	end

end
