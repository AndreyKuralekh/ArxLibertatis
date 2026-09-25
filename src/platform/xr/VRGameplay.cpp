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

#include "platform/xr/VRGameplay.h"

#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "core/Config.h"
#include "game/EntityManager.h"
#include "graphics/Draw.h"
#include "graphics/Renderer.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "io/log/Logger.h"
#include "platform/xr/OpenXR.h"
#include "scene/Light.h"


namespace vr {

namespace {

// Hand joints in the order of XrHandJointEXT
enum HandJoint {
	JointPalm = 0,
	JointWrist = 1,
	JointThumbMetacarpal = 2,
	JointIndexMetacarpal = 6,
	JointMiddleMetacarpal = 11,
	JointRingMetacarpal = 16,
	JointLittleMetacarpal = 21
};

struct HandSkeleton {
	std::array<Vec3f, xr::HandJointCount> positions;
	std::array<float, xr::HandJointCount> radii;
};

std::vector<TexturedVertex> g_handVertices;

/*!
 * A simple hand skeleton for runtimes that do not track the fingers while holding controllers:
 * a flat hand pointing where the controller points, fingers curled by the trigger and grip.
 */
bool createSyntheticHand(int hand, HandSkeleton & skeleton) {
	
	Vec3f position;
	glm::mat3 orientation;
	if(!xr::getHandPose(hand, true, position, orientation)) {
		return false;
	}
	Vec3f aimPosition;
	glm::mat3 aimOrientation;
	if(xr::getHandPose(hand, false, aimPosition, aimOrientation)) {
		orientation = aimOrientation;
	}
	
	// Hand space in meters: x right, y down, z forward; the thumb is on the inner side
	const float side = (hand == xr::RightHand) ? 1.f : -1.f;
	const float scale = config.vr.worldScale;
	auto place = [&](Vec3f local) {
		return position + orientation * (local * scale);
	};
	
	skeleton.radii.fill(0.0085f * scale);
	skeleton.positions[JointWrist] = place(Vec3f(0.f, 0.f, -0.08f));
	skeleton.positions[JointPalm] = place(Vec3f(0.f, 0.f, -0.035f));
	skeleton.radii[JointWrist] = skeleton.radii[JointPalm] = 0.02f * scale;
	
	struct Finger {
		HandJoint metacarpal;
		float x;
		float lengths[3];
		float curl;
	};
	const float trigger = xr::getHandTrigger(hand);
	const float squeeze = xr::getHandSqueeze(hand);
	const Finger fingers[] = {
		{ JointIndexMetacarpal, -0.025f * side, { 0.045f, 0.027f, 0.022f }, trigger },
		{ JointMiddleMetacarpal, -0.008f * side, { 0.048f, 0.030f, 0.024f }, squeeze },
		{ JointRingMetacarpal, 0.009f * side, { 0.045f, 0.028f, 0.022f }, squeeze },
		{ JointLittleMetacarpal, 0.025f * side, { 0.036f, 0.021f, 0.019f }, squeeze },
	};
	const float bend[3] = { glm::radians(80.f), glm::radians(95.f), glm::radians(60.f) };
	for(const Finger & finger : fingers) {
		Vec3f knuckle(finger.x, 0.f, 0.f);
		skeleton.positions[finger.metacarpal] = place(Vec3f(finger.x * 0.5f, 0.f, -0.065f));
		skeleton.positions[finger.metacarpal + 1] = place(knuckle);
		float angle = 0.f;
		Vec3f joint = knuckle;
		for(size_t i = 0; i < 3; i++) {
			// Curl towards the palm (down)
			angle += bend[i] * finger.curl;
			joint += Vec3f(0.f, std::sin(angle), std::cos(angle)) * finger.lengths[i];
			skeleton.positions[finger.metacarpal + 2 + i] = place(joint);
		}
	}
	
	skeleton.positions[JointThumbMetacarpal] = place(Vec3f(-0.020f * side, 0.005f, -0.060f));
	skeleton.positions[JointThumbMetacarpal + 1] = place(Vec3f(-0.038f * side, 0.012f, -0.035f));
	skeleton.positions[JointThumbMetacarpal + 2] = place(Vec3f(-0.048f * side, 0.016f, -0.010f));
	skeleton.positions[JointThumbMetacarpal + 3] = place(Vec3f(-0.052f * side, 0.018f, 0.010f));
	
	return true;
}

//! Add a tapered box from a to b, lit by the lights around the player
void addBone(const Vec3f & a, float radiusA, const Vec3f & b, float radiusB,
             ShaderLight lights[], size_t lightsCount, const ColorMod & colorMod) {
	
	Vec3f axis = b - a;
	float length = glm::length(axis);
	if(length < 0.01f) {
		return;
	}
	axis /= length;
	
	Vec3f reference = (std::abs(axis.y) < 0.9f) ? Vec3f(0.f, 1.f, 0.f) : Vec3f(1.f, 0.f, 0.f);
	Vec3f u = glm::normalize(glm::cross(axis, reference));
	Vec3f v = glm::cross(axis, u);
	
	const Vec2f around[4] = { Vec2f(1.f, 1.f), Vec2f(-1.f, 1.f), Vec2f(-1.f, -1.f), Vec2f(1.f, -1.f) };
	Vec3f corners[8];
	for(size_t i = 0; i < 4; i++) {
		corners[i] = a + (u * around[i].x + v * around[i].y) * radiusA;
		corners[i + 4] = b + (u * around[i].x + v * around[i].y) * radiusB;
	}
	
	const size_t faces[6][4] = {
		{ 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }, { 0, 3, 2, 1 }, { 4, 5, 6, 7 }
	};
	Vec3f center = (a + b) * 0.5f;
	for(const auto & face : faces) {
		
		const Vec3f & p0 = corners[face[0]];
		Vec3f faceCenter = (p0 + corners[face[1]] + corners[face[2]] + corners[face[3]]) * 0.25f;
		Vec3f normal = glm::cross(corners[face[1]] - p0, corners[face[2]] - p0);
		if(glm::length(normal) < 1e-6f) {
			continue;
		}
		normal = glm::normalize(normal);
		if(glm::dot(normal, faceCenter - center) < 0.f) {
			normal = -normal;
		}
		
		// Skin tone
		Color light = Color::fromRGBA(ApplyLight(lights, lightsCount, faceCenter, normal, colorMod));
		ColorRGBA color = Color(u8(light.r * 0.95f), u8(light.g * 0.78f), u8(light.b * 0.66f)).toRGBA();
		
		for(size_t index : { face[0], face[1], face[2], face[0], face[2], face[3] }) {
			Vec4f p = worldToClipSpace(corners[index]);
			g_handVertices.emplace_back(Vec3f(p), p.w, color, Vec2f(0.f));
		}
		
	}
	
}

} // anonymous namespace

void prepareHands() {
	
	g_handVertices.clear();
	
	Entity * playerEntity = entities.player();
	if(!playerEntity) {
		return;
	}
	
	ColorMod colorMod;
	colorMod.updateFromEntity(playerEntity);
	
	for(int hand : { xr::LeftHand, xr::RightHand }) {
		
		HandSkeleton skeleton;
		bool tracked = xr::getHandJoints(hand, skeleton.positions.data(), skeleton.radii.data());
		if(!tracked && !createSyntheticHand(hand, skeleton)) {
			continue;
		}
		
		static int s_loggedSource[2] = { -1, -1 };
		if(s_loggedSource[hand] != int(tracked)) {
			s_loggedSource[hand] = int(tracked);
			LogInfo << (hand == xr::RightHand ? "Right" : "Left") << " VR hand: "
			        << (tracked ? "tracked finger joints" : "synthetic fingers from trigger and grip");
		}
		
		ShaderLight lights[llightsSize];
		size_t lightsCount = 0;
		UpdateLlights(lights, lightsCount, skeleton.positions[JointPalm], true);
		
		auto bone = [&](size_t a, size_t b, float thickness = 1.f) {
			addBone(skeleton.positions[a], skeleton.radii[a] * thickness, skeleton.positions[b],
			        skeleton.radii[b] * thickness, lights, lightsCount, colorMod);
		};
		
		// Thumb
		bone(JointWrist, JointThumbMetacarpal, 1.3f);
		for(size_t i = 0; i < 3; i++) {
			bone(JointThumbMetacarpal + i, JointThumbMetacarpal + i + 1);
		}
		
		// Palm: thick metacarpals that overlap, then the fingers
		for(HandJoint metacarpal : { JointIndexMetacarpal, JointMiddleMetacarpal, JointRingMetacarpal, JointLittleMetacarpal }) {
			bone(JointWrist, metacarpal, 1.4f);
			bone(metacarpal, metacarpal + 1, 1.5f);
			for(size_t i = 1; i < 4; i++) {
				bone(metacarpal + i, metacarpal + i + 1);
			}
		}
		bone(JointIndexMetacarpal + 1, JointLittleMetacarpal + 1, 1.2f);
		
	}
	
}

void renderHands() {
	
	if(g_handVertices.empty()) {
		return;
	}
	
	RenderState state = render3D();
	state.setCull(false);
	UseRenderState renderState(state);
	GRenderer->SetTexture(0, static_cast<Texture *>(nullptr));
	
	EERIEDRAWPRIM(Renderer::TriangleList, g_handVertices.data(), g_handVertices.size());
	
}

} // namespace vr
