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
	constexpr int LaneCount = SRhythmFieldConfig::s_LaneCount;
	bool IsValidClientId(int ClientId)
	{
		return ClientId >= 0 && ClientId < MAX_CLIENTS;
	}

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

	void FillLaneBits(uint8_t StepBits, int (&aLaneBits)[LaneCount])
	{
		aLaneBits[0] = StepBits & STEP_BIT_LEFT;
		aLaneBits[1] = StepBits & (STEP_BIT_UP | STEP_BIT_DOWN);
		aLaneBits[2] = StepBits & STEP_BIT_RIGHT;
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
	m_CurrentHoldSegment = 0;
	m_pRhythmField = nullptr;
	m_FieldAnchorPos = vec2(0.0f, 0.0f);
	m_FieldAnchorValid = false;
	mem_zero(m_aPrevInputs, sizeof(m_aPrevInputs));
	mem_zero(m_aLanePressTick, sizeof(m_aLanePressTick));
	mem_zero(m_aLaneLastHitTick, sizeof(m_aLaneLastHitTick));
	mem_zero(m_aLaneHoldTick, sizeof(m_aLaneHoldTick));
	mem_zero(m_aLaneHoldStartTick, sizeof(m_aLaneHoldStartTick));
	mem_zero(m_aLaneHoldEndTick, sizeof(m_aLaneHoldEndTick));
	mem_zero(m_aLaneHoldActive, sizeof(m_aLaneHoldActive));
	mem_zero(m_aLaneHoldEndEffectPlayed, sizeof(m_aLaneHoldEndEffectPlayed));
	mem_zero(m_aLanePressId, sizeof(m_aLanePressId));
	mem_zero(m_aLanePressUsedId, sizeof(m_aLanePressUsedId));
	mem_zero(m_aNoteLaneHitMask, sizeof(m_aNoteLaneHitMask));
	mem_zero(m_aHoldSegmentLaneHitMask, sizeof(m_aHoldSegmentLaneHitMask));
	mem_zero(m_aScores, sizeof(m_aScores));

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
				ResetClientState(i);
			}
			m_CurrentHoldSegment = 0;
			m_vHoldSegments.clear();
			m_vHoldSegmentTicks.clear();
			for(const auto &Note : m_vNotes)
			{
				if(!Note.m_IsHold)
					continue;
				const double Duration = Note.m_TimeEnd - Note.m_Time;
				for(int SegmentIndex = 1; SegmentIndex <= 3; ++SegmentIndex)
				{
					const double SegmentTime = Note.m_Time + Duration * (SegmentIndex / 3.0);
					m_vHoldSegments.push_back({SegmentTime, Note.m_StepBits});
				}
			}
			std::stable_sort(m_vHoldSegments.begin(), m_vHoldSegments.end(), [](const CHoldSegment &Left, const CHoldSegment &Right)
			{
				return Left.m_Time < Right.m_Time;
			});
			m_vHoldSegmentTicks.reserve(m_vHoldSegments.size());
			for(const auto &Segment : m_vHoldSegments)
			{
				m_vHoldSegmentTicks.push_back(NoteTimeToTick(m_RoundStartTick, Segment.m_Time, Server()->TickSpeed()));
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

	UpdateRhythmProjectileTuning();

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
		if(ParsedHold.m_StepBits == STEP_BIT_UP || ParsedHold.m_StepBits == STEP_BIT_DOWN || ParsedHold.m_StepBits == STEP_BIT_RIGHT)
			ParsedHold.m_StepBits = STEP_BIT_UP | STEP_BIT_DOWN;

		m_vNotes.push_back(ParsedHold);
	}

	std::stable_sort(m_vNotes.begin(), m_vNotes.end(), [](const CNote &Left, const CNote &Right)
	{
		return Left.m_Time < Right.m_Time;
	});

	struct CHoldWindow
	{
		double m_Time;
		double m_TimeEnd;
		uint8_t m_LaneMask;
	};
	std::vector<CHoldWindow> vHoldWindows;
	vHoldWindows.reserve(m_vNotes.size());
	for(const auto &Note : m_vNotes)
	{
		if(!Note.m_IsHold)
			continue;
		uint8_t LaneMask = 0;
		if(Note.m_StepBits & STEP_BIT_LEFT)
			LaneMask |= 1 << 0;
		if(Note.m_StepBits & (STEP_BIT_UP | STEP_BIT_DOWN))
			LaneMask |= 1 << 1;
		if(Note.m_StepBits & STEP_BIT_RIGHT)
			LaneMask |= 1 << 2;
		vHoldWindows.push_back({Note.m_Time, Note.m_TimeEnd, LaneMask});
	}

	if(!vHoldWindows.empty())
	{
		std::vector<CNote> vFiltered;
		vFiltered.reserve(m_vNotes.size());
		for(const auto &Note : m_vNotes)
		{
			if(Note.m_IsHold)
			{
				vFiltered.push_back(Note);
				continue;
			}
			uint8_t LaneMask = 0;
			if(Note.m_StepBits & STEP_BIT_LEFT)
				LaneMask |= 1 << 0;
			if(Note.m_StepBits & (STEP_BIT_UP | STEP_BIT_DOWN))
				LaneMask |= 1 << 1;
			if(Note.m_StepBits & STEP_BIT_RIGHT)
				LaneMask |= 1 << 2;
			bool Blocked = false;
			for(const auto &Hold : vHoldWindows)
			{
				if(Note.m_Time < Hold.m_Time || Note.m_Time > Hold.m_TimeEnd)
					continue;
				if(LaneMask & Hold.m_LaneMask)
				{
					Blocked = true;
					break;
				}
			}
			if(!Blocked)
				vFiltered.push_back(Note);
		}
		m_vNotes.swap(vFiltered);
	}

	m_Meta.m_HoldsCount = static_cast<int>(std::count_if(m_vNotes.begin(), m_vNotes.end(), [](const CNote &Note)
	{
		return Note.m_IsHold;
	}));
	m_Meta.m_TapCount = static_cast<int>(m_vNotes.size()) - m_Meta.m_HoldsCount;
	m_Meta.m_NotesCount = static_cast<int>(m_vNotes.size());

	json_value_free(pJsonData);
	return true;
}

