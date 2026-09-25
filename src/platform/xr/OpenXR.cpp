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
#include <cmath>
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

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/Application.h"
#include "core/Config.h"
#include "core/Core.h"
#include "game/Camera.h"
#include "graphics/Math.h"
#include "graphics/Renderer.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/image/Image.h"
#include "input/Keyboard.h"
#include "input/Mouse.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"
#include "math/Types.h"
#include "platform/ProgramOptions.h"
#include "platform/xr/XrInput.h"
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

// Head pose in the local space when the view was last recentered
static bool recenterRequested = true;
static XrVector3f recenterPosition = { 0.f, 0.f, 0.f };
static float recenterYaw = 0.f;

// Camera and eye matrices for the current frame, see applyHeadPose()
static Camera camera;
static std::array<glm::mat4x4, 2> eyeWorldToView;
//! Transform from the camera's view space to each eye's view space
static std::array<glm::dmat4, 2> eyeFromCamera;
static std::array<glm::mat4x4, 2> eyeProjection;

//! The UI panel was cleared to transparent this frame and is drawn over the world
static bool uiOverlay = false;

// Direction of the player's body in the game world (degrees) - the head turns relative to it
static bool bodyYawValid = false;
static float bodyYaw = 0.f;
static float playerYaw = 0.f; //!< Body + head yaw as last given to the player
static float pendingTurn = 0.f; //!< Snap turns not yet applied to the body (degrees)

// Controller input mapped to the game
static input::Controls controls;
static bool pointerValid = false;
static Vec2s pointer(0);
static int wheel = 0;
static std::array<bool, NUM_ACTION_KEY> actions;
static std::array<bool, NUM_ACTION_KEY> previousActions;

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
ARX_PROGRAM_OPTION_ARG("vr-dump-frames", "", "Save N VR frames, one per second, as BMP images in the user directory", &dumpVRFrames, "N")

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

