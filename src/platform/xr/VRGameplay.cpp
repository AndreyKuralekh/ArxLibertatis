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

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtx/quaternion.hpp>

#include "animation/AnimationRender.h"
#include "animation/Skeleton.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "game/Damage.h"
#include "game/Camera.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "game/Item.h"
#include "game/Player.h"
#include "graphics/Draw.h"
#include "graphics/Raycast.h"
#include "graphics/RenderBatcher.h"
#include "graphics/Renderer.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "graphics/particle/MagicFlare.h"
#include "graphics/particle/ParticleEffects.h"
#include "graphics/particle/Spark.h"
#include "gui/Interface.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "platform/xr/OpenXR.h"
#include "platform/xr/VRHandModel.h"
#include "physics/Collisions.h"
#include "physics/Physics.h"
#include "scene/Interactive.h"
#include "scene/GameSound.h"
#include "scene/Light.h"
#include "script/Script.h"


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
	const float scale = xr::getWorldScale();
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

//! Add a hand model moved by the tracked finger joints, lit smoothly by the lights around the player
void addHandModel(const HandModel & model, const glm::mat4x4 * joints,
                  ShaderLight lights[], size_t lightsCount, const ColorMod & colorMod) {
	
	static std::vector<Vec3f> positions;
	static std::vector<Vec3f> normals;
	static std::vector<TexturedVertex> vertices;
	
	model.skin(joints, positions, normals);
	
	vertices.clear();
	vertices.reserve(positions.size());
	for(size_t i = 0; i < positions.size(); i++) {
		// Brown leather glove
		Color light = Color::fromRGBA(ApplyLight(lights, lightsCount, positions[i], normals[i], colorMod));
		ColorRGBA color = Color(u8(light.r * 0.70f), u8(light.g * 0.50f), u8(light.b * 0.34f)).toRGBA();
		Vec4f p = worldToClipSpace(positions[i]);
		vertices.emplace_back(Vec3f(p), p.w, color, Vec2f(0.f));
	}
	
	g_handVertices.reserve(g_handVertices.size() + model.indices().size());
	for(u16 index : model.indices()) {
		g_handVertices.push_back(vertices[index]);
	}
	
}

/*
 * Weapon held in the right hand
 */

struct WeaponGrip {
	
	EERIE_3DOBJ * obj = nullptr;
	glm::quat calibration = quat_identity(); //!< Rotates the blade direction of the model onto the hand's forward axis
	Vec3f attach = Vec3f(0.f); //!< Where the hand holds the model (primary_attach), in model space
	float length = 0.f; //!< Distance from the grip to the farthest hit point, in model units
	
	bool valid = false; //!< The weapon is in the hand this frame
	Entity * weapon = nullptr;
	TransformInfo transform;
	
};

WeaponGrip g_weaponGrip;

//! Find how to hold a weapon model: the blade goes from primary_attach to the farthest hit_* point
void calibrateWeapon(EERIE_3DOBJ * obj) {
	
	g_weaponGrip.obj = obj;
	g_weaponGrip.calibration = quat_identity();
	g_weaponGrip.attach = Vec3f(0.f);
	g_weaponGrip.length = 0.f;
	
	for(const EERIE_ACTIONLIST & action : obj->actionlist) {
		if(action.name == "primary_attach") {
			g_weaponGrip.attach = obj->vertexlist[action.idx].v;
		}
	}
	
	Vec3f tip = g_weaponGrip.attach;
	for(const EERIE_ACTIONLIST & action : obj->actionlist) {
		if(action.name.compare(0, 4, "hit_") == 0) {
			Vec3f p = obj->vertexlist[action.idx].v;
			if(glm::distance(p, g_weaponGrip.attach) > glm::distance(tip, g_weaponGrip.attach)) {
				tip = p;
			}
		}
	}
	
	g_weaponGrip.length = glm::distance(tip, g_weaponGrip.attach);
	if(g_weaponGrip.length > 1.f) {
		// The hand's forward axis (z) points out of the fist along the controller handle.
		// Then roll the blade around its axis so that the edge cuts downwards, not sideways.
		glm::quat blade = glm::rotation(glm::normalize(tip - g_weaponGrip.attach), Vec3f(0.f, 0.f, 1.f));
		g_weaponGrip.calibration = glm::angleAxis(glm::radians(90.f), Vec3f(0.f, 0.f, 1.f)) * blade;
	}
	
}

//! Place the drawn weapon in the right hand for this frame
void updateWeaponGrip() {
	
	g_weaponGrip.valid = false;
	g_weaponGrip.weapon = nullptr;
	
	if(!(player.Interface & INTER_COMBATMODE)) {
		return;
	}
	WeaponType type = ARX_EQUIPMENT_GetPlayerWeaponType();
	if(type != WEAPON_DAGGER && type != WEAPON_1H && type != WEAPON_2H) {
		return;
	}
	Entity * weapon = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	if(!weapon || !weapon->obj) {
		return;
	}
	
	Vec3f position;
	glm::mat3 orientation;
	if(!xr::getHandPose(xr::getPrimaryHand(), true, position, orientation)) {
		return;
	}
	
	if(g_weaponGrip.obj != weapon->obj) {
		calibrateWeapon(weapon->obj);
	}
	
	// Same placement as for linked objects: the attach point of the weapon is at the hand
	TransformInfo t(position, glm::quat_cast(orientation) * g_weaponGrip.calibration, weapon->scale);
	t.pos = t(weapon->obj->vertexlist[weapon->obj->origin].v - g_weaponGrip.attach);
	
	g_weaponGrip.transform = t;
	g_weaponGrip.weapon = weapon;
	g_weaponGrip.valid = true;
	
}

