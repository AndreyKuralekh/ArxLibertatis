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

#ifndef ARX_PLATFORM_XR_XRINPUT_H
#define ARX_PLATFORM_XR_XRINPUT_H

#include <array>

#include <openxr/openxr.h>

/*!
 * VR controller input via OpenXR actions.
 *
 * Internal to the OpenXR module: the game sees the controllers through the mouse, key and
 * action state provided by platform/xr/OpenXR.h.
 */
namespace xr::input {

//! Pose, analog values and finger joints of one hand
struct HandState {
	
	bool gripValid = false;
	XrPosef grip = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }; //!< Holding pose in the local space
	bool aimValid = false;
	XrPosef aim = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }; //!< Pointing pose in the local space
	
	float trigger = 0.f;
	float squeeze = 0.f;
	
	//! Finger joints from hand tracking (XR_EXT_hand_tracking) in the local space, if available
	bool jointsValid = false;
	std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints;
	
};

enum Hand {
	LeftHand = 0,
	RightHand = 1,
	HandCount = 2
};

//! Raw controller state of one frame
struct Controls {
	
	std::array<HandState, HandCount> hands;
	
	bool aimValid = false;
	XrPosef aim = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }; //!< Right hand pointing pose in the local space
	
	bool select = false;    //!< Right trigger
	bool use = false;       //!< Right grip (use / take)
	bool jump = false;      //!< A
	bool crouch = false;    //!< B
	bool inventory = false; //!< X
	bool book = false;      //!< Y
	bool magic = false;     //!< Left grip
	bool weapon = false;    //!< Left trigger
	bool menu = false;      //!< Menu button
	bool recenter = false;  //!< Right thumbstick click
	bool freelook = false;  //!< Left thumbstick click: toggle between free look and the cursor
	
	XrVector2f move = { 0.f, 0.f }; //!< Left thumbstick
	XrVector2f turn = { 0.f, 0.f }; //!< Right thumbstick
	
};

/*!
 * Create the actions and attach them to the session.
 *
 * \param handTracking     XR_EXT_hand_tracking is enabled on the instance
 * \param controllerHands XR_EXT_hand_tracking_data_source is enabled: also get finger joints while holding controllers
 */
bool create(XrInstance instance, XrSession session, bool handTracking, bool controllerHands);

//! Destroy the actions and action spaces
void destroy();

//! Read the controller state for the given display time. Only valid while the session is focused.
void sync(XrSession session, XrSpace space, XrTime time);

//! Controller state from the last sync()
const Controls & getControls();

} // namespace xr::input

#endif // ARX_PLATFORM_XR_XRINPUT_H