void CGameControllerMod::UpdateRhythmProjectileTuning()
{
	if(m_Meta.m_Bpm <= 0.0f)
		return;

	const float BeatPeriod = 60.0f / m_Meta.m_Bpm;
	const float Speed = SRhythmFieldConfig::s_FieldHeight / BeatPeriod;
	const float Curvature = 0.0f;

	if(GlobalTuning()->m_GunSpeed == Speed && GlobalTuning()->m_GunCurvature == Curvature)
		return;

	GlobalTuning()->m_GunSpeed = Speed;
	GlobalTuning()->m_GunCurvature = Curvature;
	SendTuningParams(-1);
}

bool CGameControllerMod::IsLobbyMap() const
{
	return str_comp("lobby", Server()->GetMapName()) == 0;
}

void CGameControllerMod::ResetClientState(int ClientId)
{
	if(!IsValidClientId(ClientId))
		return;

	m_aPrevInputs[ClientId] = CNetObj_PlayerInput{};
	for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
	{
		m_aLanePressTick[ClientId][LaneIndex] = SRhythmFieldConfig::s_InvalidPressTick;
		m_aLaneLastHitTick[ClientId][LaneIndex] = SRhythmFieldConfig::s_InvalidPressTick;
		m_aLaneHoldTick[ClientId][LaneIndex] = SRhythmFieldConfig::s_InvalidPressTick;
		m_aLaneHoldStartTick[ClientId][LaneIndex] = SRhythmFieldConfig::s_InvalidPressTick;
		m_aLaneHoldEndTick[ClientId][LaneIndex] = SRhythmFieldConfig::s_InvalidPressTick;
		m_aLaneHoldActive[ClientId][LaneIndex] = false;
		m_aLaneHoldEndEffectPlayed[ClientId][LaneIndex] = false;
	}
	mem_zero(m_aLanePressId[ClientId], sizeof(m_aLanePressId[ClientId]));
	mem_zero(m_aLanePressUsedId[ClientId], sizeof(m_aLanePressUsedId[ClientId]));
	m_aNoteLaneHitMask[ClientId] = 0;
	m_aHoldSegmentLaneHitMask[ClientId] = 0;
	m_aScores[ClientId] = {};
}

