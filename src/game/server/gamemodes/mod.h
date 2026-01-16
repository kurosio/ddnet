#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H

#include <game/server/gamecontroller.h>

#include <chrono>
#include <vector>

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

class CGameControllerMod : public IGameController
{
	struct CMapMeta
	{
		char m_aAudioFile[256];
		int m_HopLength;
		float m_Bpm;
		float m_DurationSeconds;
		int m_NotesCount;
	};

	struct CNote
	{
		float m_Time;
		uint8_t m_StepBits;
	};

	EStageState m_State;
	CMapMeta m_Meta;
	std::vector<CNote> m_vNotes;
	std::chrono::steady_clock::time_point m_StartTimePoint;
	int m_CurrentNote;

	bool LoadDanceMapData(const char *pMapName);

public:
	CGameControllerMod(class CGameContext *pGameServer);
	~CGameControllerMod() override;

	void Tick() override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;

	void TickState();
	void ChangeState(EStageState State);

	bool IsLobbyMap() const;
	void UpdateNotes();
};
#endif // GAME_SERVER_GAMEMODES_MOD_H
