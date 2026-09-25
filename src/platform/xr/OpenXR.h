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

#ifndef ARX_PLATFORM_XR_OPENXR_H
#define ARX_PLATFORM_XR_OPENXR_H

#include <stddef.h>

#include "Configure.h"

#include "math/Types.h"

#if ARX_HAVE_OPENXR

struct Camera;

/*!
 * OpenXR VR support.
 *
 * All functions must be called from the main thread with the game's OpenGL context current.
 */
namespace xr {

//! \return true if VR mode was requested via the config file or the --vr command-line option
bool isRequested();

//! \return true if verbose OpenXR logging was requested (--vr-debug)
bool isDebug();

/*!
 * Create the OpenXR instance and session for the current OpenGL context.
 *
 * \return true if VR is ready. On failure the game continues in desktop mode.
 */
bool initialize();

//! Destroy the OpenXR session and instance. Safe to call if VR was never initialized.
void shutdown();

//! \return true if an OpenXR session exists
bool isActive();

/*!
 * Size of the VR UI panel in pixels.
 *
 * In VR the 2D interface (menus, HUD, cinematics) is laid out for this size instead of the window size.
 */
Vec2i getUiSize();

/*!
 * Wait for the next VR frame, begin it and locate the eye views.
 *
 * Does nothing if a frame has already been begun and not yet ended.
 */
void beginFrame();

/*!
 * Submit the current VR frame to the compositor.
 *
 * Called when the game presents a frame. Begins a frame first if needed.
 */
void endFrame();

//! \return true if the eye views of the current frame should be rendered
bool hasEyeViews();

/*!
 * Apply the tracked head pose to a game camera.
 *
 * The base camera position is where the head was when the view was last recentered.
 * Also computes the eye view and projection matrices.
 *
 * \param playerView true for the first-person player camera: the body direction is tracked
 *                   separately and turns of the player by the game are added to it.
 *                   Otherwise the yaw of the base camera is the body direction.
 *
 * \return a camera at the tracked head position with a field of view that covers both eyes.
 *         The camera stays valid until the next call.
 */
Camera * applyHeadPose(const Camera & base, bool playerView);

//! Yaw the player should face: body direction plus head yaw (valid after applyHeadPose() for the player)
float getPlayerYaw();

//! Number of eye views
constexpr size_t EyeCount = 2;

/*!
 * Render into an eye image. World geometry uses the eye's view and projection, pre-transformed
 * vertices projected for the camera from applyHeadPose() are re-projected into the eye.
 */
void bindEye(size_t eye);

//! Render into the UI panel again, with the camera from applyHeadPose()
void bindUi(bool clear);

//! Place the UI panel and the game camera origin at the current head pose
void recenter();

/*
 * Hands in the game world, valid after applyHeadPose() in the current frame.
 * Orientations map from the game's view conventions (x right, y down, z forward) to world space.
 */

constexpr int LeftHand = 0;
constexpr int RightHand = 1;

//! Number of hand joints, in the order of XrHandJointEXT (palm, wrist, thumb, index, middle, ring, little)
constexpr size_t HandJointCount = 26;

//! \param grip true for the holding pose, false for the pointing pose
bool getHandPose(int hand, bool grip, Vec3f & position, glm::mat3 & orientation);

//! Finger joint positions and radii in world units, if the runtime tracks the fingers
bool getHandJoints(int hand, Vec3f * positions, float * radii);

/*!
 * Position of a point attached to the holding pose of a hand in the tracking space, in meters.
 *
 * Unlike world positions this does not change when the player moves or turns with the thumbsticks,
 * so it measures real hand motion (e.g. the speed of a swing).
 *
 * \param offset Offset from the holding pose in the game's view conventions, in world units.
 */
bool getHandPointInTracking(int hand, const Vec3f & offset, Vec3f & position);

//! Analog trigger and grip values from 0 to 1
float getHandTrigger(int hand);
float getHandSqueeze(int hand);

/*
 * VR controller input for the game, updated once per frame. The right controller points at the
 * UI panel like a mouse, the other buttons act as game actions. Mouse button and key ids are
 * those from input/Mouse.h and input/Keyboard.h, actions are ControlAction values.
 */

//! \return true if the controller points at the UI panel, with the position in panel pixels
bool getPointer(Vec2s & position);

bool isMouseButtonPressed(int button);

//! \return 1 for wheel up, -1 for wheel down or 0
int getMouseWheel();

bool isKeyPressed(int key);

bool isActionPressed(int action);

//! \return the action state of the previous frame
bool wasActionPressed(int action);

} // namespace xr

#endif // ARX_HAVE_OPENXR

#endif // ARX_PLATFORM_XR_OPENXR_H
