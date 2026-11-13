#include "MIDIAudio.h"
#include "AudioThread.h"

#define MAESTRO_INCLUDE
#include "MaestroEngine.h"

#include "PrerenderEngine/PrerenderedEngine.h"
#ifdef _WIN32
#include "PrerenderEngine/BASSMIDI.h"
#endif
#include <iostream>

MIDIAudio::MIDIAudio()
{
	midiOut = std::make_shared<MIDIOut>();
	audioEngines[AUDIO_ENGINE_INDEX(Realtime)] = std::make_unique<AudioThread>(midiOut);
	audioEngines[AUDIO_ENGINE_INDEX(Prerendered)] = std::make_unique<PrerenderedEngine>();
	audioEngines[AUDIO_ENGINE_INDEX(Maestro)] = std::make_unique<MaestroEngine>();
}

void MIDIAudio::SwitchEngine(AudioEngineType engine)
{
	if (engine == currentEngine || engine == AudioEngineType::Count)
		return;

	AudioEngine* oldEngine = GetCurrentEngine();
	std::shared_ptr<MIDISequence> seq = oldEngine->seq;
	std::shared_ptr<MIDITimer> timer = oldEngine->timer;

	bool lastPlaying = oldEngine->IsPlaying();

	oldEngine->Destroy();
	currentEngine = engine;

	AudioEngine* newEngine = GetCurrentEngine();
	newEngine->Initialize();

	// The song may have been loaded before this engine was chosen, so it is handed the path too.
	if (!lastSourcePath.empty())
		newEngine->SetSourcePath(lastSourcePath);

	if (lastPlaying)
	{
		Start(seq, timer);
	}
}

AudioEngineList& MIDIAudio::GetEngineList()
{
	return audioEngines;
}

YAML::Node MIDIAudio::GetSettings()
{
	YAML::Node node;
	node["currentEngine"] = static_cast<int>(currentEngine);
	YAML::Node enginesNode;
	for (size_t i = 0; i < audioEngines.size(); i++)
	{
		if (audioEngines[i])
		{
			enginesNode[audioEngines[i]->GetSerializationKey()] = audioEngines[i]->GetSettings();
		}
	}
	node["engines"] = enginesNode;
	return node;
}

void MIDIAudio::LoadSettings(const YAML::Node& node)
{
	if (!node) return;
	if (node["currentEngine"])
	{
		// A config written before the GPU engines were removed can still name one of them.
		int saved = node["currentEngine"].as<int>();
		if (saved < 0 || saved >= (int)AudioEngineType::Count)
			saved = (int)AudioEngineType::Realtime;

		currentEngine = static_cast<AudioEngineType>(saved);
	}
	if (node["engines"])
	{
		const auto& enginesNode = node["engines"];
		for (size_t i = 0; i < audioEngines.size(); ++i)
		{
			if (audioEngines[i])
			{
				std::string key = audioEngines[i]->GetSerializationKey();
				if (enginesNode[key])
				{
					audioEngines[i]->LoadSettings(enginesNode[key]);
				}
			}
		}
	}
}