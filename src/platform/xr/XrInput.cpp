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

#include "platform/xr/XrInput.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "io/log/Logger.h"


namespace xr::input {

namespace {

struct BooleanAction {
	XrAction action = XR_NULL_HANDLE;
	bool Controls::* field = nullptr;
};

struct ActionState {
	
	XrActionSet set = XR_NULL_HANDLE;
	
	std::array<XrPath, HandCount> handPaths = { XR_NULL_PATH, XR_NULL_PATH };
	
	// Per hand, using subaction paths
	XrAction aim = XR_NULL_HANDLE;
	XrAction grip = XR_NULL_HANDLE;
	XrAction trigger = XR_NULL_HANDLE;
	XrAction squeeze = XR_NULL_HANDLE;
	std::array<XrSpace, HandCount> aimSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	std::array<XrSpace, HandCount> gripSpaces = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	
	XrAction move = XR_NULL_HANDLE;
	XrAction turn = XR_NULL_HANDLE;
	
	std::vector<BooleanAction> buttons;
	
	// XR_EXT_hand_tracking
	PFN_xrCreateHandTrackerEXT createHandTracker = nullptr;
	PFN_xrDestroyHandTrackerEXT destroyHandTracker = nullptr;
	PFN_xrLocateHandJointsEXT locateHandJoints = nullptr;
	std::array<XrHandTrackerEXT, HandCount> handTrackers = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	
	Controls controls;
	
};

ActionState g_xrInput;

bool xrInputCheck(XrResult result, const char * what) {
	if(XR_FAILED(result)) {
		LogError << "OpenXR input: " << what << " failed: XrResult(" << int(result) << ')';
		return false;
	}
	return true;
}

XrAction createAction(XrActionType type, const char * name, const char * localizedName, bool perHand = false) {
	
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
	std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
	if(perHand) {
		info.countSubactionPaths = uint32_t(g_xrInput.handPaths.size());
		info.subactionPaths = g_xrInput.handPaths.data();
	}
	
	XrAction action = XR_NULL_HANDLE;
	if(!xrInputCheck(xrCreateAction(g_xrInput.set, &info, &action), name)) {
		return XR_NULL_HANDLE;
	}
	return action;
}

XrPath toPath(XrInstance instance, const char * string) {
	XrPath path = XR_NULL_PATH;
	xrStringToPath(instance, string, &path);
	return path;
}

struct Binding {
	XrAction action;
	const char * path;
};

bool suggestBindings(XrInstance instance, const char * profile, const std::vector<Binding> & bindings) {
	
	std::vector<XrActionSuggestedBinding> suggested;
	for(const Binding & binding : bindings) {
		if(binding.action != XR_NULL_HANDLE) {
			suggested.push_back({ binding.action, toPath(instance, binding.path) });
		}
	}
	
	XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	info.interactionProfile = toPath(instance, profile);
	info.countSuggestedBindings = uint32_t(suggested.size());
	info.suggestedBindings = suggested.data();
	
	XrResult result = xrSuggestInteractionProfileBindings(instance, &info);
	if(XR_FAILED(result)) {
		LogWarning << "OpenXR input: bindings for " << profile << " rejected: XrResult(" << int(result) << ')';
		return false;
	}
	return true;
}

XrSpace createHandSpace(XrSession session, XrAction action, Hand hand) {
	XrActionSpaceCreateInfo info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	info.action = action;
	info.subactionPath = g_xrInput.handPaths[hand];
	info.poseInActionSpace.orientation.w = 1.f;
	XrSpace space = XR_NULL_HANDLE;
	xrInputCheck(xrCreateActionSpace(session, &info, &space), "xrCreateActionSpace");
	return space;
}

bool locate(XrSpace handSpace, XrSpace space, XrTime time, XrPosef & pose) {
	if(handSpace == XR_NULL_HANDLE) {
		return false;
	}
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(XR_FAILED(xrLocateSpace(handSpace, space, time, &location))) {
		return false;
	}
	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if((location.locationFlags & valid) != valid) {
		return false;
	}
	pose = location.pose;
	return true;
}

float getFloat(XrSession session, XrAction action, Hand hand) {
	XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = g_xrInput.handPaths[hand];
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
	if(XR_SUCCEEDED(xrGetActionStateFloat(session, &getInfo, &state)) && state.isActive) {
		return state.currentState;
	}
	return 0.f;
}

void createHandTrackers(XrInstance instance, XrSession session, bool controllerHands) {
	
	ActionState & a = g_xrInput;
	if(XR_FAILED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
	                                   reinterpret_cast<PFN_xrVoidFunction *>(&a.createHandTracker)))
	   || XR_FAILED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
	                                      reinterpret_cast<PFN_xrVoidFunction *>(&a.destroyHandTracker)))
	   || XR_FAILED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
	                                      reinterpret_cast<PFN_xrVoidFunction *>(&a.locateHandJoints)))) {
		LogWarning << "OpenXR input: hand tracking functions not available";
		return;
	}
	
	// Also get finger joints (estimated from the controller) while holding controllers
	XrHandTrackingDataSourceEXT sources[] = {
		XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
		XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT
	};
	XrHandTrackingDataSourceInfoEXT sourceInfo = { XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT };
	sourceInfo.requestedDataSourceCount = uint32_t(std::size(sources));
	sourceInfo.requestedDataSources = sources;
	
	for(Hand hand : { LeftHand, RightHand }) {
		XrHandTrackerCreateInfoEXT info = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
		info.next = controllerHands ? &sourceInfo : nullptr;
		info.hand = (hand == LeftHand) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
		info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
		if(!xrInputCheck(a.createHandTracker(session, &info, &a.handTrackers[hand]), "xrCreateHandTrackerEXT")) {
			a.handTrackers[hand] = XR_NULL_HANDLE;
		}
	}
	
	LogInfo << "OpenXR hand tracking ready" << (controllerHands ? " (also with controllers)" : "");
}

} // anonymous namespace

