#include <cstdarg>
#ifdef _WIN32
//#define _WIN32_WINNT	0x500	// for GetConsoleWindow()
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <string>
#include <math.h>

#ifdef _WIN32
extern "C" int __cdecl _getch(void);	// from conio.h
extern "C" int __cdecl _kbhit(void);
#else
#include <unistd.h>		// for STDIN_FILENO and usleep()
#include <termios.h>
#include <sys/time.h>	// for struct timeval in _kbhit()
#define	Sleep(msec)	usleep(msec * 1000)
#endif

#include <stdtype.h>
#include <common_def.h>
#include <utils/DataLoader.h>
#include <utils/FileLoader.h>
#include <utils/MemoryLoader.h>
#include <player/playerbase.hpp>
#include <player/s98player.hpp>
#include <player/droplayer.hpp>
#include <player/vgmplayer.hpp>
#include <audio/AudioStream.h>
#include <audio/AudioStream_SpcDrvFuns.h>
#include <emu/Resampler.h>
#include <emu/SoundDevs.h>
#include <emu/SoundEmu.h>	// for SndEmu_GetDevName()
#include <utils/OSMutex.h>

#include "stdtype.h"
#include "utils.hpp"
#include "config.hpp"
#include "m3uargparse.hpp"


struct AudioDriver
{
	UINT8 driverType;		// ADRVTYPE_OUT or ADRVTYPE_DISK
	INT32 dTypeID;			// [config] choose audio driver number N of driverType (-1 = by name)
	std::string dTypeName;	// [config] choose audio driver by name (empty: don't use)
	INT32 driverID;			// actual ID of the audio driver
	INT32 deviceID;			// [config] driver device ID (0 = default)
	void* data;				// data structure
};


UINT8 PlayerMain(void);
static UINT8 OpenFile(const std::string& fileName, DATA_LOADER*& dLoad, PlayerBase*& player);
static const char* GetTagForDisp(const std::map<std::string, std::string>& tags, const std::string& tagName);
static void ShowSongInfo(PlayerBase* player);
static UINT8 PlayFile(PlayerBase* player);
static int GetPressedKey(void);
static UINT8 HandleKeyPress(PlayerBase* player);
static std::string FCC2Str(UINT32 fcc);
static std::string GetTimeStr(double seconds, INT8 showHours = 0);
static UINT8 GetPlayerForFile(DATA_LOADER *dLoad, PlayerBase** retPlayer);
static UINT32 CalcCurrentVolume(UINT32 playbackSmpl);
static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* Data);
static UINT8 FilePlayCallback(PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam);
static UINT8 ChooseAudioDriver(AudioDriver* aDrv);
static UINT8 InitAudioDriver(AudioDriver* aDrv);
static UINT8 InitAudioSystem(void);
static UINT8 DeinitAudioSystem(void);
static UINT8 StartAudioDevice(void);
static UINT8 StopAudioDevice(void);
#ifndef _WIN32
static void changemode(UINT8 noEcho);
static int _kbhit(void);
#define	_getch	getchar
static void cls(void);
#endif


#define APP_NAME	"VGM Player"
#define APP_NAME_L	L"VGM Player"

// NCurses-like key constants
#define KEY_DOWN		0x102		// cursor down
#define KEY_UP			0x103		// cursor up
#define KEY_LEFT		0x104		// cursor left
#define KEY_RIGHT		0x105		// cursor right
#define KEY_NPAGE		0x152		// next page (page down)
#define KEY_PPAGE		0x153		// previouspage (page up)
#define KEY_MASK		0xFFF
#define KEY_CTRL		0x1000
#define KEY_SHIFT		0x2000
#define KEY_ALT			0x4000


static AudioDriver adOut = {ADRVTYPE_OUT, -1, "", 0, 0, NULL};
static AudioDriver adLog = {ADRVTYPE_DISK, -1, "", 0, 0, NULL};

static UINT32 smplSize;
static std::vector<WAVE_32BS> smplBuf;
static std::vector<uint8_t> audioBuf;
static OS_MUTEX* renderMtx;	// render thread mutex

static UINT32 sampleRate = 44100;
static UINT32 maxLoops = 2;
static bool manualRenderLoop = false;
static volatile UINT8 playState;

static INT32 AudioOutDrv = 3;

static UINT32 masterVol = 0x10000;	// fixed point 16.16
static UINT32 fadeSmplStart;
static UINT32 fadeSmplTime;

static bool showFileInfo = false;
static INT8 PrintMSHours = 0;
static bool showCores = true;
static UINT32 fileSize;

static std::vector<PlayerBase*> players;

extern Configuration playerCfg;
extern std::vector<SongFileList> songList;
extern std::vector<std::string> plList;

static int controlVal;
static size_t curSong;

