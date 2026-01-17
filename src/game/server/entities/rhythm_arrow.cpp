/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "rhythm_arrow.h"

#include "rhythm_field.h"

#include <generated/protocol.h>

#include <game/server/gamecontext.h>

CRhythmArrow::CRhythmArrow(CGameWorld *pGameWorld, CRhythmField *pField, vec2 Origin, vec2 Direction, float SpeedPerTick, int HitTick, int LaneIndex, float MissY, float VelScale) :
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
	m_VelScale(VelScale)
{
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

	CNetObj_Projectile *pProj = Server()->SnapNewItem<CNetObj_Projectile>(GetId());
	if(!pProj)
		return;

	pProj->m_X = (int)m_Origin.x;
	pProj->m_Y = (int)m_Origin.y;
	pProj->m_VelX = (int)(m_Direction.x * m_VelScale * 100.0f);
	pProj->m_VelY = (int)(m_Direction.y * m_VelScale * 100.0f);
	pProj->m_StartTick = m_SpawnTick;
	pProj->m_Type = WEAPON_GUN;
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

CRhythmArrow::~CRhythmArrow()
{
	if(m_pField)
		m_pField->UnregisterArrow(this);
}