/*
 * Torch held in the left hand
 */

struct TorchGrip {
	
	EERIE_3DOBJ * obj = nullptr;
	glm::quat calibration = quat_identity(); //!< Rotates the handle-to-fire direction onto the hand's forward axis
	Vec3f attach = Vec3f(0.f); //!< Where the hand holds the model, in model space
	
	bool valid = false; //!< The torch is in the hand this frame
	TransformInfo transform;
	
};

TorchGrip g_torchGrip;

//! Find how to hold a torch model: the stick goes through the fist, the fire points forward like a blade
void calibrateTorch(EERIE_3DOBJ * obj) {
	
	g_torchGrip.obj = obj;
	g_torchGrip.calibration = quat_identity();
	g_torchGrip.attach = obj->vertexlist[obj->origin].v;
	
	for(const EERIE_ACTIONLIST & action : obj->actionlist) {
		if(action.name == "primary_attach") {
			g_torchGrip.attach = obj->vertexlist[action.idx].v;
		}
	}
	
	if(obj->fastaccess.fire) {
		Vec3f fire = obj->vertexlist[obj->fastaccess.fire].v;
		if(glm::distance(fire, g_torchGrip.attach) > 1.f) {
			g_torchGrip.calibration = glm::rotation(glm::normalize(fire - g_torchGrip.attach), Vec3f(0.f, 0.f, 1.f));
		}
	}
	
}

/*
 * Bow held in the left hand and drawn with the right
 */

struct BowGrip {
	
	EERIE_3DOBJ * obj = nullptr;
	glm::mat3 calibration = glm::mat3(1.f); //!< Model to hand: limbs along the hand's y axis, shooting along z
	Vec3f attach = Vec3f(0.f); //!< Where the hand holds the model, in model space
	float brace = 0.f; //!< Distance from the grip back to the string, in model units
	
	bool valid = false; //!< The bow is in the hand this frame
	Entity * bow = nullptr;
	TransformInfo transform;
	
	bool triggerHeld = false;
	bool nocked = false; //!< An arrow is on the string, held by the right hand
	float draw = 0.f; //!< How far the bow is drawn, 0 to 1
	TransformInfo arrow;
	
};

BowGrip g_bowGrip;

const float NockReach = 30.f; //!< Maximum distance from the right hand to the string to take it
const float FullDraw = 0.45f; //!< Pull on the string for a full strength shot, in meters
const float MinDraw = 0.1f; //!< Releasing the string below this draw ratio puts the arrow back

//! Direction in which a set of points spreads the most, by power iteration on their covariance
Vec3f principalAxis(const glm::dmat3 & covariance) {
	glm::dvec3 axis(0.577, 0.577, 0.577);
	for(int i = 0; i < 64; i++) {
		glm::dvec3 next = covariance * axis;
		double length = glm::length(next);
		if(length < 1e-12) {
			break;
		}
		axis = next / length;
	}
	return Vec3f(axis);
}

/*!
 * Find how to hold a bow model: the limbs are its longest extent, and the hand holds it
 * at the front, the side away from the string.
 */
void calibrateBow(EERIE_3DOBJ * obj) {
	
	g_bowGrip.obj = obj;
	g_bowGrip.calibration = glm::mat3(1.f);
	g_bowGrip.attach = obj->vertexlist[obj->origin].v;
	g_bowGrip.brace = 0.f;
	
	size_t count = 0;
	glm::dvec3 sum(0.0);
	for(VertexId vertex : obj->vertexlist.handles()) {
		sum += glm::dvec3(obj->vertexlist[vertex].v);
		count++;
	}
	if(count < 3) {
		return;
	}
	Vec3f mean = Vec3f(sum / double(count));
	
	glm::dmat3 covariance(0.0);
	for(VertexId vertex : obj->vertexlist.handles()) {
		glm::dvec3 d = glm::dvec3(obj->vertexlist[vertex].v - mean);
		covariance += glm::outerProduct(d, d);
	}
	Vec3f limbs = principalAxis(covariance);
	
	bool hasAttach = false;
	for(const EERIE_ACTIONLIST & action : obj->actionlist) {
		if(action.name == "primary_attach") {
			g_bowGrip.attach = obj->vertexlist[action.idx].v;
			hasAttach = true;
		}
	}
	
	// The grip is in front of the middle of the model (the string and the limb tips are behind it)
	Vec3f front = (g_bowGrip.attach - mean) - limbs * glm::dot(g_bowGrip.attach - mean, limbs);
	if(!hasAttach || glm::length(front) < 1.f) {
		// No usable grip point: take the second longest extent, its sign is a guess
		double variance = glm::dot(glm::dvec3(limbs), covariance * glm::dvec3(limbs));
		glm::dvec3 l(limbs);
		front = principalAxis(covariance - glm::outerProduct(l, l) * variance);
		front -= limbs * glm::dot(front, limbs);
		hasAttach = false;
	}
	if(glm::length(front) < 1e-4f) {
		return;
	}
	front = glm::normalize(front);
	
	if(!hasAttach) {
		float reach = 0.f;
		for(VertexId vertex : obj->vertexlist.handles()) {
			reach = std::max(reach, glm::dot(obj->vertexlist[vertex].v - mean, front));
		}
		g_bowGrip.attach = mean + front * reach;
	}
	
	for(VertexId vertex : obj->vertexlist.handles()) {
		g_bowGrip.brace = std::max(g_bowGrip.brace, glm::dot(g_bowGrip.attach - obj->vertexlist[vertex].v, front));
	}
	
	// Hand space: x right, y down, z forward
	Vec3f side = glm::cross(limbs, front);
	g_bowGrip.calibration = glm::transpose(glm::mat3(side, limbs, front));
	
	LogInfo << "VR bow model: " << count << " vertices, limbs along " << limbs.x << ' ' << limbs.y << ' ' << limbs.z
	        << ", front " << front.x << ' ' << front.y << ' ' << front.z << ", grip point "
	        << (hasAttach ? "primary_attach" : "guessed") << ", brace height " << g_bowGrip.brace;
	
}

