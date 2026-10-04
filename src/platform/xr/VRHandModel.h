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

#ifndef ARX_PLATFORM_XR_VRHANDMODEL_H
#define ARX_PLATFORM_XR_VRHANDMODEL_H

#include "Configure.h"

#if ARX_HAVE_OPENXR

#include <array>
#include <vector>

#include "io/resource/ResourcePath.h"
#include "math/Types.h"
#include "platform/Platform.h"

namespace vr {

/*!
 * A hand mesh skinned to the finger joints of OpenXR hand tracking (XrHandJointEXT).
 *
 * Loaded from the files written by tools/vr/convert-hand-model.py.
 */
class HandModel {

public:
	
	static constexpr size_t JointCount = 26;
	static constexpr size_t MaxInfluences = 4;
	
	bool load(const res::path & file);
	
	bool isLoaded() const { return !m_indices.empty(); }
	
	size_t vertexCount() const { return m_positions.size(); }
	
	//! Three vertices per triangle
	const std::vector<u16> & indices() const { return m_indices; }
	
	/*!
	 * Move the vertices with the joints.
	 *
	 * \param joints    Transform to the world for each of the JointCount joints.
	 * \param positions Receives vertexCount() world positions.
	 * \param normals   Receives vertexCount() unit normals in the world.
	 */
	void skin(const glm::mat4x4 * joints, std::vector<Vec3f> & positions, std::vector<Vec3f> & normals) const;

private:
	
	std::vector<Vec3f> m_positions;
	std::vector<Vec3f> m_normals;
	std::vector<std::array<u8, MaxInfluences>> m_joints;
	std::vector<std::array<float, MaxInfluences>> m_weights;
	std::vector<u16> m_indices;
	std::array<glm::mat4x4, JointCount> m_inverseBind;
	
};

/*!
 * The model for a hand (xr::LeftHand or xr::RightHand), loaded on first use.
 *
 * \return nullptr if the model is not available.
 */
const HandModel * getHandModel(int hand);

} // namespace vr

#endif // ARX_HAVE_OPENXR

#endif // ARX_PLATFORM_XR_VRHANDMODEL_H