UINT8 PlayerMain(void)
{
	UINT8 retVal;
	size_t curPlr;
	
	adOut.dTypeID = AudioOutDrv;
	retVal = InitAudioSystem();
	if (retVal)
		return 1;
	retVal = StartAudioDevice();
	if (retVal)
	{
		DeinitAudioSystem();
		return 1;
	}
	playState = 0x00;
	
	// I'll keep the instances of the players for the program's life time.
	// This way player/chip options are kept between track changes.
	players.push_back(new VGMPlayer);
	players.push_back(new S98Player);
	players.push_back(new DROPlayer);
	
	//resVal = 0;
	controlVal = +1;	// default: next song
	for (curSong = 0; curSong < songList.size(); )
	{
		DATA_LOADER* dLoad;
		PlayerBase* player;
		
		cls();
		printf(APP_NAME);
		printf("\n----------\n");
		if (songList[curSong].playlistID != (size_t)-1)
		{
			printf("\nPlaylist File:  %s\n", plList[songList[curSong].playlistID].c_str());
			printf("Playlist Entry: %u / %u\n", 1 + (unsigned)curSong, (unsigned)songList.size());
		}
		printf("File Name:      %s\n", songList[curSong].fileName.c_str());
		fflush(stdout);
		
		retVal = OpenFile(songList[curSong].fileName, dLoad, player);
		if (retVal & 0x80)
		{
			// TODO: show error message + wait for key press
			if (controlVal == -1 && curSong == 0)
				controlVal = +1;
			curSong += controlVal;
			_getch();
			// TODO: evaluate pressed key + exit when ESC is pressed
			continue;
		}
		
		player->SetCallback(&FilePlayCallback, NULL);
		player->SetSampleRate(sampleRate);
		// call "start" before showing song info, so that we can get the sound cores
		player->Start();
		fileSize = DataLoader_GetSize(dLoad);
		
		ShowSongInfo(player);
		PlayFile(player);
		
		player->Stop();
		
		player->UnloadFile();
		DataLoader_Deinit(dLoad);
		
		if (controlVal == +1)
		{
			curSong ++;
		}
		else if (controlVal == -1)
		{
			if (curSong > 0)
				curSong --;
		}
		else if (controlVal == +9)
		{
			break;
		}
	}	// end for(curSong)
	
	for (curPlr = 0; curPlr < players.size(); curPlr ++)
		delete players[curPlr];
	players.clear();
	
	StopAudioDevice();
	DeinitAudioSystem();
	
#if defined(_DEBUG) && (_MSC_VER >= 1400)
	// doesn't work well with C++ containers
	//if (_CrtDumpMemoryLeaks())
	//	_getch();
#endif
	
	return 0;
}

static UINT8 OpenFile(const std::string& fileName, DATA_LOADER*& dLoad, PlayerBase*& player)
{
	UINT8 retVal;
	
	dLoad = FileLoader_Init(fileName.c_str());
	if (dLoad == NULL)
		return 0xFF;
	DataLoader_SetPreloadBytes(dLoad, 0x100);
	retVal = DataLoader_Load(dLoad);
	if (retVal)
	{
		DataLoader_CancelLoading(dLoad);
		fprintf(stderr, "Error 0x%02X opening file!\n", retVal);
		return 0xFF;
	}
	player = NULL;
	retVal = GetPlayerForFile(dLoad, &player);
	if (retVal)
	{
		DataLoader_CancelLoading(dLoad);
		fprintf(stderr, "Unknown file format! (Error 0x%02X)\n", retVal);
		return 0xFF;
	}
	printf("\n");
	return 0x00;
}

static void Tags_LangFilter(std::map<std::string, std::string>& tags, const std::string& tagName, const std::vector<std::string>& langPostfixes, int defaultLang)
{
	// 1. search for matching lang-tag
	// 2. save that
	// 3. remove all others
	std::vector<std::string> langTags;
	auto chosenTagIt = tags.end();
	
	for (auto lpfIt = langPostfixes.begin(); lpfIt != langPostfixes.end(); ++lpfIt)
		langTags.push_back(tagName + *lpfIt);
	
	if (defaultLang >= 0 && (size_t)defaultLang < langTags.size())
		chosenTagIt = tags.find(langTags[defaultLang]);
	if (chosenTagIt == tags.end())
	{
		for (auto ltIt = langTags.begin(); ltIt != langTags.end(); ++ltIt)
		{
			chosenTagIt = tags.find(*ltIt);
			if (chosenTagIt != tags.end())
				break;
		}
	}
	if (chosenTagIt == tags.end())
		return;
	
	if (chosenTagIt->first != tagName)
		tags[tagName] = chosenTagIt->second;
	
	for (auto ltIt = langTags.begin(); ltIt != langTags.end(); ++ltIt)
	{
		if (*ltIt == tagName)
			continue;
		tags.erase(*ltIt);
	}
	
	return;
}

static const char* GetTagForDisp(const std::map<std::string, std::string>& tags, const std::string& tagName)
{
	auto tagIt = tags.find(tagName);
	return (tagIt == tags.end()) ? "" : tagIt->second.c_str();
}

