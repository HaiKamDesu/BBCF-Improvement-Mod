#include "HitboxOverlay.h"

#include "Core/interfaces.h"
#include "Game/gamestates.h"
#include "Game/Jonb/JonbReader.h"
#include "imgui_internal.h"
#include "Core/utils.h"

void HitboxOverlay::Update()
{
	if (HasNullptrInData() || !m_windowOpen)
	{
		return;
	}

	if (!isHitboxOverlayEnabledInCurrentState())
	{
		return;
	}
	BeforeDraw();

	ImGui::Begin("##HitboxOverlay", nullptr, m_overlayWindowFlags);

	Draw();

	ImGui::End();

	AfterDraw();
}

void HitboxOverlay::BeforeDraw()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));

	this->io = ImGui::GetIO();
	displayRatio = io.DisplaySize.x / io.DisplaySize.y;
	aspectRatioAddress = GetBbcfBaseAdress() + 0x65A5E4;
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
}


// Fields the engine's own "can this entity hit right now" routine reads, but that the mod's
// CharData does not name (they sit inside its pad regions). Rather than restructure that struct and
// risk shifting every offset after them, they are read here by byte offset.
//
// The routine is at BBCF.exe+0x18C3E0, and the hit-detection sweep at +0x1594DD calls it to decide
// which entities are worth testing for collisions - so it is ground truth for "are these boxes
// live". It was found by tracing every access to the state-property bitfield at +0x25C; this is the
// only place the engine reads the 0x200/0x400 suppression bits the overlay used to key on.
namespace
{
	const unsigned int kOffsetExemptFlag      = 0x4BC;
	const unsigned int kOffsetExemptMask      = 0x150;
	const unsigned int kOffsetHitsUsed        = 0x480;
	const unsigned int kOffsetHitsAllowed     = 0x484;
	const unsigned int kOffsetAttackEnabled   = 0x254;
	const unsigned int kOffsetLinkedProvider  = 0x224;

	template <typename T>
	T ReadField(const CharData* entity, unsigned int offset)
	{
		return *(const T*)((const char*)entity + offset);
	}
}

// Reimplements BBCF.exe+0x18C3E0. The Astral branch it ends with is deliberately not reproduced:
// it needs a game call, and only applies to Astral Heats, where over-drawing a box is harmless.
bool HitboxOverlay::CanEntityHit(const CharData* entity)
{
	if (ReadField<unsigned char>(entity, kOffsetExemptFlag) != 0
		&& (ReadField<unsigned int>(entity, kOffsetExemptMask) & 0x4000000) != 0)
	{
		return false;
	}

	// Hits used against the cap for this attack. Equal means the attack is spent.
	if (ReadField<int>(entity, kOffsetHitsUsed) >= ReadField<int>(entity, kOffsetHitsAllowed))
	{
		return false;
	}

	// A positively set "attack enabled" bit, cleared by the same script handler that sets the
	// 0x200 suppression bit. The overlay never looked at it, which is why Litchi's staff - which
	// never has it set - could not be told apart from an entity that was genuinely attacking.
	if ((ReadField<unsigned int>(entity, kOffsetAttackEnabled) & 0x100) == 0)
	{
		return false;
	}

	const unsigned int stateFlags = entity->bitflags_for_curr_state_properties_or_smth;

	// 0x200 attack-off, 0x400 multihit gap, 0x4000000. The overlay used to test
	// (flags & 0xF00) == 0x200 || == 0x400, which is a different shape to the engine's test.
	if ((stateFlags & 0x4000600) != 0 || (stateFlags & 0x80) != 0)
	{
		return false;
	}

	return true;
}

// Some attacks keep their hitbox geometry on a second entity. The engine's box enumerator at
// +0x18BBD0 sums an entity's own boxes with those of the object this handle resolves to, which is
// how Litchi's 5C works: the attack belongs to Litchi, the rectangles live on her staff, and
// neither entity carries both. Evaluated in isolation the staff fails the gate above and Litchi has
// no boxes of her own, so 5C drew nothing at all.
//
// The engine resolves this through a handle-deref helper. Reading it directly could in principle
// hand back a stale pointer, so the result is only trusted when it is an entity the game currently
// has in its list.
CharData* HitboxOverlay::GetLinkedBoxProvider(const CharData* entity)
{
	CharData* linked = ReadField<CharData*>(entity, kOffsetLinkedProvider);
	if (!linked || linked == entity)
	{
		return nullptr;
	}

	for (int i = 0; i < g_gameVals.entityCount; i++)
	{
		if (linked == (CharData*)g_gameVals.pEntityList[i])
		{
			return linked;
		}
	}

	return nullptr;
}

