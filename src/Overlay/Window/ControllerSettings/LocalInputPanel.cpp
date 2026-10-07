#include "LocalInputPanel.h"

#include "DeviceArt.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Input/LocalInputManager.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	using namespace LocalInput;

	constexpr ULONGLONG kGlowMs = 180;
	const char* const kPayload = "LOCALINPUT_DEVICE";
	const char* const kConfigurePopup = "##localinput_configure";
	const char* const kDisconnectedPopup = "##localinput_disconnected";

	bool g_hiddenOpen = false;          // the hidden-devices list is expanded
	std::string g_configureId;          // device whose window is open
	bool g_renaming = false;
	char g_renameBuffer[64] = {};
	int g_captureFor = -1;              // index into kGameButtons while listening
	std::string g_quickRenameId;        // device being renamed from the right-click menu
	bool g_openQuickRename = false;
	std::string g_disconnectedName;     // set when the configured device unplugs mid-way
	bool g_openDisconnected = false;

	// Kept out of the player cards: hidden by the player, keyboards nobody has typed on, and
	// anything unplugged. They live in the "Hidden devices" list instead.
	bool Hidden(const DeviceView& d)
	{
		return d.ignored || d.idle || !d.connected;
	}

	std::string HiddenReason(const DeviceView& d)
	{
		if (!d.connected)
			return d.ignored ? L("hidden, unplugged") : L("unplugged");
		if (d.ignored)
			return L("hidden by you");
		return L("never used");
	}

	std::string KindText(const DeviceView& d)
	{
		switch (DeviceArt::StyleFor(d.kind, d.defaultName))
		{
		case DeviceArt::IconStyle::Keyboard: return L("Keyboard");
		case DeviceArt::IconStyle::XboxPad: return L("Xbox controller");
		case DeviceArt::IconStyle::ArcadeStick: return L("Arcade stick");
		default: return L("Controller");
		}
	}

	void BigText(const char* text, ImU32 color, float scale)
	{
		ImGui::PushFont(nullptr, ImGui::GetFontSize() * scale);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}

	// The mod's windows are slightly see-through; a popup over the menu must not be, or the
	// text behind it bleeds through.
	struct OpaquePopup
	{
		OpaquePopup()
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
			ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(22, 24, 30, 255));
			ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 140));
		}
		~OpaquePopup()
		{
			ImGui::PopStyleColor(2);
			ImGui::PopStyleVar();
		}
	};

	void StopCapture()
	{
		g_captureFor = -1;
		Manager::Get().EndCapture();
	}

	// The item just submitted drags this device onto a player card.
	void DrawDragPayload(const DeviceView& device)
	{
		if (!ImGui::BeginDragDropSource())
			return;
		ImGui::SetDragDropPayload(kPayload, device.id.c_str(), device.id.size() + 1);
		const ImVec2 at = ImGui::GetCursorScreenPos();
		DeviceArt::DrawIcon(ImGui::GetWindowDrawList(), at, ImGui::GetFontSize() * 1.6f,
			DeviceArt::StyleFor(device.kind, device.defaultName), IM_COL32(220, 225, 235, 255));
		ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 2.7f, ImGui::GetFontSize() * 1.6f));
		ImGui::SameLine();
		ImGui::TextUnformatted(device.name.c_str());
		ImGui::EndDragDropSource();
	}

	// ------------------------------------------------------------------ player cards

	void DrawTile(const DeviceView& device, bool* openConfigure, DeviceView* target)
	{
		ImGui::PushID(device.id.c_str());
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ULONGLONG now = GetTickCount64();
		const bool active = device.connected && (device.controls.any() || now - device.lastInputMs < kGlowMs);
		const float height = ImGui::GetFontSize() * 2.6f;
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const ImVec2 b(a.x + width, a.y + height);

		ImGui::InvisibleButton("##tile", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemDeactivated() && hovered && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 36.0f)
		{
			*openConfigure = true;
			*target = device;
		}
		DrawDragPayload(device);
		if (ImGui::BeginPopupContextItem("##tile_menu"))
		{
			if (ImGui::MenuItem(L("Test and buttons...").c_str()))
			{
				*openConfigure = true;
				*target = device;
			}
			if (ImGui::MenuItem(L("Rename...").c_str()))
			{
				g_quickRenameId = device.id;
				strncpy_s(g_renameBuffer, device.name.c_str(), _TRUNCATE);
				g_openQuickRename = true;
			}
			const Side other = device.side == Side_P1 ? Side_P2 : Side_P1;
			if (ImGui::MenuItem(other == Side_P1 ? L("Move to Player 1").c_str() : L("Move to Player 2").c_str()))
				Manager::Get().Assign(device.id, other);
			ImGui::Separator();
			if (ImGui::MenuItem(L("Hide this device").c_str()))
				Manager::Get().SetIgnored(device.id, true);
			ImGui::EndPopup();
		}

		// Tile background: lit in the player's colour while the device is used.
		const ImU32 fill = active ? DeviceArt::PlayerColor(device.side, 0.45f)
			: hovered ? IM_COL32(60, 65, 78, 255) : IM_COL32(42, 46, 56, 255);
		draw->AddRectFilled(a, b, fill, 6.0f);
		if (active)
			draw->AddRect(a, b, DeviceArt::PlayerColor(device.side), 6.0f, 0, 2.0f);

		const float iconH = height * 0.62f;
		const ImU32 iconColor = device.connected ? IM_COL32(225, 230, 240, 255) : IM_COL32(110, 115, 125, 255);
		DeviceArt::DrawIcon(draw, ImVec2(a.x + 10.0f, a.y + (height - iconH) * 0.5f), iconH,
			DeviceArt::StyleFor(device.kind, device.defaultName), iconColor);

		const float textX = a.x + 18.0f + iconH * 1.6f;
		draw->AddText(ImVec2(textX, a.y + height * 0.14f),
			device.connected ? IM_COL32(240, 240, 245, 255) : IM_COL32(130, 130, 140, 255), device.name.c_str());
		std::string sub = KindText(device);
		if (!device.connected)
			sub += "  -  " + L("unplugged");
		else if (device.pinned)
			sub += "  -  " + L("always this player");
		draw->AddText(ImVec2(textX, a.y + height * 0.54f), IM_COL32(150, 155, 168, 255), sub.c_str());

		ImGui::Dummy(ImVec2(0, 2.0f));
		ImGui::PopID();
	}

	void DrawCard(Side side, const std::vector<DeviceView>& devices, float width, float height,
		bool* openConfigure, DeviceView* target)
	{
		ImGui::PushID(side);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(26, 28, 35, 235));
		ImGui::BeginChild("##card", ImVec2(width, height), ImGuiChildFlags_Borders);

		// Header: the player's name and a dot that lights while that player presses anything.
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool hasInput = (Manager::Get().PlayerWord(side, false) | Manager::Get().PlayerWord(side, true)) != 0;
		BigText(side == Side_P1 ? L("Player 1").c_str() : L("Player 2").c_str(), IM_COL32(235, 237, 242, 255), 1.3f);
		const float dotR = ImGui::GetFontSize() * 0.32f;
		const ImVec2 dot(p.x + ImGui::GetContentRegionAvail().x - dotR - 2.0f, p.y + ImGui::GetFontSize() * 0.65f);
		draw->AddCircleFilled(dot, dotR, hasInput ? DeviceArt::PlayerColor(side) : IM_COL32(60, 64, 74, 255));
		ImGui::Separator();
		ImGui::Spacing();

		int shown = 0;
		for (const DeviceView& device : devices)
		{
			if (device.side != side || Hidden(device))
				continue;
			DrawTile(device, openConfigure, target);
			++shown;
		}
		if (shown == 0)
		{
			ImGui::Spacing();
			ImGui::TextDisabled("%s", L("Plug in a controller, or drag one here.").c_str());
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayload))
				Manager::Get().Assign(std::string(static_cast<const char*>(payload->Data)), side);
			ImGui::EndDragDropTarget();
		}
		ImGui::PopID();
	}

	// ------------------------------------------------------------------ configure window

	void DrawMappingFallback(const DeviceView& device)
	{
		Manager& manager = Manager::Get();
		Mapping mapping = manager.GetMapping(device.id);
		bool changed = false;
		if (g_captureFor >= 0)
		{
			int control = -1;
			if (manager.PollCapture(&control))
			{
				mapping[control] |= kGameButtons[g_captureFor].bit;
				changed = true;
				StopCapture();
			}
		}
		if (ImGui::BeginTable("##mapping", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn(L("Game button").c_str(), ImGuiTableColumnFlags_WidthFixed, 130.0f);
			ImGui::TableSetupColumn(L("Bound to").c_str());
			ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			for (int b = 0; b < kGameButtonCount; ++b)
			{
				const GameButtonInfo& info = kGameButtons[b];
				ImGui::PushID(b);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (device.buttons & info.bit)
					ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "%s", info.name);
				else
					ImGui::TextUnformatted(info.name);
				ImGui::TableNextColumn();
				for (auto it = mapping.begin(); it != mapping.end(); ++it)
				{
					if (!(it->second & info.bit))
						continue;
					ImGui::PushID(it->first);
					const std::string name = Manager::ControlName(device.kind, it->first) + "  x";
					if (ImGui::SmallButton(name.c_str()))
					{
						it->second &= ~info.bit;
						changed = true;
					}
					ImGui::PopID();
					ImGui::SameLine();
				}
				ImGui::NewLine();
				ImGui::TableNextColumn();
				if (g_captureFor == b)
				{
					if (ImGui::SmallButton(L("Cancel").c_str()))
						StopCapture();
				}
				else if (ImGui::SmallButton(L("+ Bind").c_str()))
				{
					g_captureFor = b;
					manager.BeginCapture(device.id);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (g_captureFor >= 0)
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), L("Press a button on %s for %s... (the game ignores input while listening)").c_str(),
				device.name.c_str(), kGameButtons[g_captureFor].name);
		if (changed)
		{
			for (auto it = mapping.begin(); it != mapping.end();)
				it = it->second ? std::next(it) : mapping.erase(it);
			manager.SetMapping(device.id, mapping);
		}

	}

	void DrawConfigureWindow(const std::vector<DeviceView>& devices)
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		OpaquePopup opaque;
		if (!ImGui::BeginPopupModal(kConfigurePopup, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
			return;

		Manager& manager = Manager::Get();
		auto it = std::find_if(devices.begin(), devices.end(), [](const DeviceView& d) { return d.id == g_configureId; });
		if (it == devices.end() || !it->connected)
		{
			// Unplugged mid-way: close this and say so.
			g_disconnectedName = it != devices.end() ? it->name : std::string();
			g_openDisconnected = true;
			StopCapture();
			g_renaming = false;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const DeviceView& device = *it;
		const float contentWidth = 620.0f;

		// Header: icon, name (Rename to edit), and which player it plays for.
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const float iconH = ImGui::GetFontSize() * 1.8f;
		DeviceArt::DrawIcon(draw, ImGui::GetCursorScreenPos(), iconH, DeviceArt::StyleFor(device.kind, device.defaultName),
			IM_COL32(225, 230, 240, 255));
		ImGui::Dummy(ImVec2(iconH * 1.6f, iconH));
		ImGui::SameLine();
		ImGui::BeginGroup();
		if (g_renaming)
		{
			ImGui::SetNextItemWidth(260.0f);
			ImGui::SetKeyboardFocusHere();
			const bool done = ImGui::InputText("##rename", g_renameBuffer, sizeof(g_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			if (done || ImGui::SmallButton(L("Save").c_str()))
			{
				manager.Rename(device.id, g_renameBuffer);
				g_renaming = false;
			}
		}
		else
		{
			BigText(device.name.c_str(), IM_COL32(240, 240, 245, 255), 1.3f);
			ImGui::SameLine();
			if (ImGui::SmallButton(L("Rename").c_str()))
			{
				strncpy_s(g_renameBuffer, device.name.c_str(), _TRUNCATE);
				g_renaming = true;
			}
		}
		ImGui::TextDisabled("%s", KindText(device).c_str());
		ImGui::EndGroup();

		ImGui::Spacing();
		ImGui::TextUnformatted(L("Plays for").c_str());
		ImGui::SameLine();
		for (Side side : { Side_P1, Side_P2 })
		{
			const bool current = device.side == side;
			ImGui::PushStyleColor(ImGuiCol_Button, current ? DeviceArt::PlayerColor(side) : IM_COL32(50, 54, 64, 255));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DeviceArt::PlayerColor(side, 0.75f));
			ImGui::PushStyleColor(ImGuiCol_Text, current ? IM_COL32(15, 20, 20, 255) : IM_COL32(230, 230, 235, 255));
			if (ImGui::Button(side == Side_P1 ? L("Player 1").c_str() : L("Player 2").c_str(), ImVec2(110.0f, 0.0f)) && !current)
				manager.Assign(device.id, side);
			ImGui::PopStyleColor(3);
			if (side == Side_P1)
				ImGui::SameLine();
		}

		ImGui::Spacing();
		ImGui::SeparatorText(L("What the game sees").c_str());
		DeviceArt::DrawGameInput(device.buttons, ImGui::GetFontSize() * 6.0f);

		ImGui::SeparatorText(KindText(device).c_str());
		const float artWidth = device.kind == DeviceKind::Keyboard ? contentWidth : contentWidth * 0.72f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (contentWidth - artWidth) * 0.5f);
		DeviceArt::DrawDevice(device, manager.GetMapping(device.id), artWidth);

		ImGui::Spacing();
		const bool custom = manager.HasCustomMapping(device.id);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentWidth);
		if (custom)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s",
				L("This device uses the buttons set below instead of the game's Button Settings.").c_str());
			if (ImGui::Button(L("Use the game's Button Settings again").c_str()))
				manager.ResetMapping(device.id);
		}
		else
		{
			ImGui::TextDisabled("%s", L("Set this device's buttons in the game's own Button Settings (Options, character select, or the pause menu). They are remembered for this device, on either player.").c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::SetNextItemWidth(contentWidth);
		if (ImGui::CollapsingHeader(L("Change this device's buttons in the mod instead").c_str()))
			DrawMappingFallback(device);

		ImGui::Spacing();
		if (ImGui::Button(L("Done").c_str(), ImVec2(120.0f, 0.0f)))
		{
			StopCapture();
			g_renaming = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void DrawQuickRenamePopup()
	{
		const char* const id = "##localinput_quick_rename";
		if (g_openQuickRename)
		{
			ImGui::OpenPopup(id);
			g_openQuickRename = false;
		}
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		OpaquePopup opaque;
		if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted(L("Rename").c_str());
			ImGui::SetNextItemWidth(280.0f);
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("##quick_rename", g_renameBuffer, sizeof(g_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
			if (enter || ImGui::Button(L("Save").c_str(), ImVec2(120.0f, 0.0f)))
			{
				Manager::Get().Rename(g_quickRenameId, g_renameBuffer);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("Cancel").c_str(), ImVec2(120.0f, 0.0f)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void DrawDisconnectedPopup()
	{
		if (g_openDisconnected)
		{
			ImGui::OpenPopup(kDisconnectedPopup);
			g_openDisconnected = false;
		}
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		// Always, not Appearing: an auto-sized window has no size on its first frame, so a
		// centred pivot would land it off to one side (docs/ImGuiModalCentering.md).
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		OpaquePopup opaque;
		if (ImGui::BeginPopupModal(kDisconnectedPopup, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
		{
			BigText(L("Controller disconnected").c_str(), IM_COL32(255, 200, 120, 255), 1.25f);
			if (!g_disconnectedName.empty())
				ImGui::Text(L("%s was unplugged. Plug it back in and it returns to the same player.").c_str(), g_disconnectedName.c_str());
			ImGui::Spacing();
			if (ImGui::Button(L("OK").c_str(), ImVec2(120.0f, 0.0f)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
}

namespace
{
	// The devices kept out of the cards. Its header lights exactly while one of them is held -
	// hold and it stays lit, tap and it blinks once - so a device hidden by mistake can be
	// matched to what the player is doing.
	void DrawHiddenList(const std::vector<DeviceView>& devices)
	{
		std::vector<const DeviceView*> hidden;
		bool held = false;
		const ULONGLONG now = GetTickCount64();
		for (const DeviceView& d : devices)
		{
			if (!Hidden(d))
				continue;
			hidden.push_back(&d);
			if (d.connected && (d.controls.any() || now - d.lastInputMs < kGlowMs))
				held = true; // steady while used, short trailing glow like the tiles, never blinking
		}
		if (hidden.empty())
			return;
		const bool flashing = held;

		ImGui::Spacing();
		char label[128];
		snprintf(label, sizeof(label), L("Hidden devices (%d)").c_str(), static_cast<int>(hidden.size()));
		ImGui::PushStyleColor(ImGuiCol_Header, flashing ? DeviceArt::PlayerColor(Side_P1, 0.9f) : IM_COL32(40, 44, 54, 255));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(55, 60, 72, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, flashing ? IM_COL32(10, 20, 20, 255) : IM_COL32(220, 222, 228, 255));
		ImGui::SetNextItemOpen(g_hiddenOpen, ImGuiCond_Always);
		g_hiddenOpen = ImGui::CollapsingHeader(label);
		ImGui::PopStyleColor(3);
		if (held && !g_hiddenOpen && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", L("A hidden device is being pressed. Open the list to see which one.").c_str());
		if (!g_hiddenOpen)
			return;

		for (const DeviceView* d : hidden)
		{
			ImGui::PushID(d->id.c_str());
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const float height = ImGui::GetFontSize() * 2.0f;
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			const bool active = d->connected && (d->controls.any() || now - d->lastInputMs < kGlowMs);
			draw->AddRectFilled(a, ImVec2(a.x + width, a.y + height),
				active ? DeviceArt::PlayerColor(Side_P1, 0.45f) : IM_COL32(34, 37, 46, 255), 5.0f);
			const float iconH = height * 0.62f;
			DeviceArt::DrawIcon(draw, ImVec2(a.x + 8.0f, a.y + (height - iconH) * 0.5f), iconH,
				DeviceArt::StyleFor(d->kind, d->defaultName), d->connected ? IM_COL32(200, 205, 215, 255) : IM_COL32(100, 104, 112, 255));
			const float textX = a.x + 16.0f + iconH * 1.6f;
			draw->AddText(ImVec2(textX, a.y + height * 0.18f), d->connected ? IM_COL32(225, 228, 235, 255) : IM_COL32(120, 124, 132, 255), d->name.c_str());
			const std::string reason = HiddenReason(*d);
			const float nameW = ImGui::CalcTextSize(d->name.c_str()).x;
			draw->AddText(ImVec2(textX + nameW + 10.0f, a.y + height * 0.18f), IM_COL32(140, 145, 158, 255), reason.c_str());

			// Unplugged devices that were never hidden have nothing to show; they reappear on
			// their own when plugged back in.
			if (d->ignored || (d->idle && d->connected))
			{
				const std::string show = L("Show");
				const float buttonW = ImGui::CalcTextSize(show.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				// The row itself drags onto a player: that shows it and puts it on that side.
				ImGui::SetCursorScreenPos(a);
				ImGui::InvisibleButton("##hidden_row", ImVec2((std::max)(1.0f, width - buttonW - 16.0f), height));
				if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
					ImGui::SetTooltip("%s", L("Drag onto Player 1 or Player 2 to use it there.").c_str());
				DrawDragPayload(*d);
				ImGui::SetCursorScreenPos(ImVec2(a.x + width - buttonW - 8.0f, a.y + (height - ImGui::GetFrameHeight()) * 0.5f));
				if (ImGui::Button(show.c_str()))
					Manager::Get().SetIgnored(d->id, false);
			}
			ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + height + 3.0f));
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::PopID();
		}
	}

	void DrawFooter()
	{
		Manager& manager = Manager::Get();
		ImGui::Spacing();
		if (manager.SteamInputLikely())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", Messages.Steam_input_active_notice());
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Steam_input_limitations());
		}

		bool autoDetect = Settings::settingsIni.autoUpdateControllers;
		if (ImGui::Checkbox(L("Automatically detect controllers").c_str(), &autoDetect))
		{
			Settings::settingsIni.autoUpdateControllers = autoDetect;
			Settings::changeSetting("AutomaticallyUpdateControllers", autoDetect ? "1" : "0");
			manager.SetAutoDetect(autoDetect);
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Notices controllers being plugged in and out while the game runs. Turn off only if your PC stutters when devices change; then use Refresh after plugging something in.").c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(L("Refresh controllers").c_str()))
			manager.Refresh();
		ImGui::SameLine();
		if (ImGui::SmallButton(L("Reset sides").c_str()))
			manager.ResetAssignments();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Forgets every device you moved by hand: keyboards go back to Player 1, the first controller to Player 1, the second to Player 2.").c_str());

		if (Settings::settingsIni.enableInDevelopmentFeatures)
		{
			if (ImGui::SmallButton(Messages.Open_Joy_cpl()))
				ControllerOverrideManager::GetInstance().OpenControllerControlPanel();
			ImGui::SameLine();
			ImGui::TextDisabled(Messages.STEAM_INPUT_s(), manager.SteamInputLikely() ? Messages.ON() : Messages.OFF());
		}
	}
}

void LocalInputPanel::Draw()
{
	Manager& manager = Manager::Get();
	manager.Tick(false);
	if (!manager.IsActive())
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled("%s", L("The new controller system is off (NewControllerSystem in Settings), or controller hooks are disabled on this platform.").c_str());
		return;
	}

	const std::vector<DeviceView> devices = manager.Devices();

	ImGui::TextWrapped("%s", L("Drag a keyboard or controller to the other player to switch sides. Controllers can be plugged in at any time: the first one joins Player 1, the second Player 2. Click one to test it.").c_str());
	ImGui::Spacing();

	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float cardWidth = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
	int rows[2] = {};
	for (const DeviceView& d : devices)
		if (!Hidden(d))
			rows[d.side]++;
	const float tile = ImGui::GetFontSize() * 2.6f + ImGui::GetStyle().ItemSpacing.y + 2.0f;
	const float cardHeight = ImGui::GetFontSize() * 3.2f + tile * (std::max)(2, (std::max)(rows[0], rows[1])) + 12.0f;

	bool openConfigure = false;
	DeviceView target;
	DrawCard(Side_P1, devices, cardWidth, cardHeight, &openConfigure, &target);
	ImGui::SameLine();
	DrawCard(Side_P2, devices, cardWidth, cardHeight, &openConfigure, &target);
	if (openConfigure)
	{
		g_configureId = target.id;
		g_renaming = false;
		StopCapture();
		ImGui::OpenPopup(kConfigurePopup);
	}
	DrawConfigureWindow(devices);
	DrawQuickRenamePopup();
	DrawDisconnectedPopup();

	DrawHiddenList(devices);
	DrawFooter();
}
