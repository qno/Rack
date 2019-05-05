#include "plugin.hpp"
#include "system.hpp"
#include "network.hpp"
#include "asset.hpp"
#include "string.hpp"
#include "app.hpp"
#include "app/common.hpp"
#include "plugin/callbacks.hpp"
#include "settings.hpp"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h> // for MAXPATHLEN
#include <fcntl.h>
#include <thread>
#include <map>
#include <stdexcept>

#define ZIP_STATIC
#include <zip.h>
#include <jansson.h>

#if defined ARCH_WIN
	#include <windows.h>
	#include <direct.h>
	#define mkdir(_dir, _perms) _mkdir(_dir)
#else
	#include <dlfcn.h>
#endif
#include <dirent.h>
#include <osdialog.h>


namespace rack {
namespace plugin {


////////////////////
// private API
////////////////////

static bool loadPlugin(std::string path) {
	// Load plugin.json
	std::string metadataFilename = path + "/plugin.json";
	FILE *file = fopen(metadataFilename.c_str(), "r");
	if (!file) {
		WARN("Plugin metadata file %s does not exist", metadataFilename.c_str());
		return false;
	}
	DEFER({
		fclose(file);
	});

	json_error_t error;
	json_t *rootJ = json_loadf(file, 0, &error);
	if (!rootJ) {
		WARN("JSON parsing error at %s %d:%d %s", error.source, error.line, error.column, error.text);
		return false;
	}
	DEFER({
		json_decref(rootJ);
	});

	// Load plugin library
	std::string libraryFilename;
#if defined ARCH_LIN
	libraryFilename = path + "/" + "plugin.so";
#elif defined ARCH_WIN
	libraryFilename = path + "/" + "plugin.dll";
#elif ARCH_MAC
	libraryFilename = path + "/" + "plugin.dylib";
#endif

	// Check file existence
	if (!system::isFile(libraryFilename)) {
		WARN("Plugin file %s does not exist", libraryFilename.c_str());
		return false;
	}

	// Load dynamic/shared library
#if defined ARCH_WIN
	SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);
	HINSTANCE handle = LoadLibrary(libraryFilename.c_str());
	SetErrorMode(0);
	if (!handle) {
		int error = GetLastError();
		WARN("Failed to load library %s: code %d", libraryFilename.c_str(), error);
		return false;
	}
#else
	void *handle = dlopen(libraryFilename.c_str(), RTLD_NOW);
	if (!handle) {
		WARN("Failed to load library %s: %s", libraryFilename.c_str(), dlerror());
		return false;
	}
#endif

	// Call plugin's init() function
	typedef void (*InitCallback)(Plugin *);
	InitCallback initCallback;
#if defined ARCH_WIN
	initCallback = (InitCallback) GetProcAddress(handle, "init");
#else
	initCallback = (InitCallback) dlsym(handle, "init");
#endif
	if (!initCallback) {
		WARN("Failed to read init() symbol in %s", libraryFilename.c_str());
		return false;
	}

	// Construct and initialize Plugin instance
	Plugin *plugin = new Plugin;
	plugin->path = path;
	plugin->handle = handle;
	initCallback(plugin);
	plugin->fromJson(rootJ);

	// Check slug
	if (!isSlugValid(plugin->slug)) {
		WARN("Plugin slug \"%s\" is invalid", plugin->slug.c_str());
		// TODO Fix memory leak with `plugin`
		return false;
	}

	// Reject plugin if slug already exists
	Plugin *oldPlugin = getPlugin(plugin->slug);
	if (oldPlugin) {
		WARN("Plugin \"%s\" is already loaded, not attempting to load it again", plugin->slug.c_str());
		// TODO Fix memory leak with `plugin`
		return false;
	}

	// Add plugin to list
	plugins.push_back(plugin);
	INFO("Loaded plugin %s v%s from %s", plugin->slug.c_str(), plugin->version.c_str(), libraryFilename.c_str());

	// Normalize tags
	for (Model *model : plugin->models) {
		std::vector<std::string> normalizedTags;
		for (const std::string &tag : model->tags) {
			std::string normalizedTag = normalizeTag(tag);
			if (!normalizedTag.empty())
				normalizedTags.push_back(normalizedTag);
		}
		model->tags = normalizedTags;
	}

	// Search for presets
	for (Model *model : plugin->models) {
		std::string presetDir = asset::plugin(plugin, "presets/" + model->slug);
		for (const std::string &presetPath : system::listEntries(presetDir)) {
			model->presetPaths.push_back(presetPath);
		}
	}