//! Place the drawn bow in the left hand, take the string with the right trigger and shoot on release
void updateBow() {
	
	g_bowGrip.valid = false;
	g_bowGrip.bow = nullptr;
	
	Entity * bow = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	Vec3f grip;
	glm::mat3 orientation;
	if(!(player.Interface & INTER_COMBATMODE) || ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BOW
	   || !bow || !bow->obj || !xr::getHandPose(xr::getOffHand(), true, grip, orientation)) {
		g_bowGrip.nocked = false;
		g_bowGrip.triggerHeld = false;
		g_bowGrip.draw = 0.f;
		return;
	}
	
	if(g_bowGrip.obj != bow->obj) {
		calibrateBow(bow->obj);
	}
	
	// With the fist upright the controller handle (the hand's z axis) points up and forward at about
	// 45 degrees: tilt the bow by that much so that its limbs are vertical and it shoots forward
	Vec3f forward = glm::normalize(orientation * Vec3f(0.f, 1.f, 1.f));
	Vec3f down = orientation * Vec3f(0.f, 1.f, -1.f);

	Vec3f hand;
	glm::mat3 handOrientation;
	bool handValid = xr::getHandPose(xr::getPrimaryHand(), true, hand, handOrientation);
	bool trigger = handValid && xr::getHandTrigger(xr::getPrimaryHand()) > 0.5f;
	float brace = g_bowGrip.brace * bow->scale;
	
	if(trigger && !g_bowGrip.triggerHeld && !g_bowGrip.nocked && !BLOCK_PLAYER_CONTROLS) {
		Vec3f string = grip - forward * brace;
		if(glm::distance(hand, string) < NockReach && getInventoryItemWithLowestDurability("arrows", 1.f)) {
			g_bowGrip.nocked = true;
		}
	}
	g_bowGrip.triggerHeld = trigger;
	
	if(g_bowGrip.nocked && !handValid) {
		g_bowGrip.nocked = false;
	}
	
	if(g_bowGrip.nocked) {
		// The arrow, and with it the bow, points from the hand on the string to the hand on the bow
		float distance = glm::distance(grip, hand);
		if(distance > 5.f) {
			forward = (grip - hand) / distance;
		}
		g_bowGrip.draw = glm::clamp((distance - brace) / (FullDraw * xr::getWorldScale()), 0.f, 1.f);
		if(!trigger) {
			g_bowGrip.nocked = false;
			if(g_bowGrip.draw >= MinDraw && arrowobj) {
				launchPlayerArrow(grip, forward, g_bowGrip.draw);
			}
		}
	}
	if(!g_bowGrip.nocked) {
		g_bowGrip.draw = 0.f;
	}
	
	down -= forward * glm::dot(down, forward);
	if(glm::length(down) < 1e-3f) {
		down = orientation * Vec3f(1.f, 0.f, 0.f);
		down -= forward * glm::dot(down, forward);
	}
	down = glm::normalize(down);
	glm::mat3 world(glm::cross(down, forward), down, forward);
	
	TransformInfo t(grip, glm::quat_cast(world * g_bowGrip.calibration), bow->scale);
	t.pos = t(bow->obj->vertexlist[bow->obj->origin].v - g_bowGrip.attach);
	g_bowGrip.transform = t;
	g_bowGrip.bow = bow;
	g_bowGrip.valid = true;
	
	if(g_bowGrip.nocked && arrowobj && arrowobj->vertexlist.size() >= 2) {
		// The tail of the arrow is at the hand that holds the string
		VertexId attach = getArrowAttachVertex();
		Vec3f base = arrowobj->vertexlist[attach].v;
		Vec3f axis = arrowobj->vertexlist[getArrowHitVertex(attach)].v - base;
		if(glm::length(axis) > 1e-3f) {
			axis = glm::normalize(axis);
			float tail = 0.f;
			for(VertexId vertex : arrowobj->vertexlist.handles()) {
				tail = std::min(tail, glm::dot(arrowobj->vertexlist[vertex].v - base, axis));
			}
			TransformInfo a(hand, glm::rotation(axis, forward), 1.f);
			a.pos = a(arrowobj->vertexlist[arrowobj->origin].v - (base + axis * tail));
			g_bowGrip.arrow = a;
		} else {
			g_bowGrip.nocked = false;
		}
	}
	
}

