#include "LoadingDialog.h"
#include "../../MIDI/AbstractMIDILoader.h"

void LoadingDialog::DrawContent()
{
	const bool loading = app->IsLoading();
	const bool processingVisuals = !loading && app->IsProcessingVisuals();

	if (loading)
	{
		prog = app->GetProgress();
		if (!prog)
		{
			ImGui::Text("Waiting for load...");
		}
		else
		{
			ImGui::Text(prog->GetName().c_str());

			for (int i = 0; i < prog->GetBarCount(); i++)
			{
				double pVal = prog->GetBar(i)();
				if (pVal > 0)
					ImGui::ProgressBar(pVal, ImVec2(0.0f, 0.0f));
			}

			if (ImGui::Button("Cancel"))
			{
				prog->Stop();
				ImGui::CloseCurrentPopup();
				std::cout << "Loading canceled" << std::endl;
			}
		}
	}
	else if (processingVisuals)
	{
		// The MIDI is parsed; the selected renderer still needs its per-key store. This is part
		// of opening the file, so it belongs here instead of a second floating progress window.
		ImGui::Text("Processing Visuals");
		ImGui::ProgressBar(app->GetVisualProcessingProgress(), ImVec2(320.0f, 0.0f));
		ImGui::TextDisabled("Building the per-key data this renderer draws with.");
	}

	if (!loading && !processingVisuals)
		ImGui::CloseCurrentPopup();
}