	return true;
}

static bool syncPlugin(std::string slug, json_t *manifestJ, bool dryRun) {
	// Check that "status" is "available"
	json_t *statusJ = json_object_get(manifestJ, "status");
	if (!statusJ) {
		return false;
	}
	std::string status = json_string_value(statusJ);
	if (status != "available") {
		return false;
	}

	// Get latest version
	json_t *latestVersionJ = json_object_get(manifestJ, "latestVersion");
	if (!latestVersionJ) {
		WARN("Could not get latest version of plugin %s", slug.c_str());
		return false;
	}
	std::string latestVersion = json_string_value(latestVersionJ);

	// Check whether we already have a plugin with the same slug and version
	Plugin *plugin = getPlugin(slug);
	if (plugin && plugin->version == latestVersion) {
		return false;
	}

	json_t *nameJ = json_object_get(manifestJ, "name");
	std::string name;
	if (nameJ) {
		name = json_string_value(nameJ);
	}
	else {
		name = slug;
	}

#if defined ARCH_WIN
	std::string arch = "win";
#elif ARCH_MAC
	std::string arch = "mac";
#elif defined ARCH_LIN
	std::string arch = "lin";
#endif

	std::string downloadUrl;
	downloadUrl = app::API_URL;
	downloadUrl += "/download";
	if (dryRun) {
		downloadUrl += "/available";
	}
	downloadUrl += "?token=" + network::encodeUrl(settings::token);
	downloadUrl += "&slug=" + network::encodeUrl(slug);
	downloadUrl += "&version=" + network::encodeUrl(latestVersion);
	downloadUrl += "&arch=" + network::encodeUrl(arch);

	if (dryRun) {
		// Check if available
		json_t *availableResJ = network::requestJson(network::GET, downloadUrl, NULL);
		if (!availableResJ) {
			WARN("Could not check whether download is available");
			return false;
		}
		DEFER({
			json_decref(availableResJ);
		});
		json_t *successJ = json_object_get(availableResJ, "success");
		return json_boolean_value(successJ);
	}
	else {
		downloadName = name;
		downloadProgress = 0.0;
		INFO("Downloading plugin %s %s %s", slug.c_str(), latestVersion.c_str(), arch.c_str());

		// Download zip
		std::string pluginDest = asset::user("plugins/" + slug + ".zip");
		if (!network::requestDownload(downloadUrl, pluginDest, &downloadProgress)) {
			WARN("Plugin %s download was unsuccessful", slug.c_str());
			return false;
		}

		downloadName = "";
		return true;
	}
}

static void loadPlugins(std::string path) {
	std::string message;
	for (std::string pluginPath : system::listEntries(path)) {
		if (!system::isDirectory(pluginPath))
			continue;
		if (!loadPlugin(pluginPath)) {
			// Ignore bad plugins. They are reported in log.txt.
		}
	}
}

/** Returns 0 if successful */
static int extractZipHandle(zip_t *za, const char *dir) {
	int err;
	for (int i = 0; i < zip_get_num_entries(za, 0); i++) {
		zip_stat_t zs;
		err = zip_stat_index(za, i, 0, &zs);
		if (err) {
			WARN("zip_stat_index() failed: error %d", err);
			return err;
		}
		int nameLen = strlen(zs.name);

		char path[MAXPATHLEN];
		snprintf(path, sizeof(path), "%s/%s", dir, zs.name);

		if (zs.name[nameLen - 1] == '/') {
			if (mkdir(path, 0755)) {
				if (errno != EEXIST) {
					WARN("mkdir(%s) failed: error %d", path, errno);
					return errno;
				}
			}
		}
		else {
			zip_file_t *zf = zip_fopen_index(za, i, 0);
			if (!zf) {
				WARN("zip_fopen_index() failed");
				return -1;
			}

			FILE *outFile = fopen(path, "wb");
			if (!outFile)
				continue;

			while (1) {
				char buffer[1<<15];
				int len = zip_fread(zf, buffer, sizeof(buffer));
				if (len <= 0)
					break;
				fwrite(buffer, 1, len, outFile);
			}

			err = zip_fclose(zf);
			if (err) {
				WARN("zip_fclose() failed: error %d", err);
				return err;
			}
			fclose(outFile);
		}
	}
	return 0;
}

/** Returns 0 if successful */
static int extractZip(const char *filename, const char *path) {
	int err;
	zip_t *za = zip_open(filename, 0, &err);
	if (!za) {
		WARN("Could not open zip %s: error %d", filename, err);
		return err;
	}
	DEFER({
		zip_close(za);
	});

	err = extractZipHandle(za, path);
	return err;
}

static void extractPackages(const std::string &path) {
	std::string message;

	for (std::string packagePath : system::listEntries(path)) {
		if (string::filenameExtension(packagePath) != "zip")
			continue;
		INFO("Extracting package %s", packagePath.c_str());
		// Extract package
		if (extractZip(packagePath.c_str(), path.c_str())) {
			WARN("Package %s failed to extract", packagePath.c_str());
			message += string::f("Could not extract package %s\n", packagePath.c_str());
			continue;
		}
		// Remove package
		if (remove(packagePath.c_str())) {
			WARN("Could not delete file %s: error %d", packagePath.c_str(), errno);
		}
	}
	if (!message.empty()) {
		osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, message.c_str());
	}
}