static void ShowSongInfo(PlayerBase* player)
{
	PLR_SONG_INFO sInf;
	std::map<std::string, std::string> tags;
	std::vector<std::string> langPostfixes;
	char verStr[0x20];
	
	langPostfixes.push_back("");
	langPostfixes.push_back("-JPN");
	
	const char* const* tagList = player->GetTags();
	for (const char* const* t = tagList; *t != NULL; t += 2)
		tags[t[0]] = t[1];
	
	Tags_LangFilter(tags, "TITLE", langPostfixes, -1);
	Tags_LangFilter(tags, "GAME", langPostfixes, -1);
	Tags_LangFilter(tags, "SYSTEM", langPostfixes, -1);
	Tags_LangFilter(tags, "ARTIST", langPostfixes, -1);
	
	INT16 volGain = 0x00;
	player->GetSongInfo(sInf);
	if (player->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
		const VGM_HEADER* vgmhdr = vgmplay->GetFileHeader();
		
		sprintf(verStr, "VGM %X.%02X", (vgmhdr->fileVer >> 8) & 0xFF, (vgmhdr->fileVer >> 0) & 0xFF);
		volGain = vgmhdr->volumeGain;
		fileSize = vgmplay->GetFileHeader()->dataEnd;
	}
	else if (player->GetPlayerType() == FCC_S98)
	{
		S98Player* s98play = dynamic_cast<S98Player*>(player);
		const S98_HEADER* s98hdr = s98play->GetFileHeader();
		
		sprintf(verStr, "S98 v%u", s98hdr->fileVer);
	}
	else if (player->GetPlayerType() == FCC_DRO)
	{
		DROPlayer* droplay = dynamic_cast<DROPlayer*>(player);
		const DRO_HEADER* drohdr = droplay->GetFileHeader();
		
		sprintf(verStr, "DRO v%u", drohdr->verMajor);	// DRO has a "verMinor" field, but it's always 0
	}
	
	printf("Track Title:    %s\n", GetTagForDisp(tags, "TITLE"));
	printf("Game Name:      %s\n", GetTagForDisp(tags, "GAME"));
	printf("System:         %s\n", GetTagForDisp(tags, "SYSTEM"));
	printf("Composer:       %s\n", GetTagForDisp(tags, "ARTIST"));
	if (player->GetPlayerType() == FCC_S98)
	{
		S98Player* s98play = dynamic_cast<S98Player*>(player);
		const S98_HEADER* s98hdr = s98play->GetFileHeader();
		
		printf("Release:        %-12sTick Rate: %u/%u\n", GetTagForDisp(tags, "DATE"),
			s98hdr->tickMult, s98hdr->tickDiv);
	}
	else
	{
		printf("Release:        %s\n", GetTagForDisp(tags, "DATE"));
	}
	printf("Format:         %-12s", verStr);
	printf("Gain:%5.2f    ", pow(2.0, volGain / (double)0x100));
	if (player->GetPlayerType() == FCC_DRO)
	{
		DROPlayer* droplay = dynamic_cast<DROPlayer*>(player);
		const DRO_HEADER* drohdr = droplay->GetFileHeader();
		const char* hwType;
		
		if (drohdr->hwType == 0)
			hwType = "OPL2";
		else if (drohdr->hwType == 1)
			hwType = "DualOPL2";
		else if (drohdr->hwType == 2)
			hwType = "OPL3";
		else
			hwType = "unknown";
		printf("HWType: %s\n", hwType);
	}
	else
	{
		UINT32 loopLen = player->GetLoopTicks();
		if (sInf.loopTick != (UINT32)-1)
			printf("Loop: Yes (%s)\n", GetTimeStr(player->Tick2Second(loopLen), -1).c_str());
		else
			printf("Loop: No\n");
	}
	printf("VGM by:         %s\n", GetTagForDisp(tags, "ENCODED_BY"));
	printf("Notes:          %s\n", GetTagForDisp(tags, "COMMENT"));
	printf("\n");
	
	std::vector<PLR_DEV_INFO> diList;
	player->GetSongDeviceInfo(diList);
	printf("Used chips:     ");
	for (size_t curDev = 0; curDev < diList.size(); curDev ++)
	{
		const PLR_DEV_INFO& pdi = diList[curDev];
		const char* chipName;
		
		chipName = SndEmu_GetDevName(pdi.type, 0x01, pdi.devCfg);
		if (curDev + 1 < diList.size())
		{
			const PLR_DEV_INFO& pdi1 = diList[curDev + 1];
			if (pdi.type == pdi1.type)
			{
				if (! (pdi.core != pdi1.core && showCores))
				{
					printf("2x");
					curDev ++;
				}
			}
		}
		if (showCores)
			printf("%s (%s), ", chipName, FCC2Str(pdi.core).c_str());
		else
			printf("%s, ", chipName);
	}
	printf("\b\b \n\n");
	return;
}