/*
 * Swings
 */

struct Swing {
	bool active = false;
	bool hit = false;
	bool previousValid = false;
	Vec3f previous = Vec3f(0.f);
};

std::array<Swing, 2> g_swings;

const float SwingStartSpeed = 2.5f; //!< m/s
const float SwingEndSpeed = 1.2f; //!< m/s

//! Speed of a point attached to a hand in m/s, from real hand motion only
float measureSpeed(Swing & swing, int hand, const Vec3f & offset, float seconds) {
	
	Vec3f position;
	if(seconds <= 0.f || !xr::getHandPointInTracking(hand, offset, position)) {
		swing.previousValid = false;
		return 0.f;
	}
	
	float speed = swing.previousValid ? glm::distance(position, swing.previous) / seconds : 0.f;
	swing.previous = position;
	swing.previousValid = true;
	return speed;
}

//! Strength of a swing from its speed, like the aim ratio of a charged strike
float swingStrength(float speed) {
	return glm::clamp((speed - 1.5f) / 4.5f, 0.1f, 1.f);
}

void swingWeapon(float seconds) {
	
	Swing & swing = g_swings[size_t(xr::getPrimaryHand())];
	Entity * io = entities.player();
	Entity * weapon = g_weaponGrip.weapon;
	
	// Hit points follow the hand
	DrawEERIEInter_ModelTransform(weapon->obj, g_weaponGrip.transform);
	
	Vec3f tip(0.f, 0.f, g_weaponGrip.length * weapon->scale);
	float speed = measureSpeed(swing, xr::getPrimaryHand(), tip, seconds);
	
	if(!swing.active && speed > SwingStartSpeed) {
		swing.active = true;
		swing.hit = false;
		ARX_PLAYER_Remove_Invisibility();
		WeaponType type = ARX_EQUIPMENT_GetPlayerWeaponType();
		std::string_view name = (type == WEAPON_DAGGER) ? "dagger" : (type == WEAPON_2H) ? "2h" : "1h";
		SendIOScriptEvent(nullptr, io, SM_STRIKE, name);
	}
	
	if(swing.active) {
		player.m_strikeAimRatio = swingStrength(speed);
		if(!swing.hit) {
			swing.hit = ARX_EQUIPMENT_Strike_Check(io, weapon, player.m_strikeAimRatio, 0);
		} else {
			// Only blood and sparks after the first hit of a swing
			ARX_EQUIPMENT_Strike_Check(io, weapon, player.m_strikeAimRatio, 1);
		}
		if(speed < SwingEndSpeed) {
			swing.active = false;
		}
	}
	
}

void punch(int hand, float seconds) {
	
	Swing & swing = g_swings[size_t(hand)];
	Entity * io = entities.player();
	
	float speed = measureSpeed(swing, hand, Vec3f(0.f), seconds);
	
	Vec3f position;
	glm::mat3 orientation;
	if(!xr::getHandPose(hand, true, position, orientation)) {
		swing.active = false;
		return;
	}
	
	if(!swing.active && speed > SwingStartSpeed) {
		swing.active = true;
		swing.hit = false;
		ARX_PLAYER_Remove_Invisibility();
		SendIOScriptEvent(nullptr, io, SM_STRIKE, "bare");
	}
	
	if(swing.active && !swing.hit) {
		Sphere sphere;
		sphere.origin = position;
		sphere.radius = 25.f;
		Entity * target = nullptr;
		if(CheckAnythingInSphere(sphere, io, 0, &target)) {
			player.m_strikeAimRatio = swingStrength(speed);
			float damages = (player.m_miscFull.damages + 1) * player.m_strikeAimRatio;
			tryToDoDamage(position, damages, 40, *io);
			ParticleSparkSpawnContinous(position, unsigned(damages), Color3f(0.45f, 0.1f, 0.f).toRGB());
			if(target) {
				ARX_SOUND_PlayCollision(target->material, MATERIAL_FLESH, 1.f, 1.f, position, nullptr);
			}
			swing.hit = true;
		}
	}
	
	if(swing.active && speed < SwingEndSpeed) {
		swing.active = false;
	}
	
}

/*
 * Items held in the right hand
 */

struct Grab {
	EntityHandle held;
	Vec3f offset = Vec3f(0.f); //!< Item position relative to the hand
	bool handValid = false;
	Vec3f hand = Vec3f(0.f);
	Vec3f velocity = Vec3f(0.f); //!< Hand velocity in world units per second
};

Grab g_grab;

const float GrabReach = 35.f; //!< Maximum distance from the hand to an item that can be taken
const float StashDistance = 35.f; //!< Releasing an item this close to the head puts it into the inventory

Entity * findItemInReach(const Vec3f & hand) {
	
	Entity * best = nullptr;
	float bestDistance = GrabReach;
	for(Entity & entity : entities.inScene(IO_ITEM)) {
		if(!(entity.gameFlags & GFLAG_INTERACTIVITY) || entity.show != SHOW_FLAG_IN_SCENE
		   || entity.owner() || !entity.obj) {
			continue;
		}
		float distance = glm::distance(entity.pos, hand);
		if(distance < bestDistance) {
			best = &entity;
			bestDistance = distance;
		}
	}
	
	return best;
}

