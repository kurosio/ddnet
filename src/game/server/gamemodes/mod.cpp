#include "mod.h"

#include <base/math.h>
#include <base/system.h>
#include <algorithm>
#include <cmath>

#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/storage.h>
#include <engine/shared/protocolglue.h>

#include <game/collision.h>
#include <game/server/player.h>
#include <game/server/entities/character.h>
#include <game/server/entities/rhythm_field.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

// Exchange this to a string that identifies your game mode.
// DM, TDM and CTF are reserved for teeworlds original modes.
// DDraceNetwork and TestDDraceNetwork are used by DDNet.
#define GAME_TYPE_NAME "Dance"

namespace
{
	bool ParseStepBits(const json_value &Value, uint8_t *pOut)
	{
		if(Value.type != json_string)
			return false;

		const char *pBits = Value.u.string.ptr;
		if(str_length(pBits) != 4)
			return false;

		const uint8_t aFlags[4] = {STEP_BIT_LEFT, STEP_BIT_RIGHT, STEP_BIT_UP, STEP_BIT_DOWN};
		uint8_t Bits = 0;
		for(int i = 0; i < 4; ++i)
		{
			if(pBits[i] == '1')
				Bits |= aFlags[i];
			else if(pBits[i] != '0')
				return false;
		}
		*pOut = Bits;
		return true;
	}

	int NoteTimeToTick(int RoundStartTick, double NoteTime, int TickSpeed)
	{
		return RoundStartTick + round_to_int(NoteTime * TickSpeed);
	}
}

CGameControllerMod::CGameControllerMod(class CGameContext *pGameServer) :
	IGameController(pGameServer)
{
	m_State = EStageState::STATE_LOBBY;
	m_pGameType = GAME_TYPE_NAME;
	mem_zero(&m_Meta, sizeof(m_Meta));
	m_CurrentNote = 0;
	m_NextSpawnNote = 0;
	m_pRhythmField = nullptr;
	m_FieldAnchorPos = vec2(0.0f, 0.0f);
	m_FieldAnchorValid = false;
	mem_zero(m_aPrevInputs, sizeof(m_aPrevInputs));
	mem_zero(m_aBufferedInputs, sizeof(m_aBufferedInputs));
	mem_zero(m_aHasBufferedInputs, sizeof(m_aHasBufferedInputs));
	mem_zero(m_aLanePressTick, sizeof(m_aLanePressTick));
	mem_zero(m_aNoteLaneHitMask, sizeof(m_aNoteLaneHitMask));
	mem_zero(m_aScores, sizeof(m_aScores));
	mem_zero(m_aRhythmLateInputs, sizeof(m_aRhythmLateInputs));
	mem_zero(m_aRhythmSkippedInputs, sizeof(m_aRhythmSkippedInputs));
	mem_zero(m_aRhythmLastLogTick, sizeof(m_aRhythmLastLogTick));

	if(!IsLobbyMap())
	{
		LoadDanceMapData(Server()->GetMapName());
	}
}

CGameControllerMod::~CGameControllerMod() = default;

void CGameControllerMod::Tick()
{
	if(m_State == EStageState::STATE_LOBBY)
	{
		if(!IsLobbyMap())
			ChangeState(EStageState::STATE_ENTER);
	}
	else if(Server()->ClientCount() <= 0)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "inactive dance battle session (transition to lobby)");
		ChangeState(EStageState::STATE_LOBBY);
		return;
	}

	TickState();
	IGameController::Tick();
}