////////////////////
// public API
////////////////////

void init() {
	// Load Core
	Plugin *corePlugin = new Plugin;
	// This function is defined in Core/plugin.cpp
	::init(corePlugin);
	plugins.push_back(corePlugin);

	// Get user plugins directory
	std::string pluginsDir = asset::user("plugins");
	mkdir(pluginsDir.c_str(), 0755);

	// Extract packages and load plugins
	extractPackages(pluginsDir);
	loadPlugins(pluginsDir);

	// Copy Fundamental package to plugins directory if Fundamental is not loaded
	std::string fundamentalSrc = asset::system("Fundamental.zip");
	std::string fundamentalDir = asset::user("plugins/Fundamental");
	if (!settings::devMode && !getPlugin("Fundamental") && system::isFile(fundamentalSrc)) {
		extractZip(fundamentalSrc.c_str(), pluginsDir.c_str());
		loadPlugin(fundamentalDir);
	}
}

void destroy() {
	for (Plugin *plugin : plugins) {
		// Free library handle
#if defined ARCH_WIN
		if (plugin->handle)
			FreeLibrary((HINSTANCE) plugin->handle);
#else
		if (plugin->handle)
			dlclose(plugin->handle);
#endif

		// For some reason this segfaults.
		// It might be best to let them leak anyway, because "crash on exit" issues would occur with badly-written plugins.
		// delete plugin;
	}
	plugins.clear();
}

void logIn(const std::string &email, const std::string &password) {
	loginStatus = "Logging in...";
	json_t *reqJ = json_object();
	json_object_set(reqJ, "email", json_string(email.c_str()));
	json_object_set(reqJ, "password", json_string(password.c_str()));
	std::string url = app::API_URL;
	url += "/token";
	json_t *resJ = network::requestJson(network::POST, url, reqJ);
	json_decref(reqJ);

	if (!resJ) {
		loginStatus = "No response from server";
		return;
	}

	json_t *errorJ = json_object_get(resJ, "error");
	if (errorJ) {
		const char *errorStr = json_string_value(errorJ);
		loginStatus = errorStr;
	}
	else {
		json_t *tokenJ = json_object_get(resJ, "token");
		if (tokenJ) {
			const char *tokenStr = json_string_value(tokenJ);
			settings::token = tokenStr;
			loginStatus = "";
		}
		else {
			loginStatus = "No token in response";
		}
	}
	json_decref(resJ);
}

void logOut() {
	settings::token = "";
}

void query() {
	if (settings::token.empty())
		return;

	// Get user's plugins list
	json_t *pluginsReqJ = json_object();
	json_object_set(pluginsReqJ, "token", json_string(settings::token.c_str()));
	std::string pluginsUrl = app::API_URL;
	pluginsUrl += "/plugins";
	json_t *pluginsResJ = network::requestJson(network::GET, pluginsUrl, pluginsReqJ);
	json_decref(pluginsReqJ);
	if (!pluginsResJ) {
		WARN("Request for user's plugins failed");
		return;
	}
	DEFER({
		json_decref(pluginsResJ);
	});

	json_t *errorJ = json_object_get(pluginsResJ, "error");
	if (errorJ) {
		WARN("Request for user's plugins returned an error: %s", json_string_value(errorJ));
		return;
	}

	// Get community manifests
	std::string manifestsUrl = app::API_URL;
	manifestsUrl += "/community/manifests";
	json_t *manifestsResJ = network::requestJson(network::GET, manifestsUrl, NULL);
	if (!manifestsResJ) {
		WARN("Request for community manifests failed");
		return;
	}
	DEFER({
		json_decref(manifestsResJ);
	});

	json_dumpf(pluginsResJ, stderr, JSON_INDENT(2));
	json_dumpf(manifestsResJ, stderr, JSON_INDENT(2));
}

void sync() {
#if 0
	if (settings::token.empty())
		return false;

	bool available = false;

	if (!dryRun) {
		downloadProgress = 0.0;
		downloadName = "Updating plugins...";
	}

	// Check each plugin in list of plugin slugs
	json_t *pluginsJ = json_object_get(pluginsResJ, "plugins");
	if (!pluginsJ) {
		WARN("No plugins array");
		return false;
	}
	json_t *manifestsJ = json_object_get(manifestsResJ, "manifests");
	if (!manifestsJ) {
		WARN("No manifests object");
		return false;
	}

	size_t slugIndex;
	json_t *slugJ;
	json_array_foreach(pluginsJ, slugIndex, slugJ) {
		std::string slug = json_string_value(slugJ);
		// Search for slug in manifests
		const char *manifestSlug;
		json_t *manifestJ = NULL;
		json_object_foreach(manifestsJ, manifestSlug, manifestJ) {
			if (slug == std::string(manifestSlug))
				break;
		}

		if (!manifestJ)
			continue;

		if (syncPlugin(slug, manifestJ, dryRun)) {
			available = true;
		}
	}

	return available;
#endif
}