void CGameControllerMod::TryStartHold(int ClientId, int LaneIndex, int PressTick, int HitWindowTicks, bool UseTickNotes)
{
	if(!IsValidClientId(ClientId))
		return;

	if(PressTick == SRhythmFieldConfig::s_InvalidPressTick)
		return;

	if(m_aLaneHoldActive[ClientId][LaneIndex] && PressTick <= m_aLaneHoldEndTick[ClientId][LaneIndex])
		return;

	const int HoldWindowTicks = SRhythmFieldConfig::s_BadWindowTicks + HitWindowTicks;
	int BestNoteIndex = -1;
	int BestDelta = 0;

	for(int NoteIndex = m_CurrentNote; NoteIndex < (int)m_vNotes.size(); ++NoteIndex)
	{
		const CNote &Note = m_vNotes[NoteIndex];
		if(!Note.m_IsHold)
			continue;

		const int NoteTick = UseTickNotes ? m_vNoteTicks[NoteIndex] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());
		if(NoteTick < PressTick - HoldWindowTicks)
			continue;
		if(NoteTick > PressTick + HoldWindowTicks)
			break;

		const int NoteEndTick = NoteTimeToTick(m_RoundStartTick, Note.m_TimeEnd, Server()->TickSpeed());
		if(PressTick > NoteEndTick + HoldWindowTicks)
			continue;

		int aLaneBits[LaneCount];
		FillLaneBits(Note.m_StepBits, aLaneBits);
		if(!aLaneBits[LaneIndex])
			continue;

		const int RawDelta = std::abs(PressTick - NoteTick);
		if(BestNoteIndex == -1 || RawDelta < BestDelta)
		{
			BestNoteIndex = NoteIndex;
			BestDelta = RawDelta;
		}
	}

	if(BestNoteIndex == -1)
		return;

	const CNote &Note = m_vNotes[BestNoteIndex];
	const int NoteTick = UseTickNotes ? m_vNoteTicks[BestNoteIndex] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());
	if(m_aLaneHoldStartTick[ClientId][LaneIndex] == NoteTick && m_aLaneHoldActive[ClientId][LaneIndex])
		return;

	const int NoteEndTick = NoteTimeToTick(m_RoundStartTick, Note.m_TimeEnd, Server()->TickSpeed());
	m_aLaneHoldActive[ClientId][LaneIndex] = true;
	m_aLaneHoldStartTick[ClientId][LaneIndex] = NoteTick;
	m_aLaneHoldEndTick[ClientId][LaneIndex] = NoteEndTick;
	m_aLaneHoldTick[ClientId][LaneIndex] = PressTick;
	m_aLaneHoldEndEffectPlayed[ClientId][LaneIndex] = false;
	m_aLanePressUsedId[ClientId][LaneIndex] = m_aLanePressId[ClientId][LaneIndex];

	const int RatingDelta = maximum(0, BestDelta - HitWindowTicks);
	ScoreHit(ClientId, RatingDelta);

	if(m_pRhythmField)
	{
		const vec2 HitPos = m_pRhythmField->HitZonePos();
		const float HalfWidth = SRhythmFieldConfig::s_LaneWidth * 1.5f;
		const float X = HitPos.x - HalfWidth + SRhythmFieldConfig::s_LaneWidth * (LaneIndex + 0.5f);
		const vec2 EffectPos(X, HitPos.y);
		CClientMask Mask;
		Mask.set(ClientId);
		GameServer()->CreateDeath(EffectPos, ClientId, Mask);
		GameServer()->CreateSound(EffectPos, SOUND_PICKUP_HEALTH, Mask);
	}
}

