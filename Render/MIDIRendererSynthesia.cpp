#include "MIDIRendererSynthesia.h"
#include "Utils.h"
#include "App/Fonts.h"
#include "App/Localization/Localization.h"
#include "App/MIDIApp.h"
#include "App/UI/NavigationBar.h"
#include "MIDI/Timer/MIDITimer.h"
#include "MIDI/TempoMap.h"
#include "RenderView.h"
#include "App/Dialog/DialogMacros.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#define LOAD_VAL(n, key, target) \
        if (n && n[key]) { target = n[key].as<std::decay_t<decltype(target)>>(); }

namespace
{
	constexpr int SHEET_LETTER[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };

	// MIDI 0 is C-1, MIDI 60 is C4, MIDI 120 is C9. This is the same octave spelling
	// Synthesia uses on its keyboard labels.
	int DiatonicNumber(int midiNote)
	{
		return (midiNote / 12 - 1) * 7 + SHEET_LETTER[midiNote % 12];
	}
}

#pragma region Shaders
inline constexpr const char* rectVert = R"(#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 rectPos;
layout (location = 2) in vec2 rectSize;
layout (location = 3) in float rotation;
layout (location = 4) in vec2 uv0;
layout (location = 5) in vec2 uv1;
layout (location = 6) in int textureIndex;
layout (location = 7) in uint aMeta; 

flat out uint meta;
flat out int texID;

out vec2 uv;

uniform float aspect;

void main()
{
    meta = aMeta;
    texID = textureIndex;
    uv = mix(uv0, uv1, aPos);

    vec2 localPos = (aPos - vec2(0.5)) * rectSize;

    // Convert to aspect-correct space
    localPos.x *= aspect;

    float c = cos(rotation);
    float s = sin(rotation);
    mat2 rot = mat2(c, -s,
                    s,  c);

    localPos = rot * localPos;

    // Undo aspect correction
    localPos.x /= aspect;

    vec2 pos = rectPos + rectSize * 0.5 + localPos;

    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";

inline constexpr const char* rectFrag = R"(#version 330 core
flat in uint meta;
flat in int texID;
in vec2 uv;

out vec4 fragColor;

uniform sampler2DArray textures;

void main()
{
	bool isTexture = (meta & 0x80000000u) != 0u;
	
	vec4 rectColor = vec4(float((meta & 0xFF0000u) >> 16u) / 255.0,
		float((meta & 0xFF00u) >> 8u) / 255.0,
		float(meta & 0xFFu) / 255.0,
		float((meta & 0x7F000000u) >> 24u) / 127.0);

	if (isTexture)
		fragColor = texture(textures, vec3(uv, float(texID))) * rectColor;
	else
		fragColor = rectColor;
})";

inline constexpr const char* notesVert = R"(#version 330 core
layout (location = 0) in vec2 vPos;
layout (location = 1) in vec2 aPos;
layout (location = 2) in vec2 aSize;
layout (location = 3) in uint aMeta;

out vec2 noteSize;
out vec2 uv;
flat out uint meta;

uniform float kbHeight;
uniform float capHeight;

void main()
{
	bool isOOB   = (aMeta & (1u << 25u)) != 0u;

	uv = vPos;
	noteSize = aSize;
	if (noteSize.y < capHeight * 2.0 && !isOOB) noteSize.y = capHeight * 2.0;
	meta = aMeta;

	vec2 pos = aPos + vPos * noteSize;
	pos.y = pos.y * (1.0 - kbHeight) + kbHeight;
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";

inline constexpr const char* notesFrag = R"(#version 330 core
in vec2 noteSize;
in vec2 uv;
flat in uint meta;

uniform float capAspectRatio;
uniform float aspectRatio;
uniform float capHeight;

uniform sampler2D noteWhiteBody;
uniform sampler2D noteWhiteTop;
uniform sampler2D noteWhiteBottom;
uniform sampler2D noteBlackBody;
uniform sampler2D noteBlackTop;
uniform sampler2D noteBlackBottom;
uniform sampler2D noteOOB;

uniform bool nativeColors;

uniform float brightnessBoost;

void main()
{
	vec3 noteColor = vec3(float((meta & 0xFF0000u) >> 16u),
		float((meta & 0xFF00u) >> 8u),
		float(meta & 0xFFu)) / 255.0;
	vec3 noteColor2 = noteColor * brightnessBoost;

	bool isBlack = (meta & (1u << 24u)) != 0u;
	bool isOOB   = (meta & (1u << 25u)) != 0u;
	bool isRight = (meta & (1u << 26u)) != 0u;

	if (isBlack && !nativeColors)
	{
		noteColor *= 0.7;
		noteColor2 *= 0.7;
	}

	vec4 noteTexColor;

	if (!isOOB)
	{
		if (isBlack)
			noteTexColor = texture(noteBlackBody, uv);
		else
			noteTexColor = texture(noteWhiteBody, uv);

		float bottomDistance = uv.y * noteSize.y;

		if (bottomDistance < capHeight)
		{
			vec2 capUV;
			capUV.x = uv.x;
			capUV.y = 1.0 - bottomDistance / capHeight;

			if (isBlack)
				noteTexColor = texture(noteBlackBottom, capUV);
			else
				noteTexColor = texture(noteWhiteBottom, capUV);
		}

		float topDistance = (1.0 - uv.y) * noteSize.y;

		if (topDistance < capHeight)
		{
			vec2 capUV;
			capUV.x = uv.x;
			capUV.y = topDistance / capHeight;

			if (isBlack)
				noteTexColor = texture(noteBlackTop, capUV);
			else
				noteTexColor = texture(noteWhiteTop, capUV);
		}

		gl_FragColor = vec4(noteTexColor.rgb * noteColor2, noteTexColor.a);
	}
	else
	{
		vec2 arrowUV = isRight ? vec2(1.0 - uv.x, uv.y) : uv;
		noteTexColor = texture(noteOOB, arrowUV);

		gl_FragColor = vec4(noteTexColor.rgb * noteColor, noteTexColor.a);
	}

	
})";
#pragma endregion

#pragma region Particles

float KeyHazeParticle::maxLife = 0.2;

KeyHazeParticle::KeyHazeParticle(uint8_t key, std::mt19937& random)
{
	std::uniform_real_distribution<double> dist(0.0, 1.0);

	this->key = key;
	life = maxLife;
	pos = { dist(random), dist(random) };
}

void KeyHazeParticle::Step(double delta)
{
	life -= delta;
	brightness = 1.0 - std::abs((life * 2 - maxLife) / maxLife);
}

float KeySparkParticle::maxLife = 0.2;
float KeySparkParticle::minSize = 0.7;

KeySparkParticle::KeySparkParticle(uint8_t key, std::mt19937& random)
{
	std::uniform_real_distribution<double> dist(0.0, 1.0);

	this->key = key;
	life = maxLife;
	pos = { dist(random), dist(random) };
	rotation = dist(random) * 2.0 - 1.0;
	flipped = dist(random) < 0.5;
}

void KeySparkParticle::Step(double delta)
{
	life -= delta;

	brightness = 1.0 - std::abs((life * 2 - maxLife) / maxLife);
	size = brightness * (1.0 - minSize) + minSize;
}

KeyDebrisParticle::KeyDebrisParticle(uint8_t key, std::mt19937& random)
{
	std::uniform_real_distribution<double> dist(0.0, 1.0);

	this->key = key;

	pos = { 0, 0 };
	vel = { (dist(random) - 0.5) * 0.01, dist(random) * 0.003 };
	life = 0.3;
	rotation = dist(random) * 2.0 - 1.0;
	size = dist(random) * 0.5 + 0.2;
}

void KeyDebrisParticle::Step(double delta)
{
	life -= delta;

	pos += glm::vec2(vel.x * delta * 60.0, vel.y * delta * 60.0);
	if (pos.y < 0) life = 0;
	vel.y -= 0.001 * delta * 60;
}

#pragma endregion

inline std::vector<uint32_t> nativeSynthesiaColors = {
	// blue
	0x80A3CB,
	0x2A5EA7,
	// green
	0x9DE04D,
	0x4C9301,
	// orange
	0xF8A829,
	0xEF7100,
	// yellow
	0xFAE944,
	0xDFCA24,
	// purple
	0xC48ACB,
	0x7E6583,
	// red
	0xE76F6D,
	0xE13C34,
};

inline uint32_t SampleNativeColor(uint16_t track, uint8_t channel, bool isBlack)
{
	size_t index = (((size_t)track << 4u) | (size_t)channel) << 1;
	index = index % nativeSynthesiaColors.size();
	return nativeSynthesiaColors[index | isBlack];
}

void MIDIRendererSynthesia::Initialize()
{
	AbstractMIDIRenderer::Initialize();

	std::array<float, 8> quadVertices{
			0.0f, 1.0f,
			1.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f
	};

	std::array<int, 6> quadIndices{
		0, 1, 3,
		1, 2, 3
	};

#pragma region Rect shader initialization
	rectProgram = ShaderProgram::Create(rectVert, rectFrag);
	rectVAO = std::make_unique<VertexArray>();
	rectVBO = std::make_unique<Buffer>(GL_ARRAY_BUFFER);
	rectIBO = std::make_unique<Buffer>(GL_ARRAY_BUFFER);
	rectEBO = std::make_unique<Buffer>(GL_ELEMENT_ARRAY_BUFFER);

	{
		VertexArrayBind vaoBind(*rectVAO);

		rectVBO->Bind();
		rectVBO->SetData(quadVertices, GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, false, 2 * sizeof(float), (void*)0);

		rectEBO->Bind();
		rectEBO->SetData(quadIndices, GL_STATIC_DRAW);

		rectIBO->Bind();
		rectIBO->SetData(renderRects, GL_DYNAMIC_DRAW);

		rectVAO->SetFloatAttribute(1, 2, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, position));
		rectVAO->SetFloatAttribute(2, 2, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, size));
		rectVAO->SetFloatAttribute(3, 2, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, rotation));
		rectVAO->SetFloatAttribute(4, 2, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, uv0));
		rectVAO->SetFloatAttribute(5, 2, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, uv1));
		rectVAO->SetIntAttribute(6, 1, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, textureIndex));
		rectVAO->SetIntAttribute(7, 1, sizeof(TexturedRectInstance), offsetof(TexturedRectInstance, meta));

		glVertexAttribDivisor(1, 1);
		glVertexAttribDivisor(2, 1);
		glVertexAttribDivisor(3, 1);
		glVertexAttribDivisor(4, 1);
		glVertexAttribDivisor(5, 1);
		glVertexAttribDivisor(6, 1);
		glVertexAttribDivisor(7, 1);
	}

	// The sampler uniform only needs to be set once - it just points at a
	// texture unit, and we always bind the array to that same unit.
	{
		ShaderBind shaderBind(*rectProgram);
		rectProgram->SetInt("textures", TEXTURE_ARRAY_SLOT);
	}