void cancelDownload() {
	// TODO
}

bool isLoggedIn() {
	return settings::token != "";
}

Plugin *getPlugin(const std::string &pluginSlug) {
	for (Plugin *plugin : plugins) {
		if (plugin->slug == pluginSlug) {
			return plugin;
		}
	}
	return NULL;
}

Model *getModel(const std::string &pluginSlug, const std::string &modelSlug) {
	Plugin *plugin = getPlugin(pluginSlug);
	if (!plugin)
		return NULL;
	Model *model = plugin->getModel(modelSlug);
	if (!model)
		return NULL;
	return model;
}


/** List of allowed tags in human display form, alphabetized.
All tags here should be in sentence caps for display consistency.
However, tags are case-insensitive in plugin metadata.
*/
const std::set<std::string> allowedTags = {
	"Arpeggiator",
	"Attenuator", // With a level knob and not much else.
	"Blank", // No parameters or ports. Serves no purpose except visual.
	"Chorus",
	"Clock generator",
	"Clock modulator", // Clock dividers, multipliers, etc.
	"Compressor", // With threshold, ratio, knee, etc parameters.
	"Controller", // Use only if the artist "performs" with this module. Simply having knobs is not enough. Examples: on-screen keyboard, XY pad.
	"Delay",
	"Digital",
	"Distortion",
	"Drum",
	"Dual", // The core functionality times two. If multiple channels are a requirement for the module to exist (ring modulator, mixer, etc), it is not a Dual module.
	"Dynamics",
	"Effect",
	"Envelope follower",
	"Envelope generator",
	"Equalizer",
	"Expander", // Expands the functionality of a "mother" module when placed next to it. Expanders should inherit the tags of its mother module.
	"External",
	"Flanger",
	"Function generator",
	"Granular",
	"LFO",
	"Limiter",
	"Logic",
	"Low pass gate",
	"MIDI",
	"Mixer",
	"Multiple",
	"Noise",
	"Panning",
	"Phaser",
	"Physical modeling",
	"Polyphonic",
	"Quad", // The core functionality times four. If multiple channels are a requirement for the module to exist (ring modulator, mixer, etc), it is not a Quad module.
	"Quantizer",
	"Random",
	"Recording",
	"Reverb",
	"Ring modulator",
	"Sample and hold",
	"Sampler",
	"Sequencer",
	"Slew limiter",
	"Switch",
	"Synth voice", // A synth voice must have, at the minimum, a built-in oscillator and envelope.
	"Tuner",
	"Utility", // Serves only extremely basic functions, like inverting, max, min, multiplying by 2, etc.
	"VCA",
	"VCF",
	"VCO",
	"Visual",
	"Vocoder",
	"Waveshaper",
};


/** List of common synonyms for allowed tags.
Aliases and tags must be lowercase.
*/
const std::map<std::string, std::string> tagAliases = {
	{"amplifier", "vca"},
	{"clock", "clock generator"},
	{"drums", "drum"},
	{"eq", "equalizer"},
	{"filter", "vcf"},
	{"low frequency oscillator", "lfo"},
	{"lowpass gate", "low pass gate"},
	{"oscillator", "vco"},
	{"percussion", "drum"},
	{"poly", "polyphonic"},
	{"s&h", "sample and hold"},
	{"voltage controlled amplifier", "vca"},
	{"voltage controlled filter", "vcf"},
	{"voltage controlled oscillator", "vco"},
};


std::string normalizeTag(const std::string &tag) {
	std::string lowercaseTag = string::lowercase(tag);
	// Transform aliases
	auto it = tagAliases.find(lowercaseTag);
	if (it != tagAliases.end())
		lowercaseTag = it->second;
	// Find allowed tag
	for (const std::string &allowedTag : allowedTags) {
		if (lowercaseTag == string::lowercase(allowedTag))
			return allowedTag;
	}
	return "";
}

bool isSlugValid(const std::string &slug) {
	for (char c : slug) {
		if (!(std::isalnum(c) || c == '-' || c == '_'))
			return false;
	}
	return true;
}


std::vector<Plugin*> plugins;

std::string loginStatus;
float downloadProgress = 0.f;
std::string downloadName;


} // namespace plugin
} // namespace rack