void HitboxOverlay::Draw()
{
	for (int i = 0; i < g_gameVals.entityCount; i++)
	{
		CharData* pEntity = (CharData*)g_gameVals.pEntityList[i];

		// The status read below happened before this check, so an empty slot was dereferenced
		// every frame on the chance the list is ever sparse.
		if (!pEntity)
		{
			continue;
		}

		const bool isCharacter = i < 2;
		const bool isEntityActive = pEntity->unknownStatus1 == 1 && pEntity->pJonbEntryBegin;

		if (!isCharacter && !isEntityActive)
		{
			continue;
		}

		if (!IsOwnerEnabled(pEntity->ownerEntity))
		{
			continue;
		}

		// Hitboxes are drawn only for an entity the engine says can currently hit. Hurtboxes and
		// the per-entity overlays are unconditional, as before.
		const bool canHit = CanEntityHit(pEntity);

		const ImVec2 entityWorldPos = CalculateObjWorldPosition(pEntity);
		DrawCollisionAreas(pEntity, entityWorldPos, canHit, true, true);

		if (!canHit)
		{
			continue;
		}

		// Draw the attack's boxes wherever they actually live. They are rendered with the
		// provider's own transform - they are its geometry - but they belong to this attack, so
		// they appear under this entity's owner's toggle and disappear when its attack ends.
		CharData* linked = GetLinkedBoxProvider(pEntity);
		if (linked && !CanEntityHit(linked))
		{
			DrawCollisionAreas(linked, CalculateObjWorldPosition(linked), true, false, false);
		}
	}
}

void HitboxOverlay::AfterDraw()
{
ImGui::PopStyleColor();
ImGui::PopStyleVar(2);
}

bool HitboxOverlay::IsOwnerEnabled(CharData* ownerCharInfo)
{
	// ownerEntity turns out to hold the ROOT player, not the immediate parent: measured over a
	// Kokonoe/Ragna and a Kokonoe/Litchi session, all 194 distinct entity situations reported by
	// EntityDiagnostics sat at depth 0, Litchi's staff and her Lcef effect entities included. So
	// the climb below is currently a no-op and one extra pointer compare.
	//
	// It is kept because it is free and because the one-level test was only ever correct by
	// accident - nothing guarantees that flattening for every character, and Carl, Relius, Arakune
	// and Nu have not been sampled. The self-owner break is not hypothetical: a player's own
	// ownerEntity points at itself, which the log confirms.
	const int maxOwnerDepth = 16;

	for (int depth = 0; ownerCharInfo && depth < maxOwnerDepth; depth++)
	{
		for (int i = 0; i < 2; i++)
		{
			if (ownerCharInfo == (CharData*)g_gameVals.pEntityList[i])
			{
				return drawCharacterHitbox[i];
			}
		}

		CharData* nextOwner = ownerCharInfo->ownerEntity;
		if (nextOwner == ownerCharInfo)
		{
			break;
		}

		ownerCharInfo = nextOwner;
	}

	return false;
}

bool HitboxOverlay::HasNullptrInData()
{
	return !g_gameVals.pEntityList;
}

ImVec2 HitboxOverlay::CalculateObjWorldPosition(const CharData* charObj)
{
	float posX = charObj->position_x_dupe - charObj->offsetX_1 + charObj->offsetX_2;
	float posY = charObj->position_y_dupe + charObj->offsetY_2;

	return ImVec2(
		floor(posX / 1000 * m_scale),
		floor(posY / 1000 * m_scale)
	);
}

ImVec2 HitboxOverlay::CalculateScreenPosition(ImVec2 worldPos)
{
	D3DXVECTOR3 result;
	D3DXVECTOR3 vec3WorldPos(worldPos.x, worldPos.y, 0.0f);
	WorldToScreen(g_interfaces.pD3D9ExWrapper, g_gameVals.viewMatrix, g_gameVals.projMatrix, &vec3WorldPos, &result);

	return ImVec2(floor(result.x), floor(result.y));
}

