#pragma once

#include <memory>
#include <string>

namespace RTE {

	class AllegroScreen;
	class GUIInputWrapper;
	class GUIControlManager;
	class GUICollectionBox;
	class GUILabel;
	class GUIButton;
	class GUITextBox;

	/// The main menu's online co-op screen: host or join a session and see who's in it.
	class CoopMenuGUI {

	public:
		/// What the main menu should do after the player interacted with this screen.
		enum class CoopMenuUpdateResult {
			NoEvent,
			BackToMain,
			ChooseActivity
		};

#pragma region Creation
		/// Constructor method used to instantiate a CoopMenuGUI object in system memory and make it ready for use.
		/// @param guiScreen Pointer to a GUIScreen interface that will be used by this CoopMenuGUI's GUIControlManager. Ownership is NOT transferred!
		/// @param guiInput Pointer to a GUIInput interface that will be used by this CoopMenuGUI's GUIControlManager. Ownership is NOT transferred!
		CoopMenuGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput);
#pragma endregion

#pragma region Concrete Methods
		/// Fills the text boxes from the settings. Call when the screen is shown.
		void Refresh();

		/// Handles the player interaction with the CoopMenuGUI GUI elements.
		/// @return What the main menu should do next.
		CoopMenuUpdateResult HandleInputEvents();

		/// Draws the CoopMenuGUI to the screen.
		void Draw() const;
#pragma endregion

	private:
		std::unique_ptr<GUIControlManager> m_GUIControlManager; //!< The GUIControlManager which holds all the GUIControls of this menu. Owned by this.

		GUIButton* m_BackToMainButton; //!< Button for returning to the main menu.
		GUITextBox* m_NameTextBox; //!< The player's name.
		GUITextBox* m_AddressTextBox; //!< The address to join.
		GUITextBox* m_PortTextBox; //!< The port to host on.
		GUIButton* m_JoinButton; //!< Joins the session at the entered address.
		GUIButton* m_HostButton; //!< Hosts a session on the entered port.
		GUILabel* m_PlayersLabel; //!< Lists the players in the session.
		GUILabel* m_StatusLabel; //!< Describes the session's state.
		GUIButton* m_ChooseActivityButton; //!< Host: goes to the scenario menu to start an activity for everyone.
		GUIButton* m_LeaveButton; //!< Leaves the session.

		/// Saves the text boxes' contents to the settings.
		void SaveFields();

		/// Updates the labels and buttons from the session's state.
		void UpdateSessionDisplay();

		// Disallow the use of some implicit methods.
		CoopMenuGUI(const CoopMenuGUI& reference) = delete;
		CoopMenuGUI& operator=(const CoopMenuGUI& rhs) = delete;
	};
} // namespace RTE
