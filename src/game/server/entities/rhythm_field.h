/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_RHYTHM_FIELD_H
#define GAME_SERVER_ENTITIES_RHYTHM_FIELD_H

#include <game/server/entity.h>

#include <vector>

class CRhythmArrow;

class CRhythmField : public CEntity
{
public:
	CRhythmField(CGameWorld *pGameWorld, vec2 Pos, float Bpm, float HitRadius);
	~CRhythmField() override;

	void Reset() override;
	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;

	void SetBpm(float Bpm);
	void SetAutoSpawn(bool Auto);
	void SetHitZone(vec2 Pos);
	int BeatIntervalTicks() const { return m_BeatIntervalTicks; }
	void SpawnLaneArrow(int LaneIndex, int HitTick);

	void RegisterArrow(CRhythmArrow *pArrow);
	void UnregisterArrow(CRhythmArrow *pArrow);

	const std::vector<CRhythmArrow *> &ActiveArrows() const { return m_vArrows; }
	float Bpm() const { return m_Bpm; }
	float BeatPeriod() const { return m_BeatPeriod; }
	vec2 HitZonePos() const { return m_HitZonePos; }
	float HitZoneRadius() const { return m_HitZoneRadius; }

private:
	void EnsureSnapIds();
	void UpdateBeatTiming();
	void SpawnArrow(vec2 Origin, vec2 Direction, int HitTick);
	void SpawnArrow();

	float m_Bpm;
	float m_BeatPeriod;
	int m_BeatIntervalTicks;
	int m_NextBeatTick;
	int m_NextDirectionIndex;
	float m_ArrowTravelDistance;
	bool m_AutoSpawn;

	std::vector<CRhythmArrow *> m_vArrows;
	vec2 m_HitZonePos;
	float m_HitZoneRadius;
	int m_HitLineLaserId;
};

#endif