void CGameControllerMod::ScoreHit(int ClientId, int RatingDelta)
{
	if(!IsValidClientId(ClientId))
		return;

	if(RatingDelta <= SRhythmFieldConfig::s_PerfectWindowTicks)
	{
		m_aScores[ClientId].m_Perfect++;
		m_aScores[ClientId].m_LastGrade = ERhythmHitGrade::PERFECT;
	}
	else if(RatingDelta <= SRhythmFieldConfig::s_GoodWindowTicks)
	{
		m_aScores[ClientId].m_Good++;
		m_aScores[ClientId].m_LastGrade = ERhythmHitGrade::GOOD;
	}
	else if(RatingDelta <= SRhythmFieldConfig::s_BadWindowTicks)
	{
		m_aScores[ClientId].m_Bad++;
		m_aScores[ClientId].m_LastGrade = ERhythmHitGrade::BAD;
	}
	else
	{
		m_aScores[ClientId].m_Miss++;
		m_aScores[ClientId].m_LastGrade = ERhythmHitGrade::MISS;
	}
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

void CGameControllerMod::OnDirectInput(int ClientId, const CNetObj_PlayerInput *pNewInput)
{
	if(!IsValidClientId(ClientId))
		return;

	if(m_State != EStageState::STATE_ACTIVE || !pNewInput)
		return;

	CCharacter *pChar = GameServer()->GetPlayerChar(ClientId);
	if(!pChar)
	{
		ResetClientState(ClientId);
		return;
	}

	if(!m_pRhythmField)
	{
		m_aPrevInputs[ClientId] = *pNewInput;
		return;
	}

	const int CurrentTick = Server()->Tick();
	const int HitWindowTicks = g_Config.m_SvRhythmHitWindowTicks;
	const bool UseTickNotes = m_vNoteTicks.size() == m_vNotes.size();

	CNetObj_PlayerInput &PrevInput = m_aPrevInputs[ClientId];

	const bool aLanePressed[LaneCount] = {
		pNewInput->m_Direction < 0 && PrevInput.m_Direction >= 0,
		CountInput(PrevInput.m_Jump, pNewInput->m_Jump).m_Presses > 0,
		pNewInput->m_Direction > 0 && PrevInput.m_Direction <= 0,
	};
	const bool aLaneHeld[LaneCount] = {
		pNewInput->m_Direction < 0,
		(pNewInput->m_Jump & 1) != 0,
		pNewInput->m_Direction > 0,
	};

	for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
	{
		if(aLanePressed[LaneIndex])
		{
			m_aLanePressId[ClientId][LaneIndex]++;
			m_aLanePressTick[ClientId][LaneIndex] = CurrentTick;
		}
		if(aLaneHeld[LaneIndex])
			m_aLaneHoldTick[ClientId][LaneIndex] = CurrentTick;
	}

	const vec2 HitPos = m_pRhythmField->HitZonePos();
	const float HalfWidth = SRhythmFieldConfig::s_LaneWidth * 1.5f;

	for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
	{
		if(aLanePressed[LaneIndex])
			TryStartHold(ClientId, LaneIndex, m_aLanePressTick[ClientId][LaneIndex], HitWindowTicks, UseTickNotes);
		else if(aLaneHeld[LaneIndex] && !m_aLaneHoldActive[ClientId][LaneIndex])
			TryStartHold(ClientId, LaneIndex, m_aLaneHoldTick[ClientId][LaneIndex], HitWindowTicks, UseTickNotes);
	}

	for(int NoteIndex = m_CurrentNote; NoteIndex < (int)m_vNotes.size(); ++NoteIndex)
	{
		const CNote &Note = m_vNotes[NoteIndex];
		const int NoteTick = UseTickNotes ? m_vNoteTicks[NoteIndex] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());

		if(CurrentTick < NoteTick - SRhythmFieldConfig::s_BadWindowTicks - HitWindowTicks)
			break;

		if(Note.m_IsHold)
			continue;

		if(CurrentTick > NoteTick + SRhythmFieldConfig::s_BadWindowTicks + HitWindowTicks)
			continue;

		int aLaneBits[LaneCount];
		FillLaneBits(Note.m_StepBits, aLaneBits);

		for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
		{
			if(!aLaneBits[LaneIndex])
				continue;

			const uint8_t LaneMask = 1u << LaneIndex;
			if(m_aNoteLaneHitMask[ClientId] & LaneMask)
				continue;

			if(m_pRhythmField->IsHiddenArrowForClient(LaneIndex, NoteTick, ClientId))
				continue;
			if(m_aLanePressId[ClientId][LaneIndex] == m_aLanePressUsedId[ClientId][LaneIndex])
				continue;
			int PressTick = m_aLanePressTick[ClientId][LaneIndex];
			if(NoteTick <= m_aLaneLastHitTick[ClientId][LaneIndex])
				continue;
			int RawDelta = std::abs(PressTick - NoteTick);
			const int WindowTicks = SRhythmFieldConfig::s_BadWindowTicks + HitWindowTicks;
			if(RawDelta > WindowTicks && aLaneHeld[LaneIndex])
			{
				const int HeldTick = m_aLaneHoldTick[ClientId][LaneIndex];
				const int HeldDelta = std::abs(HeldTick - NoteTick);
				if(HeldDelta <= WindowTicks)
				{
					PressTick = HeldTick;
					RawDelta = HeldDelta;
				}
			}
			if(RawDelta > WindowTicks)
				continue;

			const int RatingDelta = maximum(0, RawDelta - HitWindowTicks);

			const float X = HitPos.x - HalfWidth + SRhythmFieldConfig::s_LaneWidth * (LaneIndex + 0.5f);
			const vec2 EffectPos(X, HitPos.y);
			CClientMask Mask;
			Mask.set(ClientId);
			GameServer()->CreateExplosion(EffectPos, -1, WEAPON_GRENADE, true, -1, Mask);
			GameServer()->CreateSound(EffectPos, SOUND_PICKUP_HEALTH, Mask);
			ScoreHit(ClientId, RatingDelta);
			m_pRhythmField->HideArrowForClient(LaneIndex, NoteTick, ClientId);
			m_aNoteLaneHitMask[ClientId] |= LaneMask;
			m_aLaneLastHitTick[ClientId][LaneIndex] = NoteTick;
			m_aLanePressUsedId[ClientId][LaneIndex] = m_aLanePressId[ClientId][LaneIndex];
		}
	}

	m_aPrevInputs[ClientId] = *pNewInput;
}

