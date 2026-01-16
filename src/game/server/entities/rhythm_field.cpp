/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "rhythm_field.h"

#include "rhythm_arrow.h"

#include <generated/protocol.h>

#include <game/server/gamecontext.h>

#include <algorithm>
#include <cmath>

namespace
{
constexpr vec2 s_aDirections[] = {
	vec2(1.0f, 0.0f),
	vec2(0.0f, -1.0f),
	vec2(-1.0f, 0.0f),
	vec2(0.0f, 1.0f),
};
constexpr int s_DirectionCount = (int)(sizeof(s_aDirections) / sizeof(s_aDirections[0]));
} // namespace

CRhythmField::CRhythmField(CGameWorld *pGameWorld, vec2 Pos, float Bpm, float HitRadius) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_RHYTHM_FIELD, Pos),
	m_Bpm(Bpm),
	m_BeatPeriod(0.0f),
	m_BeatIntervalTicks(0),
	m_NextBeatTick(0),
	m_NextDirectionIndex(0),
	m_ArrowTravelDistance(200.0f),
	m_AutoSpawn(true),
	m_HitZonePos(Pos),
	m_HitZoneRadius(HitRadius)
{
	for(int &Id : m_aLaserIds)
		Id = -1;
	for(int &Id : m_aPickupIds)
		Id = -1;
	for(int &Id : m_aProjectileIds)
		Id = -1;
	EnsureSnapIds();
	UpdateBeatTiming();
	m_NextBeatTick = Server()->Tick() + m_BeatIntervalTicks;

	GameWorld()->InsertEntity(this);
}

CRhythmField::~CRhythmField()
{
	for(int &Id : m_aLaserIds)
	{
		if(Id >= 0)
			Server()->SnapFreeId(Id);
	}
	for(int &Id : m_aPickupIds)
	{
		if(Id >= 0)
			Server()->SnapFreeId(Id);
	}
	for(int &Id : m_aProjectileIds)
	{
		if(Id >= 0)
			Server()->SnapFreeId(Id);
	}
}

void CRhythmField::Reset()
{
	for(CRhythmArrow *pArrow : m_vArrows)
	{
		if(pArrow)
		{
			pArrow->DetachField();
			pArrow->Reset();
		}
	}
	m_vArrows.clear();
	m_MarkedForDestroy = true;
}

void CRhythmField::Tick()
{
	if(!m_AutoSpawn)
		return;

	if(Server()->Tick() < m_NextBeatTick)
		return;

	while(Server()->Tick() >= m_NextBeatTick)
	{
		SpawnArrow();
		m_NextBeatTick += m_BeatIntervalTicks;
	}
}

void CRhythmField::TickPaused()
{
}

void CRhythmField::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	const int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	const bool Sixup = Server()->IsSixup(SnappingClient);
	const CSnapContext Context(SnappingClientVersion, Sixup, SnappingClient);

	constexpr float LaneWidth = 64.0f;
	constexpr float FieldHeight = 128.0f;
	constexpr float HitLineDepth = 20.0f;
	constexpr float MarkerOffset = 24.0f;

	const vec2 Top = m_HitZonePos - vec2(0.0f, FieldHeight);
	const vec2 Bottom = m_HitZonePos + vec2(0.0f, HitLineDepth);
	const float HalfWidth = LaneWidth * 2.0f;

	for(int i = 0; i < 5; ++i)
	{
		const float X = m_HitZonePos.x - HalfWidth + LaneWidth * i;
		const vec2 From(X, Top.y);
		const vec2 To(X, Bottom.y);
		GameServer()->SnapLaserObject(Context, m_aLaserIds[i], To, From, Server()->Tick(), -1, LASERTYPE_DOOR);
	}

	const vec2 HitFrom(m_HitZonePos.x - HalfWidth, m_HitZonePos.y);
	const vec2 HitTo(m_HitZonePos.x + HalfWidth, m_HitZonePos.y);
	GameServer()->SnapLaserObject(Context, m_aLaserIds[5], HitTo, HitFrom, Server()->Tick(), -1, LASERTYPE_DOOR);

	for(int i = 0; i < 4; ++i)
	{
		const float X = m_HitZonePos.x - HalfWidth + LaneWidth * (i + 0.5f);
		const vec2 MarkerPos(X, m_HitZonePos.y + MarkerOffset);
		GameServer()->SnapPickup(Context, m_aPickupIds[i], MarkerPos, POWERUP_WEAPON, WEAPON_GUN, -1, 0);

		CNetObj_Projectile *pProj = Server()->SnapNewItem<CNetObj_Projectile>(m_aProjectileIds[i]);
		if(pProj)
		{
			pProj->m_X = (int)MarkerPos.x;
			pProj->m_Y = (int)MarkerPos.y;
			pProj->m_VelX = 0;
			pProj->m_VelY = 0;
			pProj->m_StartTick = Server()->Tick();
			pProj->m_Type = WEAPON_GUN;
		}
	}
}

void CRhythmField::SetBpm(float Bpm)
{
	m_Bpm = Bpm;
	UpdateBeatTiming();
}

void CRhythmField::SetAutoSpawn(bool Auto)
{
	m_AutoSpawn = Auto;
}

void CRhythmField::SetHitZone(vec2 Pos)
{
	m_Pos = Pos;
	m_HitZonePos = Pos;
}

void CRhythmField::EnsureSnapIds()
{
	for(int &Id : m_aLaserIds)
	{
		if(Id < 0)
			Id = Server()->SnapNewId();
	}
	for(int &Id : m_aPickupIds)
	{
		if(Id < 0)
			Id = Server()->SnapNewId();
	}
	for(int &Id : m_aProjectileIds)
	{
		if(Id < 0)
			Id = Server()->SnapNewId();
	}
}

void CRhythmField::RegisterArrow(CRhythmArrow *pArrow)
{
	if(!pArrow)
		return;
	m_vArrows.push_back(pArrow);
}

void CRhythmField::UnregisterArrow(CRhythmArrow *pArrow)
{
	if(!pArrow)
		return;
	auto Iter = std::find(m_vArrows.begin(), m_vArrows.end(), pArrow);
	if(Iter != m_vArrows.end())
		m_vArrows.erase(Iter);
}

void CRhythmField::UpdateBeatTiming()
{
	if(m_Bpm <= 0.0f)
		m_Bpm = 120.0f;

	m_BeatPeriod = 60.0f / m_Bpm;
	m_BeatIntervalTicks = std::max(1, (int)std::round(m_BeatPeriod * Server()->TickSpeed()));
}

void CRhythmField::SpawnArrow(vec2 Direction, int HitTick)
{
	const int TravelTicks = std::max(1, HitTick - Server()->Tick());
	const float SpeedPerTick = m_ArrowTravelDistance / (float)TravelTicks;
	const vec2 Origin = m_HitZonePos - Direction * m_ArrowTravelDistance;

	GameServer()->CreateRhythmArrow(this, Origin, Direction, SpeedPerTick, HitTick);
}

void CRhythmField::SpawnArrow()
{
	const vec2 Direction = s_aDirections[m_NextDirectionIndex % s_DirectionCount];
	m_NextDirectionIndex++;

	const int SpawnTick = Server()->Tick();
	const int HitTick = SpawnTick + m_BeatIntervalTicks;

	SpawnArrow(Direction, HitTick);
}
