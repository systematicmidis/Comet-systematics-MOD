#include "MIDIRendererSheetMusic.h"

#include "imgui.h"

void MIDIRendererSheetMusic::Initialize()
{
	AbstractMIDIRenderer::Initialize();
}

void MIDIRendererSheetMusic::LoadSequence(std::shared_ptr<MIDISequence> sequence)
{
	AbstractMIDIRenderer::LoadSequence(sequence);
}

void MIDIRendererSheetMusic::Render(double)
{
	// ScoreOverlay draws the complete white page after the render pass. There is no piano roll
	// or keyboard underneath this renderer.
}

void MIDIRendererSheetMusic::RenderSettings()
{
	ImGui::TextUnformatted("The Sheet Music renderer shows only the score. Its page fills the window; "
		"the overlay size setting applies to the other renderers.");
	AbstractMIDIRenderer::RenderSettings();
}