void CGameControllerMod::Snap(int SnappingClient)
{
	CNetObj_GameInfo *pGameInfoObj = Server()->SnapNewItem<CNetObj_GameInfo>(0);
	if(!pGameInfoObj)
		return;

	pGameInfoObj->m_GameFlags = GameFlags_ClampToSix(m_GameFlags);
	pGameInfoObj->m_GameStateFlags = 0;
	if(m_GameOverTick != -1)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(GameServer()->m_World.m_Paused)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	pGameInfoObj->m_RoundStartTick = m_RoundStartTick;
	pGameInfoObj->m_WarmupTimer = m_Warmup;

	pGameInfoObj->m_RoundNum = 0;
	pGameInfoObj->m_RoundCurrent = m_RoundCount + 1;

	CNetObj_GameInfoEx *pGameInfoEx = Server()->SnapNewItem<CNetObj_GameInfoEx>(0);
	if(!pGameInfoEx)
		return;

	pGameInfoEx->m_Flags =
		GAMEINFOFLAG_GAMETYPE_RACE |
		GAMEINFOFLAG_GAMETYPE_DDRACE |
		GAMEINFOFLAG_GAMETYPE_DDNET |
		GAMEINFOFLAG_UNLIMITED_AMMO |
		GAMEINFOFLAG_ALLOW_EYE_WHEEL |
		GAMEINFOFLAG_ALLOW_HOOK_COLL |
		GAMEINFOFLAG_ALLOW_ZOOM |
		GAMEINFOFLAG_BUG_DDRACE_GHOST |
		GAMEINFOFLAG_PREDICT_DDRACE |
		GAMEINFOFLAG_PREDICT_DDRACE_TILES |
		GAMEINFOFLAG_ENTITIES_DDNET |
		GAMEINFOFLAG_ENTITIES_DDRACE |
		GAMEINFOFLAG_ENTITIES_RACE |
		GAMEINFOFLAG_RACE;
	pGameInfoEx->m_Flags2 = GAMEINFOFLAG2_HUD_DDRACE | GAMEINFOFLAG2_DDRACE_TEAM;
	if(g_Config.m_SvNoWeakHook)
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_NO_WEAK_HOOK;
	pGameInfoEx->m_Version = GAMEINFO_CURVERSION;
}

void CGameControllerMod::OnPlayerConnect(CPlayer *pPlayer)
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
	if(m_State == EStageState::STATE_WARMUP)
	{
		if(m_Warmup == Server()->TickSpeed() * 3)
			GameServer()->CreateSoundGlobal(SOUND_SELF_3_2_1_GO);
		else if(!m_Warmup)
			ChangeState(EStageState::STATE_ACTIVE);
	}

	else if(m_State == EStageState::STATE_ACTIVE)
	{
		UpdateNotes();

		const int DurationTicks = round_to_int(m_Meta.m_DurationSeconds * Server()->TickSpeed());
		if(m_GameOverTick == -1 && Server()->Tick() > (m_RoundStartTick + DurationTicks))
		{
			EndRound();
		}
		else if(m_GameOverTick != -1 && Server()->Tick() > m_GameOverTick + Server()->TickSpeed() * 10)
		{
			ChangeState(EStageState::STATE_FINISHED);
		}
	}
}

void CGameControllerMod::ChangeState(EStageState State)
{
	if(m_State == State)
		return;

	m_State = State;

	switch(m_State)
	{
		default: break;

		case EStageState::STATE_LOBBY:
			DoWarmup(-1);
			ChangeMap("lobby");
			for(auto *pPlayer : GameServer()->m_apPlayers)
			{
				if(pPlayer)
					pPlayer->ClearFixedView();
			}
			break;

		case EStageState::STATE_ENTER:
			ChangeState(EStageState::STATE_WARMUP);
			break;

		case EStageState::STATE_WARMUP:
			DoWarmup(15);
			break;

		case EStageState::STATE_ACTIVE:
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Music: %s", m_Meta.m_aAudioFile);
			GameServer()->SendChatTarget(-1, aBuf);
			str_format(aBuf, sizeof(aBuf), "BPM: %0.2f, Notes: %d", m_Meta.m_Bpm, m_Meta.m_NotesCount);
			GameServer()->SendChatTarget(-1, aBuf);
			str_format(aBuf, sizeof(aBuf), "Length: %0.2f s.", m_Meta.m_DurationSeconds);
			GameServer()->SendChatTarget(-1, aBuf);
			GameServer()->SendChatTarget(-1, "The dance competition has begun.");
			GameServer()->CreateSoundGlobal(SOUND_SELF_MUSIC);

			m_CurrentNote = 0;
			m_NextSpawnNote = 0;
			m_GameOverTick = -1;
			m_FieldAnchorValid = FindFieldAnchorFromMap(m_FieldAnchorPos);
			m_vNoteTicks.clear();
			m_vNoteTicks.reserve(m_vNotes.size());
			for(const auto &Note : m_vNotes)
			{
				m_vNoteTicks.push_back(NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed()));
			}
			if(m_pRhythmField)
			{
				m_pRhythmField->Reset();
				m_pRhythmField = nullptr;
			}
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				m_aPrevInputs[i] = CNetObj_PlayerInput{};
				m_aBufferedInputs[i] = CNetObj_PlayerInput{};
				m_aHasBufferedInputs[i] = false;
				m_aNoteLaneHitMask[i] = 0;
				m_aScores[i] = {};
				m_aRhythmInputQueue[i].clear();
				m_aRhythmLateInputs[i] = 0;
				m_aRhythmSkippedInputs[i] = 0;
				m_aRhythmLastLogTick[i] = 0;
			}
			for(auto *pPlayer : GameServer()->m_apPlayers)
			{
				if(!pPlayer)
					continue;
				if(m_FieldAnchorValid)
					pPlayer->SetFixedView(m_FieldAnchorPos + vec2(SRhythmFieldConfig::s_FieldViewOffsetX, SRhythmFieldConfig::s_FieldViewOffsetY));
				else
					pPlayer->ClearFixedView();
			}
			break;

		case EStageState::STATE_FINISHED:
			DoWarmup(-1);
			ChangeMap("lobby");
			for(auto *pPlayer : GameServer()->m_apPlayers)
			{
				if(pPlayer)
					pPlayer->ClearFixedView();
			}
			break;
	}
}