#pragma endregion

#pragma region Note shader initialization
	notesProgram = ShaderProgram::Create(notesVert, notesFrag);
	notesVAO = std::make_unique<VertexArray>();
	notesVBO = std::make_unique<Buffer>(GL_ARRAY_BUFFER);
	notesIBO = std::make_unique<Buffer>(GL_ARRAY_BUFFER);
	notesEBO = std::make_unique<Buffer>(GL_ELEMENT_ARRAY_BUFFER);

	{
		VertexArrayBind vaoBind(*notesVAO);

		notesVBO->Bind();
		notesVBO->SetData(quadVertices, GL_STATIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, false, 2 * sizeof(float), (void*)0);

		notesEBO->Bind();
		notesEBO->SetData(quadIndices, GL_STATIC_DRAW);

		notesIBO->Bind();
		notesIBO->SetData(renderNotes, GL_DYNAMIC_DRAW);

		notesVAO->SetFloatAttribute(1, 2, sizeof(RenderNoteSynthesia), offsetof(RenderNoteSynthesia, x));
		notesVAO->SetFloatAttribute(2, 2, sizeof(RenderNoteSynthesia), offsetof(RenderNoteSynthesia, width));
		notesVAO->SetIntAttribute(3, 1, sizeof(RenderNoteSynthesia), offsetof(RenderNoteSynthesia, meta));

		glVertexAttribDivisor(1, 1);
		glVertexAttribDivisor(2, 1);
		glVertexAttribDivisor(3, 1);
	}

	{
		ShaderBind shaderBind(*notesProgram);
		notesProgram->SetInt("nativeColors", renderSettings.useNativeNoteColors ? 1 : 0);
	}
#pragma endregion

#pragma region Keyboard data initialization
	int b = 0;
	int w = 0;
	for (int i = 0; i < MIDI_KEYS; i++)
	{
		if (IS_BLACK(i)) keyNum[i] = b++;
		else keyNum[i] = w++;
	}

	std::vector<uint8_t> blackIDs;
	blackIDs.reserve(53);
	std::vector<uint8_t> whiteIDs;
	whiteIDs.reserve(75);

	uint8_t keyID = 0;
	for (int key = 0; key < MIDI_KEYS; key++)
	{
		bool black = IS_BLACK(key);
		if (black) blackIDs.push_back(key);
		else whiteIDs.push_back(key);
	}

	int i = 0;
	for (auto& white : whiteIDs)
	{
		kbIDs[i++] = white;
	}
	for (auto& black : blackIDs)
	{
		kbIDs[i++] = black;
	}

	// InitializeTextures();
	UpdateStyle();
	InitializeParticleSystems();

	initialized = true;
#pragma endregion
}

void MIDIRendererSynthesia::CalculateKeyboardData()
{
	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = config->render.GetKeyFirst();
	int keyLast = config->render.GetKeyLast() + 1;

	if (renderSettings.style == SYNTHESIA_10 ||
		renderSettings.style == SYNTHESIA_9 ||
		renderSettings.style == GLOW)
	{
		initialKeyboardHeight = 0.158;
		initialBarHeight = 0.0045;
	}

	if (renderSettings.style == SYNTHESIA_8)
	{
		initialKeyboardHeight = 0.16;
		initialBarHeight = 0.007;
	}

	keyboardHeight = initialKeyboardHeight / (keyLast - keyFirst) * 128.0f;
	keyboardHeight = keyboardHeight / (1920.0 / 1080.0) * ((float)this->width / (float)this->height);
	if (keyboardHeight > 0.2468) keyboardHeight = 0.2468;
}

void MIDIRendererSynthesia::LoadKeyboardTextures(SynthesiaStyle style)
{
	const std::filesystem::path keyboardPath = "./assets/textures/synthesia/keyboards";
	const std::filesystem::path particlePath = "./assets/textures/synthesia/particles";

	size_t numTextures = 0;
	switch (style)
	{
		case SYNTHESIA_10:
		case SYNTHESIA_9:
		{
			numTextures = S9_LAYER_COUNT;
			break;
		}
		case SYNTHESIA_8:
		{
			numTextures = S8_LAYER_COUNT;
			break;
		}
		case GLOW:
		{
			numTextures = GLOW_LAYER_COUNT;
			break;
		}
	}

	numTextures += PARTICLE_LAYER_COUNT;

	// layer -> file path, in the same order as the TextureLayer enum.
	std::vector<std::filesystem::path> layerPaths;
	switch (style)
	{
		case SYNTHESIA_10:
		case SYNTHESIA_9:
		{
			layerPaths = {
				keyboardPath / "s9/bar.png",
				keyboardPath / "s9/blackKeys.png",
				keyboardPath / "s9/blackKeysPressed.png",
				keyboardPath / "s9/whiteKeys.png",
				keyboardPath / "s9/whiteKeysPressed.png",
				keyboardPath / "s9/whiteKeyWhole.png",
				keyboardPath / "s9/whiteKeyWholePressed.png",
				keyboardPath / "s9/shadowLarge.png",
				keyboardPath / "s9/shadowUnpressed.png",
				keyboardPath / "s9/shadowPressed.png",
				particlePath / "keyDebris.png",
				particlePath / "keySpark.png",
				particlePath / "keyHaze.png"
			};
			break;
		}
		case SYNTHESIA_8:
		{
			layerPaths = {
				keyboardPath / "s8/bar.png",
				keyboardPath / "s7/blackKey.png",
				keyboardPath / "s7/blackKeyPressed.png",
				keyboardPath / "s8/whiteKeys.png",
				keyboardPath / "s8/whiteKeysPressed.png",
				keyboardPath / "s8/whiteKeyWhole.png",
				keyboardPath / "s8/whiteKeyWholePressed.png",
				particlePath / "keyDebris.png",
				particlePath / "keySpark.png",
				particlePath / "keyHaze.png"
			};
			break;
		}
		case GLOW:
		{
			layerPaths = {
				keyboardPath / "glow/bar.png",
				keyboardPath / "glow/blackKeys.png",
				keyboardPath / "glow/blackKeysPressed.png",
				keyboardPath / "glow/whiteKeys.png",
				keyboardPath / "glow/whiteKeysPressed.png",
				keyboardPath / "glow/whiteKeyWhole.png",
				keyboardPath / "glow/whiteKeyWholePressed.png",
				particlePath / "keyDebris.png",
				particlePath / "keySpark.png",
				particlePath / "keyHaze.png"
			};
			break;
		}
	}

	// GL_TEXTURE_2D_ARRAY requires every layer to share one width/height, so
	// probe every source image first and size the array to fit the largest.
	int maxWidth = 1;
	int maxHeight = 1;
	for (const auto& path : layerPaths)
	{
		int w, h;
		if (TextureArray::ProbeDimensions(Utils::TryGetStream(path), w, h))
		{
			maxWidth = std::max(maxWidth, w);
			maxHeight = std::max(maxHeight, h);
		}
	}

	textures = std::make_unique<TextureArray>(maxWidth, maxHeight, numTextures);

	for (int layer = 0; layer < numTextures; layer++)
	{
		layerUV[layer] = textures->LoadLayer(layer, Utils::TryGetStream(layerPaths[layer]));
	}
}

void MIDIRendererSynthesia::InitializeParticleSystems()
{
	fullParticlesArray.clear();

	for (size_t i = 0; i < MIDI_KEYS; i++)
	{
		keyHazeParticles[i].clear();
		fullParticlesArray.push_back(&keyHazeParticles[i]);
		keySparkParticles[i].clear();
		fullParticlesArray.push_back(&keySparkParticles[i]);
	}

	fullParticlesArray.push_back(&keyDebrisParticles);
}

void MIDIRendererSynthesia::GenerateKeyLayoutArrays()
{
	if (!keyArrayDirty) return;

	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = config->render.GetKeyFirst();
	int keyLast = config->render.GetKeyLast() + 1;

	std::array<float, 5> additionalOffsets = { 0.0, 0.0, 0.0, 0.0, 0.0 };
	float blackKeyScale = 0.6f;

	if (renderSettings.style == SYNTHESIA_9 || renderSettings.style == SYNTHESIA_10 || renderSettings.style == GLOW)
	{
		additionalOffsets = { 0.02, -0.035, 0.0, -0.02, -0.03 };
		blackKeyScale = 0.63f;
	}
	
	const float blackKey2setOffset = 0.3f;
	const float blackKey3setOffset = 0.5f;

	float wdth;

	std::array<float, MIDI_KEYS> leftArray{};
	std::array<float, MIDI_KEYS> widthArray{};

	for (int i = 0; i < MIDI_KEYS; i++)
	{
		if (!IS_BLACK(i))
		{
			leftArray[i] = keyNum[i];
			widthArray[i] = 1.0f;
			continue;
		}

		int _i = std::min(i + 1, (int)MIDI_KEYS - 1);
		wdth = blackKeyScale;
		int bknum = keyNum[i] % 5;
		float offset = wdth * 0.5f;
		if (bknum == 0) offset += wdth * 0.5f * blackKey2setOffset;
		if (bknum == 2) offset += wdth * 0.5f * blackKey3setOffset;
		if (bknum == 1) offset -= wdth * 0.5f * blackKey2setOffset;
		if (bknum == 4) offset -= wdth * 0.5f * blackKey3setOffset;

		offset -= additionalOffsets[bknum] * wdth * 0.5f;

		leftArray[i] = keyNum[_i] - offset;
		widthArray[i] = wdth;
	}

	float knmfn = leftArray[keyFirst];
	float knmln = leftArray[keyLast - 1] + widthArray[keyLast - 1];
	float width = knmln - knmfn;

	for (int i = 0; i < MIDI_KEYS; i++)
	{
		keyPos[i] = (leftArray[i] - knmfn) / width;
		keyWidth[i] = widthArray[i] / width;
	}

	blackKeyWidth = blackKeyScale / width;
	whiteKeyWidth = 1.0f / width;
	keyArrayDirty = false;
}

