/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "rhythm_arrow.h"

#include "rhythm_field.h"

#include <base/math.h>

#include <generated/protocol.h>

#include <game/server/gamecontext.h>

CRhythmArrow::CRhythmArrow(CGameWorld *pGameWorld, CRhythmField *pField, vec2 Origin, vec2 Direction, float SpeedPerTick, int HitTick, int LaneIndex, float MissY, float VelScale, float TailLength) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_RHYTHM_ARROW, Origin),
	m_pField(pField),
	m_Origin(Origin),
	m_Direction(Direction),
	m_Phase(0.0f),
	m_Speed(SpeedPerTick),
	m_SpawnTick(Server()->Tick()),
	m_HitTick(HitTick),
	m_LaneIndex(LaneIndex),
	m_MissY(MissY),
	m_VelScale(VelScale),
	m_TailLength(TailLength),
	m_TailLaserId(-1)
{
	if(m_TailLength > 0.0f)
		m_TailLaserId = Server()->SnapNewId();
	if(m_pField)
		m_pField->RegisterArrow(this);

	GameWorld()->InsertEntity(this);
}

void CRhythmArrow::Reset()
{
	m_MarkedForDestroy = true;
}

void CRhythmArrow::Tick()
{
	const int Tick = Server()->Tick();
	m_Phase = (Tick - m_SpawnTick) * m_Speed;
	m_Pos = m_Origin + m_Direction * m_Phase;

	if(m_Pos.y >= m_MissY)
	{
		m_MarkedForDestroy = true;
	}
}

void CRhythmArrow::TickPaused()
{
}

void CRhythmArrow::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(SnappingClient >= 0 && m_HiddenMask.test(SnappingClient))
		return;

	const int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	float StartVelScale = m_VelScale;
	if(StartVelScale <= 0.0f)
	{
		const float Speed = GameServer()->GlobalTuning()->m_GunSpeed;
		StartVelScale = Speed > 0.0f ? (m_Speed * Server()->TickSpeed()) / Speed : 0.0f;
	}
	const vec2 StartVel = m_Direction * StartVelScale;

	if(SnappingClientVersion >= VERSION_DDNET_ENTITY_NETOBJS)
	{
		CNetObj_DDNetProjectile *pProj = static_cast<CNetObj_DDNetProjectile *>(Server()->SnapNewItem(NETOBJTYPE_DDNETPROJECTILE, GetId(), sizeof(CNetObj_DDNetProjectile)));
		if(!pProj)
			return;

		pProj->m_X = round_to_int(m_Origin.x * 100.0f);
		pProj->m_Y = round_to_int(m_Origin.y * 100.0f);
		pProj->m_VelX = round_to_int(StartVel.x * 1e6f);
		pProj->m_VelY = round_to_int(StartVel.y * 1e6f);
		pProj->m_Type = WEAPON_GUN;
		pProj->m_StartTick = m_SpawnTick;
		pProj->m_Owner = -1;
		pProj->m_Flags = 0;
		pProj->m_SwitchNumber = 0;
		pProj->m_TuneZone = 0;
	}
	else
	{
		CNetObj_Projectile *pProj = Server()->SnapNewItem<CNetObj_Projectile>(GetId());
		if(!pProj)
			return;

		pProj->m_X = (int)m_Origin.x;
		pProj->m_Y = (int)m_Origin.y;
		pProj->m_VelX = (int)(StartVel.x * 100.0f);
		pProj->m_VelY = (int)(StartVel.y * 100.0f);
		pProj->m_Type = WEAPON_GUN;
		pProj->m_StartTick = m_SpawnTick;
	}
	if(m_TailLaserId >= 0 && m_TailLength > 0.0f)
	{
		const bool Sixup = Server()->IsSixup(SnappingClient);
		const CSnapContext Context(SnappingClientVersion, Sixup, SnappingClient);
		const vec2 TailPos = m_Pos - m_Direction * m_TailLength;
		GameServer()->SnapLaserObject(Context, m_TailLaserId, m_Pos, TailPos, Server()->Tick(), -1, LASERTYPE_SHOTGUN);
	}
}

void CRhythmArrow::DetachField()
{
	m_pField = nullptr;
}

void CRhythmArrow::HideForClient(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	m_HiddenMask.set(ClientId);
}

bool CRhythmArrow::IsHiddenForClient(int ClientId) const
{
	return m_HiddenMask.test(ClientId);
}

CRhythmArrow::~CRhythmArrow()
{
	if(m_pField)
		m_pField->UnregisterArrow(this);
	if(m_TailLaserId >= 0)
		Server()->SnapFreeId(m_TailLaserId);
}