ImVec2 HitboxOverlay::RotatePoint(ImVec2 center, float angleInRad, ImVec2 point)
{
	if (!angleInRad)
	{
		return point;
	}

	// translate point back to origin:
	point.x -= center.x;
	point.y -= center.y;

	float s = sin(angleInRad);
	float c = cos(angleInRad);

	// rotate point
	float xNew = point.x * c - point.y * s;
	float yNew = point.x * s + point.y * c;

	// translate point back:
	point.x = xNew + center.x;
	point.y = yNew + center.y;
	return point;
}

void HitboxOverlay::fixAspectRatio(ImVec2& point)
{	
	if (*this->aspectRatioAddress == 1) {
		if (displayRatio > aspectRatio) {
			float scaling = (io.DisplaySize.x / (io.DisplaySize.y * aspectRatio));
			float offset = (io.DisplaySize.x - io.DisplaySize.y * aspectRatio) / 2;

			point.x = point.x / scaling + offset;
		}
		else if (displayRatio < aspectRatio) {
			float scaling = (io.DisplaySize.y / (io.DisplaySize.x / aspectRatio));
			float offset = (io.DisplaySize.y - io.DisplaySize.x / aspectRatio) / 2;

			point.y = point.y / scaling + offset;
		}
	}
}

void HitboxOverlay::DrawOriginLine(ImVec2 worldPos, float rotationRad)
{
	const unsigned int colorOrange = 0xFFFF9900;
	const int horizontalLength = 20;
	const int verticalLength = 50;

	ImVec2 horizontalFrom = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x - horizontalLength / 2, worldPos.y));
	ImVec2 horizontalTo = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + horizontalLength / 2, worldPos.y));
	horizontalFrom = CalculateScreenPosition(horizontalFrom);
	horizontalTo = CalculateScreenPosition(horizontalTo);
	fixAspectRatio(horizontalFrom);
	fixAspectRatio(horizontalTo);
	RenderLine(horizontalFrom, horizontalTo, colorOrange, m_rectThickness);

	ImVec2 verticalFrom = worldPos;
	ImVec2 verticalTo = RotatePoint(verticalFrom, rotationRad, ImVec2(verticalFrom.x, verticalFrom.y + verticalLength));
	verticalFrom = CalculateScreenPosition(verticalFrom);
	verticalTo = CalculateScreenPosition(verticalTo);
	fixAspectRatio(verticalFrom);
	fixAspectRatio(verticalTo);
	RenderLine(verticalFrom, verticalTo, colorOrange, m_rectThickness);
}
void HitboxOverlay::DrawCollisionBoxes(ImVec2 worldPos, float rotationRad, const CharData* charObj)
{
	const unsigned int colorPurple = 0xFF8E00FF;
	int baddY = 0;
	if (charObj->BoundingAddY>0) {
		 baddY = charObj->BoundingAddY;
	}
	int baddX = 0;
	if (charObj->BoundingAddX > 0) {
		baddX = charObj->BoundingAddX;
	}
	int h_len_from_origin = floor((charObj->BoundingX / 2 + (baddX)) * m_scale);
	int v_len_from_origin = floor((charObj->BoundingY + baddY) * m_scale);
	//So far only izanami was found to need this, otherwise during float BoundingAddY adds 110000 units to her collision box height
	if (charObj->BoundingFixY > 0) {
		v_len_from_origin = floor((charObj->BoundingY) * m_scale);
	}


	ImVec2 horizontalFrom = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x - h_len_from_origin, worldPos.y));
	ImVec2 horizontalTo = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + h_len_from_origin, worldPos.y));
	ImVec2 colA = CalculateScreenPosition(horizontalFrom);
	ImVec2 colB = CalculateScreenPosition(horizontalTo);
	fixAspectRatio(colA);
	fixAspectRatio(colB);


	ImVec2 verticalFrom = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x - h_len_from_origin, worldPos.y + v_len_from_origin));
	ImVec2 verticalTo = RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + h_len_from_origin, worldPos.y + v_len_from_origin));
	ImVec2 colD = CalculateScreenPosition(verticalFrom);
	ImVec2 colC = CalculateScreenPosition(verticalTo);
	fixAspectRatio(colC);
	fixAspectRatio(colD);





	RenderRect(colA, colB, colC, colD, colorPurple, m_rectThickness);
}