void MIDIRendererSynthesia::LoadSequence(std::shared_ptr<MIDISequence> sequence)
{
	if (seq != sequence) AbstractMIDIRenderer::UnloadSequence();
	AbstractMIDIRenderer::LoadSequence(sequence);

	colors.LoadColors();
	seq = sequence;
	
	lastTime = 0;

	for (auto& id : startBlockIDs)
		id = 0;
}

void MIDIRendererSynthesia::UpdateRenderer()
{
	MIDIPlayerConfig* config = app->GetConfig();
	if (config->render.ConsumeKeyRangeChanged())
	{
		CalculateKeyboardData();
		keyArrayDirty = true;
	}

	GenerateKeyLayoutArrays();
}

void MIDIRendererSynthesia::UpdateStyle()
{
	GenerateKeyLayoutArrays();
	CalculateKeyboardData();

	std::filesystem::path notesPath;
	std::filesystem::path noteWhiteBodyPath;
	std::filesystem::path noteWhiteTopPath;
	std::filesystem::path noteWhiteBottomPath;
	std::filesystem::path noteBlackBodyPath;
	std::filesystem::path noteBlackTopPath;
	std::filesystem::path noteBlackBottomPath;

	float brightnessBoost = 1.0f;

	switch (renderSettings.style)
	{
		case SYNTHESIA_10:
		{
			notesPath = "./assets/textures/synthesia/notes/s10";
			noteWhiteBodyPath = notesPath / "note.png";
			noteWhiteTopPath = notesPath / "noteTop.png";
			noteWhiteBottomPath = notesPath / "noteBottom.png";
			noteBlackBodyPath = notesPath / "note.png";
			noteBlackTopPath = notesPath / "noteTop.png";
			noteBlackBottomPath = notesPath / "noteBottom.png";

			break;
		}
		case SYNTHESIA_9:
		case SYNTHESIA_8:
		{
			notesPath = "./assets/textures/synthesia/notes/s9";
			noteWhiteBodyPath = notesPath / "noteWhite.png";
			noteWhiteTopPath = notesPath / "noteTopWhite.png";
			noteWhiteBottomPath = notesPath / "noteBottomWhite.png";
			noteBlackBodyPath = notesPath / "noteBlack.png";
			noteBlackTopPath = notesPath / "noteTopBlack.png";
			noteBlackBottomPath = notesPath / "noteBottomBlack.png";

			brightnessBoost = 2.0f;
			break;
		}
		case GLOW:
		{
			notesPath = "./assets/textures/synthesia/notes/glow";
			noteWhiteBodyPath = notesPath / "note.png";
			noteWhiteTopPath = notesPath / "noteTop.png";
			noteWhiteBottomPath = notesPath / "noteBottom.png";
			noteBlackBodyPath = notesPath / "note.png";
			noteBlackTopPath = notesPath / "noteTop.png";
			noteBlackBottomPath = notesPath / "noteBottom.png";

			break;
		}
	}

	noteWhiteBody = std::make_unique<GPUImage>(Utils::TryGetStream(noteWhiteBodyPath));
	noteWhiteTop = std::make_unique<GPUImage>(Utils::TryGetStream(noteWhiteTopPath));
	noteWhiteBottom = std::make_unique<GPUImage>(Utils::TryGetStream(noteWhiteBottomPath));
	noteBlackBody = std::make_unique<GPUImage>(Utils::TryGetStream(noteBlackBodyPath));
	noteBlackTop = std::make_unique<GPUImage>(Utils::TryGetStream(noteBlackTopPath));
	noteBlackBottom = std::make_unique<GPUImage>(Utils::TryGetStream(noteBlackBottomPath));
	noteOOB = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/notes/noteOOB.png"));

	// Sheet-music symbols are pre-rasterized from the Segoe UI Symbol clef glyphs. Keeping
	// them as textures lets the notation use crisp shapes without depending on a music font
	// being installed on the user's machine.
	sheetTreble = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/treble.png"));
	sheetBass = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/bass.png"));
	sheetSharp = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/sharp.png"));
	sheetFlat = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/flat.png"));

	{
		ShaderBind bind(*notesProgram);
		notesProgram->SetFloat("brightnessBoost", brightnessBoost);
	}

	LoadKeyboardTextures(renderSettings.style);

	{
		ShaderBind bind(*notesProgram);

		{
			TextureBind tex(*noteWhiteBody, 0);
			notesProgram->SetInt("noteWhiteBody", 0);
		}

		{
			TextureBind tex(*noteWhiteTop, 1);
			notesProgram->SetInt("noteWhiteTop", 1);
		}

		{
			TextureBind tex(*noteWhiteBottom, 2);
			notesProgram->SetInt("noteWhiteBottom", 2);
		}

		{
			TextureBind tex(*noteBlackBody, 3);
			notesProgram->SetInt("noteBlackBody", 3);
		}

		{
			TextureBind tex(*noteBlackTop, 4);
			notesProgram->SetInt("noteBlackTop", 4);
		}

		{
			TextureBind tex(*noteBlackBottom, 5);
			notesProgram->SetInt("noteBlackBottom", 5);
		}

		{
			TextureBind tex(*noteOOB, 6);
			notesProgram->SetInt("noteOOB", 6);
		}
	}
}

void MIDIRendererSynthesia::Render(double deltaTime)
{
	if (!initialized) return;
	rectDrawCount = 0;

	UpdateRenderer();

	sceneFramebuffer->Bind();
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// if background drawing
	if (renderSettings.renderBackground)
	{
		RenderBackground();
		RenderLines();

		RenderQuads(rectDrawCount);
		rectDrawCount = 0;
	}

	RenderNotes();
	if (renderSettings.showOutOfBoundNotes) RenderOutOfBoundNotes();

	if (renderSettings.renderKeySparkle)
	{
		GenerateParticles(deltaTime);
		RenderParticles();
		UpdateParticles(deltaTime);

		RenderQuads(rectDrawCount);
		rectDrawCount = 0;
	}

	RenderKeyboard();
	RenderQuads(rectDrawCount);
	if (renderSettings.showSheetMusic) RenderSheetMusic();
	if (renderSettings.showKeyOctaves) RenderOctaveTextOverlays();

	glDisable(GL_BLEND);

	sceneFramebuffer->Unbind();

	ResetKeyboardState();
}

void MIDIRendererSynthesia::RenderBackground()
{
	// The real Synthesia room is not a flat fill: it has a soft light falloff toward the
	// keyboard and a darker ceiling. A small number of horizontal bands gives that depth
	// without another shader or a full-screen texture.
	constexpr int bands = 48;
	for (int i = 0; i < bands; ++i)
	{
		const float t = (float)i / (float)(bands - 1);
		const float shade = 0.095f + t * 0.055f;
		const int c = (int)std::lround(shade * 255.0f);
		PushQuad(0.0f, (float)i / bands, 1.0f, 1.0f / bands, 0xFF000000u | ((uint32_t)c << 16) | ((uint32_t)c << 8) | (uint32_t)c);
	}

	// A little vignette on the sides frames the keyboard the way Synthesia's stage does.
	constexpr int vignetteBands = 16;
	for (int i = 0; i < vignetteBands; ++i)
	{
		const float t = (float)i / (float)vignetteBands;
		const uint32_t alpha = (uint32_t)std::lround((1.0f - t) * 22.0f) << 24;
		const float inset = t * 0.12f;
		PushQuad(0.0f, 0.0f, 0.12f - inset, 1.0f, alpha);
		PushQuad(0.88f + inset, 0.0f, 0.12f - inset, 1.0f, alpha);
	}
}

void MIDIRendererSynthesia::RenderLines()
{
	RenderMeasureLines();
	RenderDividerLines();
}

void MIDIRendererSynthesia::RenderMeasureLines()
{
	const float lineWidth = 0.02f;
	const float lineHeight = lineWidth * ((float)this->width / (float)this->height);

	if (!seq) return;
	std::vector<TimeSignatureEvent>& timeSigEvents = seq->timeSignatures;
	if (timeSigEvents.empty()) return;

	auto* renderView = app->GetRenderView();
	if (!renderView) return;

	TempoMap* tempoMap = seq->GetTempoMap();
	if (!tempoMap) return;

	const double playbackSeconds = app->GetTimer()->Elapsed();
	const long currentTicks = tempoMap->SecsToTicksFromMap(seq->resolution, playbackSeconds);

	const double currentTime = isTimeBased ? playbackSeconds : (double)currentTicks;
	const double viewRegion = isTimeBased
		? (double)renderView->viewTicks / 1000.0
		: (double)renderView->viewTicks;

	const double invViewRegion = 1.0 / viewRegion;
	const double renderEnd = currentTime + viewRegion;

	long measureTime = 0;

	auto measurePos = [&](long ticks) -> double
		{
			return isTimeBased
				? tempoMap->TicksToSecsFromMap(seq->resolution, ticks)
				: (double)ticks;
		};

	for (size_t i = 0; i < timeSigEvents.size(); i++)
	{
		TimeSignatureEvent* timeSignature = &timeSigEvents[i];
		TimeSignatureEvent* futureTimeSignature =
			(i + 1 < timeSigEvents.size()) ? &timeSigEvents[i + 1] : nullptr;

		const long measureInc = static_cast<long>(seq->resolution * 4) / timeSignature->denominator;
		int beat = 0;

		long futureTimeSigTick = 0l;
		if (futureTimeSignature)
			futureTimeSigTick = isTimeBased
			? tempoMap->SecsToTicksFromMap(seq->resolution, (double)futureTimeSignature->tick / TIME_BASED_MULTIPLIER)
			: futureTimeSignature->tick;

		while (futureTimeSignature ? (measureTime < futureTimeSigTick) : (measurePos(measureTime) < renderEnd))
		{
			const double pos = measurePos(measureTime);

			const float linePos = (float)((pos - currentTime) * invViewRegion) * (1.0 - keyboardHeight) + keyboardHeight;
			const float top = linePos + whiteKeyWidth * lineHeight;
			const float bottom = linePos - whiteKeyWidth * lineHeight;

			if (beat == 0 && pos > currentTime && pos < renderEnd)
			{
				PushQuad(0, bottom, 1, top - bottom, 0xFF323232);
			}

			measureTime += measureInc;
			beat++;
			if (beat >= timeSignature->numerator) beat = 0;
		}
		if (futureTimeSignature) measureTime = futureTimeSigTick;
	}
}