bool CGameControllerMod::LoadDanceMapData(const char *pMapName)
{
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "maps/%s.json", pMapName);

	void *pFileData = nullptr;
	unsigned FileSize = 0;
	if(!GameServer()->Storage()->ReadFile(aFilename, IStorage::TYPE_ALL, &pFileData, &FileSize))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "Missing dance data json file '%s' for map '%s'", aFilename, pMapName);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return false;
	}

	json_settings JsonSettings{};
	char aError[256];
	json_value *pJsonData = json_parse_ex(&JsonSettings, (const json_char *)pFileData, FileSize, aError);
	free(pFileData);
	if(pJsonData == nullptr)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "Failed to parse dance data json '%s': %s", aFilename, aError);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return false;
	}

	bool Error = false;
	Error = Error || pJsonData->type != json_object;
	if(Error)
	{
		json_value_free(pJsonData);
		return false;
	}

	m_vNotes.clear();
	m_vNoteTicks.clear();
	mem_zero(&m_Meta, sizeof(m_Meta));

	const json_value &Root = *pJsonData;
	const json_value &Meta = Root["meta"];
	const json_value &Notes = Root["notes"];
	const json_value &Holds = Root["holds"];

	Error = false;
	Error = Error || Meta.type != json_object;
	Error = Error || Notes.type != json_array;
	Error = Error || Holds.type != json_array;
	if(Error)
	{
		json_value_free(pJsonData);
		return false;
	}

	const json_value &AudioFile = Meta["audio_file"];
	const json_value &HopLength = Meta["hop_length"];
	const json_value &Bpm = Meta["bpm"];
	const json_value &DurationSeconds = Meta["duration_seconds"];
	const json_value &NotesCount = Meta["notes_count"];
	const json_value &TapCount = Meta["tap_count"];
	const json_value &HoldsCount = Meta["holds_count"];

	Error = false;
	Error = Error || AudioFile.type != json_string;
	Error = Error || HopLength.type != json_integer;
	Error = Error || Bpm.type != json_double;
	Error = Error || DurationSeconds.type != json_double;
	Error = Error || NotesCount.type != json_integer;
	Error = Error || TapCount.type != json_integer;
	Error = Error || HoldsCount.type != json_integer;
	if(Error)
	{
		json_value_free(pJsonData);
		return false;
	}

	str_copy(m_Meta.m_aAudioFile, json_string_get(&AudioFile), sizeof(m_Meta.m_aAudioFile));
	m_Meta.m_HopLength = json_int_get(&HopLength);
	m_Meta.m_Bpm = (float)json_double_get(&Bpm);
	m_Meta.m_DurationSeconds = (float)json_double_get(&DurationSeconds);
	m_Meta.m_NotesCount = json_int_get(&NotesCount);
	m_Meta.m_TapCount = json_int_get(&TapCount);
	m_Meta.m_HoldsCount = json_int_get(&HoldsCount);

	for(unsigned i = 0; i < Notes.u.array.length; ++i)
	{
		const json_value &Note = Notes[i];
		const json_value &T = Note["t"];
		const json_value &StepBits = Note["step_bits"];

		Error = false;
		Error = Error || Note.type != json_object;
		Error = Error || T.type != json_double;
		Error = Error || StepBits.type == json_none;
		if(Error)
		{
			json_value_free(pJsonData);
			return false;
		}

		const double NoteTime = json_double_get(&T);
		Error = Error || !std::isfinite(NoteTime) || NoteTime < 0.0;
		if(Error)
		{
			json_value_free(pJsonData);
			return false;
		}

		CNote ParsedNote{};
		ParsedNote.m_Time = NoteTime;
		ParsedNote.m_TimeEnd = NoteTime;
		ParsedNote.m_IsHold = false;

		if(!ParseStepBits(StepBits, &ParsedNote.m_StepBits))
		{
			json_value_free(pJsonData);
			return false;
		}

		m_vNotes.push_back(ParsedNote);
	}

	for(unsigned i = 0; i < Holds.u.array.length; ++i)
	{
		const json_value &Hold = Holds[i];
		const json_value &T = Hold["t"];
		const json_value &TEnd = Hold["t_end"];
		const json_value &StepBits = Hold["step_bits"];

		Error = false;
		Error = Error || Hold.type != json_object;
		Error = Error || T.type != json_double;
		Error = Error || TEnd.type != json_double;
		Error = Error || StepBits.type == json_none;
		if(Error)
		{
			json_value_free(pJsonData);
			return false;
		}

		const double HoldTime = json_double_get(&T);
		const double HoldTimeEnd = json_double_get(&TEnd);
		Error = Error || !std::isfinite(HoldTime) || HoldTime < 0.0;
		Error = Error || !std::isfinite(HoldTimeEnd) || HoldTimeEnd < 0.0;
		Error = Error || HoldTimeEnd < HoldTime;
		if(Error)
		{
			json_value_free(pJsonData);
			return false;
		}

		CNote ParsedHold{};
		ParsedHold.m_Time = HoldTime;
		ParsedHold.m_TimeEnd = HoldTimeEnd;
		ParsedHold.m_IsHold = true;

		if(!ParseStepBits(StepBits, &ParsedHold.m_StepBits))
		{
			json_value_free(pJsonData);
			return false;
		}

		m_vNotes.push_back(ParsedHold);
	}

	std::stable_sort(m_vNotes.begin(), m_vNotes.end(), [](const CNote &Left, const CNote &Right)
	{
		return Left.m_Time < Right.m_Time;
	});
	m_Meta.m_HoldsCount = static_cast<int>(std::count_if(m_vNotes.begin(), m_vNotes.end(), [](const CNote &Note)
	{
		return Note.m_IsHold;
	}));
	m_Meta.m_TapCount = static_cast<int>(m_vNotes.size()) - m_Meta.m_HoldsCount;
	m_Meta.m_NotesCount = static_cast<int>(m_vNotes.size());

	json_value_free(pJsonData);
	return true;
}

