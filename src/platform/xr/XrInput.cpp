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
	
	XrAction aim = XR_NULL_HANDLE;
	XrSpace aimSpace = XR_NULL_HANDLE;
	
	XrAction move = XR_NULL_HANDLE;
	XrAction turn = XR_NULL_HANDLE;
	
	std::vector<BooleanAction> buttons;
	
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

XrAction createAction(XrActionType type, const char * name, const char * localizedName) {
	
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
	std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
	
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

} // anonymous namespace

bool create(XrInstance instance, XrSession session) {
	
	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	std::snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "gameplay");
	std::snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Gameplay");
	if(!xrInputCheck(xrCreateActionSet(instance, &setInfo, &g_xrInput.set), "xrCreateActionSet")) {
		return false;
	}
	
	ActionState & a = g_xrInput;
	a.aim = createAction(XR_ACTION_TYPE_POSE_INPUT, "aim", "Point");
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
		{ a.aim, "/user/hand/right/input/aim/pose" },
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
		{ a.aim, "/user/hand/right/input/aim/pose" },
		{ select, "/user/hand/right/input/select/click" },
		{ menu, "/user/hand/left/input/menu/click" },
	});
	
	if(!touch && !simple) {
		return false;
	}
	
	XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	spaceInfo.action = a.aim;
	spaceInfo.poseInActionSpace.orientation.w = 1.f;
	if(!xrInputCheck(xrCreateActionSpace(session, &spaceInfo, &a.aimSpace), "xrCreateActionSpace")) {
		return false;
	}
	
	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &a.set;
	if(!xrInputCheck(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets")) {
		return false;
	}
	
	LogInfo << "OpenXR controller actions ready";
	
	return true;
}

void destroy() {
	
	if(g_xrInput.aimSpace != XR_NULL_HANDLE) {
		xrDestroySpace(g_xrInput.aimSpace);
	}
	if(g_xrInput.set != XR_NULL_HANDLE) {
		// Also destroys the actions
		xrDestroyActionSet(g_xrInput.set);
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
	
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(XR_SUCCEEDED(xrLocateSpace(a.aimSpace, space, time, &location))) {
		const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		if((location.locationFlags & valid) == valid) {
			a.controls.aimValid = true;
			a.controls.aim = location.pose;
		}
	}
	
}

const Controls & getControls() {
	return g_xrInput.controls;
}

} // namespace xr::input