void MIDIRendererSynthesia::RenderDividerLines()
{
	MIDIPlayerConfig* config = app->GetConfig();
	const int keyFirst = config->render.GetKeyFirst();
	const int keyLast = config->render.GetKeyLast() + 1;

	const float lineWidth = 0.02f;

	// divider lines
	for (int i = keyFirst; i <= keyLast; i++)
	{
		if (i >= MIDI_KEYS) continue;

		float line = keyPos[i];
		float left = line - whiteKeyWidth * lineWidth;
		float right = line + whiteKeyWidth * lineWidth;

		if (i % 12 == 0)
		{
			PushQuad(left, 0.0f, right - left, 1.0f, 0xFF505050);
		}
		if (i % 12 == 5)
		{
			PushQuad(left, 0.0f, right - left, 1.0f, 0xFF323232);
		}
	}
}

void MIDIRendererSynthesia::RenderKeyboard()
{
	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = config->render.GetKeyFirst();
	int keyLast = config->render.GetKeyLast();

	if (IS_BLACK(keyFirst)) keyFirst--;
	if (IS_BLACK(keyLast)) keyLast++;

	float keyTop = keyboardHeight * (1.0f - initialBarHeight);

#pragma region Synthesia 9/10 Keyboard
	if (renderSettings.style == SYNTHESIA_9 || renderSettings.style == SYNTHESIA_10)
	{
		const float frac = 1.0f / 7.0f;
		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (i >= MIDI_KEYS) continue;

			if (IS_BLACK(i)) continue;
			float creamR = 1.0f;
			float creamG = 0.98f;
			float creamB = 0.9f;

			uint32_t creamColor = Utils::PackRGBA(creamR, creamG, creamB, 1.0f, Utils::ARGB);

			size_t part = keyNum[i] % 7;

			float split = 0.5f;
			if (part == 3) split = 0.3f;
			if (part == 6) split = 0.7f;

			float uvLeft = part * frac;
			float uvRight = (part + 1) * frac;
			float uvMiddle = (part + split) * frac;

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];
			float middle = left + (right - left) * split;

			uint32_t color = 0x7F000000 | keyStates[i].color;

			if (i == keyLast)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, S9_LAYER_KEY_WHITE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, S9_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, S9_LAYER_KEY_WHITE, creamColor);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, S9_LAYER_KEY_WHITE_WHOLE, creamColor);
				}
			}
			else if (i == keyFirst)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, S9_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, S9_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, S9_LAYER_KEY_WHITE_WHOLE, creamColor);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, S9_LAYER_KEY_WHITE, creamColor);
				}
			}
			else
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, S9_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, S9_LAYER_KEY_WHITE, creamColor);
				}
			}

#pragma region Shadows
			float aspectRatio = (float)this->width / (float)this->height;
			if (i != 0 && i != keyFirst)
			{
				const auto& RenderLarge = [this, left, keyTop, aspectRatio]()
					{
						float shadowRight = left + keyTop * textures->GetLayerAspectRatio(S9_LAYER_SHADOW_LARGE) / aspectRatio;
						PushQuad(left, 0, shadowRight - left, keyTop, 0, 0, 1, 1, S9_LAYER_SHADOW_LARGE);
					};

				const auto& RenderUnpressed = [this, left, keyTop, aspectRatio, i]()
					{
						float keyRight = keyPos[i - 1] + keyWidth[i - 1];
						float shadowRight = keyRight + keyTop * textures->GetLayerAspectRatio(S9_LAYER_SHADOW_UNPRESSED) / aspectRatio;
						PushQuad(keyRight, 0, shadowRight - keyRight, keyTop, 0, 0, 1, 1, S9_LAYER_SHADOW_UNPRESSED);
					};

				const auto& RenderPressed = [this, left, keyTop, aspectRatio, i]()
					{
						float keyRight = keyPos[i - 1] + keyWidth[i - 1];
						float shadowRight = keyRight + keyTop * textures->GetLayerAspectRatio(S9_LAYER_SHADOW_PRESSED) / aspectRatio;
						PushQuad(keyRight, 0, shadowRight - keyRight, keyTop, 0, 0, 1, 1, S9_LAYER_SHADOW_PRESSED);
					};

				if (keyStates[i].pressed)
				{
					if (IS_BLACK(i - 1))
					{
						if (!keyStates[i - 2].pressed) RenderLarge();
						if (keyStates[i - 1].pressed) RenderPressed();
						else RenderUnpressed();
					}
					else
					{
						if (!keyStates[i - 1].pressed) RenderLarge();
					}
				}
				else
				{
					if (IS_BLACK(i - 1))
					{
						if (!keyStates[i - 1].pressed) RenderPressed();
					}
				}
			}

#pragma endregion
		}
		// split in two to allow colored bar
		const float barFrac = 3.0f / 11.0f;
		const float barSplitHeight = barFrac * initialBarHeight;

		ImVec4 barColor = config->render.GetBarColor();
		uint32_t barColorPacked = Utils::PackRGBA(barColor.x, barColor.y, barColor.z, 1.0, Utils::ARGB);
		PushQuad(0, keyTop, 1, barSplitHeight, 0, 0, 1, barFrac, S9_LAYER_BAR, barColorPacked); // colored bar ooOOO.. default is 0xFFA02020
		PushQuad(0, keyTop + barSplitHeight, 1, initialBarHeight - barSplitHeight, 0, barFrac, 1, 1.0f - barFrac, S9_LAYER_BAR);

		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (!IS_BLACK(i)) continue;

			size_t part = keyNum[i] % 5;

			float bKeyBottom = keyboardHeight / 2.9f;
			float bKeyTopPressed = keyTop + keyboardHeight * 0.006;
			float bKeyTopUnpressed = keyTop + keyboardHeight * 0.015;

			float uvLeft = part * (1.0f / 5.0f);
			float uvRight = (part + 1) * (1.0f / 5.0f);

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];

			if (keyStates[i].pressed)
				PushQuad(left, bKeyBottom, right - left, bKeyTopPressed - bKeyBottom, uvLeft, 0, uvRight, 1, S9_LAYER_KEY_BLACK_PRESSED, 0x7F000000 | keyStates[i].color);
			else
				PushQuad(left, bKeyBottom, right - left, bKeyTopUnpressed - bKeyBottom, uvLeft, 0, uvRight, 1, S9_LAYER_KEY_BLACK);
		}
	}
	
#pragma endregion

#pragma region Synthesia 8 Keyboard
	if (renderSettings.style == SYNTHESIA_8)
	{
		const float frac = 1.0f / 7.0f;
		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (i >= MIDI_KEYS) continue;

			if (IS_BLACK(i)) continue;
			float creamR = 1.0f;
			float creamG = 0.98f;
			float creamB = 0.9f;

			uint32_t creamColor = Utils::PackRGBA(creamR, creamG, creamB, 1.0f, Utils::ARGB);

			size_t part = keyNum[i] % 7;

			float split = 0.5f;
			if (part == 3) split = 0.3f;
			if (part == 6) split = 0.7f;

			float uvLeft = part * frac;
			float uvRight = (part + 1) * frac;
			float uvMiddle = (part + split) * frac;

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];
			float middle = left + (right - left) * split;

			uint32_t color = 0x7F000000 | keyStates[i].color;

			if (i == keyLast)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, S8_LAYER_KEY_WHITE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, S8_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, S8_LAYER_KEY_WHITE, 0x7FFFFFFF);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, S8_LAYER_KEY_WHITE_WHOLE, 0x7FFFFFFF);
				}
			}
			else if (i == keyFirst)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, S8_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, S8_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, S8_LAYER_KEY_WHITE_WHOLE, 0x7FFFFFFF);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, S8_LAYER_KEY_WHITE, 0x7FFFFFFF);
				}
			}
			else
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, S8_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, S8_LAYER_KEY_WHITE, 0x7FFFFFFF);
				}
			}
		}

		const float barFrac = 1.0f / 3.0f;
		const float barSplitHeight = barFrac * initialBarHeight;

		ImVec4 barColor = config->render.GetBarColor();
		uint32_t barColorPacked = Utils::PackRGBA(barColor.x, barColor.y, barColor.z, 1.0, Utils::ARGB);
		PushQuad(0, keyTop, 1, barSplitHeight, 0, 0, 1, barFrac, S8_LAYER_BAR, barColorPacked);
		PushQuad(0, keyTop + barSplitHeight, 1, initialBarHeight - barSplitHeight, 0, barFrac, 1, 1.0f - barFrac, S8_LAYER_BAR);

		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (!IS_BLACK(i)) continue;

			float bKeyBottom = keyboardHeight / 2.65f;
			float bKeyTop = keyTop + keyboardHeight * 0.023;

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];

			if (keyStates[i].pressed)
				PushQuad(left, bKeyBottom, right - left, bKeyTop - bKeyBottom, 0, 0, 1, 1, S8_LAYER_KEY_BLACK_PRESSED, 0x7F000000 | keyStates[i].color);
			else
				PushQuad(left, bKeyBottom, right - left, bKeyTop - bKeyBottom, 0, 0, 1, 1, S8_LAYER_KEY_BLACK);
		}
	}
#pragma endregion

