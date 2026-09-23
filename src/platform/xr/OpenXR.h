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

#include "Configure.h"

#if ARX_HAVE_OPENXR

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

} // namespace xr

#endif // ARX_HAVE_OPENXR

#endif // ARX_PLATFORM_XR_OPENXR_H
