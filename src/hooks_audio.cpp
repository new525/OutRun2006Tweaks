#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "input_manager.hpp"
#include <mmiscapi.h>
#include <fstream>
#include <algorithm>
#include <random>
#include <cctype>

namespace Settings
{
	Setting<bool> AllowHorn{ "Audio", "AllowHorn", true,
		"Allows using horn outside of the \"beep the horn!\" girlfriend missions." };
	Setting<bool> AllowWAV{ "Audio", "AllowWAV", true,
		"Adds support for WAV to the games BGM loader. WAVs should use the same filename as the original OGG, "
		"eg. \"Sound\\14_Rush_a_Difficulty_1989.wav\"." };

	Setting<bool> CDSwitcherEnable{ "CDSwitcher", "SwitcherEnable", false,
		"Installs an aftermarket CD switcher onto each of your cars, which can be used to change music tracks during a race. "
		"Tracks are changed with the Z and X keys by default, can be rebound in the bindings config." };
	Setting<bool> CDSwitcherDisplayTitle{ "CDSwitcher", "SwitcherDisplayTitle", true,
		"Whether to display track title after switching." };
	Setting<int> CDSwitcherTitleFont{ "CDSwitcher", "SwitcherTitleFont", 2,
		"Allows customizing the track title font (certain fonts may be missing glyphs such as square brackets).",
		Range<int>{ 0, 9 } };
	Setting<float> CDSwitcherTitleFontSizeX{ "CDSwitcher", "SwitcherTitleFontSizeX", 0.3f,
		"Size the track title is drawn at." };
	Setting<float> CDSwitcherTitleFontSizeY{ "CDSwitcher", "SwitcherTitleFontSizeY", 0.5f,
		"Size the track title is drawn at." };
	Setting<int> CDSwitcherTitlePositionX{ "CDSwitcher", "SwitcherTitlePositionX", 375,
		"Where the track title is drawn on screen, based on games original 640x480 screen dimensions." };
	Setting<int> CDSwitcherTitlePositionY{ "CDSwitcher", "SwitcherTitlePositionY", 450,
		"Where the track title is drawn on screen, based on games original 640x480 screen dimensions." };
	Setting<bool> CDSwitcherShuffleTracks{ "CDSwitcher", "SwitcherShuffleTracks", false,
		"Shuffles the order the CD switcher steps through the tracks in, on game launch. "
		"The track picked in the music menu still plays at the start of a race." };
}

std::string BGMOverridePath;

class BGMLoaderHook : public Hook
{
	// Hook games BGM loader to check the type of the audio file being loaded
	// Game already has CWaveFile code to load in uncompressed WAV audio, but it goes left unused
	// We'll just check if any WAV file exists with the same filename, and switch over to it if so
	// (fortunately games BGM code seems based on DirectX DXUTsound.cpp sample, which included the CWaveFile class)
	const static int CSoundManager__CreateStreaming_HookAddr = 0x11FA3;

	inline static char CurWavFilePath[4096];

	inline static SafetyHookMid hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		std::filesystem::path strWaveFileName = *(const char**)(ctx.esp + 0x54);
		if (!BGMOverridePath.empty())
		{
			if (!std::filesystem::exists(BGMOverridePath))
				BGMOverridePath = ".\\Sound\\" + BGMOverridePath;

			if (std::filesystem::exists(BGMOverridePath))
			{
				strcpy_s(CurWavFilePath, BGMOverridePath.c_str());
				*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
				strWaveFileName = BGMOverridePath;
			}
			BGMOverridePath.clear();
		}

		// If a .wav file exists with the same filename, let's try checking that file instead
		std::filesystem::path fileNameAsWav = strWaveFileName;
		fileNameAsWav = fileNameAsWav.replace_extension(".wav");
		std::filesystem::path fileNameAsFlac = strWaveFileName;
		fileNameAsFlac = fileNameAsFlac.replace_extension(".flac");

		enum class WaveFileType
		{
			WAV = 1,
			FLAC = 2,
			OGG = 3
		};

