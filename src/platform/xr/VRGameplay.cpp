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

#include <glm/gtx/quaternion.hpp>

#include "animation/AnimationRender.h"
#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Damage.h"
#include "game/Camera.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "game/Item.h"
#include "game/Player.h"
#include "graphics/Draw.h"
#include "graphics/Renderer.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "graphics/particle/MagicFlare.h"
#include "graphics/particle/Spark.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "platform/xr/OpenXR.h"
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
	
	Entity * candidate = valid ? findItemInReach(hand) : nullptr;
	xr::setGrabCandidate(candidate != nullptr);
	if(!candidate) {
		return;
	}
	
	candidate->highlightColor = Color3f::gray(40.f);
	if(xr::isGrabbing()) {
		takeItem(*candidate);
	}
	
}

void updateCombat() {
	
	updateWeaponGrip();
	
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
	
	if(g_handVertices.empty() && !g_weaponGrip.valid) {
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
	
}

} // namespace vr
