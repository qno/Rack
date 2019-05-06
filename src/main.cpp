#include "common.hpp"
#include "random.hpp"
#include "asset.hpp"
#include "midi.hpp"
#include "rtmidi.hpp"
#include "keyboard.hpp"
#include "gamepad.hpp"
#include "bridge.hpp"
#include "settings.hpp"
#include "engine/Engine.hpp"
#include "app/Scene.hpp"
#include "plugin.hpp"
#include "app.hpp"
#include "patch.hpp"
#include "ui.hpp"
#include "system.hpp"
#include "string.hpp"

#include <osdialog.h>
#include <thread>
#include <unistd.h> // for getopt
#include <signal.h> // for signal

#if defined ARCH_WIN
	#include <windows.h> // for CreateMutex
#endif

using namespace rack;


static void fatalSignalHandler(int sig) {
	// Only catch one signal
	static bool caught = false;
	bool localCaught = caught;
	caught = true;
	if (localCaught)
		exit(1);

	FATAL("Fatal signal %d. Stack trace:\n%s", sig, system::getStackTrace().c_str());

	// osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Rack has crashed. See log.txt for details.");

	exit(1);
}


int main(int argc, char *argv[]) {
#if defined ARCH_WIN
	// Windows global mutex to prevent multiple instances
	// Handle will be closed by Windows when the process ends
	HANDLE instanceMutex = CreateMutexA(NULL, true, app::APP_NAME);
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, "Rack is already running. Multiple Rack instances are not supported.");
		exit(1);
	}
	(void) instanceMutex;
#endif

	std::string patchPath;

	// Parse command line arguments
	int c;
	opterr = 0;
	while ((c = getopt(argc, argv, "ds:u:")) != -1) {
		switch (c) {
			case 'd': {
				settings::devMode = true;
			} break;
			case 'h': {
				settings::headless = true;
			} break;
			case 's': {
				asset::systemDir = optarg;
			} break;
			case 'u': {
				asset::userDir = optarg;
			} break;
			default: break;
		}
	}
	if (optind < argc) {
		patchPath = argv[optind];
	}

	asset::init();
	logger::init();

	// We can now install a signal handler and log the output
	// Mac has its own decent crash handler
#if 0
	if (!settings::devMode) {
		signal(SIGABRT, fatalSignalHandler);
		signal(SIGFPE, fatalSignalHandler);
		signal(SIGILL, fatalSignalHandler);
		signal(SIGSEGV, fatalSignalHandler);
		signal(SIGTERM, fatalSignalHandler);
	}
#endif

	// Log environment
	INFO("%s v%s", app::APP_NAME, app::APP_VERSION);
	INFO("%s", system::getOperatingSystemInfo().c_str());
	if (settings::devMode)
		INFO("Development mode");
	INFO("System directory: %s", asset::systemDir.c_str());
	INFO("User directory: %s", asset::userDir.c_str());

	// Load settings
	try {
		settings::load(asset::user("settings.json"));
	}
	catch (UserException &e) {
		std::string msg = e.what();
		msg += "\n\nReset settings to default?";
		if (!osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK_CANCEL, msg.c_str())) {
			exit(1);
		}
	}

	// Check existence of the system res/ directory
	std::string resDir = asset::system("res");
	if (!system::isDirectory(resDir)) {
		std::string message = string::f("Rack's resource directory \"%s\" does not exist. Make sure Rack is correctly installed and launched.", resDir.c_str());
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, message.c_str());
		exit(1);
	}

	INFO("Initializing environment");
	random::init();
	midi::init();
	rtmidiInit();
	bridgeInit();
	keyboard::init();
	gamepad::init();
	plugin::init();
	if (!settings::headless) {
		ui::init();
		windowInit();
	}

	// Initialize app
	INFO("Initializing app");
	appInit();

	// removed for now to be compliant with official GLEW version
	// see https://github.com/VCVRack/Rack/issues/1269
	// https://stackoverflow.com/questions/22952536/how-do-i-pass-a-filename-from-finder-into-my-os-x-application
	//const char *openedFilename = glfwGetOpenedFilename();
	const char *openedFilename = nullptr;
	if (openedFilename) {
		patchPath = openedFilename;
	}

	if (!settings::headless) {
		APP->patch->init(patchPath);
	}

	INFO("Starting engine");
	APP->engine->start();

	if (!settings::headless) {
		INFO("Running window");
		APP->window->run();
		INFO("Stopped window");
	}
	else {
		// TEMP Prove that the app doesn't crash
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
	INFO("Stopping engine");
	APP->engine->stop();

	// Destroy app
	if (!settings::headless) {
		APP->patch->save(asset::user("autosave.vcv"));
	}
	INFO("Destroying app");
	appDestroy();
	settings::save(asset::user("settings.json"));

	// Destroy environment
	INFO("Destroying environment");
	if (!settings::headless) {
		windowDestroy();
		ui::destroy();
	}
	plugin::destroy();
	bridgeDestroy();
	midi::destroy();
	INFO("Destroying logger");
	logger::destroy();

	return 0;
}
