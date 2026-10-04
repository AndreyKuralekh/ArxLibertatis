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

#include "platform/xr/VRHandModel.h"

#if ARX_HAVE_OPENXR

#include <cstring>
#include <iterator>

#include <glm/gtc/matrix_transform.hpp>
#include <string>

#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "platform/xr/OpenXR.h"

namespace vr {

namespace {

//! Sequential reader for the little endian arrays of the model file
class Reader {
	
	const std::string & m_data;
	size_t m_pos = 0;
	bool m_ok = true;

public:
	
	explicit Reader(const std::string & data) : m_data(data) { }
	
	template <typename T>
	void read(T * values, size_t count) {
		size_t size = sizeof(T) * count;
		if(!m_ok || size > m_data.size() - m_pos) {
			m_ok = false;
			return;
		}
		std::memcpy(values, m_data.data() + m_pos, size);
		m_pos += size;
	}
	
	bool ok() const { return m_ok; }
	bool atEnd() const { return m_ok && m_pos == m_data.size(); }
	
};

} // anonymous namespace

bool HandModel::load(const res::path & file) {
	
	m_indices.clear();
	
	std::string data = g_resources->read(file);
	if(data.empty()) {
		LogWarning << "VR hand model not found: " << file;
		return false;
	}
	
	// See tools/vr/convert-hand-model.py for the layout
	Reader reader(data);
	char magic[4] = { };
	u32 header[3] = { };
	reader.read(magic, 4);
	reader.read(header, 3);
	const u32 version = header[0], vertices = header[1], indices = header[2];
	if(!reader.ok() || std::memcmp(magic, "AVRH", 4) != 0 || version != 1
	   || vertices == 0 || vertices > 65535 || indices == 0 || indices % 3 != 0 || indices > 1000000) {
		LogWarning << "Bad VR hand model: " << file;
		return false;
	}
	
	static_assert(sizeof(Vec3f) == 3 * sizeof(float), "unexpected vector layout");
	static_assert(sizeof(glm::mat4x4) == 16 * sizeof(float), "unexpected matrix layout");
	
	std::vector<Vec2f> uvs(vertices);
	std::vector<u16> triangles(indices);
	m_positions.resize(vertices);
	m_normals.resize(vertices);
	m_joints.resize(vertices);
	m_weights.resize(vertices);
	reader.read(m_positions.data(), vertices);
	reader.read(m_normals.data(), vertices);
	reader.read(uvs.data(), vertices);
	reader.read(m_joints.data(), vertices);
	reader.read(m_weights.data(), vertices);
	reader.read(triangles.data(), indices);
	reader.read(m_inverseBind.data(), JointCount);
	
	bool valid = reader.atEnd();
	for(size_t i = 0; valid && i < indices; i++) {
		valid = triangles[i] < vertices;
	}
	for(size_t i = 0; valid && i < vertices; i++) {
		for(u8 joint : m_joints[i]) {
			valid = valid && joint < JointCount;
		}
	}
	if(!valid) {
		LogWarning << "Bad VR hand model: " << file;
		return false;
	}
	
	for(size_t i = 0; i < JointCount; i++) {
		m_bind[i] = glm::inverse(m_inverseBind[i]);
	}
	
	m_indices = std::move(triangles);
	
	LogInfo << "Loaded VR hand model " << file << ": " << vertices << " vertices, " << (indices / 3) << " triangles";
	
	return true;
}

void HandModel::skin(const glm::mat4x4 * joints, std::vector<Vec3f> & positions,
                     std::vector<Vec3f> & normals) const {
	
	std::array<glm::mat4x4, JointCount> matrices;
	for(size_t i = 0; i < JointCount; i++) {
		matrices[i] = joints[i] * m_inverseBind[i];
	}
	
	positions.resize(m_positions.size());
	normals.resize(m_positions.size());
	for(size_t i = 0; i < m_positions.size(); i++) {
		Vec4f position(m_positions[i], 1.f);
		Vec4f normal(m_normals[i], 0.f);
		Vec3f skinnedPosition(0.f);
		Vec3f skinnedNormal(0.f);
		for(size_t k = 0; k < MaxInfluences; k++) {
			float weight = m_weights[i][k];
			if(weight > 0.f) {
				const glm::mat4x4 & matrix = matrices[m_joints[i][k]];
				skinnedPosition += Vec3f(matrix * position) * weight;
				skinnedNormal += Vec3f(matrix * normal) * weight;
			}
		}
		float length = glm::length(skinnedNormal);
		positions[i] = skinnedPosition;
		normals[i] = (length > 1e-6f) ? skinnedNormal / length : Vec3f(0.f, -1.f, 0.f);
	}
	
}

void HandModel::curlFingers(const float * curls, glm::mat4x4 * joints) const {
	
	// XrHandJointEXT: palm, wrist, then four thumb joints and five joints for each other finger,
	// each from the metacarpal to the tip
	const size_t wrist = 1;
	const size_t first[FingerCount] = { 2, 6, 11, 16, 21 };
	const size_t count[FingerCount] = { 4, 5, 5, 5, 5 };
	// Bend at the joints after the metacarpal for a fist (degrees); the thumb bends less
	const float thumbBend[2] = { 35.f, 50.f };
	const float fingerBend[3] = { 80.f, 100.f, 60.f };
	
	std::array<glm::mat4x4, JointCount> posed = m_bind;
	for(size_t finger = 0; finger < FingerCount; finger++) {
		size_t tip = first[finger] + count[finger] - 1;
		size_t bends = (finger == 0) ? std::size(thumbBend) : std::size(fingerBend);
		for(size_t i = 0; i < bends; i++) {
			size_t joint = first[finger] + 1 + i;
			float angle = ((finger == 0) ? thumbBend[i] : fingerBend[i]) * glm::clamp(curls[finger], 0.f, 1.f);
			// In the space of a joint -z points to the fingertip and +y out of the back of the hand:
			// turn around x so that the fingertip moves towards the palm, for this joint and all after it
			glm::mat4x4 bend = glm::rotate(glm::mat4x4(1.f), glm::radians(-angle), Vec3f(1.f, 0.f, 0.f));
			glm::mat4x4 delta = posed[joint] * bend * glm::inverse(posed[joint]);
			for(size_t k = joint; k <= tip; k++) {
				posed[k] = delta * posed[k];
			}
		}
	}
	
	// Hang the posed model on the wrist
	glm::mat4x4 modelToWorld = joints[wrist] * m_inverseBind[wrist];
	for(size_t i = 0; i < JointCount; i++) {
		if(i != wrist) {
			joints[i] = modelToWorld * posed[i];
		}
	}
	
}

const HandModel * getHandModel(int hand) {
	
	static HandModel models[2];
	static bool tried[2] = { false, false };
	
	if(hand != xr::LeftHand && hand != xr::RightHand) {
		return nullptr;
	}
	
	size_t i = (hand == xr::RightHand) ? 1 : 0;
	if(!tried[i]) {
		tried[i] = true;
		models[i].load(i ? "graph/obj3d/vr/hand_right.vrhand" : "graph/obj3d/vr/hand_left.vrhand");
	}
	
	return models[i].isLoaded() ? &models[i] : nullptr;
}

} // namespace vr

#endif // ARX_HAVE_OPENXR
