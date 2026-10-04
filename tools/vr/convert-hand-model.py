#!/usr/bin/env python3
#
# Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
#
# This file is part of Arx Libertatis.
#
# Arx Libertatis is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Arx Libertatis is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.

"""
Convert the WebXR "generic-hand" glTF models (tools/vr/generic-hand/*.glb, see LICENSE.generic-hand)
into the simple binary files that the VR hands are drawn from (data/core/graph/obj3d/vr/*.vrhand).

A glove cuff around the wrist is added to the hand mesh.

File layout, little endian:
  char[4] "AVRH", u32 version (1), u32 vertex count, u32 index count
  float[3] position per vertex, in meters in the space of the model
  float[3] normal per vertex
  float[2] texture coordinate per vertex
  u8[4]    joints per vertex, as XrHandJointEXT values
  float[4] joint weights per vertex
  u16      vertex per index, three per triangle
  float[16] inverse bind matrix per joint for all 26 XrHandJointEXT values, column-major

Usage: convert-hand-model.py (from anywhere, paths are relative to this script)
"""

import json
import math
import os
import struct

# WebXR joint names in the order of XrHandJointEXT, which starts with the palm that WebXR does not have
FINGERS = (
	('thumb', ('metacarpal', 'phalanx-proximal', 'phalanx-distal', 'tip')),
	('index-finger', ('metacarpal', 'phalanx-proximal', 'phalanx-intermediate', 'phalanx-distal', 'tip')),
	('middle-finger', ('metacarpal', 'phalanx-proximal', 'phalanx-intermediate', 'phalanx-distal', 'tip')),
	('ring-finger', ('metacarpal', 'phalanx-proximal', 'phalanx-intermediate', 'phalanx-distal', 'tip')),
	('pinky-finger', ('metacarpal', 'phalanx-proximal', 'phalanx-intermediate', 'phalanx-distal', 'tip')),
)
XR_JOINTS = ['palm', 'wrist'] + [finger + '-' + part for finger, parts in FINGERS for part in parts]
assert len(XR_JOINTS) == 26
WRIST = XR_JOINTS.index('wrist')

CUFF_SEGMENTS = 24
# (offset along the forearm from the end of the hand mesh in meters, radius relative to the wrist)
CUFF_PROFILE = ((-0.035, 1.12), (0.0, 1.22), (0.035, 1.45), (0.07, 1.8))


def mat_mul(a, b):
	# Column-major 4x4 matrices as flat lists
	return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def mat_point(m, p):
	return [m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] for r in range(3)]


def mat_vector(m, v):
	return [m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] for r in range(3)]


