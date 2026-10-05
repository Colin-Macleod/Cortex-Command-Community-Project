#include "CoopMenuGUI.h"

#include "LockstepMan.h"
#include "SettingsMan.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUILabel.h"
#include "GUIButton.h"
#include "GUITextBox.h"

using namespace RTE;

CoopMenuGUI::CoopMenuGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput) {
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_GUIControlManager->Load("Base.rte/GUIs/CoopMenuGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* rootBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"));
	rootBox->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	GUICollectionBox* coopMenuBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxCoop"));
	coopMenuBox->CenterInParent(true, true);
	coopMenuBox->SetPositionAbs(coopMenuBox->GetXPos(), (rootBox->GetHeight() < 540) ? coopMenuBox->GetYPos() - 15 : 140);

	m_BackToMainButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMainMenu"));
	m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, coopMenuBox->GetYPos() + coopMenuBox->GetHeight() + 10);

	m_NameTextBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextBoxName"));
	m_NameTextBox->SetMaxTextLength(24);
	m_AddressTextBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextBoxAddress"));
	m_AddressTextBox->SetMaxTextLength(64);
	m_PortTextBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextBoxPort"));
	m_PortTextBox->SetNumericOnly(true);
	m_PortTextBox->SetMaxNumericValue(65535);
	m_PortTextBox->SetMaxTextLength(5);
	m_JoinButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonJoin"));
	m_HostButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonHost"));
	m_PlayersLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelPlayers"));
	m_StatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelStatus"));
	m_ChooseActivityButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonChooseActivity"));
	m_LeaveButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLeave"));

	Refresh();
}

void CoopMenuGUI::Refresh() {
	m_NameTextBox->SetText(g_SettingsMan.GetCoopPlayerName());
	m_AddressTextBox->SetText(g_SettingsMan.GetCoopJoinAddress());
	m_PortTextBox->SetText(std::to_string(g_SettingsMan.GetCoopHostPort()));
	UpdateSessionDisplay();
}

void CoopMenuGUI::SaveFields() {
	g_SettingsMan.SetCoopPlayerName(m_NameTextBox->GetText());
	m_NameTextBox->SetText(g_SettingsMan.GetCoopPlayerName());
	g_SettingsMan.SetCoopJoinAddress(m_AddressTextBox->GetText());
	const int port = std::atoi(m_PortTextBox->GetText().c_str());
	if (port > 0 && port <= 65535) {
		g_SettingsMan.SetCoopHostPort(port);
	}
	m_PortTextBox->SetText(std::to_string(g_SettingsMan.GetCoopHostPort()));
}

void CoopMenuGUI::UpdateSessionDisplay() {
	const LockstepMan::Role role = g_LockstepMan.GetRole();
	const bool inSession = role != LockstepMan::Role::None;

	std::string players;
	if (inSession) {
		const std::vector<std::string> names = g_LockstepMan.GetLobbyPlayerNames();
		for (size_t i = 0; i < names.size(); ++i) {
			players += std::to_string(i + 1) + ". " + names[i] + (i == 0 ? "  (host)" : "") + "\n";
		}
	}
	m_PlayersLabel->SetText(players.empty() ? "-" : players);

	std::string status;
	if (!inSession) {
		status = "Host a game, or enter the host's address and join. The host's UDP port has to be reachable from the other players' machines (on the internet, forward it on the host's router). Everyone needs the same game version and mods.";
	} else if (role == LockstepMan::Role::Host) {
		status = g_LockstepMan.GetStatusMessage() + ". Choose an activity to start it for everyone, or wait for more players.";
	} else {
		status = g_LockstepMan.GetStatusMessage();
	}
	m_StatusLabel->SetText(status);

	m_ChooseActivityButton->SetVisible(role == LockstepMan::Role::Host);
	m_LeaveButton->SetVisible(inSession);
	m_HostButton->SetText(role == LockstepMan::Role::Host ? "Restart Hosting" : "Host Game");
	m_JoinButton->SetText(role == LockstepMan::Role::Client ? "Rejoin" : "Join Game");
}

CoopMenuGUI::CoopMenuUpdateResult CoopMenuGUI::HandleInputEvents() {
	m_GUIControlManager->Update();

	CoopMenuUpdateResult result = CoopMenuUpdateResult::NoEvent;
	GUIEvent guiEvent;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_BackToMainButton) {
				SaveFields();
				result = CoopMenuUpdateResult::BackToMain;
			} else if (guiEvent.GetControl() == m_HostButton) {
				SaveFields();
				g_GUISound.ButtonPressSound()->Play();
				g_LockstepMan.HostSession(static_cast<unsigned short>(g_SettingsMan.GetCoopHostPort()));
				g_SettingsMan.UpdateSettingsFile();
			} else if (guiEvent.GetControl() == m_JoinButton) {
				SaveFields();
				g_GUISound.ButtonPressSound()->Play();
				g_LockstepMan.JoinSession(g_SettingsMan.GetCoopJoinAddress());
				g_SettingsMan.UpdateSettingsFile();
			} else if (guiEvent.GetControl() == m_LeaveButton) {
				g_GUISound.BackButtonPressSound()->Play();
				g_LockstepMan.LeaveSession();
			} else if (guiEvent.GetControl() == m_ChooseActivityButton) {
				SaveFields();
				result = CoopMenuUpdateResult::ChooseActivity;
			}
		} else if (guiEvent.GetType() == GUIEvent::Notification) {
			if (guiEvent.GetMsg() == GUIButton::Focused && dynamic_cast<GUIButton*>(guiEvent.GetControl())) {
				g_GUISound.SelectionChangeSound()->Play();
			} else if (guiEvent.GetMsg() == GUITextBox::Enter && guiEvent.GetControl() == m_AddressTextBox) {
				SaveFields();
				g_LockstepMan.JoinSession(g_SettingsMan.GetCoopJoinAddress());
				g_SettingsMan.UpdateSettingsFile();
			}
		}
	}
	UpdateSessionDisplay();
	return result;
}

void CoopMenuGUI::Draw() const {
	m_GUIControlManager->Draw();
}