static UINT8 PlayFile(PlayerBase* player)
{
	UINT8 retVal;
	bool needRefresh;
	
	fadeSmplTime = player->GetSampleRate() * 4;
	fadeSmplStart = (UINT32)-1;
	
	if (showFileInfo)
	{
		PLR_SONG_INFO sInf;
		std::vector<PLR_DEV_INFO> diList;
		size_t curDev;
		
		player->GetSongInfo(sInf);
		player->GetSongDeviceInfo(diList);
		printf("SongInfo: %s v%X.%X, Rate %u/%u, Len %u, Loop at %d, devices: %u\n",
			FCC2Str(sInf.format).c_str(), sInf.fileVerMaj, sInf.fileVerMin,
			sInf.tickRateMul, sInf.tickRateDiv, sInf.songLen, sInf.loopTick, sInf.deviceCnt);
		for (curDev = 0; curDev < diList.size(); curDev ++)
		{
			const PLR_DEV_INFO& pdi = diList[curDev];
			printf(" Dev %d: Type 0x%02X #%d, Core %s, Clock %u, Rate %u, Volume 0x%X\n",
				(int)pdi.id, pdi.type, (INT8)pdi.instance, FCC2Str(pdi.core).c_str(), pdi.devCfg->clock, pdi.smplRate, pdi.volume);
		}
	}
	
	if (adOut.data != NULL)
		retVal = AudioDrv_SetCallback(adOut.data, FillBuffer, player);
	else
		retVal = 0xFF;
	manualRenderLoop = (retVal != AERR_OK);
#ifndef _WIN32
	changemode(1);
#endif
	controlVal = 0;
	playState &= ~PLAYSTATE_END;
	needRefresh = true;
	while(! (playState & PLAYSTATE_END))
	{
		if (! (playState & PLAYSTATE_PAUSE))
			needRefresh = true;	// always update when playing
		if (needRefresh)
		{
			const char* pState;
			
			if (playState & PLAYSTATE_PAUSE)
				pState = "Paused ";
			else if (fadeSmplStart != (UINT32)-1)
				pState = "Fading ";
			else
				pState = "Playing";
			printf("%s%6.2f%%  %s / %s seconds  \r", pState,
					100.0 * player->GetCurPos(PLAYPOS_FILEOFS) / fileSize,
					GetTimeStr(player->Sample2Second(player->GetCurPos(PLAYPOS_SAMPLE)), PrintMSHours).c_str(),
					GetTimeStr(player->Tick2Second(player->GetTotalPlayTicks(maxLoops)), PrintMSHours).c_str());
			fflush(stdout);
			needRefresh = false;
		}
		
		if (manualRenderLoop && ! (playState & PLAYSTATE_PAUSE))
		{
			UINT32 wrtBytes = FillBuffer(adOut.data, player, audioBuf.size(), &audioBuf[0]);
			AudioDrv_WriteData(adOut.data, wrtBytes, &audioBuf[0]);
		}
		else
		{
			Sleep(50);
		}
		
		retVal = HandleKeyPress(player);
		if (retVal)
		{
			needRefresh = true;
			if (retVal >= 0x10)
				break;
		}
	}
#ifndef _WIN32
	changemode(0);
#endif
	// remove callback to prevent further rendering
	// also waits for render thread to finish its work
	if (adOut.data != NULL)
		AudioDrv_SetCallback(adOut.data, NULL, NULL);
	
	if (! controlVal)
	{
		if (/*stopAfterSong*/false)
			controlVal = 9;	// quit
		else
			controlVal = +1;	// finished normally - next song
	}
	printf("\n");
	
	return 0x00;
}

#ifdef WIN32
static int GetPressedKey(void)
{
	int keyCode = _getch();
	switch(keyCode)
	{
	case 0xE0:	// Special Key #1
		// Windows cursor key codes
		// Shift + Cursor results in the same code as if Shift wasn't pressed
		//	Key     None    Ctrl    Alt
		//	Up      E0 48   E0 8D   00 98
		//	Down    E0 50   E0 91   00 A0
		//	Left    E0 4B   E0 73   00 9B
		//	Right   E0 4D   E0 74   00 9D
		keyCode = _getch();	// Get 2nd Key
		switch(KeyCode)
		{
		case 0x48:
			return KEY_UP;
		case 0x49:
			return KEY_PPAGE;
		case 0x4B:
			return KEY_LEFT;
		case 0x4D:
			return KEY_RIGHT;
		case 0x50:
			return KEY_DOWN;
		case 0x51:
			return KEY_NPAGE;
		case 0x73:
			return KEY_CTRL | KEY_LEFT;
		case 0x74:
			return KEY_CTRL | KEY_RIGHT;
		case 0x8D:
			return KEY_CTRL | KEY_UP;
		case 0x91:
			return KEY_CTRL | KEY_DOWN;
		}
		break;
	case 0x00:	// Special Key #2
		keyCode = _getch();	// Get 2nd Key
		switch(KeyCode)
		{
		case 0x98:
			return KEY_ALT | KEY_UP;
		case 0x9B:
			return KEY_ALT | KEY_LEFT;
		case 0x9D:
			return KEY_ALT | KEY_RIGHT;
		case 0xA0:
			return KEY_ALT | KEY_DOWN;
		}
		break;
	}
	return keyCode;
}
#else
static int GetPressedKey(void)
{
	int keyCode = _getch();
	if (keyCode != 0x1B)
		return keyCode;
	
	int keyModifers = 0x00;
	
	// handle escape codes
	keyCode = _getch();
	if (keyCode == 0x1B || keyCode == 0x00)
		return 0x1B;	// ESC Key pressed
	switch(keyCode)
	{
	case 0x5B:
		// cursor only:     1B 41..44
		// Shift + cursor:  1B 31 3B 32 41..44
		// Alt + cursor:    1B 31 3B 33 41..44
		// Ctrl + cursor:   1B 31 3B 35 41..44
		// page down/up:    1B 35/36 7E
		keyCode = _getch();	// get 2nd Key
		if (keyCode == 0x31)	// Ctrl or Alt key
		{
			keyCode = _getch();
			if (keyCode == 0x3B)
			{
				keyCode = _getch();
				if ((keyCode - 0x31) & 0x01)
					keyModifers |= KEY_SHIFT;
				if ((keyCode - 0x31) & 0x02)
					keyModifers |= KEY_ALT;
				if ((keyCode - 0x31) & 0x04)
					keyModifers |= KEY_CTRL;
				keyCode = _getch();
			}
		}
		switch(keyCode)
		{
		case 0x35:
			_getch();	// skip 7E key code
			return KEY_PPAGE;
		case 0x36:
			_getch();	// skip 7E key code
			return KEY_NPAGE;
		case 0x41:
			return keyModifers | KEY_UP;
		case 0x42:
			return keyModifers | KEY_DOWN;
		case 0x43:
			return keyModifers | KEY_RIGHT;
		case 0x44:
			return keyModifers | KEY_LEFT;
		}
	}
	return keyCode;
}
#endif