		// Normally hardcoded to 3/OGG, but changing to 1/WAV allows using CWaveFile
		// 2/FLAC is checked for by our hooks_flac.cpp code
		WaveFileType waveFileType = WaveFileType(ctx.eax);
		if (Settings::AllowFLAC && std::filesystem::exists(fileNameAsFlac))
		{
			waveFileType = WaveFileType::FLAC;

			// Switch the file game is trying to load to the flac instead
			strcpy_s(CurWavFilePath, fileNameAsFlac.string().c_str());
			*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
		}
		else if (Settings::AllowWAV && std::filesystem::exists(fileNameAsWav))
		{
			waveFileType = WaveFileType::WAV;

			// Switch the file game is trying to load to the wav instead
			strcpy_s(CurWavFilePath, fileNameAsWav.string().c_str());
			*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
		}
		ctx.eax = int(waveFileType);
	}

public:
	std::string_view description() override
	{
		return "BGMLoaderHook";
	}

	bool validate() override
	{
		return (Settings::AllowWAV || Settings::AllowFLAC || Settings::CDSwitcherEnable);
	}

	bool apply() override
	{
		hook = safetyhook::create_mid(Module::exe_ptr(CSoundManager__CreateStreaming_HookAddr), destination);
		return !!hook;
	}

	static BGMLoaderHook instance;
};
BGMLoaderHook BGMLoaderHook::instance;

class CDSwitcher : public Hook
{
	constexpr static int SongTitleDisplaySeconds = 2;
	constexpr static int SongTitleDisplayFrames = SongTitleDisplaySeconds * 60;
	constexpr static float SongTitleFadeBeginSeconds = 0.75;
	constexpr static int SongTitleFadeBeginFrame = int(SongTitleFadeBeginSeconds * 60.f);

	inline static int SongTitleDisplayTimer = 0;
	inline static bool PrevKeyStatePrev = false;
	inline static bool PrevKeyStateNext = false;

	// The game's own music list, in its music menu's order, as read by its BGM
	// loader (0x42FBA0). Entries 28 and 29 are LAST_WAVE and SEGA441, which the
	// menu doesn't offer.
	constexpr static int GameBgmTable_Addr = 0x371AD8;
	constexpr static uint32_t GameBgmCount = 28;

	// Position in Settings::CDTracks of what's playing, -1 if the CD list
	// doesn't have it. Kept apart from sel_bgm_kind_buf, which is the music
	// menu's choice: the two only line up when CDTracks is in the game's order,
	// and the shuffle option or extra tracks at the top of the list break that.
	inline static int CurrentCDTrack = -1;

	static std::string lowerStem(const std::string& path)
	{
		std::string stem = std::filesystem::path(path).stem().string();
		std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		return stem;
	}

	static const char* gameBgmFile(uint32_t bgmIdx)
	{
		if (bgmIdx >= GameBgmCount)
			return nullptr;
		return Module::exe_ptr<const char*>(GameBgmTable_Addr)[bgmIdx];
	}

	// The CDTracks entry with the same file name (ignoring folder, extension
	// and case) as the game's own track, so "01_Splash_wave.FLAC" stands in for
	// "01_Splash_wave.ogg".
	static int cdTrackForGameBgm(uint32_t bgmIdx)
	{
		const char* gameFile = gameBgmFile(bgmIdx);
		if (!gameFile)
			return -1;

		const std::string stem = lowerStem(gameFile);
		for (size_t i = 0; i < Settings::CDTracks.size(); i++)
			if (lowerStem(Settings::CDTracks[i].first) == stem)
				return int(i);
		return -1;
	}

	static int gameBgmForCDTrack(int cdTrack)
	{
		if (cdTrack < 0 || cdTrack >= int(Settings::CDTracks.size()))
			return -1;

		const std::string stem = lowerStem(Settings::CDTracks[cdTrack].first);
		for (uint32_t i = 0; i < GameBgmCount; i++)
			if (const char* gameFile = gameBgmFile(i); gameFile && lowerStem(gameFile) == stem)
				return int(i);
		return -1;
	}

	// Legacy input only - the new system binds ModAction::MusicNext/Previous
	// instead, which is one input per action rather than a chord.
	static constexpr uint32_t PadButtonCombo_Next = XINPUT_GAMEPAD_BACK;
	static constexpr uint32_t PadButtonCombo_Prev = XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK;