void takeItem(Entity & item) {
	
	item.setOwner(nullptr);
	if(item.obj && item.obj->pbox) {
		item.obj->pbox->active = 0;
	}
	item.show = SHOW_FLAG_IN_SCENE;
	
	g_grab.held = item.index();
	g_grab.offset = item.pos - g_grab.hand;
	
	ARX_PLAYER_Remove_Invisibility();
	ARX_SOUND_PlayInterface(g_snd.INVSTD);
}

void releaseItem(Entity & item) {
	
	g_grab.held = EntityHandle();
	
	if(g_camera && glm::distance(g_grab.hand, g_camera->m_pos) < StashDistance) {
		// Over the shoulder into the backpack
		if(item.ioflags & IO_GOLD) {
			ARX_PLAYER_AddGold(&item);
			return;
		}
		if(entities.player()->inventory && entities.player()->inventory->insert(&item)) {
			ARX_SOUND_PlayInterface(g_snd.INVSTD);
			return;
		}
	}
	
	item.show = SHOW_FLAG_IN_SCENE;
	item.soundtime = 0;
	item.soundcount = 0;
	item.gameFlags &= ~GFLAG_NOCOMPUTATION;
	if(item.obj && item.obj->pbox) {
		// Throw with the hand's velocity (the physics scale launch vectors by 250)
		Vec3f velocity = g_grab.velocity;
		float speed = glm::length(velocity);
		if(speed > 1500.f) {
			velocity *= 1500.f / speed;
		}
		EERIE_PHYSICS_BOX_Launch(item.obj, item.pos, item.angle, velocity / 250.f + Vec3f(0.f, 0.01f, 0.f));
		if(speed > 300.f) {
			ARX_SOUND_PlaySFX(g_snd.WHOOSH, &item.pos);
		}
	}
}

/*
 * Runes drawn with the fingertip
 */

struct RuneDrawing {
	bool valid = false; //!< Drawing with the hand this frame
	bool stroke = false; //!< The trigger is held
	Vec3f origin = Vec3f(0.f); //!< Where the stroke started
	Vec3f right = Vec3f(1.f, 0.f, 0.f); //!< Plane of the stroke, fixed when it starts
	Vec3f down = Vec3f(0.f, 1.f, 0.f);
	Vec2s recognitionPoint = Vec2s(0);
	Vec2s screenPoint = Vec2s(0);
};

RuneDrawing g_rune;

bool getFingertip(Vec3f & tip) {
	
	std::array<Vec3f, xr::HandJointCount> joints;
	std::array<float, xr::HandJointCount> radii;
	if(xr::getHandJoints(xr::getPrimaryHand(), joints.data(), radii.data())) {
		tip = joints[JointIndexMetacarpal + 4];
		return true;
	}
	
	// Without finger tracking: a point just in front of the controller
	Vec3f position;
	glm::mat3 orientation;
	if(xr::getHandPose(xr::getPrimaryHand(), false, position, orientation)) {
		tip = position + orientation * Vec3f(0.f, 0.f, 0.05f * xr::getWorldScale());
		return true;
	}
	
	return false;
}

/*
 * Doors, levers, chests and NPCs used by touching them with the right hand
 */

const float TouchReach = 15.f; //!< Maximum distance from the hand or fingertip to the surface of a model

bool g_touchUsed = false; //!< The current grip press already used a touched entity

//! Closest point to p on the triangle abc (Ericson, Real-Time Collision Detection, 5.1.5)
Vec3f closestPointOnTriangle(const Vec3f & p, const Vec3f & a, const Vec3f & b, const Vec3f & c) {
	
	Vec3f ab = b - a;
	Vec3f ac = c - a;
	Vec3f ap = p - a;
	float d1 = glm::dot(ab, ap);
	float d2 = glm::dot(ac, ap);
	if(d1 <= 0.f && d2 <= 0.f) {
		return a;
	}
	
	Vec3f bp = p - b;
	float d3 = glm::dot(ab, bp);
	float d4 = glm::dot(ac, bp);
	if(d3 >= 0.f && d4 <= d3) {
		return b;
	}
	
	float vc = d1 * d4 - d3 * d2;
	if(vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
		return a + ab * (d1 / (d1 - d3));
	}
	
	Vec3f cp = p - c;
	float d5 = glm::dot(ab, cp);
	float d6 = glm::dot(ac, cp);
	if(d6 >= 0.f && d5 <= d6) {
		return c;
	}
	
	float vb = d5 * d2 - d1 * d6;
	if(vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
		return a + ac * (d2 / (d2 - d6));
	}
	
	float va = d3 * d6 - d5 * d4;
	if(va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	}
	
	float denom = 1.f / (va + vb + vc);
	return a + ab * (vb * denom) + ac * (vc * denom);
}