static UINT8 HandleKeyPress(PlayerBase* player)
{
	if (! _kbhit())
		return 0;
	
	int keyCode = GetPressedKey();
	if (keyCode >= 'a' && keyCode <= 'z')
		keyCode = toupper(keyCode);
	switch(keyCode)
	{
	case 0x1B:	// ESC
	case 'Q':	// quit
		playState |= PLAYSTATE_END;
		controlVal = +9;
		return 0x10;
	case ' ':
	case 'P':	// pause
		playState ^= PLAYSTATE_PAUSE;
		if (adOut.data != NULL)
		{
			if (playState & PLAYSTATE_PAUSE)
				AudioDrv_Pause(adOut.data);
			else
				AudioDrv_Resume(adOut.data);
		}
		break;
	case 'F':	// fade out
		fadeSmplStart = player->GetCurPos(PLAYPOS_SAMPLE);
		break;
	case 'R':	// restart
		OSMutex_Lock(renderMtx);
		player->Reset();
		fadeSmplStart = (UINT32)-1;
		OSMutex_Unlock(renderMtx);
		break;
	case KEY_LEFT:
	case KEY_RIGHT:
	case KEY_CTRL | KEY_LEFT:
	case KEY_CTRL | KEY_RIGHT:
		{
			UINT32 destPos;
			INT32 seekSmpls;
			
			INT32 secs = (keyCode & KEY_CTRL) ? 60 : 5;	// 5s [not ctrl] or 60s [ctrl]
			if ((keyCode & KEY_MASK) == KEY_LEFT)
				secs *= -1;	// seek back
			
			OSMutex_Lock(renderMtx);
			destPos = player->GetCurPos(PLAYPOS_SAMPLE);
			seekSmpls = player->GetSampleRate() * secs;
			if (seekSmpls < 0 && (UINT32)-seekSmpls > destPos)
				destPos = 0;
			else
				destPos += seekSmpls;
			player->Seek(PLAYPOS_SAMPLE, destPos);
			if (player->GetCurPos(PLAYPOS_SAMPLE) < fadeSmplStart)
				fadeSmplStart = (UINT32)-1;
			OSMutex_Unlock(renderMtx);
		}
		break;
	case 'B':	// previous file (back)
	case KEY_PPAGE:
		if (curSong > 0)
		{
			playState |= PLAYSTATE_END;
			controlVal = -1;
			return 0x10;
		}
		break;
	case 'N':	// next file
	case KEY_NPAGE:
		if (curSong + 1 < songList.size())
		{
			playState |= PLAYSTATE_END;
			controlVal = +1;
			return 0x10;
		}
		break;
	default:
		if (keyCode >= '0' && keyCode <= '9')
		{
			UINT32 maxPos;
			UINT8 pbPos10;
			UINT32 destPos;
			
			OSMutex_Lock(renderMtx);
			maxPos = player->GetTotalPlayTicks(maxLoops);
			pbPos10 = keyCode - '0';
			destPos = maxPos * pbPos10 / 10;
			player->Seek(PLAYPOS_TICK, destPos);
			if (player->GetCurPos(PLAYPOS_SAMPLE) < fadeSmplStart)
				fadeSmplStart = (UINT32)-1;
			OSMutex_Unlock(renderMtx);
		}
		break;
	}
	
	return 0x01;
}

static std::string FCC2Str(UINT32 fcc)
{
	std::string result(4, '\0');
	result[0] = (char)((fcc >> 24) & 0xFF);
	result[1] = (char)((fcc >> 16) & 0xFF);
	result[2] = (char)((fcc >>  8) & 0xFF);
	result[3] = (char)((fcc >>  0) & 0xFF);
	return result;
}

