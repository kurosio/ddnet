/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_RHYTHM_ARROW_H
#define GAME_SERVER_ENTITIES_RHYTHM_ARROW_H

#include <game/server/entity.h>

class CRhythmField;

class CRhythmArrow : public CEntity
{
public:
	CRhythmArrow(CGameWorld *pGameWorld, CRhythmField *pField, vec2 Origin, vec2 Direction, float SpeedPerTick, int HitTick, float MissY, float VelScale);
	~CRhythmArrow() override;

	void Reset() override;
	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;

	void DetachField();

	vec2 Direction() const { return m_Direction; }
	float Phase() const { return m_Phase; }
	float Speed() const { return m_Speed; }
	int HitTick() const { return m_HitTick; }
	float MissY() const { return m_MissY; }

private:
	CRhythmField *m_pField;
	vec2 m_Origin;
	vec2 m_Direction;
	float m_Phase;
	float m_Speed;
	int m_SpawnTick;
	int m_HitTick;
	float m_MissY;
	float m_VelScale;
};

#endif