#pragma region Glow keyboard
	if (renderSettings.style == GLOW)
	{
		const float frac = 1.0f / 7.0f;
		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (i >= MIDI_KEYS) continue;

			if (IS_BLACK(i)) continue;
			float creamR = 1.0f;
			float creamG = 0.98f;
			float creamB = 0.9f;

			uint32_t creamColor = Utils::PackRGBA(creamR, creamG, creamB, 1.0f, Utils::ARGB);

			size_t part = keyNum[i] % 7;

			float split = 0.5f;
			if (part == 3) split = 0.3f;
			if (part == 6) split = 0.7f;

			float uvLeft = part * frac;
			float uvRight = (part + 1) * frac;
			float uvMiddle = (part + split) * frac;

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];
			float middle = left + (right - left) * split;

			uint32_t color = 0x7F000000 | keyStates[i].color;

			if (i == keyLast)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, GLOW_LAYER_KEY_WHITE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, GLOW_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, uvLeft, 0, uvMiddle, 1, GLOW_LAYER_KEY_WHITE, 0x7FFFFFFF);
					PushQuad(middle, 0, right - middle, keyTop, split, 0, 1, 1, GLOW_LAYER_KEY_WHITE_WHOLE, 0x7FFFFFFF);
				}
			}
			else if (i == keyFirst)
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, GLOW_LAYER_KEY_WHITE_WHOLE_PRESSED, color);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, GLOW_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, middle - left, keyTop, 0, 0, split, 1, GLOW_LAYER_KEY_WHITE_WHOLE, 0x7FFFFFFF);
					PushQuad(middle, 0, right - middle, keyTop, uvMiddle, 0, uvRight, 1, GLOW_LAYER_KEY_WHITE, 0x7FFFFFFF);
				}
			}
			else
			{
				if (keyStates[i].pressed)
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, GLOW_LAYER_KEY_WHITE_PRESSED, color);
				}
				else
				{
					PushQuad(left, 0, right - left, keyTop, uvLeft, 0, uvRight, 1, GLOW_LAYER_KEY_WHITE, 0x7FFFFFFF);
				}
			}
		}

		PushQuad(0, keyTop, 1, initialBarHeight, 0, 0, 1, 1, GLOW_LAYER_BAR);
		for (int i = keyFirst; i <= keyLast; i++)
		{
			if (!IS_BLACK(i)) continue;

			size_t part = keyNum[i] % 5;

			float bKeyBottom = keyboardHeight / 2.9f;
			float bKeyTopPressed = keyTop + keyboardHeight * 0.006;
			float bKeyTopUnpressed = keyTop + keyboardHeight * 0.015;

			float uvLeft = part * (1.0f / 5.0f);
			float uvRight = (part + 1) * (1.0f / 5.0f);

			float left = keyPos[i];
			float right = keyPos[i] + keyWidth[i];

			if (keyStates[i].pressed)
				PushQuad(left, bKeyBottom, right - left, bKeyTopPressed - bKeyBottom, uvLeft, 0, uvRight, 1, GLOW_LAYER_KEY_BLACK_PRESSED, 0x7F000000 | keyStates[i].color);
			else
				PushQuad(left, bKeyBottom, right - left, bKeyTopUnpressed - bKeyBottom, uvLeft, 0, uvRight, 1, GLOW_LAYER_KEY_BLACK);
		}
	}
#pragma endregion
}

void MIDIRendererSynthesia::RenderQuads(size_t count)
{
	if (count == 0) return;

	ShaderBind shaderBind(*rectProgram);
	VertexArrayBind vaoBind(*rectVAO);
	BufferBind iboBind(*rectIBO);
	TextureArrayBind texBind(*textures, TEXTURE_ARRAY_SLOT);

	rectProgram->SetFloat("aspect", (float)width / (float)height);

	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(TexturedRectInstance) * count, renderRects.data());
	glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, count);
}

void MIDIRendererSynthesia::RenderNotes()
{
	if (!seq) return;
	std::vector<NoteSequence>& notes = seq->mergedNotes;
	if (notes.empty()) return;
	auto* renderView = app->GetRenderView();
	if (!renderView) return;

	size_t notesToRender = 0;
	double playbackSeconds = app->GetTimer()->Elapsed();

	TempoMap* tempoMap = seq->GetTempoMap();
	long time = tempoMap->SecsToTicksFromMap(seq->resolution, playbackSeconds);
	double bpm = tempoMap->GetBPMAtTick(time);
	noteCounterInfo->tick.value = time >= 0 ? time : 0;
	noteCounterInfo->timeSeconds.value = playbackSeconds;
	noteCounterInfo->bpm.value = bpm;

	ShaderBind shaderBind(*notesProgram);

	TextureBind noteWhiteBodyBind(*noteWhiteBody, 0);
	TextureBind noteWhiteTopBind(*noteWhiteTop, 1);
	TextureBind noteWhiteBottomBind(*noteWhiteBottom, 2);
	TextureBind noteBlackBodyBind(*noteBlackBody, 3);
	TextureBind noteBlackTopBind(*noteBlackTop, 4);
	TextureBind noteBlackBottomBind(*noteBlackBottom, 5);

	VertexArrayBind notesVAOBind(*notesVAO);
	BufferBind notesVBOBind(*notesVBO);
	BufferBind notesEBOBind(*notesEBO);
	if (notesIBO) notesIBO->Bind();

	float renderAspectRatio = (float)width / (float)height;
	float capAspectRatio = (float)noteWhiteTop->width / (float)noteWhiteTop->height;
	
	notesProgram->SetFloat("kbHeight", keyboardHeight);
	notesProgram->SetFloat("aspectRatio", renderAspectRatio);
	notesProgram->SetFloat("capHeight", whiteKeyWidth / capAspectRatio * renderAspectRatio);
	notesProgram->SetFloat("capAspectRatio", capAspectRatio);

	size_t noteID = 0;
	size_t notesPassed = 0;
	size_t polyphony = 0;
	size_t batchCount = 0;

	const double accTime = isTimeBased ? playbackSeconds : time;
	const double viewRegion = isTimeBased ? (double)renderView->viewTicks / 1000 : (double)renderView->viewTicks;
	const double invViewRegion = 1.0 / viewRegion;
	const double invTimeMultiplier = 1.0 / (double)TIME_BASED_MULTIPLIER;

	double targetTick = accTime; if (isTimeBased) targetTick *= TIME_BASED_MULTIPLIER;

	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = config->render.GetKeyFirst();
	int keyLast = config->render.GetKeyLast();

	for (uint8_t id : kbIDs)
	{
		uint16_t lastTrack = 0;
		uint8_t lastChannel = 0;
		uint32_t lastTick = 0;
		uint32_t lastGate = 0;
		uint32_t lastNote = 0;

		NoteSequence& notesNote = notes[id];
		const std::vector<NoteBlock>& blocks = seq->noteBlocks[id];
		if (blocks.empty()) continue;

#pragma region Note block culling

		size_t& blockIndex = startBlockIDs[id];
		size_t noteBegin = blockIndex * NOTE_BLOCK_SIZE;

		if (lastTime < time)
		{
			while (blockIndex < blocks.size() && blocks[blockIndex].maxBound <= targetTick)
			{
				++blockIndex;
			}
		}
		else if (lastTime > time)
		{
			while (blockIndex > 0 && blocks[blockIndex - 1].maxBound > targetTick)
			{
				--blockIndex;
			}
		}

		noteBegin = std::min(notesNote.Size(), blockIndex * NOTE_BLOCK_SIZE);
		notesPassed += noteBegin;

#pragma endregion

		bool isBlack = blackArr[id];
		size_t numNotes = notesNote.Size();

		size_t i = noteBegin;

		while (i < numNotes)
		{
			const size_t blockIndex = i / NOTE_BLOCK_SIZE;
			const size_t blockEnd = std::min(numNotes, (blockIndex + 1) * NOTE_BLOCK_SIZE);

			const NoteBlock& currBlock = blocks[blockIndex];
			double blockMin = currBlock.minBound;
			double blockMax = currBlock.maxBound;

			if (isTimeBased)
			{
				blockMin *= invTimeMultiplier;
				blockMax *= invTimeMultiplier;
			}

			if (blockMax <= accTime)
			{
				notesPassed += blockEnd - i;
				i = blockEnd;
				continue;
			}

			if (blockMin > accTime + viewRegion) break;

			bool beyondView = false;

			for (; i < blockEnd; ++i)
			{
				uint32_t nTick = notesNote.tick[i];
				uint32_t nGate = notesNote.gate[i];
				uint8_t nNote = notesNote.GetKey(i);
				uint16_t nTrack = notesNote.GetTrack(i);
				uint8_t nChannel = notesNote.GetChannel(i);

				double noteStart = (double)nTick;
				double noteEnd = (double)(nTick + nGate);

				if (isTimeBased)
				{
					noteStart *= invTimeMultiplier;
					noteEnd *= invTimeMultiplier;
				}

				if (noteStart > accTime + viewRegion)
				{
					beyondView = true;
					break;
				}

				if (noteEnd <= accTime)
				{
					notesPassed++;
					continue;
				}
				if (noteStart <= accTime)
				{
					keyStates[nNote].pressed = true;
					keyStates[nNote].color = renderSettings.useNativeNoteColors ?
						SampleNativeColor(nTrack, nChannel, isBlack) :
						colors.GetColor(nTrack, nChannel);
					notesPassed++;
					polyphony++;
				}

				if (id < keyFirst || id > keyLast)
				{
					beyondView = true;
					break;
				}

				if (lastNote &&
					nTick == lastTick &&
					nNote == lastNote &&
					nChannel == lastChannel &&
					nTrack == lastTrack &&
					nGate == lastGate)
				{
					continue;
				}

				lastTick = nTick;
				lastNote = nNote;
				lastChannel = nChannel;
				lastTrack = nTrack;
				lastGate = nGate;

				float x = keyPos[id];
				float width = keyWidth[id];
				float y = (float)((noteStart - accTime) * invViewRegion);
				float y2 = (float)((noteEnd - accTime) * invViewRegion);

				renderNotes[noteID++] = RenderNoteSynthesia(
					x, y,
					width, y2 - y,
					((renderSettings.useNativeNoteColors ?
						SampleNativeColor(nTrack, nChannel, isBlack) :
						colors.GetColor(nTrack, nChannel)) & 0xFFFFFF) | (isBlack << 24)
				);

				if (noteID >= NOTE_BUFFER_SIZE)
				{
					FlushNotes(NOTE_BUFFER_SIZE);
					batchCount++;
					if (batchCount >= NOTES_MAX_BATCHES)
					{
						glFlush();
						batchCount = 0;
					}
					noteID = 0;
				}
			}

			if (beyondView) break;
		}
	}

	if (noteID != 0)
	{
		FlushNotes(noteID);
		batchCount++;
		if (batchCount >= NOTES_MAX_BATCHES)
		{
			glFlush();
			batchCount = 0;
		}
	}
	
	noteCounterInfo->notesPassed = static_cast<uint64_t>(notesPassed);
	noteCounterInfo->polyphony = static_cast<uint64_t>(polyphony);

	if (!noteCounterInfo->npsHistory.empty() && playbackSeconds < noteCounterInfo->npsHistory.back().timeSeconds)
	{
		noteCounterInfo->npsHistory.clear();
	}

	noteCounterInfo->npsHistory.push_back({ playbackSeconds, static_cast<uint64_t>(notesPassed) });
	while (!noteCounterInfo->npsHistory.empty() &&
		(playbackSeconds - noteCounterInfo->npsHistory.front().timeSeconds) > 1.0)
	{
		noteCounterInfo->npsHistory.pop_front();
	}

	if (!noteCounterInfo->npsHistory.empty())
	{
		uint64_t notesOneSecondAgo = noteCounterInfo->npsHistory.front().totalNotes;
		uint64_t currentNotes = static_cast<uint64_t>(notesPassed);

		if (currentNotes >= notesOneSecondAgo)
		{
			noteCounterInfo->notesPerSecond.value = currentNotes - notesOneSecondAgo;
		}
		else
		{
			noteCounterInfo->notesPerSecond.value = 0;
		}
	}
	else
	{
		noteCounterInfo->notesPerSecond = 0;
	}

	lastTime = time;
}

