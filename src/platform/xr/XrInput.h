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

#include <openxr/openxr.h>

/*!
 * VR controller input via OpenXR actions.
 *
 * Internal to the OpenXR module: the game sees the controllers through the mouse, key and
 * action state provided by platform/xr/OpenXR.h.
 */
namespace xr::input {

//! Raw controller state of one frame
struct Controls {
	
	bool aimValid = false;
	XrPosef aim = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }; //!< Right hand pointing pose in the local space
	
	bool select = false;    //!< Right trigger
	bool use = false;       //!< Right grip
	bool jump = false;      //!< A
	bool crouch = false;    //!< B
	bool inventory = false; //!< X
	bool book = false;      //!< Y
	bool magic = false;     //!< Left grip
	bool weapon = false;    //!< Left trigger
	bool menu = false;      //!< Menu button
	bool recenter = false;  //!< Right thumbstick click
	
	XrVector2f move = { 0.f, 0.f }; //!< Left thumbstick
	XrVector2f turn = { 0.f, 0.f }; //!< Right thumbstick
	
};

//! Create the actions and attach them to the session
bool create(XrInstance instance, XrSession session);

//! Destroy the actions and action spaces
void destroy();

//! Read the controller state for the given display time. Only valid while the session is focused.
void sync(XrSession session, XrSpace space, XrTime time);

//! Controller state from the last sync()
const Controls & getControls();

} // namespace xr::input

#endif // ARX_PLATFORM_XR_XRINPUT_H