static std::string GetTimeStr(double seconds, INT8 showHours)
{
	// showHours:
	//	-1 - auto
	//	 0 - show mm:ss
	//	 1 - show h:mm:ss
	UINT32 csec;
	UINT32 sec;
	UINT32 min;
	UINT32 hrs;
	char timeStr[0x20];
	
	csec = (UINT32)(seconds * 100 + 0.5);
	sec = csec / 100;
	csec %= 100;
	min = sec / 60;
	sec %= 60;
	if (showHours < 0)
		showHours = (min >= 60);
	if (showHours == 0)
	{
		sprintf(timeStr, "%02u:%02u.%02u", min, sec, csec);
	}
	else
	{
		showHours ++;
		hrs = min / 60;
		min %= 60;
		sprintf(timeStr, "%0*u:%02u:%02u.%02u", showHours, hrs, min, sec, csec);
	}
	
	return std::string(timeStr);
}

static UINT8 GetPlayerForFile(DATA_LOADER *dLoad, PlayerBase** retPlayer)
{
	UINT8 retVal;
	PlayerBase* player;
	size_t curPlr;
	
	player = NULL;
	for (curPlr = 0; curPlr < players.size(); curPlr ++)
	{
		retVal = players[curPlr]->CanLoadFile(dLoad);
		if (! retVal)
		{
			player = players[curPlr];
			break;
		}
	}
	if (player == NULL)
		return 0xFF;
	
	retVal = player->LoadFile(dLoad);
	if (retVal < 0x80)
		*retPlayer = player;
	return retVal;
}

#if 1
#define VOLCALC64
#define VOL_BITS	16	// use .X fixed point for working volume
#else
#define VOL_BITS	8	// use .X fixed point for working volume
#endif
#define VOL_SHIFT	(16 - VOL_BITS)	// shift for master volume -> working volume

// Pre- and post-shifts are used to make the calculations as accurate as possible
// without causing the sample data (likely 24 bits) to overflow while applying the volume gain.
// Smaller values for VOL_PRESH are more accurate, but have a higher risk of overflows during calculations.
// (24 + VOL_POSTSH) must NOT be larger than 31
#define VOL_PRESH	4	// sample data pre-shift
#define VOL_POSTSH	(VOL_BITS - VOL_PRESH)	// post-shift after volume multiplication

static UINT32 CalcCurrentVolume(UINT32 playbackSmpl)
{
	UINT32 curVol;	// 16.16 fixed point
	
	// 1. master volume
	curVol = masterVol;
	
	// 2. apply fade-out factor
	if (playbackSmpl >= fadeSmplStart)
	{
		UINT32 fadeSmpls;
		UINT64 fadeVol;	// 64 bit for less type casts when doing multiplications with .16 fixed point
		
		fadeSmpls = playbackSmpl - fadeSmplStart;
		if (fadeSmpls >= fadeSmplTime)
			return 0x0000;	// going beyond fade time -> volume 0
		
		fadeVol = (UINT64)fadeSmpls * 0x10000 / fadeSmplTime;
		fadeVol = 0x10000 - fadeVol;	// fade from full volume to silence
		fadeVol = fadeVol * fadeVol;	// logarithmic fading sounds nicer
		curVol = (UINT32)((fadeVol * curVol) >> 32);
	}
	
	return curVol;
}

