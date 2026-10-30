#include "AbstractOverlayRenderer.h"
#include "imgui.h"

void OverlayUtils::RightAlignedTableText(const char* text)
{
	float textWidth = ImGui::CalcTextSize(text).x;
	float avail = ImGui::GetContentRegionAvail().x;

	if (avail > textWidth)
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - textWidth);
		ImGui::TextUnformatted(text);
		return;
	}

	// The value is wider than its column (e.g. a huge multiplied note count), so wrap it to keep it
	// inside the counter instead of letting it spill off the edge of the screen.
	float wrapRight = ImGui::GetCursorPosX() + (avail > 0.0f ? avail : ImGui::GetContentRegionMax().x);
	ImGui::PushTextWrapPos(wrapRight);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
}