void MIDIRendererSynthesia::RenderOutOfBoundNotes()
{
	if (!seq) return;
	std::vector<NoteSequence>& notes = seq->mergedNotes;
	if (notes.empty()) return;
	auto* renderView = app->GetRenderView();
	if (!renderView) return;

	double playbackSeconds = app->GetTimer()->Elapsed();
	TempoMap* tempoMap = seq->GetTempoMap();
	long time = tempoMap->SecsToTicksFromMap(seq->resolution, playbackSeconds);

	const double accTime = isTimeBased ? playbackSeconds : time;
	const double viewRegion = isTimeBased ? (double)renderView->viewTicks / 1000 : (double)renderView->viewTicks;
	const double invViewRegion = 1.0 / viewRegion;
	const double invTimeMultiplier = 1.0 / (double)TIME_BASED_MULTIPLIER;

	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = config->render.GetKeyFirst();
	int keyLast = config->render.GetKeyLast();

	size_t nID = 0;
	size_t batchCount = 0;

	ShaderBind shaderBind(*notesProgram);
	TextureBind tex(*noteOOB, 6);

	VertexArrayBind notesVAOBind(*notesVAO);
	BufferBind notesVBOBind(*notesVBO);
	BufferBind notesEBOBind(*notesEBO);
	if (notesIBO) notesIBO->Bind();

	for (uint8_t id : kbIDs)
	{
		if (id >= keyFirst && id <= keyLast) continue;

		uint16_t lastTrack = 0;
		uint8_t lastChannel = 0;
		uint32_t lastTick = 0;
		uint32_t lastGate = 0;
		uint32_t lastNote = 0;

		NoteSequence& notesNote = notes[id];

		size_t noteBegin = startBlockIDs[id] * NOTE_BLOCK_SIZE;

		for (size_t i = noteBegin; i < notesNote.Size(); ++i)
		{
			uint32_t nTick = notesNote.tick[i];
			uint32_t nGate = notesNote.gate[i];
			uint8_t nNote = notesNote.GetKey(i);
			uint16_t nTrack = notesNote.GetTrack(i);
			uint8_t nChannel = notesNote.GetChannel(i);

			double noteStart = isTimeBased ? (double)nTick * invTimeMultiplier : (double)nTick;
			
			if (noteStart < accTime) continue;
			if (noteStart > accTime + viewRegion) break;

			if (lastNote &&
				nTick == lastTick &&
				nNote == lastNote &&
				nChannel == lastChannel &&
				nTrack == lastTrack &&
				nGate == lastGate)
			{
				continue;
			}

			lastTick = nTick;
			lastNote = nNote;
			lastChannel = nChannel;
			lastTrack = nTrack;
			lastGate = nGate;

			float arrowX = 0.0f;

			bool isLeft = nNote < keyFirst;
			if (isLeft) arrowX = 0.015;
			else arrowX = 0.985;

			float arrowY = (float)((noteStart - accTime) * invViewRegion);

			float arrowWidth = 0.023;
			float arrowHeight = arrowWidth * ((float)width / (float)height);

			arrowX -= arrowWidth * 0.5f;
			arrowY -= arrowHeight * 0.5f;

			renderNotes[nID++] = {
				arrowX, arrowY,
				arrowWidth, arrowHeight,
				((renderSettings.useNativeNoteColors ?
				SampleNativeColor(nTrack, nChannel, false) :
					colors.GetColor(nTrack, nChannel)) & 0xFFFFFF) | (1 << 25) | ((!isLeft) << 26)
			};

			if (nID >= NOTE_BUFFER_SIZE)
			{
				FlushNotes(NOTE_BUFFER_SIZE);
				batchCount++;
				if (batchCount >= NOTES_MAX_BATCHES)
				{
					glFlush();
					batchCount = 0;
				}
				nID = 0;
			}
		}
	}

	if (nID != 0)
	{
		FlushNotes(nID);
		batchCount++;
		if (batchCount >= NOTES_MAX_BATCHES)
		{
			glFlush();
		}
	}
}

void MIDIRendererSynthesia::FlushNotes(size_t count)
{
	if (count == 0) return;
	
	if (!notesIBO) return;

	GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
	void* ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, count * sizeof(RenderNoteSynthesia), mapFlags);
	if (ptr)
	{
		memcpy(ptr, renderNotes.data(), count * sizeof(RenderNoteSynthesia));
		glUnmapBuffer(GL_ARRAY_BUFFER);
	}

	glDrawElementsInstanced(
		GL_TRIANGLES,
		6,
		GL_UNSIGNED_INT,
		nullptr,
		count
	);
}

void MIDIRendererSynthesia::GenerateParticles(double deltaTime)
{
	std::uniform_real_distribution<double> gen(0.0, 1.0);

	double framesElapsed = deltaTime * 60.0;
	double hazeChance = 1.0 - std::pow(1.0 - 0.5, framesElapsed);
	double sparkChance = 1.0 - std::pow(1.0 - 0.5, framesElapsed);
	double debrisChance = 1.0 - std::pow(1.0 - 0.02, framesElapsed);

	for (size_t i = 0; i < MIDI_KEYS; i++)
	{
		if (!keyStates[i].pressed)
		{
			if (keyHazeParticles[i].size() > 0) keyHazeParticles[i].clear();
			if (keySparkParticles[i].size() > 0) keySparkParticles[i].clear();
			continue;
		}

		if (gen(random) < hazeChance && keyHazeParticles[i].size() < 20) keyHazeParticles[i].push_back(std::make_unique<KeyHazeParticle>(i, random));
		if (gen(random) < sparkChance && keySparkParticles[i].size() < 20) keySparkParticles[i].push_back(std::make_unique<KeySparkParticle>(i, random));
		if (gen(random) < debrisChance && keyDebrisParticles.size() < 200) keyDebrisParticles.push_back(std::make_unique<KeyDebrisParticle>(i, random));
	}
}

void MIDIRendererSynthesia::RenderParticles()
{
	int particleTexOffset = 0;
	if (renderSettings.style == SYNTHESIA_9 || renderSettings.style == SYNTHESIA_10)
		particleTexOffset = S9_LAYER_COUNT;
	if (renderSettings.style == SYNTHESIA_8)
		particleTexOffset = S8_LAYER_COUNT;
	if (renderSettings.style == GLOW)
		particleTexOffset = GLOW_LAYER_COUNT;

	float aspect = (float)width / (float)height;
	for (auto& particle : keyDebrisParticles)
	{
		KeyDebrisParticle* p = dynamic_cast<KeyDebrisParticle*>(particle.get());
		float left = keyPos[p->key];
		float right = keyPos[p->key] + keyWidth[p->key];
		glm::vec2 pos = { (left + right) / 2.0, keyboardHeight };

		glm::vec2 offset = p->pos;
		offset.y *= aspect;
		offset *= keyboardHeight / 0.15;
		pos += offset;
		float sizeX = whiteKeyWidth * p->size;
		float sizeY = sizeX * aspect;

		PushRotatedQuad(pos.x - sizeX * 0.5f, pos.y - sizeY * 0.5f, sizeX, sizeY, p->rotation, 0, 0, 1, 1, particleTexOffset+LAYER_PARTICLE_DEBRIS);
	}

	for (auto& particleArr : keySparkParticles)
	{
		for (auto& particle : particleArr)
		{
			KeySparkParticle* p = dynamic_cast<KeySparkParticle*>(particle.get());

			float left = keyPos[p->key];
			float right = keyPos[p->key] + keyWidth[p->key];
			float width = right - left;
			left += width * 0.4f;
			right -= width * 0.4f;
			glm::vec2 pos = { left + (right - left) * p->pos.x, keyboardHeight + (right - left) * p->pos.y * aspect };

			uint32_t blendCol = keyStates[p->key].color;
			blendCol = Utils::BlendRGBA(blendCol, Utils::PackRGBA(1.0, 1.0, 1.0, 1.0), 0.8);

			float ang = p->rotation / 5.0f;
			float sizeX = width * p->size * 2.8f;
			float sizeY = sizeX * aspect;

			if (p->flipped)
			{
				PushRotatedQuad(pos.x - sizeX * 0.5, pos.y - sizeY * 0.5, sizeX, sizeY, ang, 0, 0, 1, 1, particleTexOffset+LAYER_PARTICLE_SPARKLE, blendCol);
			}
			else
			{
				PushRotatedQuad(pos.x - sizeX * 0.5, pos.y - sizeY * 0.5, sizeX, sizeY, ang, 1, 0, 0, 1, particleTexOffset+LAYER_PARTICLE_SPARKLE, blendCol);
			}
		}
	}

	for (auto& particleArr : keyHazeParticles)
	{
		for (auto& particle : particleArr)
		{
			KeyHazeParticle* p = dynamic_cast<KeyHazeParticle*>(particle.get());

			float left = keyPos[p->key];
			float right = keyPos[p->key] + keyWidth[p->key];
			float width = right - left;
			left += width * 0.4f;
			right -= width * 0.4f;
			glm::vec2 pos = { left + (right - left) * p->pos.x, keyboardHeight + (right - left) * p->pos.y * aspect };
			pos.y += keyboardHeight * 0.02f;

			float sizeX = width * 1.8f;
			float sizeY = sizeX * aspect;

			uint32_t alpha = (uint32_t)(std::clamp(p->brightness, 0.0f, 1.0f) * 0.2f * 127.0f) << 24;
			PushQuad(pos.x - sizeX, pos.y - sizeY, sizeX * 2.0f, sizeY * 2.0f, 0, 0, 1, 1, particleTexOffset+LAYER_PARTICLE_HAZE, alpha | 0xFFFFFF);
		}
	}
}

