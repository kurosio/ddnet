#include "mod.h"

#include <base/math.h>
#include <cmath>
#include <base/system.h>

#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/storage.h>

#include <game/server/player.h>
#include <game/server/entities/character.h>
#include <game/server/entities/rhythm_field.h>
#include <game/gamecore.h>

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
}

CGameControllerMod::CGameControllerMod(class CGameContext *pGameServer) :
	IGameController(pGameServer)
{
	m_State = EStageState::STATE_LOBBY;
	m_pGameType = GAME_TYPE_NAME;
	mem_zero(&m_Meta, sizeof(m_Meta));
	m_CurrentNote = 0;
	m_NextSpawnNote = 0;
	mem_zero(m_apRhythmFields, sizeof(m_apRhythmFields));
	mem_zero(m_aPrevInputs, sizeof(m_aPrevInputs));
	mem_zero(m_aLanePressTick, sizeof(m_aLanePressTick));

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
			m_vNoteTicks.clear();
			m_vNoteTicks.reserve(m_vNotes.size());
			for(const auto &Note : m_vNotes)
			{
				const int NoteTick = m_RoundStartTick + round_to_int(Note.m_Time * Server()->TickSpeed());
				m_vNoteTicks.push_back(NoteTick);
			}
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(m_apRhythmFields[i])
				{
					m_apRhythmFields[i]->Reset();
					m_apRhythmFields[i] = nullptr;
				}
			}
			break;

		case EStageState::STATE_FINISHED:
			DoWarmup(-1);
			ChangeMap("lobby");
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

	Error = false;
	Error = Error || Meta.type != json_object;
	Error = Error || Notes.type != json_array;
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

	Error = false;
	Error = Error || AudioFile.type != json_string;
	Error = Error || HopLength.type != json_integer;
	Error = Error || Bpm.type != json_double;
	Error = Error || DurationSeconds.type != json_double;
	Error = Error || NotesCount.type != json_integer;
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

		CNote ParsedNote{};
		ParsedNote.m_Time = (float)json_double_get(&T);

		if(!ParseStepBits(StepBits, &ParsedNote.m_StepBits))
		{
			json_value_free(pJsonData);
			return false;
		}

		m_vNotes.push_back(ParsedNote);
	}

	json_value_free(pJsonData);
	return true;
}

bool CGameControllerMod::IsLobbyMap() const
{
	return str_comp("lobby", Server()->GetMapName()) == 0;
}

void CGameControllerMod::UpdateNotes()
{
	constexpr float FieldHitRadius = 32.0f;

	const int CurrentTick = Server()->Tick();
	const float ElapsedSec = (CurrentTick - m_RoundStartTick) / float(Server()->TickSpeed());
	const bool UseTickNotes = m_vNoteTicks.size() == m_vNotes.size();
	int LeadTicks = 0;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CCharacter *pChar = GameServer()->GetPlayerChar(i);
		if(!pChar)
		{
			if(m_apRhythmFields[i])
			{
				m_apRhythmFields[i]->Reset();
				m_apRhythmFields[i] = nullptr;
			}
			continue;
		}

		if(!m_apRhythmFields[i])
		{
			vec2 FieldPos = pChar->m_Pos + vec2(0.0f, SRhythmFieldConfig::s_FieldOffsetY);
			m_apRhythmFields[i] = GameServer()->CreateRhythmField(FieldPos, m_Meta.m_Bpm, FieldHitRadius);
			if(m_apRhythmFields[i])
				m_apRhythmFields[i]->SetAutoSpawn(false);
		}

		if(m_apRhythmFields[i])
		{
			m_apRhythmFields[i]->SetHitZone(pChar->m_Pos + vec2(0.0f, SRhythmFieldConfig::s_FieldOffsetY));
			m_apRhythmFields[i]->SetBpm(m_Meta.m_Bpm);
			LeadTicks = maximum(LeadTicks, (int)std::round(m_apRhythmFields[i]->BeatIntervalTicks() * SRhythmFieldConfig::s_LeadBeats));
		}

		pChar->Freeze(1);

		const CNetObj_PlayerInput CurrentInput = GameServer()->GetLastPlayerInput(i);
		CNetObj_PlayerInput &PrevInput = m_aPrevInputs[i];

		const bool LeftPressed = CurrentInput.m_Direction < 0 && PrevInput.m_Direction >= 0;
		const bool RightPressed = CurrentInput.m_Direction > 0 && PrevInput.m_Direction <= 0;
		const bool JumpPressed = CountInput(PrevInput.m_Jump, CurrentInput.m_Jump).m_Presses > 0;

		if(LeftPressed)
			m_aLanePressTick[i][0] = CurrentTick;
		if(JumpPressed)
			m_aLanePressTick[i][1] = CurrentTick;
		if(RightPressed)
			m_aLanePressTick[i][2] = CurrentTick;

		PrevInput = CurrentInput;
	}

	if(LeadTicks <= 0)
		return;

	while(m_NextSpawnNote < (int)m_vNotes.size() &&
		(UseTickNotes ? CurrentTick >= m_vNoteTicks[m_NextSpawnNote] - LeadTicks : (double)m_vNotes[m_NextSpawnNote].m_Time <= (ElapsedSec + (float)LeadTicks / Server()->TickSpeed())))
	{
		const CNote &Note = m_vNotes[m_NextSpawnNote];
		const int NoteTick = UseTickNotes ? m_vNoteTicks[m_NextSpawnNote] : (m_RoundStartTick + round_to_int(Note.m_Time * Server()->TickSpeed()));

		const int aLaneBits[3] = {
			Note.m_StepBits & STEP_BIT_LEFT,
			Note.m_StepBits & (STEP_BIT_UP | STEP_BIT_DOWN),
			Note.m_StepBits & STEP_BIT_RIGHT,
		};

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			CRhythmField *pField = m_apRhythmFields[i];
			if(!pField)
				continue;

			for(int LaneIndex = 0; LaneIndex < 3; ++LaneIndex)
			{
				if(aLaneBits[LaneIndex])
					pField->SpawnLaneArrow(LaneIndex, NoteTick);
			}
		}

		++m_NextSpawnNote;
	}

	while(m_CurrentNote < (int)m_vNotes.size() && (UseTickNotes ? CurrentTick >= m_vNoteTicks[m_CurrentNote] : (double)m_vNotes[m_CurrentNote].m_Time <= ElapsedSec))
	{
		const CNote &Note = m_vNotes[m_CurrentNote];
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			auto *pChar = GameServer()->GetPlayerChar(i);
			if(pChar)
				GameServer()->CreateDeath(pChar->m_Pos, i);
		}

		++m_CurrentNote;
	}
}