void HitboxOverlay::DrawRangeCheckBoxes(ImVec2 worldPos, float rotationRad, const CharData* charObj) {

	//right now the current situation is:
	// Throw range check must reach the other player's x origin to throw
	// Vector check box must intersect with other player's collision box to throw
	// Prob those obs are incomplete, but that's the current progress
	const int h_col_len = floor((charObj->BoundingX + (charObj->BoundingAddX * 2)) * m_scale);

	const unsigned int colorGreen = 0xFF00FF00;
	const unsigned int colorYellow = 0xFFFFFF00;
	int vcX_1 = 0;
	int vcX_2 = 0;
	int vcY_1 = 0;
	int vcY_2 = 0;

	if (charObj->scaleX != 0) {
		vcX_1 = floor(floor(charObj->vectorcheckX_1 / charObj->scaleX) * m_scale);
		vcX_2 = floor(floor(charObj->vectorcheckX_2 / charObj->scaleX) * m_scale);
	}
	if (charObj->scaleY != 0) {
		vcY_1 = floor(floor(charObj->vectorcheckY_1 / charObj->scaleY) * m_scale);
		vcY_2 = floor(floor(charObj->vectorcheckY_2 / charObj->scaleY) * m_scale);
	}
	if (charObj->facingLeft)
	{
		vcX_2 = -vcX_2;
		vcX_1 = -vcX_1;
	}

	if (charObj->vectorcheckX_1 != -1 || charObj->vectorcheckX_2 != -1 || charObj->vectorcheckY_1 != -1 || charObj->vectorcheckY_2 != -1) {

		if (charObj->vectorcheckX_1 == -1) {
			vcX_1 = -40000;
		}
		if (charObj->vectorcheckX_2 == -1) {
			vcX_2 = 40000;
		}
		if (charObj->vectorcheckY_1 == -1) {
			vcY_1 = 40000;
		}
		if (charObj->vectorcheckY_2 == -1) {
			vcY_2 = -40000;
		}

		ImVec2 tstA = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + vcX_2, worldPos.y + vcY_2)));
		ImVec2 tstB = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + vcX_1, worldPos.y + vcY_2)));
		ImVec2 tstC = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + vcX_1, worldPos.y + vcY_1)));
		ImVec2 tstD = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + vcX_2, worldPos.y + vcY_1)));
		fixAspectRatio(tstA);
		fixAspectRatio(tstB);
		fixAspectRatio(tstC);
		fixAspectRatio(tstD);
		//RenderLine(tstA, tst2, colorGreen, 3);

		RenderRect(tstA, tstB, tstC, tstD, colorGreen, m_rectThickness / 2);
	}
	if (charObj->ThrowRange > 0) {
		int ThrowRange = 0;
		if (charObj->scaleX != 0) {
			ThrowRange = (charObj->ThrowRange / charObj->scaleX) * m_scale;
		}
		if (charObj->vectorcheckX_1 == -1) {
			vcY_1 = -40000;
		}
		if (charObj->vectorcheckX_2 == -1) {
			vcY_2 = 40000;
		}


		ImVec2 trA = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x - (ThrowRange + h_col_len / 2), worldPos.y + vcY_2)));
		ImVec2 trB = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + ThrowRange + h_col_len / 2, worldPos.y + vcY_2)));
		ImVec2 trC = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x + ThrowRange + h_col_len / 2, worldPos.y + vcY_1)));
		ImVec2 trD = CalculateScreenPosition(RotatePoint(worldPos, rotationRad, ImVec2(worldPos.x - (ThrowRange + h_col_len / 2), worldPos.y + vcY_1)));
		fixAspectRatio(trA);
		fixAspectRatio(trB);
		fixAspectRatio(trC);
		fixAspectRatio(trD);
		RenderRect(trA, trB, trC, trD, colorYellow, m_rectThickness / 2);



		int a = 0;
	}
}
void HitboxOverlay::DrawCollisionAreas(const CharData* charObj, const ImVec2 playerWorldPos,
	bool drawHitboxes, bool drawHurtboxes, bool drawEntityOverlays)
{
	// Rotation describes the entity, not a single box, so it is the same for every entry below.
	float entityRotationDeg = charObj->rotationDegrees / 1000.0f;
	if (!charObj->facingLeft && entityRotationDeg)
	{
		entityRotationDeg = 360.0f - entityRotationDeg;
	}
	const float entityRotationRad = D3DXToRadian(entityRotationDeg);

	std::vector<JonbEntry> entries = JonbReader::getJonbEntries(charObj);

	for (const JonbEntry& entry : entries)
	{
		// Whether a hitbox is live is decided once per entity by CanEntityHit, which reimplements
		// the engine's own test. This used to be guessed here from the state-property bitfield
		// alone, which cannot tell an idle Litchi staff from a swinging one - both read 0x200.
		if (entry.type == JonbChunkType_Hitbox && !drawHitboxes)
		{
			continue;
		}

		if (entry.type == JonbChunkType_Hurtbox && !drawHurtboxes)
		{
			continue;
		}

		float scaleX = charObj->scaleX / 1000.0f;
		float scaleY = charObj->scaleY / 1000.0f;
		float offsetX =  floor(entry.offsetX * m_scale * scaleX);
		float offsetY = -floor(entry.offsetY * m_scale * scaleY);
		float width =    floor(entry.width * m_scale * scaleX);
		float height =  -floor(entry.height * m_scale * scaleY);

		if (!charObj->facingLeft)
		{
			offsetX = -offsetX;
			width = -width;
		}

		ImVec2 pointA(playerWorldPos.x + offsetX, playerWorldPos.y + offsetY);
		ImVec2 pointB(playerWorldPos.x + offsetX + width, playerWorldPos.y + offsetY);
		ImVec2 pointC(playerWorldPos.x + offsetX + width, playerWorldPos.y + offsetY + height);
		ImVec2 pointD(playerWorldPos.x + offsetX, playerWorldPos.y + offsetY + height);

		pointA = RotatePoint(playerWorldPos, entityRotationRad, pointA);
		pointB = RotatePoint(playerWorldPos, entityRotationRad, pointB);
		pointC = RotatePoint(playerWorldPos, entityRotationRad, pointC);
		pointD = RotatePoint(playerWorldPos, entityRotationRad, pointD);
		
		pointA = CalculateScreenPosition(pointA);
		pointB = CalculateScreenPosition(pointB);
		pointC = CalculateScreenPosition(pointC);
		pointD = CalculateScreenPosition(pointD);

		//Fixes aspect ratio when the game is in fullscreen/borderless
		fixAspectRatio(pointA);
		fixAspectRatio(pointB);
		fixAspectRatio(pointC);
		fixAspectRatio(pointD);

		const unsigned int colorBlue = 0xFF0033CC;
		const unsigned int colorRed = 0xFFFF0000;
		const unsigned int rectBorderColor = entry.type == JonbChunkType_Hurtbox ? colorBlue : colorRed;
		if (this->drawHitboxHurtbox) {
			RenderRect(pointA, pointB, pointC, pointD, rectBorderColor, m_rectThickness);
		}
		const unsigned char transparency = 0xFF * m_rectFillTransparency;
		unsigned int clearedTransparencyBits = (rectBorderColor & ~0xFF000000);
		unsigned int transparencyPercentage = ((int)transparency << 24) & 0xFF000000;
		const unsigned int rectFillColor = clearedTransparencyBits | transparencyPercentage;
		if (this->drawHitboxHurtbox) {
			RenderRectFilled(pointA, pointB, pointC, pointD, rectFillColor);
		}
		
	}

	// Collision box, throw range and origin describe the entity itself, not one of its boxes. They
	// used to be drawn inside the loop above, which redrew them once per box and skipped them
	// entirely for an entity that currently has none - an idle Litchi staff being exactly that case.
	// Skipped when this entity is only lending its geometry to somebody else's attack; it gets them
	// drawn on its own pass through the entity list.
	if (!drawEntityOverlays)
	{
		return;
	}

	if (this->drawCollisionBoxes) {
		DrawCollisionBoxes(playerWorldPos, entityRotationRad, charObj);
	}

	if (this->drawRangeCheckBoxes) {
		DrawRangeCheckBoxes(playerWorldPos, entityRotationRad, charObj);
	}

	if (this->drawOriginLine)
	{
		DrawOriginLine(playerWorldPos, entityRotationRad);
	}
}

