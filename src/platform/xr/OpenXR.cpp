/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/xr/OpenXR.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Must come first: pulls in windows.h and the libepoxy GL declarations
#include "graphics/opengl/OpenGLUtil.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/Config.h"
#include "io/log/Logger.h"
#include "platform/ProgramOptions.h"


namespace xr {

namespace state {

static bool requested = false;
static bool debug = false;
static unsigned dumpFrames = 0;

static XrInstance instance = XR_NULL_HANDLE;

} // namespace state

static void enableVR() {
	state::requested = true;
}
ARX_PROGRAM_OPTION("vr", "", "Start in VR mode (OpenXR)", &enableVR)

static void enableVRDebug() {
	state::requested = true;
	state::debug = true;
}
ARX_PROGRAM_OPTION("vr-debug", "", "Start in VR mode with verbose OpenXR logging", &enableVRDebug)

static void dumpVRFrames(u32 count) {
	state::dumpFrames = count;
}
ARX_PROGRAM_OPTION_ARG("vr-dump-frames", "", "Save the first N VR frames as PNG images", &dumpVRFrames, "N")

static const char * resultToString(XrResult result) {
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if(state::instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(state::instance, result, buffer))) {
		std::snprintf(buffer, sizeof(buffer), "XrResult(%d)", int(result));
	}
	return buffer;
}

static bool check(XrResult result, const char * what) {
	if(XR_FAILED(result)) {
		LogError << "OpenXR: " << what << " failed: " << resultToString(result);
		return false;
	}
	return true;
}

bool isRequested() {
	return state::requested || config.vr.enabled;
}

bool isDebug() {
	return state::debug;
}

bool isActive() {
	return state::instance != XR_NULL_HANDLE;
}

static bool createInstance() {
	
	uint32_t count = 0;
	if(!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties")) {
		return false;
	}
	std::vector<XrExtensionProperties> available(count, { XR_TYPE_EXTENSION_PROPERTIES, nullptr, {}, 0 });
	if(!check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()),
	          "xrEnumerateInstanceExtensionProperties")) {
		return false;
	}
	
	bool hasOpenGL = false;
	for(const XrExtensionProperties & extension : available) {
		if(state::debug) {
			LogInfo << "OpenXR extension: " << extension.extensionName << " v" << extension.extensionVersion;
		}
		if(std::strcmp(extension.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) {
			hasOpenGL = true;
		}
	}
	if(!hasOpenGL) {
		LogError << "OpenXR: the active runtime does not support " << XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
		return false;
	}
	
	std::vector<const char *> extensions = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	
	XrInstanceCreateInfo info = { XR_TYPE_INSTANCE_CREATE_INFO };
	std::snprintf(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "Arx Libertatis VR");
	info.applicationInfo.applicationVersion = 1;
	std::snprintf(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "Arx Libertatis");
	info.applicationInfo.engineVersion = 1;
	info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.enabledExtensionNames = extensions.data();
	
	if(!check(xrCreateInstance(&info, &state::instance), "xrCreateInstance")) {
		state::instance = XR_NULL_HANDLE;
		return false;
	}
	
	XrInstanceProperties properties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(check(xrGetInstanceProperties(state::instance, &properties), "xrGetInstanceProperties")) {
		LogInfo << "OpenXR runtime: " << properties.runtimeName << ' '
		        << XR_VERSION_MAJOR(properties.runtimeVersion) << '.'
		        << XR_VERSION_MINOR(properties.runtimeVersion) << '.'
		        << XR_VERSION_PATCH(properties.runtimeVersion);
	}
	
	return true;
}

bool initialize() {
	
	arx_assert(state::instance == XR_NULL_HANDLE);
	
	LogInfo << "Initializing OpenXR";
	
	if(!createInstance()) {
		shutdown();
		LogWarning << "VR initialization failed, continuing in desktop mode";
		return false;
	}
	
	return true;
}

void shutdown() {
	
	if(state::instance != XR_NULL_HANDLE) {
		xrDestroyInstance(state::instance);
		state::instance = XR_NULL_HANDLE;
		LogInfo << "OpenXR shut down";
	}
	
}

} // namespace xr