void MIDIRendererSynthesia::UpdateParticles(double deltaTime)
{
	for (auto& particleArray : fullParticlesArray)
	{
		for (size_t i = 0; i < particleArray->size();)
        {
            Particle& p = *(*particleArray)[i];

            p.Step(deltaTime);

            if (p.life <= 0.0f)
            {
				if (i != particleArray->size() - 1)
				{
					std::swap((*particleArray)[i], particleArray->back());
				}
                particleArray->pop_back();
                continue;
            }

            ++i;
        }
	}
}

void MIDIRendererSynthesia::RenderOctaveTextOverlays()
{
	ImDrawList* drawList = ImGui::GetBackgroundDrawList();
	ImFont* font = Fonts::DefaultFont != nullptr ? Fonts::DefaultFont : ImGui::GetFont();
	if (font == nullptr) return;

	MIDIPlayerConfig* config = app->GetConfig();
	int keyFirst = std::clamp(config->render.GetKeyFirst(), 0, MIDI_KEYS - 1);
	int keyLast = std::clamp(config->render.GetKeyLast() + 1, 0, MIDI_KEYS);

	for (int k = keyFirst; k < keyLast; ++k)
	{
		if (k % 12 != 0) continue;

		const int octave = (k / 12) - 1;
		const float left = keyPos[k];
		const float right = keyPos[k] + keyWidth[k];
		const float keyPixels = (right - left) * width;
		if (keyPixels < 4.0f) continue;

		const std::string txt = "C" + std::to_string(octave);
		float fontSize = std::clamp(keyPixels * 1.15f, 7.0f, height * 0.035f);
		float textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, txt.c_str()).x;
		// C labels are seven white keys apart, so they have room to be wider than one key. The
		// old one-key limit made the 256-key range's labels unreadably small.
		if (textWidth > keyPixels * 5.0f && textWidth > 0.0f)
			fontSize *= keyPixels * 5.0f / textWidth;

		textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, txt.c_str()).x;
		const float x = std::clamp((left + right) * 0.5f * width - textWidth * 0.5f, 0.0f, width - textWidth);
		const float y = height - fontSize - std::max(2.0f, keyPixels * 0.22f);

		const ImU32 col = k == 5 * 12 ? IM_COL32(0, 0, 0, 163) : IM_COL32(0, 0, 0, 92);
		drawList->AddText(font, fontSize, ImVec2(x, y), col, txt.c_str());
	}
}

void MIDIRendererSynthesia::DrawSheetImage(std::unique_ptr<GPUImage>& image, float x, float y, float imageHeight, uint32_t tint)
{
	if (!image || !image->IsValidTexture() || image->height <= 0) return;

	const float imageWidth = imageHeight * (float)image->width / (float)image->height;
	// GPUImage uploads the first PNG row as the OpenGL bottom row. Flip the V axis for ImGui's
	// top-left coordinate system so the clef isn't upside down.
	ImGui::GetBackgroundDrawList()->AddImage(
		(ImTextureID)(intptr_t)image->GetTextureID(),
		ImVec2(x, y), ImVec2(x + imageWidth, y + imageHeight),
		ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), (ImU32)tint);
}

void MIDIRendererSynthesia::RenderSheetMusic()
{
	if (!seq || !renderSettings.showSheetMusic || isTimeBased) return;
	if (seq->mergedNotes.size() < MIDI_KEYS) return;

	TempoMap* tempoMap = seq->GetTempoMap();
	if (!tempoMap) return;

	const double elapsed = std::max(0.0, app->GetTimer()->Elapsed());
	const long currentTick = tempoMap->SecsToTicksFromMap(seq->resolution, elapsed);

	// The first time signature is enough for the scrolling notation. A change in the middle of
	// a song is rare in this renderer's target files and is still represented by bar lines.
	int numerator = 4;
	int denominator = 4;
	if (!seq->timeSignatures.empty())
	{
		const TimeSignatureEvent& signature = seq->timeSignatures.front();
		if (signature.numerator > 0) numerator = signature.numerator;
		if (signature.denominator > 0) denominator = signature.denominator;
	}

	const long measureTicks = std::max(1L, (long)seq->resolution * 4L * numerator / denominator);
	const long windowTicks = measureTicks * 4L;
	const long windowStart = std::max(0L, currentTick) / measureTicks * measureTicks;
	const long windowEnd = windowStart + windowTicks;

	const float uiWidth = ImGui::GetIO().DisplaySize.x;
	const float uiHeight = ImGui::GetIO().DisplaySize.y;
	if (uiWidth <= 0.0f || uiHeight <= 0.0f) return;

	ImDrawList* draw = ImGui::GetBackgroundDrawList();

	// The navigation bar is drawn above the renderer. In the live preview, start the paper below
	// it so the treble staff and title are not hidden behind the chrome. Video renders have no
	// navigation bar and keep the reference's edge-to-edge layout.
	float panelTop = 0.0f;
	if (!app->IsRendering())
	{
		if (NavigationBar* navigationBar = app->GetNavigationBar())
			panelTop = std::max(0.0f, navigationBar->GetPosition().y + navigationBar->GetResolution().y);
	}

	// Reference measurements: the sheet occupies roughly the top quarter of the frame, with
	// two five-line staves and a comfortable margin for ledger lines.
	const float availableHeight = std::max(120.0f, uiHeight - panelTop);
	const float panelHeight = std::clamp(availableHeight * 0.30f, 170.0f, 430.0f);
	const float panelBottom = panelTop + panelHeight;
	const float space = std::clamp(std::min(panelHeight * 0.028f, uiWidth * 0.0062f), 4.0f, 15.0f);
	const float paperLeft = 0.0f;
	const float staffLeft = std::max(8.0f, uiWidth * 0.025f);
	const float noteLeft = std::max(70.0f, uiWidth * 0.095f);
	const float staffRight = uiWidth - std::max(10.0f, uiWidth * 0.025f);

	const float trebleTop = panelTop + panelHeight * 0.145f;
	const float trebleBottom = trebleTop + space * 4.0f;
	const float bassTop = trebleBottom + space * 5.2f;
	const float bassBottom = bassTop + space * 4.0f;

	const ImU32 paper = IM_COL32(247, 246, 241, 255);
	const ImU32 paperShadow = IM_COL32(8, 8, 8, 110);
	const ImU32 ink = IM_COL32(28, 28, 28, 255);
	const ImU32 staffInk = IM_COL32(77, 77, 77, 255);
	const ImU32 measureHighlight = IM_COL32(117, 185, 230, 75);

	draw->AddRectFilled(ImVec2(paperLeft, panelBottom), ImVec2(uiWidth, panelBottom + space * 1.8f), paperShadow);
	draw->AddRectFilled(ImVec2(paperLeft, panelTop), ImVec2(uiWidth, panelBottom), paper);
	draw->AddLine(ImVec2(paperLeft, panelBottom), ImVec2(uiWidth, panelBottom), IM_COL32(65, 65, 65, 255), 1.0f);

	draw->PushClipRect(ImVec2(0.0f, panelTop), ImVec2(uiWidth, panelBottom), true);

	const float contentWidth = std::max(1.0f, staffRight - noteLeft);
	auto measureX = [&](int measure)
		{
			return noteLeft + contentWidth * (float)measure / 4.0f;
		};

	// The current measure is tinted blue in the reference. It is always the leftmost measure
	// because the notation scrolls one measure at a time.
	draw->AddRectFilled(ImVec2(staffLeft, trebleTop - space * 1.7f),
		ImVec2(measureX(1), bassBottom + space * 1.7f), measureHighlight);

	auto staffLines = [&](float top, float bottom)
		{
			for (int line = 0; line < 5; ++line)
			{
				const float y = top + (bottom - top) * (float)line / 4.0f;
				draw->AddLine(ImVec2(staffLeft, y), ImVec2(staffRight, y), staffInk, std::max(1.0f, space * 0.075f));
			}
		};

	staffLines(trebleTop, trebleBottom);
	staffLines(bassTop, bassBottom);

	for (int measure = 0; measure <= 4; ++measure)
	{
		const float x = measure == 0 ? staffLeft : measureX(measure);
		draw->AddLine(ImVec2(x, trebleTop), ImVec2(x, trebleBottom), staffInk, std::max(1.0f, space * 0.11f));
		draw->AddLine(ImVec2(x, bassTop), ImVec2(x, bassBottom), staffInk, std::max(1.0f, space * 0.11f));
	}

	// Connect the two staves at the outer edges, like a grand staff bracket.
	draw->AddLine(ImVec2(staffLeft, trebleTop), ImVec2(staffLeft, bassBottom), staffInk, std::max(1.0f, space * 0.13f));
	draw->AddLine(ImVec2(staffRight, trebleTop), ImVec2(staffRight, bassBottom), staffInk, std::max(1.0f, space * 0.13f));

	const float clefHeight = space * 7.2f;
	DrawSheetImage(sheetTreble, staffLeft + space * 0.35f, trebleTop - space * 2.7f, clefHeight, 0xFFFFFFFFu);
	DrawSheetImage(sheetBass, staffLeft + space * 0.45f, bassTop - space * 1.7f, clefHeight * 0.9f, 0xFFFFFFFFu);

	// Time signature on the top staff. The clef and signature sit before the first note.
	const std::string numeratorText = std::to_string(numerator);
	const std::string denominatorText = std::to_string(denominator);
	ImFont* font = Fonts::DefaultFont != nullptr ? Fonts::DefaultFont : ImGui::GetFont();
	if (font != nullptr)
	{
		const float signatureSize = space * 2.2f;
		const float signatureX = staffLeft + space * 4.3f;
		draw->AddText(font, signatureSize, ImVec2(signatureX, trebleTop - space * 0.05f), ink, numeratorText.c_str());
		draw->AddText(font, signatureSize, ImVec2(signatureX, trebleTop + space * 1.95f), ink, denominatorText.c_str());
	}

	const int trebleBase = 4 * 7 + 2; // E4, bottom line of the treble staff.
	const int bassBase = 2 * 7 + 4;   // G2, bottom line of the bass staff.
	const long quarterTicks = std::max(1L, (long)seq->resolution);

	size_t renderedNotes = 0;
	constexpr size_t MAX_SHEET_NOTES = 2400;

	for (int key = 0; key < 128 && renderedNotes < MAX_SHEET_NOTES; ++key)
	{
		const NoteSequence& notes = seq->mergedNotes[key];
		if (notes.Empty()) continue;

		const bool treble = key >= 60;
		const int base = treble ? trebleBase : bassBase;
		const float staffTop = treble ? trebleTop : bassTop;
		const float staffBottom = treble ? trebleBottom : bassBottom;
		const int diatonicStep = DiatonicNumber(key) - base;
		const float noteY = staffBottom - (float)diatonicStep * space * 0.5f;

		auto begin = std::lower_bound(notes.tick.begin(), notes.tick.end(), (uint32_t)windowStart);
		size_t index = (size_t)std::distance(notes.tick.begin(), begin);
		uint32_t previousTick = UINT32_MAX;
		uint32_t previousGate = UINT32_MAX;

		for (; index < notes.Size() && renderedNotes < MAX_SHEET_NOTES; ++index)
		{
			const uint32_t tick = notes.tick[index];
			if (tick >= (uint32_t)windowEnd) break;
			if (tick == previousTick && notes.gate[index] == previousGate) continue;
			previousTick = tick;
			previousGate = notes.gate[index];

			const float rawX = (float)((double)((long)tick - windowStart) / (double)windowTicks);
			const float x = noteLeft + rawX * contentWidth;
			if (x < staffLeft || x > staffRight) continue;

			const uint32_t gate = std::max<uint32_t>(1, notes.gate[index]);
			const bool openHead = gate >= quarterTicks * 2;
			const bool wholeHead = gate >= quarterTicks * 4;
			const float headRX = space * 0.92f;
			const float headRY = space * 0.62f;
			const ImVec2 center(x, noteY);

			// Ledger lines only belong on line positions (even diatonic steps), never in the
			// space immediately next to the staff.
			if (diatonicStep <= -2)
			{
				for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep - 1; step < 0; step += 2)
				{
					const float ledgerY = staffBottom - (float)step * space * 0.5f;
					draw->AddLine(ImVec2(x - headRX * 1.8f, ledgerY), ImVec2(x + headRX * 1.8f, ledgerY), staffInk, std::max(1.0f, space * 0.085f));
				}
			}
			else if (diatonicStep >= 10)
			{
				for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep + 1; step > 8; step -= 2)
				{
					const float ledgerY = staffBottom - (float)step * space * 0.5f;
					draw->AddLine(ImVec2(x - headRX * 1.8f, ledgerY), ImVec2(x + headRX * 1.8f, ledgerY), staffInk, std::max(1.0f, space * 0.085f));
				}
			}

			// Stems are on the right when they point up and on the left when they point down.
			const bool stemUp = noteY > (staffTop + staffBottom) * 0.5f;
			const float stemX = x + (stemUp ? headRX : -headRX);
			const float stemTip = noteY + (stemUp ? -space * 3.3f : space * 3.3f);

			if (!wholeHead)
			{
				draw->AddLine(ImVec2(stemX, noteY), ImVec2(stemX, stemTip), ink, std::max(1.0f, space * 0.11f));
				if (gate < quarterTicks)
				{
					// One flag is enough to read an eighth note; real beaming would require grouping
					// adjacent voices, which is not meaningful for the dense files this renderer sees.
					const float direction = stemUp ? 1.0f : -1.0f;
					draw->AddLine(ImVec2(stemX, stemTip), ImVec2(stemX + space * 0.8f, stemTip + direction * space * 0.7f), ink, std::max(1.0f, space * 0.09f));
				}
			}

			if (openHead)
				 draw->AddEllipse(center, ImVec2(headRX, headRY), ink, 0.0f, 16, std::max(1.0f, space * 0.16f));
			else
				draw->AddEllipseFilled(center, ImVec2(headRX, headRY), ink, 0.0f, 16);

			if (IsSharp(key))
			{
				const float accidentalHeight = space * 2.15f;
				if (sheetSharp && sheetSharp->IsValidTexture())
				{
					const float accidentalWidth = accidentalHeight * (float)sheetSharp->width / (float)sheetSharp->height;
					DrawSheetImage(sheetSharp, x - headRX - accidentalWidth - space * 0.25f, noteY - accidentalHeight * 0.55f, accidentalHeight, 0xFFFFFFFFu);
				}
			}

			++renderedNotes;
		}
	}

	draw->PopClipRect();
}