float& HitboxOverlay::GetScale()
{
	return m_scale;
}

void HitboxOverlay::DrawRectThicknessSlider()
{
	ImGui::SliderFloat("Border thickness", &m_rectThickness, 0.0f, 5.0f, "%.1f");
}

void HitboxOverlay::DrawRectFillTransparencySlider()
{
	ImGui::SliderFloat("Fill transparency", &m_rectFillTransparency, 0.0f, 1.0f, "%.2f");
}

void HitboxOverlay::RenderLine(const ImVec2& from, const ImVec2& to, uint32_t color, float thickness)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xff;
	float r = (color >> 16) & 0xff;
	float g = (color >> 8) & 0xff;
	float b = (color) & 0xff;

	window->DrawList->AddLine(from, to, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), thickness);
}

void HitboxOverlay::RenderCircle(const ImVec2& position, float radius, uint32_t color, float thickness, uint32_t segments)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xff;
	float r = (color >> 16) & 0xff;
	float g = (color >> 8) & 0xff;
	float b = (color) & 0xff;

	window->DrawList->AddCircle(position, radius, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), segments, thickness);
}

void HitboxOverlay::RenderCircleFilled(const ImVec2& position, float radius, uint32_t color, uint32_t segments)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xff;
	float r = (color >> 16) & 0xff;
	float g = (color >> 8) & 0xff;
	float b = (color) & 0xff;

	window->DrawList->AddCircleFilled(position, radius, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), segments);
}