	inline static SafetyHookInline Game_Ctrl = {};
	static void destination()
	{
		Game_Ctrl.call();

		bool KeyStateNext = false;
		bool KeyStatePrev = false;

		if (Settings::UseNewInput)
		{
			KeyStateNext = InputManager_ModActionHeld(ModAction::MusicNext);
			KeyStatePrev = InputManager_ModActionHeld(ModAction::MusicPrevious);
		}
		else
		{
			bool PadStateNext = Input::PadReleased(PadButtonCombo_Next);
			bool PadStatePrev = Input::PadReleased(PadButtonCombo_Prev);

			// Prev is a superset of Next, so a Prev press also satisfies Next.
			if (PadStateNext && PadStatePrev)
				PadStateNext = false;

			KeyStateNext = ((GetAsyncKeyState('X') & 1) || PadStateNext);
			KeyStatePrev = ((GetAsyncKeyState('Z') & 1) || PadStatePrev);
		}

		if (Settings::CDTracks.empty())
			return;

		const int trackCount = int(Settings::CDTracks.size());
		bool BGMChanged = false;

		if (KeyStatePrev != PrevKeyStatePrev)
		{
			PrevKeyStatePrev = KeyStatePrev;
			if (KeyStatePrev)
			{
				CurrentCDTrack = CurrentCDTrack - 1;
				if (CurrentCDTrack < 0 || CurrentCDTrack >= trackCount)
					CurrentCDTrack = trackCount - 1;
				BGMChanged = true;
			}
		}
		if (KeyStateNext != PrevKeyStateNext)
		{
			PrevKeyStateNext = KeyStateNext;
			if (KeyStateNext)
			{
				CurrentCDTrack = CurrentCDTrack + 1;
				if (CurrentCDTrack < 0 || CurrentCDTrack >= trackCount)
					CurrentCDTrack = 0;
				BGMChanged = true;
			}
		}

		if (BGMChanged)
		{
			// If the game has this track too, make it the music menu's choice,
			// so the menu shows it and the next race starts with it. A track
			// the game doesn't have leaves the menu's choice alone.
			if (int gameBgm = gameBgmForCDTrack(CurrentCDTrack); gameBgm >= 0)
				*Game::sel_bgm_kind_buf = gameBgm;

			BGMOverridePath = Settings::CDTracks[CurrentCDTrack].first;
			Game::adxPlay(0, 0, 0);

			SongTitleDisplayTimer = SongTitleDisplayFrames;
		}
	}

	static void __cdecl PettyAutosceneCmdTblAnalysis_adxPlay_dest(int a1, uint32_t bgmIdx, int a3)
	{
		if (!Settings::CDTracks.size())
		{
			Game::adxPlay(a1, bgmIdx, a3);
			return;
		}

		// Both call sites are the game's scene commands: one plays a fixed track
		// number, the other (the race start) the music menu's choice. Either way
		// bgmIdx is an index into the game's own list (GameBgmTable_Addr).
		// Play that track, taking the CD list's copy of it if there is one
		// (the menu's preview already plays FLAC/WAV replacements by file
		// name), rather than whatever sits at the same position in the CD
		// list: with SwitcherShuffleTracks, or tracks added at the top of the
		// list, that is a different song.
		if (bgmIdx < GameBgmCount)
		{
			CurrentCDTrack = cdTrackForGameBgm(bgmIdx);
			if (CurrentCDTrack >= 0)
			{
				BGMOverridePath = Settings::CDTracks[CurrentCDTrack].first;
				Game::adxPlay(0, 0, 0);
			}
			else
				Game::adxPlay(a1, bgmIdx, a3); // not in the CD list: the game's own file
			return;
		}

		// Past the menu's music: a scene asking for LAST_WAVE or SEGA441 by
		// number. Nothing in the CD list stands in for those.
		CurrentCDTrack = -1;
		Game::adxPlay(a1, bgmIdx, a3);
	}

public:
	static void draw(int numUpdates)
	{
		if (SongTitleDisplayTimer > 0 && CurrentCDTrack >= 0 && CurrentCDTrack < int(Settings::CDTracks.size()))
		{
			Game::sprSetFontPriority(4);
			Game::sprSetPrintFont(Settings::CDSwitcherTitleFont);
			Game::sprSetFontScale(Settings::CDSwitcherTitleFontSizeX, Settings::CDSwitcherTitleFontSizeY);
			Game::sprLocateP(Settings::CDSwitcherTitlePositionX, Settings::CDSwitcherTitlePositionY);

			uint32_t color = 0xFFFFFF;
			if (SongTitleDisplayTimer > SongTitleFadeBeginFrame)
				color |= 0xFF000000;
			else
			{
				uint8_t alpha = uint8_t((float(SongTitleDisplayTimer) / SongTitleFadeBeginFrame) * 255.f);
				color |= (alpha << 24);
			}
			Game::sprSetFontColor(color);

			const auto& song = Settings::CDTracks[CurrentCDTrack].second;
			Game::sprPrintf("#%02d. %s", CurrentCDTrack + 1, song.c_str());

			SongTitleDisplayTimer -= numUpdates;
			if (SongTitleDisplayTimer <= 0)
				SongTitleDisplayTimer = 0;
		}
	}