static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data)
{
	PlayerBase* player;
	UINT32 basePbSmpl;
	UINT32 smplCount;
	UINT32 smplRendered;
	INT16* SmplPtr16;
	UINT32 curSmpl;
	WAVE_32BS fnlSmpl;	// final sample value
	INT32 curVolume;
	
	smplCount = bufSize / smplSize;
	if (! smplCount)
		return 0;
	
	player = (PlayerBase*)userParam;
	if (player == NULL)
	{
		memset(data, 0x00, smplCount * smplSize);
		return smplCount * smplSize;
	}
	if (! (player->GetState() & PLAYSTATE_PLAY))
	{
		fprintf(stderr, "Player Warning: calling Render while not playing! playState = 0x%02X\n", player->GetState());
		memset(data, 0x00, smplCount * smplSize);
		return smplCount * smplSize;
	}
	
	OSMutex_Lock(renderMtx);
	if (smplCount > smplBuf.size())
		smplCount = smplBuf.size();
	memset(&smplBuf[0], 0, smplCount * sizeof(WAVE_32BS));
	basePbSmpl = player->GetCurPos(PLAYPOS_SAMPLE);
	smplRendered = player->Render(smplCount, &smplBuf[0]);
	smplCount = smplRendered;
	
	curVolume = (INT32)CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
	SmplPtr16 = (INT16*)data;
	for (curSmpl = 0; curSmpl < smplCount; curSmpl ++, basePbSmpl ++, SmplPtr16 += 2)
	{
		if (basePbSmpl >= fadeSmplStart)
		{
			UINT32 fadeSmpls;
			
			fadeSmpls = basePbSmpl - fadeSmplStart;
			if (fadeSmpls >= fadeSmplTime && ! (playState & PLAYSTATE_END))
			{
				playState |= PLAYSTATE_END;
				break;
			}
			
			curVolume = (INT32)CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
		}
		
		// Input is about 24 bits (some cores might output a bit more)
		fnlSmpl = smplBuf[curSmpl];
		
#ifdef VOLCALC64
		fnlSmpl.L = (INT32)( ((INT64)fnlSmpl.L * curVolume) >> VOL_BITS );
		fnlSmpl.R = (INT32)( ((INT64)fnlSmpl.R * curVolume) >> VOL_BITS );
#else
		fnlSmpl.L = ((fnlSmpl.L >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
		fnlSmpl.R = ((fnlSmpl.R >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
#endif
		
		fnlSmpl.L >>= 8;	// 24 bit -> 16 bit
		fnlSmpl.R >>= 8;
		if (fnlSmpl.L < -0x8000)
			fnlSmpl.L = -0x8000;
		else if (fnlSmpl.L > +0x7FFF)
			fnlSmpl.L = +0x7FFF;
		if (fnlSmpl.R < -0x8000)
			fnlSmpl.R = -0x8000;
		else if (fnlSmpl.R > +0x7FFF)
			fnlSmpl.R = +0x7FFF;
		SmplPtr16[0] = (INT16)fnlSmpl.L;
		SmplPtr16[1] = (INT16)fnlSmpl.R;
	}
	OSMutex_Unlock(renderMtx);
	
	return curSmpl * smplSize;
}

static UINT8 FilePlayCallback(PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam)
{
	switch(evtType)
	{
	case PLREVT_START:
		//printf("Playback started.\n");
		break;
	case PLREVT_STOP:
		//printf("Playback stopped.\n");
		break;
	case PLREVT_LOOP:
		{
			UINT32* curLoop = (UINT32*)evtParam;
			if (*curLoop >= maxLoops)
			{
				if (fadeSmplTime)
				{
					if (fadeSmplStart == (UINT32)-1)
						fadeSmplStart = player->GetCurPos(PLAYPOS_SAMPLE);
				}
				else
				{
					//printf("Loop End.\n");
					playState |= PLAYSTATE_END;
					return 0x01;
				}
			}
			if (player->GetState() & PLAYSTATE_SEEK)
				break;
			//printf("Loop %u.\n", 1 + *curLoop);
		}
		break;
	case PLREVT_END:
		playState |= PLAYSTATE_END;
		//printf("Song End.\n");
		break;
	}
	return 0x00;
}

static UINT8 ChooseAudioDriver(AudioDriver* aDrv)
{
	UINT32 drvCount;
	UINT32 curDrv;
	AUDDRV_INFO* drvInfo;
	
	aDrv->driverID = -1;
	if (aDrv->dTypeID != -1)
	{
		INT32 typedDrv;
		// go through all audio drivers get the ID of the requested Output/Disk Writer driver
		drvCount = Audio_GetDriverCount();
		for (typedDrv = 0, curDrv = 0; curDrv < drvCount; curDrv ++)
		{
			Audio_GetDriverInfo(curDrv, &drvInfo);
			if (drvInfo->drvType == aDrv->driverType)
			{
				if (typedDrv == aDrv->dTypeID)	// choose Nth driver with the respective type
				{
					aDrv->driverID = curDrv;
					break;
				}
				typedDrv ++;
			}
		}
	}
	else
	{
		if (aDrv->dTypeName.empty())
			return 0x00;	// no driver chosen
		
		// go through all audio drivers get the ID of the requested Output/Disk Writer driver
		drvCount = Audio_GetDriverCount();
		for (curDrv = 0; curDrv < drvCount; curDrv ++)
		{
			Audio_GetDriverInfo(curDrv, &drvInfo);
			if (drvInfo->drvType == aDrv->driverType)
			{
				if (drvInfo->drvName == aDrv->dTypeName)	// choose driver with the requested name
				{
					aDrv->driverID = curDrv;
					break;
				}
			}
		}
	}
	
	return (aDrv->driverID == -1) ? 0xFF : 0x00;
}

static UINT8 InitAudioDriver(AudioDriver* aDrv)
{
	AUDDRV_INFO* drvInfo;
	UINT8 retVal;
	
	aDrv->data = NULL;
	retVal = AudioDrv_Init(aDrv->driverID, &aDrv->data);
	if (retVal)
		return retVal;
	
	Audio_GetDriverInfo(aDrv->driverID, &drvInfo);
#ifdef AUDDRV_DSOUND
	if (drvInfo->drvSig == ADRVSIG_DSOUND)
		DSound_SetHWnd(AudioDrv_GetDrvData(aDrv->data), GetDesktopWindow());
#endif
#ifdef AUDDRV_PULSE
	if (drvInfo->drvSig == ADRVSIG_PULSE)
		Pulse_SetStreamDesc(AudioDrv_GetDrvData(aDrv->data), "VGM Player");
#endif
	
	return AERR_OK;
}

// initialize audio system and search for requested audio drivers
static UINT8 InitAudioSystem(void)
{
	AUDDRV_INFO* drvInfo;
	UINT8 retVal;
	
	retVal = OSMutex_Init(&renderMtx, 0);
	
	printf("Opening Audio Device ...\n");
	retVal = Audio_Init();
	if (retVal == AERR_NODRVS)
		return retVal;
	
	retVal = ChooseAudioDriver(&adOut);
	if (retVal)
	{
		fprintf(stderr, "Requested audio output driver not found!\n");
		Audio_Deinit();
		return AERR_NODRVS;
	}
	if (adOut.driverID == -1)
	{
		fprintf(stderr, "No audio output driver chosen!\n");
		Audio_Deinit();
		return AERR_NODRVS;
	}
	
	Audio_GetDriverInfo(adOut.driverID, &drvInfo);
	printf("Using driver %s.\n", drvInfo->drvName);
	retVal = InitAudioDriver(&adOut);
	if (retVal)
	{
		fprintf(stderr, "WaveOut: Driver Init Error: %02X\n", retVal);
		Audio_Deinit();
		return retVal;
	}
	
	return AERR_OK;
}

static UINT8 DeinitAudioSystem(void)
{
	UINT8 retVal;
	
	retVal = 0x00;
	if (adOut.data != NULL)
		retVal = AudioDrv_Deinit(&adOut.data);
	Audio_Deinit();
	
	OSMutex_Deinit(renderMtx);	renderMtx = NULL;
	
	return retVal;
}

static UINT8 StartAudioDevice(void)
{
	AUDIO_OPTS* opts;
	UINT8 retVal;
	UINT32 smplAlloc;
	UINT32 localBufSize;
	
	opts = NULL;
	if (adOut.data != NULL)
		opts = AudioDrv_GetOptions(adOut.data);
	if (opts == NULL)
		return 0xFF;
	opts->sampleRate = sampleRate;
	opts->numChannels = 2;
	opts->numBitsPerSmpl = 16;
	smplSize = opts->numChannels * opts->numBitsPerSmpl / 8;
	smplAlloc = opts->sampleRate / 4;
	localBufSize = smplAlloc * smplSize;
	
	if (adOut.data != NULL)
	{
		printf("Opening Device %u ...\n", adOut.deviceID);
		retVal = AudioDrv_Start(adOut.data, adOut.deviceID);
		if (retVal)
		{
			fprintf(stderr, "Device Init Error: %02X\n", retVal);
			return retVal;
		}
		
		smplAlloc = AudioDrv_GetBufferSize(adOut.data) / smplSize;
	}
	if (AudioDrv_SetCallback(adOut.data, NULL, NULL) == AERR_OK)
		localBufSize = 0;	// we don't need a local buffer when the audio driver itself comes with one
	
	smplBuf.resize(smplAlloc);
	audioBuf.resize(localBufSize);
	
	return AERR_OK;
}

static UINT8 StopAudioDevice(void)
{
	UINT8 retVal;
	
	retVal = AERR_OK;
	if (adOut.data != NULL)
		retVal = AudioDrv_Stop(adOut.data);
	smplBuf.clear();
	audioBuf.clear();
	
	return retVal;
}


#ifdef WIN32
static void cls(void)
{
	// CLS-Function from the MSDN Help
	HANDLE hConsole;
	COORD coordScreen = {0, 0};
	BOOL bSuccess;
	DWORD cCharsWritten;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD dwConSize;
	
	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	
	// get the number of character cells in the current buffer
	bSuccess = GetConsoleScreenBufferInfo(hConsole, &csbi);
	
	// fill the entire screen with blanks
	dwConSize = csbi.dwSize.X * csbi.dwSize.Y;
	bSuccess = FillConsoleOutputCharacter(hConsole, (TCHAR)' ', dwConSize, coordScreen,
											&cCharsWritten);
	
	// get the current text attribute
	//bSuccess = GetConsoleScreenBufferInfo(hConsole, &csbi);
	
	// now set the buffer's attributes accordingly
	//bSuccess = FillConsoleOutputAttribute(hConsole, csbi.wAttributes, dwConSize, coordScreen,
	//										&cCharsWritten);
	
	// put the cursor at (0, 0)
	bSuccess = SetConsoleCursorPosition(hConsole, coordScreen);
	
	return;
}
#endif

#ifndef _WIN32
static struct termios oldterm;
static UINT8 termmode = 0xFF;

static void changemode(UINT8 noEcho)
{
	if (termmode == 0xFF)
	{
		tcgetattr(STDIN_FILENO, &oldterm);
		termmode = 0;
	}
	if (termmode == noEcho)
		return;
	
	if (noEcho)
	{
		struct termios newterm;
		newterm = oldterm;
		newterm.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &newterm);
		termmode = 1;
	}
	else
	{
		tcsetattr(STDIN_FILENO, TCSANOW, &oldterm);
		termmode = 0;
	}
	
	return;
}

static int _kbhit(void)
{
	struct timeval tv;
	fd_set rdfs;
	
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	
	FD_ZERO(&rdfs);
	FD_SET(STDIN_FILENO, &rdfs);
	select(STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);
	
	return FD_ISSET(STDIN_FILENO, &rdfs);;
}

static void cls(void)
{
	system("clear");
	
	return;
}
#endif