bool create(XrInstance instance, XrSession session, bool handTracking, bool controllerHands) {
	
	ActionState & a = g_xrInput;
	a.handPaths = { toPath(instance, "/user/hand/left"), toPath(instance, "/user/hand/right") };
	
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	std::snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "gameplay");
	std::snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Gameplay");
	if(!xrInputCheck(xrCreateActionSet(instance, &setInfo, &a.set), "xrCreateActionSet")) {
		return false;
	}
	
	a.aim = createAction(XR_ACTION_TYPE_POSE_INPUT, "aim", "Point", true);
	a.grip = createAction(XR_ACTION_TYPE_POSE_INPUT, "grip", "Hold", true);
	a.trigger = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_value", "Trigger", true);
	a.squeeze = createAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_value", "Grip", true);
	a.move = createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
	a.turn = createAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Turn and scroll");
	
	auto button = [&](const char * name, const char * localizedName, bool Controls::* field) {
		BooleanAction action;
		action.action = createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, name, localizedName);
		action.field = field;
		a.buttons.push_back(action);
		return action.action;
	};
	XrAction select = button("select", "Select / attack", &Controls::select);
	XrAction use = button("use", "Use", &Controls::use);
	XrAction jump = button("jump", "Jump", &Controls::jump);
	XrAction crouch = button("crouch", "Crouch", &Controls::crouch);
	XrAction inventory = button("inventory", "Inventory", &Controls::inventory);
	XrAction book = button("book", "Book", &Controls::book);
	XrAction magic = button("magic", "Magic mode", &Controls::magic);
	XrAction weapon = button("weapon", "Weapon", &Controls::weapon);
	XrAction menu = button("menu", "Menu", &Controls::menu);
	XrAction recenter = button("recenter", "Recenter view", &Controls::recenter);
	XrAction freelook = button("freelook", "Free look / cursor", &Controls::freelook);
	
	bool touch = suggestBindings(instance, "/interaction_profiles/oculus/touch_controller", {
		{ a.aim, "/user/hand/left/input/aim/pose" },
		{ a.aim, "/user/hand/right/input/aim/pose" },
		{ a.grip, "/user/hand/left/input/grip/pose" },
		{ a.grip, "/user/hand/right/input/grip/pose" },
		{ a.trigger, "/user/hand/left/input/trigger/value" },
		{ a.trigger, "/user/hand/right/input/trigger/value" },
		{ a.squeeze, "/user/hand/left/input/squeeze/value" },
		{ a.squeeze, "/user/hand/right/input/squeeze/value" },
		{ a.move, "/user/hand/left/input/thumbstick" },
		{ a.turn, "/user/hand/right/input/thumbstick" },
		{ select, "/user/hand/right/input/trigger/value" },
		{ use, "/user/hand/right/input/squeeze/value" },
		{ jump, "/user/hand/right/input/a/click" },
		{ crouch, "/user/hand/right/input/b/click" },
		{ inventory, "/user/hand/left/input/x/click" },
		{ book, "/user/hand/left/input/y/click" },
		{ magic, "/user/hand/left/input/squeeze/value" },
		{ weapon, "/user/hand/left/input/trigger/value" },
		{ menu, "/user/hand/left/input/menu/click" },
		{ recenter, "/user/hand/right/input/thumbstick/click" },
		{ freelook, "/user/hand/left/input/thumbstick/click" },
	});
	
	bool simple = suggestBindings(instance, "/interaction_profiles/khr/simple_controller", {
		{ a.aim, "/user/hand/left/input/aim/pose" },
		{ a.aim, "/user/hand/right/input/aim/pose" },
		{ a.grip, "/user/hand/left/input/grip/pose" },
		{ a.grip, "/user/hand/right/input/grip/pose" },
		{ select, "/user/hand/right/input/select/click" },
		{ menu, "/user/hand/left/input/menu/click" },
	});
	
	if(!touch && !simple) {
		return false;
	}
	
	for(Hand hand : { LeftHand, RightHand }) {
		a.aimSpaces[hand] = createHandSpace(session, a.aim, hand);
		a.gripSpaces[hand] = createHandSpace(session, a.grip, hand);
	}
	
	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &a.set;
	if(!xrInputCheck(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets")) {
		return false;
	}
	
	if(handTracking) {
		createHandTrackers(instance, session, controllerHands);
	}
	
	LogInfo << "OpenXR controller actions ready";
	
	return true;
}