Vec2i getUiSize() {
	// Independent of the window, which the desktop may resize or maximize
	return Vec2i(1280, 720);
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
	
	// The UI panel needs depth for 3D content drawn into it (menu, cinematics, the player in the book)
	if(!createSwapchain(state::ui, getUiSize(), true)) {
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
				if(changed.state == XR_SESSION_STATE_VISIBLE && state::sessionState == XR_SESSION_STATE_SYNCHRONIZED) {
					// The headset display turned on (the headset was put on): the head pose from
					// before, e.g. with the headset lying on a desk, is not a useful center
					state::recenterRequested = true;
				}
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

/*
 * Coordinate systems
 *
 * OpenXR spaces: x right, y up, z backward, meters (right-handed).
 * Arx view space: x right, y down, z forward, world units. The two are related by a rotation
 * of 180 degrees around the x axis (flipYZ), so orientations convert as flipYZ * R * flipYZ.
 * Arx world to view rotation: Rz(-roll) * Rx(pitch) * Ry(yaw), see toRotationMatrix().
 */

static const glm::mat3 flipYZ(1.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, -1.f);

static Vec3f toVec3(const XrVector3f & v) {
	return Vec3f(v.x, v.y, v.z);
}

static glm::mat3 toMat3(const XrQuaternionf & q) {
	return glm::mat3_cast(glm::quat(q.w, q.x, q.y, q.z));
}

//! Rotation around the up axis of an OpenXR orientation, 0 when looking along -z
static float getYaw(const glm::mat3 & rotation) {
	Vec3f forward = rotation * Vec3f(0.f, 0.f, -1.f);
	return std::atan2(-forward.x, -forward.z);
}

static glm::mat3 rotationY(float angle) {
	return glm::mat3(glm::rotate(glm::mat4(1.f), angle, Vec3f(0.f, 1.f, 0.f)));
}

//! Convert an Arx world to view rotation to camera angles
static Anglef toCameraAngle(const glm::mat3 & worldToView) {
	
	// The view direction in world space is the third row of the world to view rotation
	Vec3f forward = glm::transpose(worldToView)[2];
	float pitch = std::asin(glm::clamp(forward.y, -1.f, 1.f));
	float yaw = std::atan2(-forward.x, forward.z);
	
	// What remains after removing pitch and yaw is Rz(-roll)
	glm::mat3 pitchYaw(toRotationMatrix(Anglef(glm::degrees(pitch), glm::degrees(yaw), 0.f)));
	glm::mat3 roll = worldToView * glm::transpose(pitchYaw);
	float rollAngle = -std::atan2(roll[0][1], roll[0][0]);
	
	return Anglef(glm::degrees(pitch), glm::degrees(yaw), glm::degrees(rollAngle));
}

//! Projection for an eye with the same conventions as createProjectionMatrix() in Camera.cpp
static glm::mat4x4 createEyeProjection(const XrFovf & fov, float nearDist, float farDist) {
	
	float left = std::tan(fov.angleLeft);
	float right = std::tan(fov.angleRight);
	float up = std::tan(fov.angleUp);
	float down = std::tan(fov.angleDown);
	float q = farDist / (farDist - nearDist);
	
	// View space y points down, clip space y up
	glm::mat4x4 projection(0.f);
	projection[0][0] = 2.f / (right - left);
	projection[2][0] = -(right + left) / (right - left);
	projection[1][1] = -2.f / (up - down);
	projection[2][1] = -(up + down) / (up - down);
	projection[2][2] = q;
	projection[3][2] = -q * nearDist;
	projection[2][3] = 1.f;
	
	return projection;
}

static void recenterNow() {
	
	// Keep the view direction: what was the head yaw relative to the body becomes body yaw
	float headYaw = getYaw(rotationY(-state::recenterYaw) * toMat3(state::views[0].pose.orientation));
	state::bodyYaw = MAKEANGLE(state::bodyYaw + glm::degrees(headYaw));
	state::playerYaw = state::bodyYaw;
	
	Vec3f head = (toVec3(state::views[0].pose.position) + toVec3(state::views[1].pose.position)) * 0.5f;
	state::recenterPosition = { head.x, head.y, head.z };
	state::recenterYaw = getYaw(toMat3(state::views[0].pose.orientation));
	state::recenterRequested = false;
	
	LogInfo << "VR view recentered at height " << head.y << " m";
}

void recenter() {
	state::recenterRequested = true;
}

float getPlayerYaw() {
	return state::playerYaw;
}

bool hasEyeViews() {
	return state::frameBegun && state::frameState.shouldRender && state::viewsValid
	       && state::eyes[0].acquired && state::eyes[1].acquired;
}

Camera * applyHeadPose(const Camera & base, bool playerView) {
	
	arx_assert(hasEyeViews());
	
	// Local space relative to the recentered head, without its yaw
	glm::mat3 unyaw = rotationY(-state::recenterYaw);
	Vec3f origin = toVec3(state::recenterPosition);
	
	// Head yaw relative to the body, positive to the left like the game's yaw
	float headYaw = glm::degrees(getYaw(unyaw * toMat3(state::views[0].pose.orientation)));
	
	float yaw = base.angle.getYaw();
	if(playerView) {
		if(state::bodyYawValid) {
			// The game (mouse, keys, scripts) turned the player since the last frame: turn the body
			float turned = MAKEANGLE(yaw - state::playerYaw + 180.f) - 180.f;
			state::bodyYaw = MAKEANGLE(state::bodyYaw + turned);
		} else {
			state::bodyYaw = MAKEANGLE(yaw - headYaw);
			state::bodyYawValid = true;
		}
		state::bodyYaw = MAKEANGLE(state::bodyYaw + state::pendingTurn);
		state::pendingTurn = 0.f;
		yaw = state::bodyYaw;
		state::playerYaw = MAKEANGLE(state::bodyYaw + headYaw);
	} else {
		// Scripted camera: its yaw is the body direction, pick up the player's direction afterwards
		state::bodyYawValid = false;
	}
	
	// Body orientation in the game world
	glm::mat3 body = glm::transpose(glm::mat3(toRotationMatrix(Anglef(0.f, yaw, 0.f))));
	
	auto toWorldPosition = [&](const XrVector3f & position) {
		return base.m_pos + body * (flipYZ * (unyaw * (toVec3(position) - origin)) * config.vr.worldScale);
	};
	auto toWorldToView = [&](const XrQuaternionf & orientation) {
		glm::mat3 viewToWorld = body * flipYZ * unyaw * toMat3(orientation) * flipYZ;
		return glm::transpose(viewToWorld);
	};
	
	float farDist = base.cdepth;
	float nearDist = std::min(config.vr.nearPlane, farDist * 0.5f);
	
	float maxVertical = 0.f;
	float maxHorizontal = 0.f;
	for(size_t i = 0; i < EyeCount; i++) {
		const XrView & view = state::views[i];
		glm::mat3 worldToView = toWorldToView(view.pose.orientation);
		Vec3f position = toWorldPosition(view.pose.position);
		glm::mat4x4 matrix(worldToView);
		matrix[3] = glm::vec4(-(worldToView * position), 1.f);
		state::eyeWorldToView[i] = matrix;
		state::eyeProjection[i] = createEyeProjection(view.fov, nearDist, farDist);
		maxVertical = std::max({ maxVertical, std::tan(view.fov.angleUp), -std::tan(view.fov.angleDown) });
		maxHorizontal = std::max({ maxHorizontal, -std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight) });
	}
	
	// The camera used for culling and CPU projection sits between the eyes and must see
	// everything that either eye sees. Its vertical field of view is config.video.fov,
	// the horizontal one follows from the aspect ratio of the game window (see createProjectionMatrix()).
	float aspect = float(g_size.width()) / float(g_size.height());
	float halfTan = std::max(maxVertical, maxHorizontal / aspect);
	float fov = std::min(2.f * std::atan(halfTan) + glm::radians(10.f), glm::radians(170.f));
	
	state::camera = base;
	Vec3f head = (toVec3(state::views[0].pose.position) + toVec3(state::views[1].pose.position)) * 0.5f;
	state::camera.m_pos = toWorldPosition({ head.x, head.y, head.z });
	state::camera.angle = toCameraAngle(toWorldToView(state::views[0].pose.orientation));
	state::camera.setFov(fov);
	
	// Relative eye transforms, computed from tracking space offsets instead of world positions:
	// matrices containing world coordinates of several thousand units lose too much precision
	// in single precision and the re-projected geometry would jitter.
	glm::dmat3 cameraRotation = glm::dmat3(glm::mat3(toRotationMatrix(state::camera.angle)));
	for(size_t i = 0; i < EyeCount; i++) {
		glm::dmat3 eyeRotation = glm::dmat3(glm::mat3(state::eyeWorldToView[i]));
		Vec3f offset = body * (flipYZ * (unyaw * (head - toVec3(state::views[i].pose.position))) * config.vr.worldScale);
		glm::dmat4 matrix = glm::dmat4(eyeRotation * glm::transpose(cameraRotation));
		matrix[3] = glm::dvec4(eyeRotation * glm::dvec3(offset), 1.0);
		state::eyeFromCamera[i] = matrix;
	}
	
	return &state::camera;
}

void bindEye(size_t eye) {
	
	arx_assert(eye < EyeCount && hasEyeViews());
	
	Swapchain & swapchain = state::eyes[eye];
	renderer()->setRenderTarget(swapchain.framebuffers[swapchain.current], swapchain.size);
	swapchain.rendered = true;
	
	GRenderer->SetViewport(Rect(swapchain.size.x, swapchain.size.y));
	GRenderer->SetViewMatrix(state::eyeWorldToView[eye]);
	GRenderer->SetProjectionMatrix(state::eyeProjection[eye]);
	
	// Vertices projected on the CPU for the camera from applyHeadPose() are in viewport pixels
	glm::dmat4 screenToView = glm::inverse(glm::dmat4(g_preparedCamera.m_viewToScreen));
	glm::mat4x4 reproject(glm::dmat4(state::eyeProjection[eye]) * state::eyeFromCamera[eye] * screenToView);
	renderer()->setTexturedVertexTransform(&reproject);
	
}

void bindUi(bool clear) {
	
	if(!state::ui.acquired) {
		return;
	}
	
	renderer()->setRenderTarget(state::ui.framebuffers[state::ui.current], state::ui.size);
	renderer()->setTexturedVertexTransform(nullptr);
	GRenderer->SetViewport(g_size);
	GRenderer->SetViewMatrix(g_preparedCamera.m_worldToView);
	GRenderer->SetProjectionMatrix(g_preparedCamera.m_viewToClip);
	
	if(clear) {
		GRenderer->Clear(Renderer::ColorBuffer | Renderer::DepthBuffer, Color());
		state::uiOverlay = true;
	}
	
}

//! Pose and size of the UI panel in the local space
static void getPanel(glm::quat & orientation, Vec3f & center, Vec2f & size) {
	orientation = glm::angleAxis(state::recenterYaw, Vec3f(0.f, 1.f, 0.f));
	center = toVec3(state::recenterPosition) + orientation * Vec3f(0.f, 0.f, -config.vr.uiDistance);
	Vec2i pixels = getUiSize();
	size = Vec2f(config.vr.uiWidth, config.vr.uiWidth * float(pixels.y) / float(pixels.x));
}

//! Where the pointing ray of the controller hits the UI panel, in panel pixels
static bool intersectPanel(const XrPosef & aim, Vec2s & result) {
	
	glm::quat orientation;
	Vec3f center;
	Vec2f size;
	getPanel(orientation, center, size);
	
	Vec3f normal = orientation * Vec3f(0.f, 0.f, 1.f);
	Vec3f origin = toVec3(aim.position);
	Vec3f direction = toMat3(aim.orientation) * Vec3f(0.f, 0.f, -1.f);
	
	float facing = glm::dot(direction, normal);
	if(facing > -0.01f) {
		return false; // Parallel to the panel or pointing away from its front
	}
	float distance = glm::dot(center - origin, normal) / facing;
	if(distance <= 0.f) {
		return false;
	}
	
	Vec3f hit = glm::inverse(orientation) * (origin + direction * distance - center);
	Vec2f uv(hit.x / size.x + 0.5f, 0.5f - hit.y / size.y);
	if(uv.x < 0.f || uv.x >= 1.f || uv.y < 0.f || uv.y >= 1.f) {
		return false;
	}
	
	Vec2i pixels = getUiSize();
	result = Vec2s(s16(uv.x * float(pixels.x)), s16(uv.y * float(pixels.y)));
	return true;
}

//! Map the controller state of this frame to mouse, key and action state for the game
static void updateControls() {
	
	const input::Controls previous = state::controls;
	state::controls = input::getControls();
	const input::Controls & controls = state::controls;
	
	state::pointerValid = controls.aimValid && intersectPanel(controls.aim, state::pointer);
	
	// Thumbstick flicks: snap turns and mouse wheel steps
	const float flick = 0.7f;
	if(controls.turn.x > flick && previous.turn.x <= flick) {
		state::pendingTurn -= config.vr.snapTurnAngle;
	} else if(controls.turn.x < -flick && previous.turn.x >= -flick) {
		state::pendingTurn += config.vr.snapTurnAngle;
	}
	state::wheel = 0;
	if(controls.turn.y > flick && previous.turn.y <= flick) {
		state::wheel = 1;
	} else if(controls.turn.y < -flick && previous.turn.y >= -flick) {
		state::wheel = -1;
	}
	
	if(controls.recenter && !previous.recenter) {
		recenter();
	}
	
	const float deadzone = 0.5f;
	state::previousActions = state::actions;
	state::actions.fill(false);
	state::actions[CONTROLS_CUST_WALKFORWARD] = controls.move.y > deadzone;
	state::actions[CONTROLS_CUST_WALKBACKWARD] = controls.move.y < -deadzone;
	state::actions[CONTROLS_CUST_STRAFELEFT] = controls.move.x < -deadzone;
	state::actions[CONTROLS_CUST_STRAFERIGHT] = controls.move.x > deadzone;
	// Take / use / open what the crosshair points at (the right mouse button toggles free look)
	state::actions[CONTROLS_CUST_USE] = controls.use;
	state::actions[CONTROLS_CUST_JUMP] = controls.jump;
	state::actions[CONTROLS_CUST_CROUCHTOGGLE] = controls.crouch;
	state::actions[CONTROLS_CUST_INVENTORY] = controls.inventory;
	state::actions[CONTROLS_CUST_BOOK] = controls.book;
	state::actions[CONTROLS_CUST_MAGICMODE] = controls.magic;
	state::actions[CONTROLS_CUST_WEAPON] = controls.weapon;
	
}

bool getPointer(Vec2s & position) {
	position = state::pointer;
	return state::pointerValid;
}

bool isMouseButtonPressed(int button) {
	switch(button) {
		case Mouse::Button_0: return state::controls.select;
		default: return false;
	}
}

int getMouseWheel() {
	return state::wheel;
}

bool isKeyPressed(int key) {
	return key == Keyboard::Key_Escape && state::controls.menu;
}

bool isActionPressed(int action) {
	return action >= 0 && action < NUM_ACTION_KEY && state::actions[size_t(action)];
}

bool wasActionPressed(int action) {
	return action >= 0 && action < NUM_ACTION_KEY && state::previousActions[size_t(action)];
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
		if(state::viewsValid && state::recenterRequested) {
			recenterNow();
		}
		if(state::sessionState == XR_SESSION_STATE_FOCUSED) {
			input::sync(state::session, state::localSpace, state::frameState.predictedDisplayTime);
		} else {
			input::sync(XR_NULL_HANDLE, XR_NULL_HANDLE, 0);
		}
		updateControls();
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

//! Save the eye and UI images of every 90th frame (about one per second) for --vr-dump-frames
static void dumpFrame() {
	
	static unsigned frame = 0;
	static unsigned saved = 0;
	if(saved >= state::dumpFrames || ++frame % 90 != 0) {
		return;
	}
	
	fs::path dir = fs::getUserDir() / "vr-frames";
	fs::create_directories(dir);
	
	auto save = [&](const Swapchain & swapchain, std::string_view name) {
		if(!swapchain.acquired) {
			return;
		}
		Image image;
		image.create(size_t(swapchain.size.x), size_t(swapchain.size.y), Image::Format_R8G8B8);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, swapchain.framebuffers[swapchain.current]);
		glReadPixels(0, 0, swapchain.size.x, swapchain.size.y, GL_RGB, GL_UNSIGNED_BYTE, image.getData());
		image.flipY();
		image.save(dir / (std::to_string(saved) + '-' + std::string(name) + ".bmp"));
	};
	if(state::eyes[0].rendered) {
		save(state::eyes[0], "left");
		save(state::eyes[1], "right");
	}
	save(state::ui, "ui");
	
	glBindFramebuffer(GL_FRAMEBUFFER, renderer()->getRenderTarget());
	
	LogInfo << "Saved VR frame " << saved << " to " << dir;
	saved++;
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
		dumpFrame();
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
		// Over the world the panel only has the HUD on a transparent background (premultiplied alpha),
		// otherwise it shows the whole game screen (menu, cinematics) and is opaque
		quad.layerFlags = state::uiOverlay ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
		quad.space = state::localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = state::ui.handle;
		quad.subImage.imageRect.extent = { state::ui.size.x, state::ui.size.y };
		// In front of the recentered head, facing it
		glm::quat facing = glm::angleAxis(state::recenterYaw, Vec3f(0.f, 1.f, 0.f));
		Vec3f position = toVec3(state::recenterPosition) + facing * Vec3f(0.f, 0.f, -config.vr.uiDistance);
		quad.pose.orientation = { facing.x, facing.y, facing.z, facing.w };
		quad.pose.position = { position.x, position.y, position.z };
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
	state::uiOverlay = false;
	
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
	
	if(!input::create(state::instance, state::session)) {
		LogWarning << "VR controllers are not available";
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
		input::destroy();
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