//! Distance from a point to the surface of an entity's model as last drawn
float distanceToModel(const EERIE_3DOBJ & obj, const Vec3f & point) {
	
	float best = std::numeric_limits<float>::max();
	for(const EERIE_FACE & face : obj.facelist) {
		if(size_t(face.vid[0]) >= obj.vertexWorldPositions.size()
		   || size_t(face.vid[1]) >= obj.vertexWorldPositions.size()
		   || size_t(face.vid[2]) >= obj.vertexWorldPositions.size()) {
			continue;
		}
		Vec3f a = obj.vertexWorldPositions[face.vid[0]].v;
		Vec3f b = obj.vertexWorldPositions[face.vid[1]].v;
		Vec3f c = obj.vertexWorldPositions[face.vid[2]].v;
		best = std::min(best, glm::distance(point, closestPointOnTriangle(point, a, b, c)));
	}
	
	return best;
}

//! The door, lever, container or NPC that the hand or fingertip touches, if any
Entity * findTouchTarget(const Vec3f * points, size_t count) {
	
	Entity * best = nullptr;
	float bestDistance = TouchReach;
	for(Entity & entity : entities.inScene(IO_FIX | IO_NPC)) {
		
		if(&entity == entities.player() || !entity.obj
		   || !(entity.gameFlags & GFLAG_INTERACTIVITY) || !(entity.gameFlags & GFLAG_ISINTREATZONE)
		   || (entity.gameFlags & (GFLAG_INVISIBILITY | GFLAG_MEGAHIDE))
		   || !(entity.script.valid || entity.inventory)) {
			continue;
		}
		
		for(size_t i = 0; i < count; i++) {
			const Vec3f & point = points[i];
			const EERIE_3D_BBOX & box = entity.bbox3D;
			if(glm::any(glm::lessThan(point, box.min - Vec3f(TouchReach)))
			   || glm::any(glm::greaterThan(point, box.max + Vec3f(TouchReach)))) {
				continue;
			}
			float distance = distanceToModel(*entity.obj, point);
			if(distance < bestDistance) {
				best = &entity;
				bestDistance = distance;
			}
		}
		
	}
	
	return best;
}

} // anonymous namespace

void updateMagic() {
	
	bool magic = (player.doingmagic == 2);
	xr::setPointerEnabled(!magic);
	
	g_rune.valid = false;
	Vec3f tip;
	if(!magic || !getFingertip(tip)) {
		g_rune.stroke = false;
		setMagicFlarePlacement(75.f, 1.f);
		return;
	}
	
	bool pressed = eeMousePressed1();
	if(pressed && !g_rune.stroke) {
		// Fix the drawing plane facing the head for the whole stroke
		const glm::mat4x4 & worldToView = g_preparedCamera.m_worldToView;
		g_rune.right = glm::normalize(Vec3f(worldToView[0][0], worldToView[1][0], worldToView[2][0]));
		g_rune.down = glm::normalize(Vec3f(worldToView[0][1], worldToView[1][1], worldToView[2][1]));
		g_rune.origin = tip;
	}
	g_rune.stroke = pressed;
	
	// About 1000 pixels per meter, around the middle of a 1280x720 screen
	const float pixelsPerUnit = 1000.f / xr::getWorldScale();
	Vec3f offset = tip - g_rune.origin;
	Vec2f point = Vec2f(640.f, 360.f) + Vec2f(glm::dot(offset, g_rune.right), glm::dot(offset, g_rune.down)) * pixelsPerUnit;
	point = glm::clamp(point, Vec2f(-30000.f), Vec2f(30000.f));
	g_rune.recognitionPoint = Vec2s(point);
	
	// Magic flares appear at the fingertip
	Vec4f clip = worldToClipSpace(tip);
	if(clip.w <= 0.f) {
		return;
	}
	Vec2f screen = glm::clamp(Vec2f(clip) / clip.w, Vec2f(-30000.f), Vec2f(30000.f));
	g_rune.screenPoint = Vec2s(screen);
	// Smaller than on the desktop: they are right in front of the eyes
	setMagicFlarePlacement((g_preparedCamera.m_worldToView * Vec4f(tip, 1.f)).z, 0.35f);
	
	g_rune.valid = true;
}

bool getRuneRecognitionPoint(Vec2s & point) {
	point = g_rune.recognitionPoint;
	return g_rune.valid;
}

bool getRuneScreenPoint(Vec2s & point) {
	point = g_rune.screenPoint;
	return g_rune.valid;
}

void updateGrab() {
	
	Vec3f hand;
	glm::mat3 orientation;
	bool valid = entities.player() && xr::getHandPose(xr::getPrimaryHand(), true, hand, orientation)
	             && !g_weaponGrip.valid && !BLOCK_PLAYER_CONTROLS;
	
	float seconds = toMsf(g_platformTime.lastFrameDuration()) / 1000.f;
	if(valid && g_grab.handValid && seconds > 0.f) {
		g_grab.velocity = (hand - g_grab.hand) / seconds;
	} else {
		g_grab.velocity = Vec3f(0.f);
	}
	g_grab.handValid = valid;
	g_grab.hand = hand;
	
	Entity * held = entities.get(g_grab.held);
	if(held) {
		if(!valid || !xr::isGrabbing() || held->show != SHOW_FLAG_IN_SCENE) {
			releaseItem(*held);
		} else {
			ARX_INTERACTIVE_Teleport(held, hand + g_grab.offset, true);
		}
		xr::setGrabCandidate(false);
		return;
	}
	g_grab.held = EntityHandle();
	
	if(!xr::isGrabbing()) {
		g_touchUsed = false;
	}
	
	Entity * candidate = valid ? findItemInReach(hand) : nullptr;
	
	// Without an item within reach: use what the hand touches, like double-clicking it
	Entity * touched = nullptr;
	if(!candidate && valid && !(player.Interface & INTER_COMBATMODE) && !player.doingmagic) {
		std::array<Vec3f, 2> points = { hand, hand };
		size_t count = getFingertip(points[1]) ? 2 : 1;
		touched = findTouchTarget(points.data(), count);
	}
	
	xr::setGrabCandidate(candidate != nullptr || touched != nullptr);
	
	if(touched) {
		touched->highlightColor = Color3f::gray(40.f);
		if(xr::isGrabbing() && !g_touchUsed) {
			g_touchUsed = true;
			ARX_INTERFACE_useEntity(touched);
		}
		return;
	}
	
	if(!candidate) {
		return;
	}
	
	candidate->highlightColor = Color3f::gray(40.f);
	if(xr::isGrabbing() && !g_touchUsed) {
		takeItem(*candidate);
	}
	
}