bool CGameControllerMod::IsLobbyMap() const
{
	return str_comp("lobby", Server()->GetMapName()) == 0;
}

bool CGameControllerMod::FindFieldAnchorFromMap(vec2 &OutPos) const
{
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	const int TargetIndex = ENTITY_OFFSET + ENTITY_RHYTHM_FIELD;
	for(int y = 0; y < Height; ++y)
	{
		for(int x = 0; x < Width; ++x)
		{
			const int Index = y * Width + x;
			if(pCollision->GetTileIndex(Index) != TargetIndex)
				continue;
			OutPos = pCollision->GetPos(Index);
			return true;
		}
	}
	return false;
}

void CGameControllerMod::UpdateNotes()
{
	constexpr float FieldHitRadius = 32.0f;
	constexpr int LaneCount = SRhythmFieldConfig::s_LaneCount;
	constexpr int MaxInputQueueSize = 64;

	const int CurrentTick = Server()->Tick();
	const int JitterBufferTicks = g_Config.m_SvRhythmJitterBufferTicks;
	const double ElapsedSec = (CurrentTick - m_RoundStartTick) / static_cast<double>(Server()->TickSpeed());
	const bool UseTickNotes = m_vNoteTicks.size() == m_vNotes.size();
	int LeadTicks = 0;
	CNetObj_PlayerInput aCurrentInputs[MAX_CLIENTS];
	bool aHasInput[MAX_CLIENTS];
	mem_zero(aHasInput, sizeof(aHasInput));

	auto ScoreHit = [this](int ClientId, int RatingDelta)
	{
		if(RatingDelta <= SRhythmFieldConfig::s_PerfectWindowTicks)
			m_aScores[ClientId].m_Perfect++;
		else if(RatingDelta <= SRhythmFieldConfig::s_GoodWindowTicks)
			m_aScores[ClientId].m_Good++;
		else if(RatingDelta <= SRhythmFieldConfig::s_BadWindowTicks)
			m_aScores[ClientId].m_Bad++;
		else
			m_aScores[ClientId].m_Miss++;
	};
	auto LogRhythmInputMetrics = [this, CurrentTick](int ClientId)
	{
		if(CurrentTick - m_aRhythmLastLogTick[ClientId] < Server()->TickSpeed())
			return;
		if(m_aRhythmLateInputs[ClientId] == 0 && m_aRhythmSkippedInputs[ClientId] == 0)
			return;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "rhythm input jitter: cid=%d late=%d skipped=%d buffer_ticks=%d", ClientId, m_aRhythmLateInputs[ClientId], m_aRhythmSkippedInputs[ClientId], g_Config.m_SvRhythmJitterBufferTicks);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "rhythm", aBuf);
		m_aRhythmLastLogTick[ClientId] = CurrentTick;
	};

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CCharacter *pChar = GameServer()->GetPlayerChar(i);
		if(!pChar)
		{
			m_aPrevInputs[i] = CNetObj_PlayerInput{};
			mem_zero(m_aLanePressTick[i], sizeof(m_aLanePressTick[i]));
			m_aNoteLaneHitMask[i] = 0;
			m_aScores[i] = {};
			m_aRhythmInputQueue[i].clear();
			m_aHasBufferedInputs[i] = false;
			m_aRhythmLateInputs[i] = 0;
			m_aRhythmSkippedInputs[i] = 0;
			aHasInput[i] = false;
			continue;
		}

		const CNetObj_PlayerInput RawInput = GameServer()->GetLastPlayerInput(i);
		auto &Queue = m_aRhythmInputQueue[i];
		Queue.push_back({CurrentTick + JitterBufferTicks, RawInput});
		if((int)Queue.size() > MaxInputQueueSize)
		{
			Queue.pop_front();
			m_aRhythmSkippedInputs[i]++;
			LogRhythmInputMetrics(i);
		}

		CNetObj_PlayerInput CurrentInput{};
		bool HasBufferedInput = false;
		int MaxLateTicks = 0;
		while(!Queue.empty() && Queue.front().m_TargetTick <= CurrentTick)
		{
			HasBufferedInput = true;
			CurrentInput = Queue.front().m_Input;
			MaxLateTicks = maximum(MaxLateTicks, CurrentTick - Queue.front().m_TargetTick);
			Queue.pop_front();
		}

		if(HasBufferedInput)
		{
			if(MaxLateTicks > 0)
			{
				m_aRhythmLateInputs[i]++;
				LogRhythmInputMetrics(i);
			}
			m_aBufferedInputs[i] = CurrentInput;
			m_aHasBufferedInputs[i] = true;
		}
		else if(m_aHasBufferedInputs[i])
		{
			CurrentInput = m_aBufferedInputs[i];
		}
		else
		{
			m_aRhythmSkippedInputs[i]++;
			LogRhythmInputMetrics(i);
			aHasInput[i] = false;
			continue;
		}

		CNetObj_PlayerInput &PrevInput = m_aPrevInputs[i];

		const bool LeftPressed = (CurrentInput.m_Direction < 0 && PrevInput.m_Direction >= 0) ||
			(CurrentInput.m_Direction == 0 && PrevInput.m_Direction > 0);
		const bool RightPressed = (CurrentInput.m_Direction > 0 && PrevInput.m_Direction <= 0) ||
			(CurrentInput.m_Direction == 0 && PrevInput.m_Direction < 0);
		const bool JumpPressed = CountInput(PrevInput.m_Jump, CurrentInput.m_Jump).m_Presses > 0;

		const bool LeftHeld = CurrentInput.m_Direction < 0;
		const bool JumpHeld = (CurrentInput.m_Jump & 1) != 0;
		const bool RightHeld = CurrentInput.m_Direction > 0;
		if(LeftPressed || LeftHeld)
			m_aLanePressTick[i][0] = CurrentTick;
		if(JumpPressed || JumpHeld)
			m_aLanePressTick[i][1] = CurrentTick;
		if(RightPressed || RightHeld)
			m_aLanePressTick[i][2] = CurrentTick;

		aCurrentInputs[i] = CurrentInput;
		aHasInput[i] = true;
	}

	if(m_FieldAnchorValid && !m_pRhythmField)
	{
		const vec2 FieldPos = m_FieldAnchorPos;
		m_pRhythmField = GameServer()->CreateRhythmField(FieldPos, m_Meta.m_Bpm, FieldHitRadius);
		if(m_pRhythmField)
		{
			m_pRhythmField->SetAutoSpawn(false);
			m_pRhythmField->SetHitZone(FieldPos);
		}
	}

	if(m_pRhythmField)
	{
		m_pRhythmField->SetBpm(m_Meta.m_Bpm);
		LeadTicks = maximum(LeadTicks, (int)std::round(m_pRhythmField->BeatIntervalTicks() * SRhythmFieldConfig::s_LeadBeats));
	}

	if(LeadTicks <= 0)
		return;

	while(m_NextSpawnNote < (int)m_vNotes.size() &&
		(UseTickNotes ? CurrentTick >= m_vNoteTicks[m_NextSpawnNote] - LeadTicks : m_vNotes[m_NextSpawnNote].m_Time <= (ElapsedSec + static_cast<double>(LeadTicks) / Server()->TickSpeed())))
	{
		const CNote &Note = m_vNotes[m_NextSpawnNote];
		const int NoteTick = UseTickNotes ? m_vNoteTicks[m_NextSpawnNote] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());

		if(Note.m_IsHold)
		{
			++m_NextSpawnNote;
			continue;
		}

		const int aLaneBits[LaneCount] = {
			Note.m_StepBits & STEP_BIT_LEFT,
			Note.m_StepBits & (STEP_BIT_UP | STEP_BIT_DOWN),
			Note.m_StepBits & STEP_BIT_RIGHT,
		};

		if(m_pRhythmField)
		{
			for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
			{
				if(aLaneBits[LaneIndex])
					m_pRhythmField->SpawnLaneArrow(LaneIndex, NoteTick);
			}
		}

		++m_NextSpawnNote;
	}

	while(m_CurrentNote < (int)m_vNotes.size())
	{
		const CNote &Note = m_vNotes[m_CurrentNote];
		const int NoteTick = UseTickNotes ? m_vNoteTicks[m_CurrentNote] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());

		if(CurrentTick < NoteTick - SRhythmFieldConfig::s_BadWindowTicks)
			break;

		if(Note.m_IsHold)
		{
			++m_CurrentNote;
			continue;
		}

		const bool WindowExpired = CurrentTick > NoteTick + SRhythmFieldConfig::s_BadWindowTicks;
		const int aLaneBits[LaneCount] = {
			Note.m_StepBits & STEP_BIT_LEFT,
			Note.m_StepBits & (STEP_BIT_UP | STEP_BIT_DOWN),
			Note.m_StepBits & STEP_BIT_RIGHT,
		};

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!m_pRhythmField)
				continue;

			if(!aHasInput[i])
				continue;
			const vec2 HitPos = m_pRhythmField->HitZonePos();
			const float HalfWidth = SRhythmFieldConfig::s_LaneWidth * 1.5f;

			for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
			{
				if(!aLaneBits[LaneIndex])
					continue;

				const uint8_t LaneMask = 1u << LaneIndex;
				if(m_aNoteLaneHitMask[i] & LaneMask)
					continue;

				const bool LaneActive = CurrentTick <= m_aLanePressTick[i][LaneIndex];
				if(LaneActive)
				{
					const int RatingDelta = std::abs(CurrentTick - NoteTick);
					if(RatingDelta > SRhythmFieldConfig::s_BadWindowTicks)
						continue;

					const float X = HitPos.x - HalfWidth + SRhythmFieldConfig::s_LaneWidth * (LaneIndex + 0.5f);
					const vec2 EffectPos(X, HitPos.y);
					CClientMask Mask;
					Mask.set(i);
					GameServer()->CreateExplosion(EffectPos, -1, WEAPON_GRENADE, true, -1, Mask);
					GameServer()->CreateSound(EffectPos, SOUND_PICKUP_HEALTH, Mask);
					ScoreHit(i, RatingDelta);
					m_pRhythmField->HideArrowForClient(LaneIndex, NoteTick, i);
					m_aNoteLaneHitMask[i] |= LaneMask;
				}
				else if(WindowExpired)
				{
					m_aScores[i].m_Miss++;
					m_aNoteLaneHitMask[i] |= LaneMask;
				}
			}
		}

		if(!WindowExpired)
			break;

		++m_CurrentNote;
		for(int i = 0; i < MAX_CLIENTS; ++i)
			m_aNoteLaneHitMask[i] = 0;
	}

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(aHasInput[i])
			m_aPrevInputs[i] = aCurrentInputs[i];
	}
}