void HitboxOverlay::RenderRect(const ImVec2& from, const ImVec2& to, uint32_t color, float rounding, uint32_t roundingCornersFlags, float thickness)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xFF;
	float r = (color >> 16) & 0xFF;
	float g = (color >> 8) & 0xFF;
	float b = (color) & 0xFF;

	window->DrawList->AddRect(from, to, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), rounding, roundingCornersFlags, thickness);
}

void HitboxOverlay::RenderRect(const ImVec2& pointA, const ImVec2& pointB, const ImVec2& pointC, const ImVec2& pointD, uint32_t color, float thickness)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xFF;
	float r = (color >> 16) & 0xFF;
	float g = (color >> 8) & 0xFF;
	float b = (color) & 0xFF;

	window->DrawList->AddQuad(pointA, pointB, pointC, pointD, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), thickness);
}

void HitboxOverlay::RenderRectFilled(const ImVec2& from, const ImVec2& to, uint32_t color, float rounding, uint32_t roundingCornersFlags)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xFF;
	float r = (color >> 16) & 0xFF;
	float g = (color >> 8) & 0xFF;
	float b = (color) & 0xFF;

	window->DrawList->AddRectFilled(from, to, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }), rounding, roundingCornersFlags);
}

void HitboxOverlay::RenderRectFilled(const ImVec2& pointA, const ImVec2& pointB, const ImVec2& pointC, const ImVec2& pointD, uint32_t color)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	float a = (color >> 24) & 0xFF;
	float r = (color >> 16) & 0xFF;
	float g = (color >> 8) & 0xFF;
	float b = (color) & 0xFF;

	window->DrawList->AddQuadFilled(pointA, pointB, pointC, pointD, ImGui::GetColorU32({ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f }));
}

bool HitboxOverlay::WorldToScreen(LPDIRECT3DDEVICE9 pDevice, D3DXMATRIX* view, D3DXMATRIX* proj, D3DXVECTOR3* pos, D3DXVECTOR3* out)
{
	D3DVIEWPORT9 viewPort;
	D3DXMATRIX world;

	pDevice->GetViewport(&viewPort);
	D3DXMatrixIdentity(&world);

	D3DXVec3Project(out, pos, &viewPort, proj, view, &world);
	if (out->z < 1) {
		return true;
	}
	return false;
}
