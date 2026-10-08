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
		if (d.kind == DeviceKind::Keyboard)
			return L("Keyboard");
		const PadFamily family = d.model.Family();
		if (d.model.arcadeStick)
		{
			switch (family)
			{
			case PadFamily::Xbox: return L("Arcade stick (Xbox)");
			case PadFamily::PlayStation: return L("Arcade stick (PlayStation)");
			case PadFamily::Nintendo: return L("Arcade stick (Switch)");
			default: return L("Arcade stick");
			}
		}
		switch (family)
		{
		case PadFamily::Xbox: return L("Xbox controller");
		case PadFamily::PlayStation: return L("PlayStation controller");
		case PadFamily::Nintendo: return L("Switch controller");
		default: return L("Controller");
		}
	}

	// The text cut to maxWidth with "..." at the end, for labels drawn straight onto the draw
	// list (they do not wrap or clip, so a long name would run out of its tile).
	std::string FitText(const std::string& text, float maxWidth)
	{
		if (maxWidth <= 0.0f)
			return std::string();
		if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
			return text;
		const float dotsW = ImGui::CalcTextSize("...").x;
		size_t len = text.size();
		while (len > 0)
		{
			--len;
			while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80)
				--len; // never cut a UTF-8 character in half
			if (ImGui::CalcTextSize(text.c_str(), text.c_str() + len).x + dotsW <= maxWidth)
				break;
		}
		return len > 0 ? text.substr(0, len) + "..." : (dotsW <= maxWidth ? std::string("...") : std::string());
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
			DeviceArt::StyleFor(device), IM_COL32(220, 225, 235, 255));
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
			DeviceArt::StyleFor(device), iconColor);

		const float textX = a.x + 18.0f + iconH * 1.6f;
		const float textW = b.x - 8.0f - textX;
		draw->AddText(ImVec2(textX, a.y + height * 0.14f),
			device.connected ? IM_COL32(240, 240, 245, 255) : IM_COL32(130, 130, 140, 255), FitText(device.name, textW).c_str());
		std::string sub = KindText(device);
		if (!device.connected)
			sub += "  -  " + L("unplugged");
		else if (device.pinned)
			sub += "  -  " + L("always this player");
		draw->AddText(ImVec2(textX, a.y + height * 0.54f), IM_COL32(150, 155, 168, 255), FitText(sub, textW).c_str());
		if (hovered && (textW < ImGui::CalcTextSize(device.name.c_str()).x || textW < ImGui::CalcTextSize(sub.c_str()).x)
			&& !ImGui::IsMouseDown(ImGuiMouseButton_Left))
			ImGui::SetTooltip("%s\n%s", device.name.c_str(), sub.c_str()); // shortened: the full text

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
			ImGui::TextDisabledWrapped("%s", L("Plug in a controller, or drag one here.").c_str());
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
					const std::string name = Manager::ControlName(device, it->first) + "  x";
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
		// Narrower on a small game window, so the window never runs off the screen.
		const float contentWidth = (std::max)(260.0f, (std::min)(620.0f,
			ImGui::GetIO().DisplaySize.x * 0.92f - ImGui::GetStyle().WindowPadding.x * 2.0f));

		// Header: icon, name (Rename to edit), and which player it plays for.
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const float iconH = ImGui::GetFontSize() * 1.8f;
		DeviceArt::DrawIcon(draw, ImGui::GetCursorScreenPos(), iconH, DeviceArt::StyleFor(device),
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
	// The item just submitted takes a device dropped on it and hides it.
	void HideDropTarget(const std::vector<DeviceView>& devices)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayload))
		{
			const std::string id(static_cast<const char*>(payload->Data));
			auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceView& d) { return d.id == id; });
			if (it != devices.end() && !Hidden(*it))
				Manager::Get().SetIgnored(id, true);
		}
		ImGui::EndDragDropTarget();
	}

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
		// Dragging a device from a player card: the list is where it can be dropped to hide it,
		// shown as a drop zone while it is empty.
		const ImGuiPayload* drag = ImGui::GetDragDropPayload();
		const bool dragging = drag && drag->IsDataType(kPayload);
		if (hidden.empty())
		{
			if (!dragging)
				return;
			ImGui::Spacing();
			const float height = ImGui::GetFontSize() * 2.0f;
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			ImGui::InvisibleButton("##hide_drop", ImVec2(width, height));
			ImDrawList* draw = ImGui::GetWindowDrawList();
			draw->AddRectFilled(a, ImVec2(a.x + width, a.y + height), IM_COL32(34, 37, 46, 255), 5.0f);
			draw->AddRect(a, ImVec2(a.x + width, a.y + height), IM_COL32(120, 126, 140, 255), 5.0f, 0, 1.5f);
			const std::string text = L("Drop here to hide it");
			const ImVec2 size = ImGui::CalcTextSize(text.c_str());
			draw->AddText(ImVec2(a.x + (width - size.x) * 0.5f, a.y + (height - size.y) * 0.5f), IM_COL32(200, 205, 215, 255), text.c_str());
			HideDropTarget(devices);
			return;
		}
		const bool flashing = held;

		ImGui::Spacing();
		char label[128];
		snprintf(label, sizeof(label), L("Hidden devices (%d)").c_str(), static_cast<int>(hidden.size()));
		std::string header = label;
		if (dragging)
			header += "  -  " + L("Drop here to hide it");
		header += "###hidden_devices";
		ImGui::PushStyleColor(ImGuiCol_Header, flashing ? DeviceArt::PlayerColor(Side_P1, 0.9f) : IM_COL32(40, 44, 54, 255));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(55, 60, 72, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, flashing ? IM_COL32(10, 20, 20, 255) : IM_COL32(220, 222, 228, 255));
		ImGui::SetNextItemOpen(g_hiddenOpen, ImGuiCond_Always);
		g_hiddenOpen = ImGui::CollapsingHeader(header.c_str());
		ImGui::PopStyleColor(3);
		HideDropTarget(devices);
		if (held && !g_hiddenOpen && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", L("A hidden device is being pressed. Open the list to see which one.").c_str());
		if (!g_hiddenOpen)
			return;

		ImGui::BeginGroup();
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
				DeviceArt::StyleFor(*d), d->connected ? IM_COL32(200, 205, 215, 255) : IM_COL32(100, 104, 112, 255));
			const float textX = a.x + 16.0f + iconH * 1.6f;
			// Unplugged devices that were never hidden have nothing to show; they reappear on
			// their own when plugged back in.
			const bool canShow = d->ignored || (d->idle && d->connected);
			const std::string show = L("Show");
			const float buttonW = ImGui::CalcTextSize(show.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			// Name, then why it is here; both shortened to stay clear of the Show button.
			const float textRight = a.x + width - (canShow ? buttonW + 16.0f : 8.0f);
			const std::string name = FitText(d->name, textRight - textX);
			draw->AddText(ImVec2(textX, a.y + height * 0.18f), d->connected ? IM_COL32(225, 228, 235, 255) : IM_COL32(120, 124, 132, 255), name.c_str());
			const float reasonX = textX + ImGui::CalcTextSize(name.c_str()).x + 10.0f;
			const std::string reason = FitText(HiddenReason(*d), textRight - reasonX);
			if (!reason.empty())
				draw->AddText(ImVec2(reasonX, a.y + height * 0.18f), IM_COL32(140, 145, 158, 255), reason.c_str());

			if (canShow)
			{
				// The row itself drags onto a player: that shows it and puts it on that side.
				ImGui::SetCursorScreenPos(a);
				ImGui::InvisibleButton("##hidden_row", ImVec2((std::max)(1.0f, width - buttonW - 16.0f), height));
				if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					// The row may show the name shortened: the tooltip has all of it.
					ImGui::SetTooltip("%s - %s\n%s", d->name.c_str(), HiddenReason(*d).c_str(),
						L("Drag onto Player 1 or Player 2 to use it there.").c_str());
				}
				DrawDragPayload(*d);
				ImGui::SetCursorScreenPos(ImVec2(a.x + width - buttonW - 8.0f, a.y + (height - ImGui::GetFrameHeight()) * 0.5f));
				if (ImGui::Button(show.c_str()))
					Manager::Get().SetIgnored(d->id, false);
			}
			ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + height + 3.0f));
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::PopID();
		}
		ImGui::EndGroup();
		HideDropTarget(devices);
	}

	void DrawFooter()
	{
		Manager& manager = Manager::Get();
		ImGui::Spacing();
		if (manager.SteamInputLikely())
		{
			ImGui::TextColoredWrapped(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", Messages.Steam_input_active_notice());
			ImGui::ShowHelpMarkerSameLine(Messages.Steam_input_limitations());
		}

		// Each control and its help marker move to the next line together when the menu is too
		// narrow for both.
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float marker = ImGui::HelpMarkerWidth();
		const std::string autoLabel = L("Automatically detect controllers");
		bool autoDetect = Settings::settingsIni.autoUpdateControllers;
		if (ImGui::Checkbox(autoLabel.c_str(), &autoDetect))
		{
			Settings::settingsIni.autoUpdateControllers = autoDetect;
			Settings::changeSetting("AutomaticallyUpdateControllers", autoDetect ? "1" : "0");
			manager.SetAutoDetect(autoDetect);
		}
		ImGui::ShowHelpMarkerSameLine(L("Notices controllers being plugged in and out while the game runs. Turn off only if your PC stutters when devices change; then use Refresh after plugging something in.").c_str());

		const std::string refreshLabel = L("Refresh controllers");
		const std::string resetLabel = L("Reset sides");
		const std::string reportLabel = L("Log what's happening now");
		ImGui::SameLineOrWrap(ImGui::ButtonWidth(refreshLabel.c_str()));
		if (ImGui::SmallButton(refreshLabel.c_str()))
			manager.Refresh();
		ImGui::SameLineOrWrap(ImGui::ButtonWidth(resetLabel.c_str()) + spacing + marker);
		if (ImGui::SmallButton(resetLabel.c_str()))
			manager.ResetAssignments();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Forgets every device you moved by hand: keyboards go back to Player 1, the first controller to Player 1, the second to Player 2.").c_str());

		// For testers: one click writes the whole input state and recent presses to DEBUG.txt,
		// so a report can point at the exact moment something went wrong.
		static int s_lastReport = -1;
		static double s_reportShownAt = -100.0;
		if (ImGui::SmallButton(reportLabel.c_str()))
		{
			s_lastReport = manager.LogReport("the player pressed Log what's happening now");
			s_reportShownAt = ImGui::GetTime();
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Found a controller problem? Press this right when it happens: everything about your controllers and your last button presses goes into DEBUG.txt in the BBCF_IM folder for the bug report.").c_str());
		if (ImGui::GetTime() - s_reportShownAt < 6.0)
		{
			char done[128];
			snprintf(done, sizeof(done), L("Written to DEBUG.txt as report #%d").c_str(), s_lastReport);
			const std::string status = s_lastReport > 0 ? std::string(done)
				: L("Debug logs are off. Turn on Generate debug logs in Settings first.");
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(status.c_str()).x);
			ImGui::TextColoredWrapped(s_lastReport > 0 ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
				"%s", status.c_str());
		}

		if (Settings::settingsIni.enableInDevelopmentFeatures)
		{
			if (ImGui::SmallButton(Messages.Open_Joy_cpl()))
				ControllerOverrideManager::GetInstance().OpenControllerControlPanel();
			char steam[128];
			snprintf(steam, sizeof(steam), Messages.STEAM_INPUT_s(), manager.SteamInputLikely() ? Messages.ON() : Messages.OFF());
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(steam).x);
			ImGui::TextDisabled("%s", steam);
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
	const float avail = ImGui::GetContentRegionAvail().x;
	// Side by side while each card can still show a device name; one above the other below that.
	const bool stacked = (avail - spacing) * 0.5f < ImGui::GetFontSize() * 14.0f;
	const float cardWidth = stacked ? avail : (avail - spacing) * 0.5f;
	int rows[2] = {};
	for (const DeviceView& d : devices)
		if (!Hidden(d))
			rows[d.side]++;
	// A tile is its button and a 2px spacer, each followed by the item spacing. Stacked, each
	// card fits its own devices; side by side, both match the taller one.
	const float tile = ImGui::GetFontSize() * 2.6f + ImGui::GetStyle().ItemSpacing.y * 2.0f + 2.0f;
	const auto cardHeight = [&](int count) {
		return ImGui::GetFontSize() * 3.2f + tile * (std::max)(stacked ? 1 : 2, count) + 12.0f;
	};
	const int both = (std::max)(rows[0], rows[1]);

	bool openConfigure = false;
	DeviceView target;
	DrawCard(Side_P1, devices, cardWidth, cardHeight(stacked ? rows[0] : both), &openConfigure, &target);
	if (!stacked)
		ImGui::SameLine();
	DrawCard(Side_P2, devices, cardWidth, cardHeight(stacked ? rows[1] : both), &openConfigure, &target);
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