def mat_from_trs(node):
	x, y, z, w = node.get('rotation', (0.0, 0.0, 0.0, 1.0))
	sx, sy, sz = node.get('scale', (1.0, 1.0, 1.0))
	tx, ty, tz = node.get('translation', (0.0, 0.0, 0.0))
	return [
		(1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0.0,
		(2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0.0,
		(2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
		tx, ty, tz, 1.0,
	]


def rigid_inverse(m):
	# Inverse of a rotation + translation matrix
	r = [m[0], m[4], m[8], 0.0, m[1], m[5], m[9], 0.0, m[2], m[6], m[10], 0.0, 0.0, 0.0, 0.0, 1.0]
	t = mat_vector(r, (m[12], m[13], m[14]))
	r[12], r[13], r[14] = -t[0], -t[1], -t[2]
	return r


def convert(source, target):

	data = open(source, 'rb').read()
	magic, version, _ = struct.unpack_from('<4sII', data, 0)
	assert magic == b'glTF' and version == 2, source
	json_length, json_type = struct.unpack_from('<I4s', data, 12)
	assert json_type == b'JSON'
	gltf = json.loads(data[20:20 + json_length])
	binary = data[20 + json_length + 8:]

	def read(accessor, fmt):
		accessor = gltf['accessors'][accessor]
		view = gltf['bufferViews'][accessor['bufferView']]
		offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
		stride = view.get('byteStride') or struct.calcsize(fmt)
		return [list(struct.unpack_from(fmt, binary, offset + i * stride)) for i in range(accessor['count'])]

	assert len(gltf['meshes']) == 1 and len(gltf['meshes'][0]['primitives']) == 1 and len(gltf['skins']) == 1
	primitive = gltf['meshes'][0]['primitives'][0]
	attributes = primitive['attributes']
	positions = read(attributes['POSITION'], '<3f')
	normals = read(attributes['NORMAL'], '<3f')
	uvs = read(attributes['TEXCOORD_0'], '<2f')
	joints = read(attributes['JOINTS_0'], '<4B')
	weights = read(attributes['WEIGHTS_0'], '<4f')
	indices = [i[0] for i in read(primitive['indices'], '<H')]

	skin = gltf['skins'][0]
	names = [gltf['nodes'][node]['name'] for node in skin['joints']]
	skin_to_xr = [XR_JOINTS.index(name) for name in names]
	inverse_bind = read(skin['inverseBindMatrices'], '<16f')

	# The joints must be at the root of the scene so that their transforms are the bind poses
	parents = {child: i for i, node in enumerate(gltf['nodes']) for child in node.get('children', ())}
	identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
	bind_error = 0.0
	for i, node in enumerate(skin['joints']):
		world = mat_from_trs(gltf['nodes'][node])
		parent = parents.get(node)
		while parent is not None:
			world = mat_mul(mat_from_trs(gltf['nodes'][parent]), world)
			parent = parents.get(parent)
		check = mat_mul(inverse_bind[i], world)
		bind_error = max(bind_error, max(abs(a - b) for a, b in zip(check, identity)))
	assert bind_error < 1e-4, 'joint nodes do not match the inverse bind matrices: %g' % bind_error

	xr_inverse_bind = [identity] * len(XR_JOINTS)
	for i, matrix in enumerate(inverse_bind):
		xr_inverse_bind[skin_to_xr[i]] = matrix
	for vertex in range(len(positions)):
		total = sum(weights[vertex])
		assert total > 0.99, 'unweighted vertex'
		joints[vertex] = [skin_to_xr[j] if w > 0.0 else WRIST for j, w in zip(joints[vertex], weights[vertex])]
		weights[vertex] = [w / total for w in weights[vertex]]

	# Glove cuff: a flared tube around the forearm end of the mesh, rigidly bound to the wrist
	# In the space of the wrist joint -z points to the fingers and +z along the forearm
	to_wrist = xr_inverse_bind[WRIST]
	from_wrist = rigid_inverse(to_wrist)
	local = [mat_point(to_wrist, p) for p in positions]
	end = max(p[2] for p in local)
	ring = [p for p in local if p[2] > end - 0.02]
	center = [(min(p[i] for p in ring) + max(p[i] for p in ring)) * 0.5 for i in range(2)]
	radius = [(max(p[i] for p in ring) - min(p[i] for p in ring)) * 0.5 for i in range(2)]
	base = len(positions)
	for row, (offset, scale) in enumerate(CUFF_PROFILE):
		previous = CUFF_PROFILE[max(row - 1, 0)]
		following = CUFF_PROFILE[min(row + 1, len(CUFF_PROFILE) - 1)]
		slope = (following[1] - previous[1]) * (radius[0] + radius[1]) * 0.5 / (following[0] - previous[0])
		for segment in range(CUFF_SEGMENTS):
			angle = 2 * math.pi * segment / CUFF_SEGMENTS
			x, y = math.cos(angle), math.sin(angle)
			point = (center[0] + x * radius[0] * scale, center[1] + y * radius[1] * scale, end + offset)
			normal = (x * radius[1], y * radius[0], -slope * math.hypot(x * radius[1], y * radius[0]))
			length = math.sqrt(sum(c * c for c in normal))
			positions.append(mat_point(from_wrist, point))
			normals.append(mat_vector(from_wrist, [c / length for c in normal]))
			uvs.append([segment / CUFF_SEGMENTS, row / (len(CUFF_PROFILE) - 1)])
			joints.append([WRIST] * 4)
			weights.append([1.0, 0.0, 0.0, 0.0])
	for row in range(len(CUFF_PROFILE) - 1):
		for segment in range(CUFF_SEGMENTS):
			a = base + row * CUFF_SEGMENTS + segment
			b = base + row * CUFF_SEGMENTS + (segment + 1) % CUFF_SEGMENTS
			indices += [a, b, a + CUFF_SEGMENTS, b, b + CUFF_SEGMENTS, a + CUFF_SEGMENTS]

	# Skinning with the bind poses must give back the model
	skin_error = 0.0
	bind = [rigid_inverse(m) for m in xr_inverse_bind]
	for vertex, position in enumerate(positions):
		skinned = [0.0, 0.0, 0.0]
		for joint, weight in zip(joints[vertex], weights[vertex]):
			p = mat_point(mat_mul(bind[joint], xr_inverse_bind[joint]), position)
			skinned = [s + c * weight for s, c in zip(skinned, p)]
		skin_error = max(skin_error, max(abs(a - b) for a, b in zip(skinned, position)))
	assert skin_error < 1e-5, 'skinning check failed: %g' % skin_error

	assert len(positions) < 65536 and len(indices) % 3 == 0
	out = bytearray(struct.pack('<4sIII', b'AVRH', 1, len(positions), len(indices)))
	for values, fmt in ((positions, '<3f'), (normals, '<3f'), (uvs, '<2f'), (joints, '<4B'), (weights, '<4f')):
		for value in values:
			out += struct.pack(fmt, *value)
	out += struct.pack('<%dH' % len(indices), *indices)
	for matrix in xr_inverse_bind:
		out += struct.pack('<16f', *matrix)
	os.makedirs(os.path.dirname(target), exist_ok=True)
	open(target, 'wb').write(out)

	print('%s -> %s: %d vertices, %d triangles, %d joints, wrist radius %.1f x %.1f mm, %d bytes'
	      % (os.path.basename(source), os.path.basename(target), len(positions), len(indices) // 3, len(names),
	         radius[0] * 1000, radius[1] * 1000, len(out)))


if __name__ == '__main__':
	here = os.path.dirname(os.path.abspath(__file__))
	output = os.path.join(here, '..', '..', 'data', 'core', 'graph', 'obj3d', 'vr')
	for side in ('left', 'right'):
		convert(os.path.join(here, 'generic-hand', side + '.glb'), os.path.join(output, 'hand_' + side + '.vrhand'))
