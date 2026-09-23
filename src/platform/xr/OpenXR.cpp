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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Must come first: pulls in windows.h and the libepoxy GL declarations
#include "graphics/opengl/OpenGLUtil.h"
#include <epoxy/wgl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/Application.h"
#include "core/Config.h"
#include "graphics/Renderer.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "io/log/Logger.h"
#include "math/Types.h"
#include "platform/ProgramOptions.h"
#include "window/RenderWindow.h"


namespace xr {

//! A swapchain with one framebuffer object per image
struct Swapchain {
	
	XrSwapchain handle = XR_NULL_HANDLE;
	Vec2i size = Vec2i(0);
	std::vector<XrSwapchainImageOpenGLKHR> images;
	std::vector<GLuint> framebuffers;
	GLuint depth = 0; //!< Shared depth renderbuffer, 0 if the swapchain has no depth
	uint32_t current = 0; //!< Index of the acquired image
	bool acquired = false;
	bool rendered = false; //!< The game rendered into the acquired image
	
};

namespace state {

static bool requested = false;
static bool debug = false;
static unsigned dumpFrames = 0;

static XrInstance instance = XR_NULL_HANDLE;
static XrSystemId system = XR_NULL_SYSTEM_ID;
static XrSession session = XR_NULL_HANDLE;
static XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
static bool sessionRunning = false;

static XrSpace localSpace = XR_NULL_HANDLE;

static int64_t colorFormat = 0;
static std::array<XrViewConfigurationView, 2> viewConfigs;
static std::array<Swapchain, 2> eyes;
static Swapchain ui;

static bool frameBegun = false;
static XrFrameState frameState = { XR_TYPE_FRAME_STATE };
static std::array<XrView, 2> views;
static bool viewsValid = false;

} // namespace state

static OpenGLRenderer * renderer() {
	return static_cast<OpenGLRenderer *>(GRenderer);
}

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
	return state::session != XR_NULL_HANDLE;
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

static bool createSystem() {
	
	XrSystemGetInfo info = { XR_TYPE_SYSTEM_GET_INFO };
	info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if(!check(xrGetSystem(state::instance, &info, &state::system), "xrGetSystem (is the headset connected?)")) {
		return false;
	}
	
	XrSystemProperties properties = { XR_TYPE_SYSTEM_PROPERTIES };
	if(check(xrGetSystemProperties(state::instance, state::system, &properties), "xrGetSystemProperties")) {
		LogInfo << "OpenXR system: " << properties.systemName << ", max swapchain "
		        << properties.graphicsProperties.maxSwapchainImageWidth << 'x'
		        << properties.graphicsProperties.maxSwapchainImageHeight
		        << ", orientation tracking " << bool(properties.trackingProperties.orientationTracking)
		        << ", position tracking " << bool(properties.trackingProperties.positionTracking);
	}
	
	uint32_t count = 0;
	if(!check(xrEnumerateViewConfigurationViews(state::instance, state::system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                            0, &count, nullptr), "xrEnumerateViewConfigurationViews")) {
		return false;
	}
	if(count != state::viewConfigs.size()) {
		LogError << "OpenXR: expected 2 stereo views, got " << count;
		return false;
	}
	state::viewConfigs.fill({ XR_TYPE_VIEW_CONFIGURATION_VIEW });
	if(!check(xrEnumerateViewConfigurationViews(state::instance, state::system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                            count, &count, state::viewConfigs.data()),
	          "xrEnumerateViewConfigurationViews")) {
		return false;
	}
	for(const XrViewConfigurationView & view : state::viewConfigs) {
		LogInfo << "OpenXR view: recommended " << view.recommendedImageRectWidth << 'x'
		        << view.recommendedImageRectHeight << ", " << view.recommendedSwapchainSampleCount << " samples";
	}
	
	return true;
}

static bool createSession() {
	
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if(!check(xrGetInstanceProcAddr(state::instance, "xrGetOpenGLGraphicsRequirementsKHR",
	                                reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)),
	          "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
		return false;
	}
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if(!check(getRequirements(state::instance, state::system, &requirements), "xrGetOpenGLGraphicsRequirementsKHR")) {
		return false;
	}
	XrVersion glVersion = XR_MAKE_VERSION(epoxy_gl_version() / 10, epoxy_gl_version() % 10, 0);
	LogInfo << "OpenXR needs OpenGL " << XR_VERSION_MAJOR(requirements.minApiVersionSupported) << '.'
	        << XR_VERSION_MINOR(requirements.minApiVersionSupported) << ", have "
	        << XR_VERSION_MAJOR(glVersion) << '.' << XR_VERSION_MINOR(glVersion);
	if(glVersion < requirements.minApiVersionSupported) {
		LogError << "OpenXR: the OpenGL context version is too old for the runtime";
		return false;
	}
	
	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if(!binding.hDC || !binding.hGLRC) {
		LogError << "OpenXR: no current OpenGL context";
		return false;
	}
	
	XrSessionCreateInfo info = { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = state::system;
	if(!check(xrCreateSession(state::instance, &info, &state::session), "xrCreateSession")) {
		state::session = XR_NULL_HANDLE;
		return false;
	}
	
	// The runtime may have changed the current context
	if(wglGetCurrentContext() != binding.hGLRC || wglGetCurrentDC() != binding.hDC) {
		LogWarning << "OpenXR: runtime changed the current OpenGL context, restoring it";
		wglMakeCurrent(binding.hDC, binding.hGLRC);
	}

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	if(!check(xrCreateReferenceSpace(state::session, &spaceInfo, &state::localSpace), "xrCreateReferenceSpace")) {
		return false;
	}
	
	return true;
}

static bool selectColorFormat() {
	
	uint32_t count = 0;
	if(!check(xrEnumerateSwapchainFormats(state::session, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) {
		return false;
	}
	std::vector<int64_t> formats(count);
	if(!check(xrEnumerateSwapchainFormats(state::session, count, &count, formats.data()), "xrEnumerateSwapchainFormats")) {
		return false;
	}
	
	// The game writes gamma-encoded colors. An sRGB swapchain without GL_FRAMEBUFFER_SRGB
	// stores them unchanged and lets the compositor interpret them correctly.
	for(int64_t preferred : { int64_t(GL_SRGB8_ALPHA8), int64_t(GL_RGBA8) }) {
		if(std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
			state::colorFormat = preferred;
			LogInfo << "OpenXR swapchain format: " << (preferred == GL_SRGB8_ALPHA8 ? "GL_SRGB8_ALPHA8" : "GL_RGBA8");
			return true;
		}
	}
	
	LogError << "OpenXR: no supported swapchain color format";
	return false;
}

static bool createSwapchain(Swapchain & swapchain, Vec2i size, bool withDepth) {
	
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = state::colorFormat;
	info.sampleCount = 1;
	info.width = uint32_t(size.x);
	info.height = uint32_t(size.y);
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if(!check(xrCreateSwapchain(state::session, &info, &swapchain.handle), "xrCreateSwapchain")) {
		swapchain.handle = XR_NULL_HANDLE;
		return false;
	}
	swapchain.size = size;
	
	uint32_t count = 0;
	if(!check(xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr), "xrEnumerateSwapchainImages")) {
		return false;
	}
	swapchain.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
	if(!check(xrEnumerateSwapchainImages(swapchain.handle, count, &count,
	                                     reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchain.images.data())),
	          "xrEnumerateSwapchainImages")) {
		return false;
	}
	
	if(withDepth) {
		glGenRenderbuffers(1, &swapchain.depth);
		glBindRenderbuffer(GL_RENDERBUFFER, swapchain.depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	
	GLint oldFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
	swapchain.framebuffers.resize(count);
	glGenFramebuffers(GLsizei(count), swapchain.framebuffers.data());
	for(uint32_t i = 0; i < count; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, swapchain.framebuffers[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swapchain.images[i].image, 0);
		if(swapchain.depth) {
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, swapchain.depth);
		}
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if(status != GL_FRAMEBUFFER_COMPLETE) {
			LogError << "OpenXR: incomplete framebuffer for swapchain image " << i << ": 0x" << std::hex << status;
			glBindFramebuffer(GL_FRAMEBUFFER, GLuint(oldFramebuffer));
			return false;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(oldFramebuffer));
	
	LogInfo << "OpenXR swapchain " << size.x << 'x' << size.y << " with " << count << " images"
	        << (withDepth ? " + depth" : "");
	
	return true;
}

static void destroySwapchain(Swapchain & swapchain) {
	if(!swapchain.framebuffers.empty()) {
		glDeleteFramebuffers(GLsizei(swapchain.framebuffers.size()), swapchain.framebuffers.data());
	}
	if(swapchain.depth) {
		glDeleteRenderbuffers(1, &swapchain.depth);
	}
	if(swapchain.handle != XR_NULL_HANDLE) {
		xrDestroySwapchain(swapchain.handle);
	}
	swapchain = Swapchain();
}

static bool createSwapchains() {
	
	if(!selectColorFormat()) {
		return false;
	}
	
	for(size_t i = 0; i < state::eyes.size(); i++) {
		Vec2i size(state::viewConfigs[i].recommendedImageRectWidth, state::viewConfigs[i].recommendedImageRectHeight);
		if(!createSwapchain(state::eyes[i], size, true)) {
			return false;
		}
	}
	
	// The UI panel has the same size as the game window so that the 2D layout does not change.
	// It needs depth for 3D content drawn into it (menu, cinematics, the player in the book).
	Vec2i uiSize = mainApp->getWindow()->getSize();
	if(!createSwapchain(state::ui, uiSize, true)) {
		return false;
	}
	
	return true;
}

static const char * sessionStateName(XrSessionState sessionState) {
	switch(sessionState) {
		case XR_SESSION_STATE_IDLE: return "idle";
		case XR_SESSION_STATE_READY: return "ready";
		case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
		case XR_SESSION_STATE_VISIBLE: return "visible";
		case XR_SESSION_STATE_FOCUSED: return "focused";
		case XR_SESSION_STATE_STOPPING: return "stopping";
		case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
		case XR_SESSION_STATE_EXITING: return "exiting";
		default: return "unknown";
	}
}

static void pollEvents() {
	
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while(xrPollEvent(state::instance, &event) == XR_SUCCESS) {
		
		switch(event.type) {
			
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto & changed = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
				state::sessionState = changed.state;
				LogInfo << "OpenXR session state: " << sessionStateName(changed.state);
				if(changed.state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo info = { XR_TYPE_SESSION_BEGIN_INFO };
					info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if(check(xrBeginSession(state::session, &info), "xrBeginSession")) {
						state::sessionRunning = true;
					}
				} else if(changed.state == XR_SESSION_STATE_STOPPING) {
					check(xrEndSession(state::session), "xrEndSession");
					state::sessionRunning = false;
				} else if(changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
					state::sessionRunning = false;
					mainApp->quit();
				}
				break;
			}
			
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				LogWarning << "OpenXR instance loss pending";
				state::sessionRunning = false;
				mainApp->quit();
				break;
			}
			
			default: {
				if(state::debug) {
					LogInfo << "OpenXR event " << int(event.type);
				}
				break;
			}
			
		}
		
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	
}

static bool acquireImage(Swapchain & swapchain) {
	
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(!check(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &swapchain.current), "xrAcquireSwapchainImage")) {
		return false;
	}
	
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if(!check(xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage")) {
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
		return false;
	}
	
	swapchain.acquired = true;
	return true;
}

static void releaseImage(Swapchain & swapchain) {
	if(swapchain.acquired) {
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		check(xrReleaseSwapchainImage(swapchain.handle, &releaseInfo), "xrReleaseSwapchainImage");
		swapchain.acquired = false;
	}
}

void beginFrame() {
	
	if(!isActive() || state::frameBegun) {
		return;
	}
	
	pollEvents();
	if(!state::sessionRunning) {
		return;
	}
	
	state::frameState = { XR_TYPE_FRAME_STATE };
	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	if(!check(xrWaitFrame(state::session, &waitInfo, &state::frameState), "xrWaitFrame")) {
		return;
	}
	
	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if(!check(xrBeginFrame(state::session, &beginInfo), "xrBeginFrame")) {
		return;
	}
	state::frameBegun = true;
	
	state::viewsValid = false;
	if(state::frameState.shouldRender) {
		XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = state::frameState.predictedDisplayTime;
		locateInfo.space = state::localSpace;
		XrViewState viewState = { XR_TYPE_VIEW_STATE };
		state::views.fill({ XR_TYPE_VIEW });
		uint32_t count = 0;
		if(check(xrLocateViews(state::session, &locateInfo, &viewState, uint32_t(state::views.size()), &count,
		                       state::views.data()), "xrLocateViews")) {
			state::viewsValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
		}
		for(Swapchain & eye : state::eyes) {
			acquireImage(eye);
		}
		acquireImage(state::ui);
	}
	
	// Everything the game draws goes to the UI panel unless a pass selects another target
	if(state::ui.acquired) {
		renderer()->setRenderTarget(state::ui.framebuffers[state::ui.current], state::ui.size);
	}
	
}

//! Fill the eye images that the game did not render this frame
static void clearUnusedEyes() {
	
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	GLfloat oldClearColor[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);
	GLint oldViewport[4];
	glGetIntegerv(GL_VIEWPORT, oldViewport);
	
	glClearColor(0.02f, 0.02f, 0.02f, 1.f);
	for(Swapchain & eye : state::eyes) {
		if(eye.acquired && !eye.rendered) {
			glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[eye.current]);
			glViewport(0, 0, eye.size.x, eye.size.y);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}
	}
	
	glBindFramebuffer(GL_FRAMEBUFFER, renderer()->getRenderTarget());
	glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
	glClearColor(oldClearColor[0], oldClearColor[1], oldClearColor[2], oldClearColor[3]);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	
}

//! Copy the VR view selected by config.vr.mirror into the desktop window
static void mirrorToWindow() {
	
	const Swapchain * source = nullptr;
	if(config.vr.mirror == "ui") {
		source = &state::ui;
	} else if(config.vr.mirror == "left") {
		source = state::eyes[0].rendered ? &state::eyes[0] : &state::ui;
	}
	if(!source || !source->acquired) {
		return;
	}
	
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	
	Vec2i window = mainApp->getWindow()->getSize();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffers[source->current]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	
	// Keep the aspect ratio of the source
	Vec2i size = window;
	if(source->size.x * window.y > window.x * source->size.y) {
		size.y = window.x * source->size.y / source->size.x;
	} else {
		size.x = window.y * source->size.x / source->size.y;
	}
	Vec2i offset((window.x - size.x) / 2, (window.y - size.y) / 2);
	if(size != window) {
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glBlitFramebuffer(0, 0, source->size.x, source->size.y, offset.x, offset.y, offset.x + size.x, offset.y + size.y,
	                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
	
	glBindFramebuffer(GL_FRAMEBUFFER, renderer()->getRenderTarget());
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	
}

void endFrame() {
	
	if(!isActive()) {
		return;
	}
	
	if(!state::frameBegun) {
		// showFrame() without a preceding beginFrame(), e.g. from the loading screen
		beginFrame();
		if(!state::frameBegun) {
			return;
		}
	}
	
	std::array<XrCompositionLayerProjectionView, 2> projectionViews;
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	std::vector<const XrCompositionLayerBaseHeader *> layers;
	
	bool eyesReady = state::viewsValid && state::eyes[0].acquired && state::eyes[1].acquired;
	bool uiReady = state::ui.acquired;
	if(state::frameState.shouldRender) {
		clearUnusedEyes();
		mirrorToWindow();
	}
	
	// Leave the swapchain images alone until the next frame begins
	renderer()->setRenderTarget(0, Vec2i(0));
	
	for(Swapchain & eye : state::eyes) {
		releaseImage(eye);
		eye.rendered = false;
	}
	releaseImage(state::ui);
	
	if(state::frameState.shouldRender && eyesReady) {
		for(size_t i = 0; i < projectionViews.size(); i++) {
			projectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			projectionViews[i].pose = state::views[i].pose;
			projectionViews[i].fov = state::views[i].fov;
			projectionViews[i].subImage.swapchain = state::eyes[i].handle;
			projectionViews[i].subImage.imageRect.extent = { state::eyes[i].size.x, state::eyes[i].size.y };
		}
		projection.space = state::localSpace;
		projection.viewCount = uint32_t(projectionViews.size());
		projection.views = projectionViews.data();
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));
	}
	
	if(state::frameState.shouldRender && uiReady) {
		// The panel shows the whole game for now, so it is opaque
		quad.layerFlags = 0;
		quad.space = state::localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = state::ui.handle;
		quad.subImage.imageRect.extent = { state::ui.size.x, state::ui.size.y };
		quad.pose.orientation.w = 1.f;
		quad.pose.position = { 0.f, 0.f, -config.vr.uiDistance };
		quad.size.width = config.vr.uiWidth;
		quad.size.height = config.vr.uiWidth * float(state::ui.size.y) / float(state::ui.size.x);
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
	}
	
	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = state::frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = uint32_t(layers.size());
	endInfo.layers = layers.data();
	check(xrEndFrame(state::session, &endInfo), "xrEndFrame");
	
	state::frameBegun = false;
	
	// Begin the next frame right away so that everything drawn until the next showFrame()
	// (including the loading screen) lands in the next UI image
	beginFrame();
}

bool initialize() {
	
	arx_assert(state::instance == XR_NULL_HANDLE);
	
	LogInfo << "Initializing OpenXR";
	
	if(!createInstance() || !createSystem() || !createSession() || !createSwapchains()) {
		shutdown();
		LogWarning << "VR initialization failed, continuing in desktop mode";
		return false;
	}
	
	// Wait for the session to become ready
	pollEvents();
	
	return true;
}

void shutdown() {
	
	if(state::session != XR_NULL_HANDLE) {
		if(GRenderer) {
			renderer()->setRenderTarget(0, Vec2i(0));
		}
		for(Swapchain & eye : state::eyes) {
			releaseImage(eye);
		}
		releaseImage(state::ui);
		if(state::sessionRunning) {
			xrRequestExitSession(state::session);
			state::sessionRunning = false;
		}
		for(Swapchain & eye : state::eyes) {
			destroySwapchain(eye);
		}
		destroySwapchain(state::ui);
		if(state::localSpace != XR_NULL_HANDLE) {
			xrDestroySpace(state::localSpace);
			state::localSpace = XR_NULL_HANDLE;
		}
		xrDestroySession(state::session);
		state::session = XR_NULL_HANDLE;
	}
	
	if(state::instance != XR_NULL_HANDLE) {
		xrDestroyInstance(state::instance);
		state::instance = XR_NULL_HANDLE;
		LogInfo << "OpenXR shut down";
	}
	
	state::system = XR_NULL_SYSTEM_ID;
	state::frameBegun = false;
	
}

} // namespace xr
