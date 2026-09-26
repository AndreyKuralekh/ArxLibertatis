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

#ifndef ARX_PLATFORM_XR_VRGAMEPLAY_H
#define ARX_PLATFORM_XR_VRGAMEPLAY_H

#include "Configure.h"

#if ARX_HAVE_OPENXR

#include "math/Types.h"

class RenderBatcher;

/*!
 * VR interaction with the game world: the player's hands and what they hold.
 *
 * Uses the hand poses from platform/xr/OpenXR.h, so it only works after xr::applyHeadPose()
 * in the current frame.
 */
namespace vr {

/*!
 * Build the geometry of the player's hands for this frame.
 *
 * Must be called once per frame after the camera, lights and scene were updated
 * and before the eye views are rendered.
 */
void prepareHands();

//! Draw the hands (and what they hold) prepared this frame into the current eye view
void renderHands();

/*!
 * Take items with the right hand: grip near an item holds it, releasing the grip drops or throws it,
 * releasing it next to the head puts it into the inventory.
 *
 * Call once per frame after the camera was updated.
 */
void updateGrab();

/*!
 * Rune drawing with the right index finger in magic mode.
 *
 * When a stroke starts (trigger pressed) a plane in front of the head is fixed. The fingertip is
 * projected onto it for the rune recognizer, and the magic flares appear at the fingertip.
 *
 * Call once per frame after the camera was updated.
 */
void updateMagic();

//! Point for the rune recognizer (in pixels, about 10 per cm of hand motion), if drawing with the hand
bool getRuneRecognitionPoint(Vec2s & point);

//! Screen position of the drawing fingertip for the magic flares, if drawing with the hand
bool getRuneScreenPoint(Vec2s & point);

/*!
 * Melee combat by swinging the controllers: the drawn weapon follows the right hand and hits
 * what it touches while it moves fast enough, bare hands punch.
 *
 * Call once per frame after the camera was updated. Replaces the animation driven melee strikes.
 */
void updateCombat();

//! Batcher for 2D effects that must go to the UI panel instead of the eye views
RenderBatcher & getUiBatcher();

//! Draw and clear the UI batcher (with the UI panel as the render target)
void renderUiBatcher();

} // namespace vr

#endif // ARX_HAVE_OPENXR

#endif // ARX_PLATFORM_XR_VRGAMEPLAY_H