RenderBatcher & getUiBatcher() {
	static RenderBatcher batcher;
	return batcher;
}

void renderUiBatcher() {
	getUiBatcher().render();
	getUiBatcher().clear();
}

void updateCombat() {
	
	updateWeaponGrip();
	updateBow();
	
	WeaponType type = ARX_EQUIPMENT_GetPlayerWeaponType();
	if(!(player.Interface & INTER_COMBATMODE) || type == WEAPON_BOW || !entities.player()) {
		for(Swing & swing : g_swings) {
			swing = Swing();
		}
		return;
	}
	
	float seconds = toMsf(g_platformTime.lastFrameDuration()) / 1000.f;
	
	if(type == WEAPON_BARE) {
		punch(xr::LeftHand, seconds);
		punch(xr::RightHand, seconds);
	} else if(g_weaponGrip.valid) {
		g_swings[size_t(xr::getOffHand())] = Swing();
		swingWeapon(seconds);
	}
	
}
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
		int source = !tracked ? 0 : (xr::areHandJointsFromController(hand) ? 2 : 1);
		if(s_loggedSource[hand] != source) {
			s_loggedSource[hand] = source;
			const char * names[] = { "synthetic fingers from trigger and grip", "tracked finger joints",
			                         "wrist from the controller, fingers from trigger and grip" };
			LogInfo << (hand == xr::RightHand ? "Right" : "Left") << " VR hand: " << names[source];
		}
		
		ShaderLight lights[llightsSize];
		size_t lightsCount = 0;
		UpdateLlights(lights, lightsCount, skeleton.positions[JointPalm], true);
		
		// A real hand model if the runtime tracks the fingers, boxes along the joints otherwise
		if(tracked) {
			std::array<glm::mat4x4, xr::HandJointCount> joints;
			const HandModel * model = getHandModel(hand);
			if(model && xr::getHandJointTransforms(hand, joints.data())) {
				if(xr::areHandJointsFromController(hand)) {
					// The runtime only knows where the controller is and reports a flat hand:
					// hold the controller instead, with the fingers following the trigger and the grip.
					// The index finger stays straight while it draws runes, to match its tip there.
					float trigger = xr::getHandTrigger(hand);
					float squeeze = xr::getHandSqueeze(hand);
					bool drawing = (player.doingmagic == 2) && hand == xr::getPrimaryHand();
					float index = drawing ? 0.f : 0.35f + 0.55f * trigger;
					float others = 0.6f + 0.4f * squeeze;
					const float curls[HandModel::FingerCount] = { 0.5f, index, others, others, others };
					model->curlFingers(curls, joints.data());
				}
				addHandModel(*model, joints.data(), lights, lightsCount, colorMod);
				continue;
			}
		}
		
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
	
	if(g_handVertices.empty() && !g_weaponGrip.valid && !g_torchGrip.valid && !g_bowGrip.valid) {
		return;
	}
	
	RenderState state = render3D();
	state.setCull(false);
	UseRenderState renderState(state);
	GRenderer->SetTexture(0, static_cast<Texture *>(nullptr));
	
	if(!g_handVertices.empty()) {
		EERIEDRAWPRIM(Renderer::TriangleList, g_handVertices.data(), g_handVertices.size());
	}
	
	if(g_weaponGrip.valid) {
		float invisibility = std::min(0.9f, entities.player()->invisibility);
		DrawEERIEInter(g_weaponGrip.weapon->obj, g_weaponGrip.transform, g_weaponGrip.weapon, true, invisibility);
		PopAllTriangleListOpaque();
		PopAllTriangleListTransparency();
	}
	
	if(g_bowGrip.valid) {
		float invisibility = std::min(0.9f, entities.player()->invisibility);
		DrawEERIEInter(g_bowGrip.bow->obj, g_bowGrip.transform, g_bowGrip.bow, true, invisibility);
		if(g_bowGrip.nocked && arrowobj) {
			DrawEERIEInter(arrowobj.get(), g_bowGrip.arrow, nullptr, true, invisibility);
		}
		PopAllTriangleListOpaque();
		PopAllTriangleListTransparency();
	}
	
	if(g_torchGrip.valid && player.torch && player.torch->obj == g_torchGrip.obj) {
		float invisibility = std::min(0.9f, entities.player()->invisibility);
		DrawEERIEInter(player.torch->obj, g_torchGrip.transform, player.torch, true, invisibility);
		PopAllTriangleListOpaque();
		PopAllTriangleListTransparency();
	}
	
}

