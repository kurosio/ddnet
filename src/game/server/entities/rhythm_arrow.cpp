/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "rhythm_arrow.h"

#include "rhythm_field.h"

#include <generated/protocol.h>

#include <game/server/gamecontext.h>

CRhythmArrow::CRhythmArrow(CGameWorld *pGameWorld, CRhythmField *pField, vec2 Origin, vec2 Direction, float Speed, int HitTick) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_RHYTHM_ARROW, Origin),
	m_pField(pField),
	m_Origin(Origin),
	m_Direction(Direction),
	m_Phase(0.0f),
	m_Speed(Speed),
	m_SpawnTick(Server()->Tick()),
	m_HitTick(HitTick)
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
	if(Tick >= m_HitTick)
	{
		m_MarkedForDestroy = true;
		return;
	}

	m_Phase = (Tick - m_SpawnTick) * m_Speed;
	m_Pos = m_Origin + m_Direction * m_Phase;
}

void CRhythmArrow::TickPaused()
{
}

void CRhythmArrow::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	const int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	const bool Sixup = Server()->IsSixup(SnappingClient);

	const vec2 From = m_Pos - m_Direction * 12.0f;
	const vec2 To = m_Pos + m_Direction * 12.0f;

	GameServer()->SnapLaserObject(CSnapContext(SnappingClientVersion, Sixup, SnappingClient), GetId(), To, From, m_SpawnTick, -1, LASERTYPE_GUN);
}

void CRhythmArrow::DetachField()
{
	m_pField = nullptr;
}

CRhythmArrow::~CRhythmArrow()
{
	if(m_pField)
		m_pField->UnregisterArrow(this);
}
