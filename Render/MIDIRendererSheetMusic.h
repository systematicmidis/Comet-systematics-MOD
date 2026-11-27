#pragma once

#include "Renderer/AbstractMIDIRenderer.h"

// Dedicated score renderer. It deliberately draws no piano roll, keyboard or note effects;
// ScoreOverlay fills the frame with the same four-measure page used over the other visuals.
class MIDIRendererSheetMusic : public AbstractMIDIRenderer
{
public:
	MIDIRendererSheetMusic(MIDIApp* app) : AbstractMIDIRenderer(app) {}

	void Initialize() override;
	void LoadSequence(std::shared_ptr<MIDISequence> sequence) override;
	void Render(double deltaTime) override;
	void RenderSettings() override;

	std::string GetSerializationKey() const override
	{
		return "sheetMusic";
	}
};