void updateTorch() {
	
	g_torchGrip.valid = false;
	
	Entity * torch = player.torch;
	Vec3f position;
	glm::mat3 orientation;
	if(!torch || !torch->obj || !xr::getHandPose(xr::getOffHand(), true, position, orientation)) {
		return;
	}
	
	if(g_torchGrip.obj != torch->obj) {
		calibrateTorch(torch->obj);
	}
	
	// Same placement as the weapon: the grip point of the torch is at the hand
	TransformInfo t(position, glm::quat_cast(orientation) * g_torchGrip.calibration, torch->scale);
	t.pos = t(torch->obj->vertexlist[torch->obj->origin].v - g_torchGrip.attach);
	// Update the world positions now for the fire, drawing happens later for each eye
	DrawEERIEInter_ModelTransform(torch->obj, t);
	
	g_torchGrip.transform = t;
	g_torchGrip.valid = true;
	
	if(torch->obj->fastaccess.fire) {
		// Flames at the tip like a burning torch in the world, and the torch light moves with the hand
		// (ManageTorch() keeps its brightness and flicker)
		Vec3f fire = torch->obj->vertexWorldPositions[torch->obj->fastaccess.fire].v;
		createFireParticles(fire, 2, 2ms);
		lightHandleGet(torchLightHandle)->pos = fire;
	}
	
}

void setPlayerFirstPersonHidden(Entity & playerEntity, bool hidden) {
	
	EERIE_3DOBJ * obj = playerEntity.obj;
	if(!obj || !obj->m_skeleton) {
		return;
	}
	const Skeleton & rig = *obj->m_skeleton;
	
	// The head and the arms are the bones with these names and everything attached below them
	enum Part { Body, Head, Arm };
	std::vector<Part> parts(obj->vertexlist.size(), Body);
	for(VertexGroupId group : obj->grouplist.handles()) {
		Part part = Body;
		size_t depth = 0;
		for(VertexGroupId bone = group; bone && depth < obj->grouplist.size(); bone = rig.bones[bone].father, depth++) {
			const std::string & name = obj->grouplist[bone].name;
			if(name == "head") {
				part = Head;
				break;
			}
			if(name == "left_shoulder" || name == "right_shoulder") {
				part = Arm;
				break;
			}
		}
		if(part == Body) {
			continue;
		}
		for(VertexId vertex : obj->m_boneVertices[group]) {
			if(size_t(vertex) < parts.size()) {
				parts[size_t(vertex)] = part;
			}
		}
	}
	
	// Nothing of the head may remain in front of the eyes, so hide every face that touches it.
	// For the arms only hide faces that are entirely theirs to keep the shoulders of the torso closed.
	size_t faces = 0;
	for(EERIE_FACE & face : obj->facelist) {
		bool anyHead = false;
		bool allArm = true;
		for(VertexId vertex : face.vid) {
			Part part = size_t(vertex) < parts.size() ? parts[size_t(vertex)] : Body;
			anyHead = anyHead || part == Head;
			allArm = allArm && part == Arm;
		}
		if(!anyHead && !allArm) {
			continue;
		}
		faces++;
		if(hidden) {
			face.facetype |= POLY_HIDE;
		} else {
			face.facetype &= ~POLY_HIDE;
		}
	}
	
	static bool s_logged = false;
	if(!s_logged && hidden) {
		s_logged = true;
		LogInfo << "VR player model: head and arms hidden, " << faces << " of " << obj->facelist.size() << " faces";
	}
	
}

void updatePointer() {
	
	// Only in cursor mode: free look and combat use the crosshair in the middle of the view
	Vec3f origin, direction;
	if(TRUE_PLAYER_MOUSELOOK_ON || (player.Interface & INTER_COMBATMODE) || BLOCK_PLAYER_CONTROLS
	   || !xr::getWorldPointerRay(origin, direction)) {
		return;
	}
	
	// Walls and floor stop the ray, then the nearest entity model in front of them
	const float reach = 1000.f;
	Vec3f end = origin + direction * reach;
	if(RaycastResult scene = raycastScene(origin, end)) {
		end = scene.pos;
	}
	Vec3f point = end;
	Entity * entity = nullptr;
	if(EntityRaycastResult hit = raycastEntities(origin, end, POLY_TRANS, RaycastIgnorePlayer)) {
		point = hit.pos;
		entity = hit.entity;
	}
	
	// The game picks what is under the cursor as projected by the camera, so place the cursor where
	// the camera sees the hit point instead of where the ray crosses the UI panel
	Vec4f clip = worldToClipSpace(point);
	if(clip.w <= 0.f) {
		return;
	}
	Vec2f screen = Vec2f(clip) / clip.w;
	if(entity && entity->bbox2D.valid()) {
		screen = glm::clamp(screen, entity->bbox2D.min, entity->bbox2D.max);
	}
	if(!Rectf(g_size).contains(screen)) {
		return;
	}
	
	bool target = entity && (entity->gameFlags & GFLAG_INTERACTIVITY);
	xr::setWorldPointer(Vec2s(screen), glm::distance(origin, point), target);
}

} // namespace vr
