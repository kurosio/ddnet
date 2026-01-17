#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H

#include <game/server/gamecontroller.h>
#include <game/server/entities/rhythm_field.h>

#include <deque>
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

	EStageState m_State;
	CMapMeta m_Meta;
	std::vector<CNote> m_vNotes;
	std::vector<int> m_vNoteTicks;
	int m_CurrentNote;
	int m_NextSpawnNote;
	struct SRhythmScore
	{
		int m_Perfect;
		int m_Good;
		int m_Bad;
		int m_Miss;
		ERhythmHitGrade m_LastGrade;
	};
	struct SRhythmInput
	{
		int m_TargetTick;
		int m_ReceiveTick;
		CNetObj_PlayerInput m_Input;
	};

	CRhythmField *m_pRhythmField;
	CNetObj_PlayerInput m_aPrevInputs[MAX_CLIENTS];
	CNetObj_PlayerInput m_aBufferedInputs[MAX_CLIENTS];
	int m_aBufferedInputReceiveTick[MAX_CLIENTS];
	bool m_aHasBufferedInputs[MAX_CLIENTS];
	int m_aLanePressTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLaneLastHitTick[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLanePressId[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	int m_aLanePressUsedId[MAX_CLIENTS][SRhythmFieldConfig::s_LaneCount];
	uint8_t m_aNoteLaneHitMask[MAX_CLIENTS];
	SRhythmScore m_aScores[MAX_CLIENTS];
	std::deque<SRhythmInput> m_aRhythmInputQueue[MAX_CLIENTS];
	int m_aRhythmLateInputs[MAX_CLIENTS];
	int m_aRhythmSkippedInputs[MAX_CLIENTS];
	int m_aRhythmLastLogTick[MAX_CLIENTS];
	int64_t m_aRhythmApplyDelaySum[MAX_CLIENTS];
	int m_aRhythmApplyDelayMax[MAX_CLIENTS];
	int m_aRhythmApplyDelayCount[MAX_CLIENTS];
	vec2 m_FieldAnchorPos;
	bool m_FieldAnchorValid;

	bool LoadDanceMapData(const char *pMapName);
	bool FindFieldAnchorFromMap(vec2 &OutPos) const;
	void ScoreHit(int ClientId, int RatingDelta);

public:
	CGameControllerMod(class CGameContext *pGameServer);
	~CGameControllerMod() override;

	void Tick() override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	void Snap(int SnappingClient) override;
	void OnDirectInput(int ClientId, const CNetObj_PlayerInput *pNewInput) override;

	void TickState();
	void ChangeState(EStageState State);

	bool IsLobbyMap() const;
	void UpdateNotes();
};
#endif // GAME_SERVER_GAMEMODES_MOD_H
