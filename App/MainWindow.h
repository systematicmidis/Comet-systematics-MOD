#include "UI/MenuBuilder.h"
#include "UI/Dialog.h"
#include "UI/NavigationBar.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <string>

class MIDIApp;
class UpdateChecker;

struct WindowRect
{
	int x, y;
	int width, height;
};

struct InputState
{
	// mouse
	int mouseButton = -1;
	bool mouseDown = false;
	bool mouseHold = false;
	bool mouseUp = false;
	
	// keyboard
	ImGuiKey key = ImGuiKey_None;
	bool ctrl = false;
	bool shift = false;
	bool alt = false;
	bool keyDown = false;
	bool keyHold = false;
	bool keyUp = false;
};

class MainWindow
{
public:
	MainWindow(const char* title);
	~MainWindow();
	void Run();
	bool CanShowNavigationBar();
	bool IsFullscreen() { return fullscreen; }
	void ToggleFullscreen();
	void SetTitleInfo(std::string midiName = "");
	// File > Open: pick a MIDI, start loading it and show the loading dialog.
	void OpenMIDIDialog();
	// The Synthesia visual theme draws its own play bar and hides the regular menu bar; the bar's
	// menu popup needs the same dialogs the menu items open.
	bool UsesSynthesiaToolbar();
	void OpenSettingsDialog();
	void OpenRenderVideoDialog();
	void OpenDiagnosisDialog();
	void OpenUpdateDialog();
	// Starts loading a known path and shows the loading dialog; used by File > Open and by a
	// path passed on the command line. The overload with an entry name is used by the archive
	// picker when the container holds more than one MIDI.
	void LoadMIDIFile(const std::string& path);
	void LoadMIDIFile(const std::string& path, const std::string& archiveEntry, bool preflight = true);
	GLFWwindow* GetInternalWindow() { return window; }

	bool IsGLReady() const
	{
		return isGLReady;
	}
private:
	// called before glfw initialization
	void InitializeApp();
	// called after glfw/glad initialization so resources can be loaded into the gpu
	void InitializeAppResources();
	bool InitializeGLFW();
	void InitializeUI();
	void InitializeDialogs();
	void InitializeTheme();
	void LoadWindowIcon();
	void PostInit();
	void DetectInput();
	void RenderUI();

	std::unique_ptr<MIDIApp> midiApp;
	std::unique_ptr<UpdateChecker> updateChecker;

	MenuBuilder menuBuilder;
	DialogManager dialogManager;
	GLFWwindow* window = nullptr;
	WindowRect lastWindowRect;
	std::string title = "Window";
	std::string titleInfo = "No MIDI Loaded";
	bool fullscreen = false;
	bool isGLReady = false;
};