void destroy() {
	
	ActionState & a = g_xrInput;
	for(XrHandTrackerEXT tracker : a.handTrackers) {
		if(tracker != XR_NULL_HANDLE && a.destroyHandTracker) {
			a.destroyHandTracker(tracker);
		}
	}
	for(Hand hand : { LeftHand, RightHand }) {
		for(XrSpace space : { a.aimSpaces[hand], a.gripSpaces[hand] }) {
			if(space != XR_NULL_HANDLE) {
				xrDestroySpace(space);
			}
		}
	}
	if(a.set != XR_NULL_HANDLE) {
		// Also destroys the actions
		xrDestroyActionSet(a.set);
	}
	g_xrInput = ActionState();
	
}

void sync(XrSession session, XrSpace space, XrTime time) {
	
	ActionState & a = g_xrInput;
	a.controls = Controls();
	if(a.set == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
		return;
	}
	
	XrActiveActionSet activeSet = { a.set, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	XrResult result = xrSyncActions(session, &syncInfo);
	if(result != XR_SUCCESS) {
		// XR_SESSION_NOT_FOCUSED: no input while another application has focus
		return;
	}
	
	for(const BooleanAction & button : a.buttons) {
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = button.action;
		XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		if(XR_SUCCEEDED(xrGetActionStateBoolean(session, &getInfo, &state)) && state.isActive) {
			a.controls.*button.field = state.currentState != XR_FALSE;
		}
	}
	
	for(auto [action, value] : { std::pair(a.move, &a.controls.move), std::pair(a.turn, &a.controls.turn) }) {
		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.action = action;
		XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
		if(XR_SUCCEEDED(xrGetActionStateVector2f(session, &getInfo, &state)) && state.isActive) {
			*value = state.currentState;
		}
	}
	
	for(Hand hand : { LeftHand, RightHand }) {
		
		HandState & state = a.controls.hands[hand];
		state.aimValid = locate(a.aimSpaces[hand], space, time, state.aim);
		state.gripValid = locate(a.gripSpaces[hand], space, time, state.grip);
		state.trigger = getFloat(session, a.trigger, hand);
		state.squeeze = getFloat(session, a.squeeze, hand);
		
		if(a.handTrackers[hand] != XR_NULL_HANDLE) {
			XrHandJointsLocateInfoEXT locateInfo = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
			locateInfo.baseSpace = space;
			locateInfo.time = time;
			XrHandJointLocationsEXT locations = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
			locations.jointCount = uint32_t(state.joints.size());
			locations.jointLocations = state.joints.data();
			if(XR_SUCCEEDED(a.locateHandJoints(a.handTrackers[hand], &locateInfo, &locations))) {
				state.jointsValid = locations.isActive != XR_FALSE;
			}
		}
		
	}
	
	a.controls.aimValid = a.controls.hands[RightHand].aimValid;
	a.controls.aim = a.controls.hands[RightHand].aim;
	
}

const Controls & getControls() {
	return g_xrInput.controls;
}

} // namespace xr::input
