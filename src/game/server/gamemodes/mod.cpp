#include "mod.h"

#include <engine/shared/config.h>
#include <game/server/player.h>

// Exchange this to a string that identifies your game mode.
// DM, TDM and CTF are reserved for teeworlds original modes.
// DDraceNetwork and TestDDraceNetwork are used by DDNet.
#define GAME_TYPE_NAME "Dance"

CGameControllerMod::CGameControllerMod(class CGameContext *pGameServer) :
	IGameController(pGameServer)
{
	m_State = StageState::STATE_LOBBY;
	m_pGameType = GAME_TYPE_NAME;
}

CGameControllerMod::~CGameControllerMod() = default;

void CGameControllerMod::Tick()
{
	if(m_State == StageState::STATE_LOBBY)
	{
		if(!IsLobbyMap())
			ChangeState(StageState::STATE_ENTER);
	}
	else if(Server()->ClientCount() <= 0)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "inactive dance battle session (transition to lobby)");
		ChangeState(StageState::STATE_LOBBY);
		return;
	}

	TickState();
	IGameController::Tick();
}

void CGameControllerMod::OnPlayerConnect(CPlayer* pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	int ClientId = pPlayer->GetCid();

	// init the player
	//Score()->PlayerData(ClientId)->Reset();

	// Can't set score here as LoadScore() is threaded, run it in
	// LoadScoreThreaded() instead
	//Score()->LoadPlayerData(ClientId);

	if(!Server()->ClientPrevIngame(ClientId))
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' entered and joined the %s", Server()->ClientName(ClientId), GetTeamName(pPlayer->GetTeam()));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf, -1, CGameContext::FLAG_SIX);
	}

	if(IsLobbyMap())
		GameServer()->SendChatTarget(ClientId, "You are in the lobby, start voting for the start of the music battle!");
}

void CGameControllerMod::TickState()
{
	// state tick
	if(m_State == StageState::STATE_WARMUP)
	{
		if(m_Warmup == Server()->TickSpeed() * 3)
			GameServer()->CreateSoundGlobal(SOUND_SELF_3_2_1_GO);
		else if(!m_Warmup)
			ChangeState(StageState::STATE_ACTIVE);
	}
}

void CGameControllerMod::ChangeState(StageState State)
{
	if(m_State == State)
		return;

	m_State = State;

	switch(m_State)
	{
		default: break;

		case StageState::STATE_LOBBY:
			DoWarmup(-1);
			ChangeMap("lobby");
			break;

		case StageState::STATE_ENTER:
			ChangeState(StageState::STATE_WARMUP);
			break;

		case StageState::STATE_WARMUP:
			DoWarmup(15);
			break;

		case StageState::STATE_ACTIVE:
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Music: %s", Server()->GetMapName());
			GameServer()->SendChatTarget(-1, aBuf);
			GameServer()->SendChatTarget(-1, "The dance competition has begun.");
			GameServer()->CreateSoundGlobal(SOUND_SELF_MUSIC);
			break;
	}
}

bool CGameControllerMod::IsLobbyMap() const
{
	return str_comp("lobby", Server()->GetMapName()) == 0;
}