void MIDIRendererSynthesia::RenderSettings()
{
	MIDIPlayerConfig* config = app->GetConfig();

	if (ImGui::BeginTabBar("##renderSettings"))
	{
		if (ImGui::BeginTabItem("General"))
		{
			BEGIN_SECTION("##sGeneral")
			{
				SETUP_SECTION;

				SECTION_ENTRY(SECTION_LABEL("Synthesia style"),
					{
						auto style = renderSettings.style;
						std::string previewText = "";

						switch (style)
						{
							case SYNTHESIA_8:
								previewText = "Synthesia 8";
								break;
							case SYNTHESIA_9:
								previewText = "Synthesia 9";
								break;
							case SYNTHESIA_10:
								previewText = "Synthesia 10";
								break;
							case GLOW:
								previewText = "Glow";
								break;
						}

						if (ImGui::BeginCombo("##synthesiaStyle", previewText.c_str()))
						{
							if (ImGui::Selectable("Synthesia 8", style == SYNTHESIA_8))
							{
								renderSettings.style = SYNTHESIA_8;
								UpdateStyle();
							}
							if (ImGui::Selectable("Synthesia 9", style == SYNTHESIA_9))
							{
								renderSettings.style = SYNTHESIA_9;
								UpdateStyle();
							}

							if (ImGui::Selectable("Synthesia 10", style == SYNTHESIA_10))
							{
								renderSettings.style = SYNTHESIA_10;
								UpdateStyle();
							}

							if (ImGui::Selectable("Glow", style == GLOW))
							{
								renderSettings.style = GLOW;
								UpdateStyle();
							}

							ImGui::EndCombo();
						}
					});

				SECTION_ENTRY(
					TABLE_LABEL_TOOLTIP("Banner scale",
						"Scales the opening Synthesia title bar and title. The bar stays full-width; "
						"1.00x matches the reference."),
					ImGui::SliderFloat("##synthBannerScale", &config->banner.synthesiaScale,
						MIN_SYNTHESIA_BANNER_SCALE, MAX_SYNTHESIA_BANNER_SCALE, "%.2fx");
				);

				SECTION_ENTRY(TABLE_LABEL_TOOLTIP("Native note colors", "Uses built-in synthesia colors instead of the color palette shared between renderers."),
					{
						if (ImGui::Checkbox("##nativeCols", &renderSettings.useNativeNoteColors))
						{
							ShaderBind shaderBind(*notesProgram);
							notesProgram->SetInt("nativeColors", renderSettings.useNativeNoteColors ? 1 : 0);
						}
					});

				SECTION_ENTRY(SECTION_LABEL("Out-of-bound notes"),
					{
						ImGui::Checkbox("##oobNotes", &renderSettings.showOutOfBoundNotes);
					});

				SECTION_ENTRY(SECTION_LABEL("Background"),
					{
						ImGui::Checkbox("##background", &renderSettings.renderBackground);
					});

				SECTION_ENTRY(SECTION_LABEL(app->GetLocalization()->Get("renderer.synthesia.sheet", "Sheet music").c_str()),
					{
						ImGui::Checkbox("##sheetMusic", &renderSettings.showSheetMusic);
					});

				SECTION_ENTRY(SECTION_LABEL(app->GetLocalization()->Get("renderer.synthesia.keyOctaves", "Key octave labels").c_str()),
					{
						ImGui::Checkbox("##keyOctavesGeneral", &renderSettings.showKeyOctaves);
					});

				END_SECTION;
			}

			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Keyboard"))
		{
			BEGIN_SECTION("##sKeyboard")
			{
				SETUP_SECTION;
				SECTION_ENTRY(SECTION_LABEL("Key sparkles"),
					{
						ImGui::Checkbox("##keySpark", &renderSettings.renderKeySparkle);
					});
				SECTION_ENTRY(SECTION_LABEL("Bar color"),
					{
						ImVec4 barColor = config->render.GetBarColor();
						if (ImGui::ColorEdit3("##barColor", &barColor.x))
						{
							config->render.SetBarColor(barColor.x, barColor.y, barColor.z);
						}
					});
				END_SECTION;
			}

			ImGui::EndTabItem();
		}
		RenderStyleSettings();

		ImGui::EndTabBar();
	}

	AbstractMIDIRenderer::RenderSettings();
}

void MIDIRendererSynthesia::RenderStyleSettings()
{
	switch (renderSettings.style)
	{
	case SYNTHESIA_10:
		break;
	default:
		break;
	}

}

void MIDIRendererSynthesia::ResetSettings()
{
	renderSettings = SynthesiaRenderSettings::Default();
}

void MIDIRendererSynthesia::OnResize(int width, int height)
{
	AbstractMIDIRenderer::OnResize(width, height);
	this->width = width;
	this->height = height;
	CalculateKeyboardData();
}

YAML::Node MIDIRendererSynthesia::GetSettings()
{
	YAML::Node node;
	node["style"] = static_cast<int>(renderSettings.style);
	node["nativeColors"] = renderSettings.useNativeNoteColors;
	node["oobNotes"] = renderSettings.showOutOfBoundNotes;
	node["keySparkle"] = renderSettings.renderKeySparkle;
	node["background"] = renderSettings.renderBackground;
	node["keyOctaves"] = renderSettings.showKeyOctaves;
	node["sheetMusic"] = renderSettings.showSheetMusic;

	return node;
}

void MIDIRendererSynthesia::LoadSettings(const YAML::Node& node)
{
	if (!node) return;

	int styleInt;
	LOAD_VAL(node, "style", styleInt);
	renderSettings.style = static_cast<SynthesiaStyle>(styleInt);

	LOAD_VAL(node, "nativeColors", renderSettings.useNativeNoteColors);
	LOAD_VAL(node, "oobNotes", renderSettings.showOutOfBoundNotes);
	LOAD_VAL(node, "keySparkle", renderSettings.renderKeySparkle);
	LOAD_VAL(node, "background", renderSettings.renderBackground);
	LOAD_VAL(node, "keyOctaves", renderSettings.showKeyOctaves);
	LOAD_VAL(node, "sheetMusic", renderSettings.showSheetMusic);

	UpdateStyle();
}

void MIDIRendererSynthesia::ResetKeyboardState()
{
	for (auto& k : keyStates)
	{
		k.pressed = false;
		k.color = 0;
	}
}

void MIDIRendererSynthesia::KillAllParticles()
{
	InitializeParticleSystems();
}

void MIDIRendererSynthesia::ResetRenderer()
{
	KillAllParticles();
	ResetKeyboardState();
}