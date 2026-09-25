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
 * Melee combat by swinging the controllers: the drawn weapon follows the right hand and hits
 * what it touches while it moves fast enough, bare hands punch.
 *
 * Call once per frame after the camera was updated. Replaces the animation driven melee strikes.
 */
void updateCombat();

} // namespace vr

#endif // ARX_HAVE_OPENXR

#endif // ARX_PLATFORM_XR_VRGAMEPLAY_H