	std::string_view description() override
	{
		return "CDSwitcher";
	}

	bool validate() override
	{
		return Settings::CDSwitcherEnable;
	}

	void declare_settings() override
	{
		Settings::CDSwitcherEnable.needs_restart();
	}

	bool apply() override
	{
		constexpr int Game_Ctrl_Addr = 0x9C840;
		constexpr int PettyAutosceneCmdTblAnalysis_adxPlay_CallAddr1 = 0x8687F;
		constexpr int PettyAutosceneCmdTblAnalysis_adxPlay_CallAddr2 = 0x868D3;

		Memory::VP::InjectHook(Module::exe_ptr(PettyAutosceneCmdTblAnalysis_adxPlay_CallAddr1), PettyAutosceneCmdTblAnalysis_adxPlay_dest, Memory::HookType::Call);
		Memory::VP::InjectHook(Module::exe_ptr(PettyAutosceneCmdTblAnalysis_adxPlay_CallAddr2), PettyAutosceneCmdTblAnalysis_adxPlay_dest, Memory::HookType::Call);

		Game_Ctrl = safetyhook::create_inline(Module::exe_ptr(Game_Ctrl_Addr), destination);
		return !!Game_Ctrl;
	}

	static CDSwitcher instance;
};
CDSwitcher CDSwitcher::instance;

void AudioHooks_Update(int numUpdates)
{
	if (numUpdates > 0 && Settings::AllowHorn)
	{
		static bool hornActive = false;
		if (Game::SwitchNow(0x10) && // Check if horn button is pressed
			*Game::current_mode == GameState::STATE_GAME)
		{
			Game::SetSndQueue(0x1BC | SND_LOOP);
			hornActive = true;
		}
		else
		{
			if (hornActive)
			{
				Game::SetSndQueue(0x1BC | SND_STOP);
				hornActive = false;
			}
		}
	}

	if (Settings::CDSwitcherDisplayTitle)
		CDSwitcher::draw(numUpdates);
}

void CDSwitcher_ReadIni(const std::filesystem::path& iniPath)
{
	if (!std::filesystem::exists(iniPath))
	{
		// TODO: fill in defaults if no INI found? for now we'll just disable switcher
		Settings::CDSwitcherEnable = false;
	}

	std::vector<std::pair<std::string, std::string>> iniTracks;

	std::ifstream file(iniPath);
	if (file && file.is_open())
	{
		std::string line;
		bool inCDTracksSection = false;

		while (std::getline(file, line))
		{
			if (inCDTracksSection)
			{
				if (line.empty())
					continue;
				line = Util::trim(line);
				if (line.front() == '[' && line.back() == ']') // Reached a new section
					break;
				if (line.front() == '#' || line.front() == ';')
					continue;

				auto delimiterPos = line.find('=');
				if (delimiterPos != std::string::npos)
				{
					std::string path = Util::trim(line.substr(0, delimiterPos));
					std::string name = Util::trim(line.substr(delimiterPos + 1));
					iniTracks.emplace_back(path, name);
					spdlog::info(" - CDTracks: Added track {} ({})", name, path);

					// Check both paths that the CDSwitcher code looks in
					if (!std::filesystem::exists(path) && !std::filesystem::exists(".\\Sound\\" + path))
						spdlog::warn("^ File \"{}\" not found for track, likely won't play properly in-game!", path);
				}
			}
			else if (line == "[CDTracks]")
			{
				inCDTracksSection = true;
			}
		}

		file.close();
	}

	if (iniTracks.size() <= 0)
		return;

	if (Settings::CDSwitcherShuffleTracks)
	{
		std::random_device rd;
		std::mt19937 g(rd());

		// Use std::shuffle to shuffle the vector
		std::shuffle(iniTracks.begin(), iniTracks.end(), g);
	}

	// replace old tracklist with the one from this INI
	// (if user had specified tracks in user.ini they likely meant to replace the defaults)
	Settings::CDTracks = iniTracks;
}
