#include <stdafx.h>

#include "Effects/Register/RegisterEffect.h"
#include "Util/Color.h"
#include "Util/Types.h"

#include "Memory/Drawing.h"
#include "Memory/WorldToScreen.h"

// This file is manually formatted.
// clang-format off

CHAOS_VAR const size_t boneCount       = 19;
CHAOS_VAR const size_t connectionCount = 14;
CHAOS_VAR const float maxDistance      = 100.0f;
CHAOS_VAR const float thickness        = 5.0f;

CHAOS_VAR Color lineColor;

CHAOS_VAR const std::array<int, boneCount> BONE_IDS = {
	0x796e, 0x9995, 0xfcd9, 0x58b7,
	0xb1c5, 0xeeeb, 0x49d9, 0x29d2,
	0x0bb0, 0x9d4d, 0x6e5c, 0xdead,
	0x2e28, 0xe39f, 0xf9bb, 0xca72,
	0x9000, 0x3779, 0xcc4d
};

CHAOS_VAR const std::array<std::array<int, 2>, connectionCount> connections = {{
	{  0,  1 }, {  1,  4 }, {  4,  5 }, {  5,  6 },
	{  1,  9 }, {  9, 10 }, { 10, 11 }, {  1, 12 },
	{ 12, 13 }, { 13, 14 }, { 12, 15 }, { 15, 16 },
	{ 14, 17 }, { 16, 18 },
}};

static bool IsValidScreenPoint(const ChaosVector2 &point)
{
	return point.x > 0.0f && point.x < 1.0f && point.y > 0.0f && point.y < 1.0f;
}

static bool TryGetBoneScreenCoords(Ped ped, int boneID, ChaosVector2 &screenCoords)
{
	Vector3 boneCoords = GET_PED_BONE_COORDS(ped, boneID, 0.0f, 0.0f, 0.0f);
	return Memory::WorldToScreen(ChaosVector3(boneCoords), &screenCoords) &&
	       IsValidScreenPoint(screenCoords);
}

static void DrawSkeleton(
	const std::array<ChaosVector2, boneCount> &points,
	const std::array<bool, boneCount> &validPoints,
	const std::array<std::array<int, 2>, connectionCount> &connections,
	Color lineColor, float thickness)
{
	for (const auto &connection : connections)
	{
		const int pointAIndex = connection[0];
		const int pointBIndex = connection[1];

		if (!validPoints[pointAIndex] || !validPoints[pointBIndex])
			continue;

		const ChaosVector2 &pointA = points[pointAIndex];
		const ChaosVector2 &pointB = points[pointBIndex];

		Drawing::QueueLine(pointA.x, pointA.y, pointB.x, pointB.y, lineColor, thickness);
	}
}

static bool WithinDistance2D(const Vector3 &from, const Vector3 &to)
{
	const float dx = from.x - to.x;
	const float dy = from.y - to.y;

	return (dx * dx + dy * dy) <= (maxDistance * maxDistance);
}

static void OnStart()
{
	lineColor = GetRandomColorRGB();
}

static void OnStop()
{
	Drawing::Cleanup();
}

static void OnTick()
{
	Drawing::BeginFrame(256);

	const Ped playerPed     = PLAYER_PED_ID();
	const Vector3 playerPos = GET_ENTITY_COORDS(playerPed, true);

	for (Ped ped : GetAllPeds())
	{
		if (ped == playerPed || !IS_ENTITY_ON_SCREEN(ped) || IS_ENTITY_DEAD(ped, false) || IS_PED_A_PLAYER(ped))
			continue;

		const Vector3 pedPos = GET_ENTITY_COORDS(ped, true);
		if (!WithinDistance2D(playerPos, pedPos))
			continue;

		std::array<ChaosVector2, boneCount> points {};
		std::array<bool, boneCount> validPoints {};

		for (size_t i = 0; i < boneCount; i++)
			validPoints[i] = TryGetBoneScreenCoords(ped, BONE_IDS[i], points[i]);

		DrawSkeleton(points, validPoints, connections, lineColor, thickness);
	}

	Drawing::EndFrame();
}

REGISTER_EFFECT(OnStart, OnStop, OnTick,
	{
		.Name = "ESP",
		.Id = "misc_esp",
		.IsTimed = true
	}
);