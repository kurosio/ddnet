#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H

#include <game/server/gamecontroller.h>
#include <game/server/entities/rhythm_field.h>
#include <game/gamecore.h>

#include <vector>

struct CNetObj_PlayerInput;

enum class EStageState : int
{
	STATE_LOBBY,
	STATE_ENTER,
	STATE_WARMUP,
	STATE_ACTIVE,
	STATE_FINISHED,
};

enum EStepBit : uint8_t
{
	STEP_BIT_LEFT = 1 << 0,
	STEP_BIT_RIGHT = 1 << 1,
	STEP_BIT_UP = 1 << 2,
	STEP_BIT_DOWN = 1 << 3,
};

enum class ERhythmHitGrade : uint8_t
{
	NONE = 0,
	PERFECT,
	GOOD,
	BAD,
	MISS,
};

class CGameControllerMod : public IGameController
{
	struct CMapMeta
	{
		char m_aAudioFile[256];
		int m_HopLength;
		float m_Bpm;
		float m_DurationSeconds;
		float m_ParticleFallSpeed;
		int m_NotesCount;
		int m_TapCount;
		int m_HoldsCount;
	};

	struct CNote
	{
		double m_Time;
		double m_TimeEnd;
		uint8_t m_StepBits;
		bool m_IsHold;
	};
	struct CHoldSegment
	{
		double m_Time;
		uint8_t m_StepBits;
	};

	EStageState m_State;
	CMapMeta m_Meta;
	std::vector<CNote> m_vNotes;
	std::vector<int> m_vNoteTicks;
	std::vector<CHoldSegment> m_vHoldSegments;
	std::vector<int> m_vHoldSegmentTicks;
	int m_CurrentNote;
	int m_NextSpawnNote;
	int m_CurrentHoldSegment;
	struct SRhythmScore
	{
		int m_Perfect;
		int m_Good;
		int m_Bad;
		int m_Miss;
		ERhythmHitGrade m_LastGrade;
	};
	CRhythmField *m_pRhythmField;
	CNetObj_PlayerInput m_aPrevInputs[MAX_CLIENTS];
	int m_aLanePressTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLaneLastHitTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLaneHoldTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLaneHoldStartTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLaneHoldEndTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	bool m_aLaneHoldActive[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	bool m_aLaneHoldEndEffectPlayed[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLanePressId[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLanePressUsedId[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	uint8_t m_aNoteLaneHitMask[MAX_CLIENTS];
	uint8_t m_aHoldSegmentLaneHitMask[MAX_CLIENTS];
	SRhythmScore m_aScores[MAX_CLIENTS];
	vec2 m_FieldAnchorPos;
	bool m_FieldAnchorValid;
	CTuningParams m_RhythmTuningBackup;
	bool m_RhythmTuningActive;

	bool LoadDanceMapData(const char *pMapName);
	bool FindFieldAnchorFromMap(vec2 &OutPos) const;
	void ScoreHit(int ClientId, int RatingDelta);
	int ScorePoints(const SRhythmScore &Score) const;
	void ResetClientState(int ClientId);
	void TryStartHold(int ClientId, int LaneIndex, int PressTick, int HitWindowTicks, bool UseTickNotes);
	void SaveRhythmResults();
	float EffectiveFallSpeedPerBeat() const;
	void ApplyRhythmTuning();
	void RestoreRhythmTuning();

public:
	CGameControllerMod(class CGameContext *pGameServer);
	~CGameControllerMod() override;

	void Tick() override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	void Snap(int SnappingClient) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	void OnDirectInput(int ClientId, const CNetObj_PlayerInput *pNewInput) override;

	void TickState();
	void ChangeState(EStageState State);

	bool IsLobbyMap() const;
	void UpdateNotes();
};
#endif // GAME_SERVER_GAMEMODES_MOD_H
