#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H

#include <game/server/gamecontroller.h>

enum class StageState : int
{
	STATE_LOBBY,
	STATE_ENTER,
	STATE_WARMUP,
	STATE_ACTIVE,
	STATE_FINISHED,
};

class CGameControllerMod : public IGameController
{
	StageState m_State;

public:
	CGameControllerMod(class CGameContext *pGameServer);
	~CGameControllerMod() override;

	void Tick() override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;

	void TickState();
	void ChangeState(StageState State);

	bool IsLobbyMap() const;
};
#endif // GAME_SERVER_GAMEMODES_MOD_H