void CGameControllerMod::UpdateNotes()
{
	constexpr float FieldHitRadius = 32.0f;

	const int CurrentTick = Server()->Tick();
	const int HitWindowTicks = g_Config.m_SvRhythmHitWindowTicks;
	const double ElapsedSec = (CurrentTick - m_RoundStartTick) / static_cast<double>(Server()->TickSpeed());
	const bool UseTickNotes = m_vNoteTicks.size() == m_vNotes.size();
	int LeadTicks = 0;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		CCharacter *pChar = GameServer()->GetPlayerChar(i);
		if(!pChar)
		{
			ResetClientState(i);
			continue;
		}
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
		const int NoteEndTick = Note.m_IsHold ? NoteTimeToTick(m_RoundStartTick, Note.m_TimeEnd, Server()->TickSpeed()) : NoteTick;
		const int HoldDurationTicks = maximum(0, NoteEndTick - NoteTick);

		int aLaneBits[LaneCount];
		FillLaneBits(Note.m_StepBits, aLaneBits);

		if(m_pRhythmField)
		{
			for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
			{
				if(aLaneBits[LaneIndex])
					m_pRhythmField->SpawnLaneArrow(LaneIndex, NoteTick, HoldDurationTicks);
			}
		}

		++m_NextSpawnNote;
	}

	while(m_CurrentNote < (int)m_vNotes.size())
	{
		const CNote &Note = m_vNotes[m_CurrentNote];
		const int NoteTick = UseTickNotes ? m_vNoteTicks[m_CurrentNote] : NoteTimeToTick(m_RoundStartTick, Note.m_Time, Server()->TickSpeed());

		if(CurrentTick < NoteTick - SRhythmFieldConfig::s_BadWindowTicks - HitWindowTicks)
			break;

		const bool WindowExpired = CurrentTick > NoteTick + SRhythmFieldConfig::s_BadWindowTicks + HitWindowTicks;
		if(Note.m_IsHold)
		{
			if(!WindowExpired)
				break;
			++m_CurrentNote;
			for(int i = 0; i < MAX_CLIENTS; ++i)
				m_aNoteLaneHitMask[i] = 0;
			continue;
		}
		int aLaneBits[LaneCount];
		FillLaneBits(Note.m_StepBits, aLaneBits);

		if(WindowExpired)
		{
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
				{
					if(!aLaneBits[LaneIndex])
						continue;

					const uint8_t LaneMask = 1u << LaneIndex;
					if(m_aNoteLaneHitMask[i] & LaneMask)
						continue;

					m_aScores[i].m_Miss++;
					m_aScores[i].m_LastGrade = ERhythmHitGrade::MISS;
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

	const int HoldWindowTicks = SRhythmFieldConfig::s_BadWindowTicks + HitWindowTicks;
	auto LaneHeld = [](const CNetObj_PlayerInput &Input, int LaneIndex)
	{
		if(LaneIndex == 0)
			return Input.m_Direction < 0;
		if(LaneIndex == 1)
			return (Input.m_Jump & 1) != 0;
		return Input.m_Direction > 0;
	};

	while(m_CurrentHoldSegment < (int)m_vHoldSegments.size())
	{
		const CHoldSegment &Segment = m_vHoldSegments[m_CurrentHoldSegment];
		const int SegmentTick = m_vHoldSegmentTicks[m_CurrentHoldSegment];

		if(CurrentTick < SegmentTick - HoldWindowTicks)
			break;

		int aLaneBits[LaneCount];
		FillLaneBits(Segment.m_StepBits, aLaneBits);

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
			{
				if(!aLaneBits[LaneIndex])
					continue;

				const uint8_t LaneMask = 1u << LaneIndex;
				if(m_aHoldSegmentLaneHitMask[i] & LaneMask)
					continue;

				if(!m_aLaneHoldActive[i][LaneIndex])
					continue;
				if(!LaneHeld(m_aPrevInputs[i], LaneIndex))
					continue;

				const int RawDelta = std::abs(m_aLaneHoldTick[i][LaneIndex] - SegmentTick);
				if(RawDelta > HoldWindowTicks)
					continue;

				const int RatingDelta = maximum(0, RawDelta - HitWindowTicks);
				ScoreHit(i, RatingDelta);
				m_aHoldSegmentLaneHitMask[i] |= LaneMask;
			}
		}

		if(CurrentTick <= SegmentTick + HoldWindowTicks)
			break;

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
			{
				if(!aLaneBits[LaneIndex])
					continue;

				const uint8_t LaneMask = 1u << LaneIndex;
				if(m_aHoldSegmentLaneHitMask[i] & LaneMask)
					continue;

				m_aScores[i].m_Miss++;
				m_aScores[i].m_LastGrade = ERhythmHitGrade::MISS;
			}
			m_aHoldSegmentLaneHitMask[i] = 0;
		}

		++m_CurrentHoldSegment;
	}

	const vec2 HitPos = m_pRhythmField->HitZonePos();
	const float HalfWidth = SRhythmFieldConfig::s_LaneWidth * 1.5f;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(!GameServer()->GetPlayerChar(i))
			continue;
		for(int LaneIndex = 0; LaneIndex < LaneCount; ++LaneIndex)
		{
			if(!m_aLaneHoldActive[i][LaneIndex])
				continue;

			if(!LaneHeld(m_aPrevInputs[i], LaneIndex) && CurrentTick - m_aLaneHoldTick[i][LaneIndex] > HoldWindowTicks)
			{
				m_aLaneHoldActive[i][LaneIndex] = false;
				m_aLaneHoldEndEffectPlayed[i][LaneIndex] = false;
				continue;
			}

			if(!m_aLaneHoldEndEffectPlayed[i][LaneIndex] &&
				CurrentTick >= m_aLaneHoldEndTick[i][LaneIndex] - HoldWindowTicks)
			{
				const int RawDelta = std::abs(m_aLaneHoldTick[i][LaneIndex] - m_aLaneHoldEndTick[i][LaneIndex]);
				if(RawDelta <= HoldWindowTicks && LaneHeld(m_aPrevInputs[i], LaneIndex))
				{
					const float X = HitPos.x - HalfWidth + SRhythmFieldConfig::s_LaneWidth * (LaneIndex + 0.5f);
					const vec2 EffectPos(X, HitPos.y);
					CClientMask Mask;
					Mask.set(i);
					GameServer()->CreateDeath(EffectPos, i, Mask);
					GameServer()->CreateSound(EffectPos, SOUND_PICKUP_HEALTH, Mask);
					m_aLaneHoldEndEffectPlayed[i][LaneIndex] = true;
				}
			}

			if(CurrentTick > m_aLaneHoldEndTick[i][LaneIndex] + HoldWindowTicks)
			{
				m_aLaneHoldActive[i][LaneIndex] = false;
				m_aLaneHoldEndEffectPlayed[i][LaneIndex] = false;
			}
		}
	}
}
