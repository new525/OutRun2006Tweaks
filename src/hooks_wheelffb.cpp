// Steering wheel force feedback, ported from the arcade release.
//
// OutRun 2 SP SDX (Sega Lindbergh, 2006) drives the deluxe cabinet's steering motor through a drive
// board, and the decisions are all in the game: its main program (Jennifer) ships unstripped, and the
// routines below are translated from its CabinetCtrl_* / DrCtrl* functions. The PC release has none
// of this code; the car and physics structures it reads are the same ones, at the offsets found by
// matching the same routines in OR2006C2C.EXE (listed with each field below).
//
// What the arcade game sends the drive board (CabinetCtrl, serial "MIDI" packets):
//  - command 0x0B every frame: the steering reaction level, 4..15 (DrCtrlDataSet). Full while the
//    front tyres' slip angle is under 6 degrees, fading along a sine to nothing at 12 (a drift lightens
//    the wheel), x0.8 for each front tyre on a loose surface, out of 10, +1 when a load ratio of the
//    front tyres passes 1.2; the level rises at rate 2 and falls at rate 31.
//  - command 0x06: damping, 2 (1 at the lowest level).
//  - command 0x7B: play one of 16 force patterns, mirrored by a direction bit (SetOutFactor picks
//    the event: a wall rebound, contact on a corner of the car, a change of surface, the two kinds of
//    marked surface under the front tyres...). The patterns themselves are uploaded by the game at
//    start-up (packge_tbl): 15 steps each, 0x80 = no force.
//
// What is ours, not the game's: how the drive board turns a level, a rate and a pattern step into
// motor torque is in its own firmware, which the game doesn't contain. Here a level is a DirectInput
// spring at level/15 of the spring strength, a rate is a change of level per tick (in 1/16 levels), a
// pattern step lasts a game tick, and every scale has a setting.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <initguid.h>   // this file defines the DirectInput GUIDs it uses (no dxguid.lib)
#include <dinput.h>
#include <cmath>
#include <algorithm>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

namespace Settings
{
	Setting<bool> WheelFFB{ "Controls", "WheelFFB", false,
		"Force feedback for steering wheels, ported from the arcade release (OutRun 2 SP SDX): the steering "
		"weight follows the front tyres' grip and lightens in a drift, and the arcade's own jolts and rumbles "
		"play on wall hits, contact with other cars and surface changes. Needs a DirectInput force feedback wheel." };
	Setting<std::string> WheelFFBDevice{ "Controls", "WheelFFBDevice", "",
		"Part of the force feedback device's name to use (empty: a Logitech PRO Racing Wheel if one is connected, "
		"else the first force feedback device found)." };
	Setting<int> WheelFFBStrength{ "Controls", "WheelFFBStrength", 70,
		"Overall force feedback strength, 0-100.", Range<int>{ 0, 100 } };
	Setting<int> WheelFFBSpring{ "Controls", "WheelFFBSpring", 100,
		"Strength of the steering weight (the arcade's reaction level), 0-100.", Range<int>{ 0, 100 } };
	Setting<int> WheelFFBDamper{ "Controls", "WheelFFBDamper", 20,
		"Strength of the damping, 0-100.", Range<int>{ 0, 100 } };
	Setting<int> WheelFFBEffects{ "Controls", "WheelFFBEffects", 100,
		"Strength of the jolts and rumbles (the arcade's force patterns), 0-100.", Range<int>{ 0, 100 } };
	Setting<bool> WheelFFBInvert{ "Controls", "WheelFFBInvert", false,
		"Reverses the jolts' direction, for a wheel that kicks the wrong way." };
}

namespace
{
	// ---------------------------------------------------------------------------------------------
	// The arcade's tables (OutRun 2 SP SDX, Jennifer)
	// ---------------------------------------------------------------------------------------------

	// packge_tbl ($081E2280): 16 force patterns, uploaded to the drive board at start-up
	// (CabinetCtrl_Check, commands 0x1D/0x1E). Byte 0 is 0x10 in every pattern; then 15 steps,
	// 0x80 = no force, below = one way, above = the other.
	const uint8_t PATTERNS[16][16] = {
		{ 0x10, 0x50, 0xb0, 0xb0, 0xb0, 0xb0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x01, 0xff, 0xff, 0xff, 0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x40, 0x40, 0x40, 0xc0, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x01, 0xff, 0xff, 0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x40, 0x40, 0xc0, 0xc0, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x40, 0x40, 0xc0, 0xc0, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x98, 0x98, 0x68, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x68 },
		{ 0x10, 0xb0, 0xb0, 0x50, 0x50, 0x50, 0xb0, 0xb0, 0x50, 0x50, 0x50, 0xb0, 0xb0, 0x50, 0x50, 0x50 },
		{ 0x10, 0x40, 0xc0, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0xc0, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x40, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0xc0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
		{ 0x10, 0x8c, 0x8c, 0x72, 0x72, 0x8c, 0x8c, 0x72, 0x72, 0x8c, 0x8c, 0x72, 0x72, 0x80, 0x80, 0x80 },
		{ 0x10, 0x98, 0x98, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x80, 0x80, 0x80 },
		{ 0x10, 0x8c, 0x8c, 0x72, 0x72, 0x8c, 0x8c, 0x72, 0x72, 0x8c, 0x8c, 0x72, 0x72, 0x80, 0x80, 0x80 },
		{ 0x10, 0x98, 0x98, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x98, 0x98, 0x68, 0x68, 0x80, 0x80, 0x80 },
	};

	// ptn_trans_tbl ($081E2380): an event ("out factor") to its pattern
	const uint8_t PATTERN_OF_EVENT[32] = {
		0x0b, 0x09, 0x00, 0x00, 0x0a, 0x08, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x01, 0x0e, 0x0f, 0x00, 0x0c, 0x0d, 0x00, 0x06, 0x07,
	};

	// out_factor_time_tbl ($081E2260): the frames an event holds off the next one
	const uint8_t EVENT_HOLD[32] = {
		0x08, 0x10, 0x04, 0x05, 0x08, 0x08, 0x05, 0x06, 0x04, 0x05, 0x06, 0x07, 0x05, 0x06, 0x07, 0x08,
		0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0c, 0x06, 0x0a, 0x0c, 0x0c, 0x08, 0x08, 0x08,
	};

	// ---------------------------------------------------------------------------------------------
	// The game's data. Offsets: arcade EVWORK_CAR -> OR2006C2C.EXE, each found by matching the routine
	// that writes it in both programs.
	// ---------------------------------------------------------------------------------------------
	namespace Car
	{
		constexpr int FrontSlipAngle   = 0x4C;   // int16, 65536 = 360 deg   (arcade 0x54; CalcTireDirection, C2C 0x500700)
		constexpr int VelocityHeading  = 0x160;  // int16                    (arcade 0x170; CopyCarWork, C2C 0x4A1140)
		constexpr int SpeedKmh         = 0x1F4;  // uint32                   (arcade 0x3AC; CopyCarWork)
		constexpr int SurfaceAll       = 0x244;  // uint32, the four wheels' surface bits OR'd (arcade 0x3FC; CopyCarWork)
		constexpr int SurfaceWheel0    = 0x24C;  // uint32, front wheels' surface bits (arcade 0x404)
		constexpr int SurfaceWheel1    = 0x250;  //                                    (arcade 0x408)
		constexpr int WallSide         = 0x281;  // uint8                    (arcade 0x439; CbwColiWall, C2C 0x5041B0)
		constexpr int ReboundState     = 0x283;  // uint8, 0x1E = rebounding off a wall (arcade 0x43B; CwReboundStatus, C2C 0x503A20)
		constexpr int ReboundHeading   = 0x286;  // int16                    (arcade 0x43E)
		constexpr int WallSurface      = 0x2A8;  // uint32                   (arcade 0x460; CarBodyWallColiCheck, C2C 0x504CE0)
		constexpr int CrushTime        = 0x2C8;  // float                    (arcade 0x480; SetCrushMotion, C2C 0x4A2270)
		constexpr int CrushFlags       = 0x2F0;  // uint32                   (arcade 0x4A4)
		// contact on the car's four corners: three sources each (arcade 0x6C8.., 0x528.., 0x5FC..;
		// CheckWhichSideColiHappen, C2C 0x4F6640)
		constexpr int ContactA[4]      = { 0x514, 0x518, 0x51C, 0x520 };   // arcade 0x6C8 0x6CC 0x6D0 0x6D4
		constexpr int ContactB[4]      = { 0x374, 0x3A8, 0x3DC, 0x410 };   // arcade 0x528 0x55C 0x590 0x5C4
		constexpr int ContactC[4]      = { 0x448, 0x47C, 0x4B0, 0x4E4 };   // arcade 0x5FC 0x630 0x664 0x698
	}
	namespace Work   // the player's CAR_WORK: the same layout in both programs
	{
		constexpr uintptr_t Address = 0x42E7F0;   // C2C 0x82E7F0 (SetCrushMotion's PlWrecker call)
		constexpr int Flags         = 0x244;
		constexpr int Front0A = 0x28C, Front0B = 0x290, Front1A = 0x380, Front1B = 0x384;
	}
	constexpr uintptr_t GetVolume_Addr = 0x53720;   // int GetVolume(int channel): 0 = steering, signed

	template <typename T> T rd(const uint8_t* p, int off) { T v; memcpy(&v, p + off, sizeof v); return v; }

	// ---------------------------------------------------------------------------------------------
	// The arcade's logic (DrCtrlDataSet, SetOutFactor), on the C2C structures
	// ---------------------------------------------------------------------------------------------
	struct ArcadeState
	{
		int  level = 4;             // the reaction level sent (DAT_0866c414), 4..15
		int  rateUp = 2, rateDown = 0x1f;
		int  damping = 2;           // DAT_0866c413
		int  hold = 0;              // frames before another event may play (DAT_0866c40c)
		uint8_t lastCorners = 0;    // DAT_0866c40d
		uint32_t lastSurface = 0;   // DAT_0866c400
		int  event = -1;            // an event to play this frame (DAT_0866c412), -1 none
		bool mirror = false;
	};
	ArcadeState arc;

	int SteeringPosition()
	{
		static auto GetVolume = (int(__cdecl*)(int))Module::exe_ptr(GetVolume_Addr);
		return GetVolume ? GetVolume(0) : 0;
	}

	// SetOutFactor ($081052E4)
	void SetOutFactor(const uint8_t* car, const uint8_t* work)
	{
		if (arc.hold != 0)
			arc.hold--;
		bool mirror = false;
		int event = -1;
		const float crush = rd<float>(car, Car::CrushTime);

		if (rd<uint8_t>(car, Car::ReboundState) == 0x1E && (crush <= 0.f || (rd<uint32_t>(car, Car::CrushFlags) & 1)))
		{
			// rebounding off a wall: the direction from the rebound's heading against the travel's
			mirror = int16_t(rd<int16_t>(car, Car::ReboundHeading) - rd<int16_t>(car, Car::VelocityHeading)) < 0;
			event = (rd<uint32_t>(car, Car::WallSurface) & 0x10f0020) == 0 ? 2 : 0;
		}
		else
		{
			if (crush > 0.f)
				return;
			if (arc.hold == 0)
			{
				// contact on the car's corners, in the arcade's bit order
				uint8_t corners = 0;
				if (rd<int>(car, Car::ContactA[1]) || rd<int>(car, Car::ContactB[3]) || rd<int>(car, Car::ContactC[3])) corners |= 1;
				if (rd<int>(car, Car::ContactA[2]) || rd<int>(car, Car::ContactB[0]) || rd<int>(car, Car::ContactC[0])) corners |= 2;
				if (rd<int>(car, Car::ContactA[0]) || rd<int>(car, Car::ContactB[2]) || rd<int>(car, Car::ContactC[2])) corners |= 4;
				if (rd<int>(car, Car::ContactA[3]) || rd<int>(car, Car::ContactB[1]) || rd<int>(car, Car::ContactC[1])) corners |= 8;
				if (corners != 0)
				{
					if (corners != arc.lastCorners)
					{
						const bool b0 = (corners & 1) != 0, b1clear = (corners & 2) == 0;
						if (b0) event = 8;
						if (!b1clear) event = 8;
						bool b2 = (corners & 4) != 0;
						if (b2) event = 9;
						mirror = b2 || (b1clear && b0);
						if (corners & 8) { mirror = false; event = 9; }
					}
					arc.lastCorners = corners;
				}
			}
			if (event == -1)
			{
				const uint32_t surface = rd<uint32_t>(car, Car::SurfaceAll);
				if (surface != arc.lastSurface && (surface & 0xf03302) && (arc.lastSurface & 0x200801c))
				{
					// onto a marked surface: the direction the wheel is turned
					event = 10;
					mirror = SteeringPosition() > 0;
				}
				else if (arc.hold == 1)
				{
					// (the arcade marks its ack here: nothing to send)
				}
				else if (arc.hold == 0)
				{
					const uint32_t speed = rd<uint32_t>(car, Car::SpeedKmh);
					const uint32_t w0 = rd<uint32_t>(car, Car::SurfaceWheel0), w1 = rd<uint32_t>(car, Car::SurfaceWheel1);
					if ((rd<uint8_t>(work, Work::Flags) & 8) && speed > 5)
					{
						event = 6;
						mirror = !(rd<uint8_t>(car, Car::WallSide) & 1);
					}
					else if (((w0 | w1) & 0xffff & 0x8014) != 0)
						event = (speed < 0x28 || w0 != w1) ? 0xc : 0xd;
					else if (((w0 | w1) & 0x2000008) != 0)
						event = (speed < 0x28 || w0 != w1) ? 0xe : 0xf;
				}
			}
		}
		if (event == -1)
			return;
		arc.event = event;
		arc.mirror = mirror;
		arc.hold = EVENT_HOLD[event];
	}

	// DrCtrlDataSet ($08104F02)
	void DrCtrlDataSet(const uint8_t* car, const uint8_t* work)
	{
		SetOutFactor(car, work);

		const uint32_t speed = rd<uint32_t>(car, Car::SpeedKmh);
		const int16_t slip = rd<int16_t>(car, Car::FrontSlipAngle);
		const int angle = std::abs(int(slip));
		float grip;
		if (speed >= 5 && float(angle) < 1092.2666f)        // under 6 degrees at speed: full
			grip = 1.f;
		else if (float(angle) >= 2912.7109f)                 // 16 degrees and over
			grip = -0.1f;
		else if (float(angle) < 2184.5332f)                  // 0..12 degrees: a sine hump peaking at 6
			grip = std::sin(float(angle * 15) * 3.1415927f * 3.0517578e-05f);
		else
			grip = 0.f;
		grip = std::clamp(grip, 0.f, 1.f);
		const uint32_t w0 = rd<uint32_t>(car, Car::SurfaceWheel0), w1 = rd<uint32_t>(car, Car::SurfaceWheel1);
		if (w0 & 0xff0fc07d) grip *= 0.8f;                    // each front tyre on a loose surface
		if (w1 & 0xff0fc07d) grip *= 0.8f;
		int level = int(std::lround(10.f * grip));
		const float load = (rd<float>(work, Work::Front1A) + rd<float>(work, Work::Front0A)) /
		                   (rd<float>(work, Work::Front1B) + rd<float>(work, Work::Front0B));
		if (load > 1.2f)
			level++;
		level = std::clamp(level, 4, 15);
		arc.level = level;
		arc.damping = level == 4 ? 1 : 2;
		arc.lastSurface = rd<uint32_t>(car, Car::SurfaceAll);
	}

	// ---------------------------------------------------------------------------------------------
	// DirectInput force feedback device
	// ---------------------------------------------------------------------------------------------
	DIOBJECTDATAFORMAT s_axisFormat[] = {
		// 0x80000000: DIDFT_OPTIONAL (as in the SDK's c_dfDIJoystick; the Windows SDK's dinput.h lacks the name)
		{ &GUID_XAxis, 0, 0x80000000 | DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION },
	};
	DIDATAFORMAT s_dataFormat = { sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 4, 1, s_axisFormat };

	struct Device
	{
		IDirectInputDevice8A* dev = nullptr;
		IDirectInputEffect* spring = nullptr;
		IDirectInputEffect* damper = nullptr;
		IDirectInputEffect* constant = nullptr;
		bool failed = false;
		int retryTicks = 0;
		std::string name;
	};
	Device d;

	GUID s_found;
	bool s_haveFound = false;
	bool s_foundPreferred = false;

	// Wheels picked ahead of other force feedback devices when WheelFFBDevice is empty (a wheel and,
	// say, a force feedback pad both connected): vendor, product
	const uint16_t PREFERRED[][2] = {
		{ 0x046d, 0xc272 },   // Logitech PRO Racing Wheel, Xbox/PC
		{ 0x046d, 0xc268 },   // Logitech PRO Racing Wheel, PlayStation/PC (PC mode)
		{ 0x046d, 0xc269 },   // Logitech PRO Racing Wheel, PlayStation/PC (PlayStation mode)
	};

	BOOL CALLBACK EnumFFDevice(const DIDEVICEINSTANCEA* inst, void*)
	{
		const std::string& want = Settings::WheelFFBDevice.get();
		std::string name = inst->tszProductName;
		// DirectInput's product GUID for a USB device starts with MAKELONG(vendor, product)
		const uint16_t vendor = LOWORD(inst->guidProduct.Data1), product = HIWORD(inst->guidProduct.Data1);
		spdlog::info("WheelFFB: force feedback device \"{}\" ({:04x}:{:04x})", name, vendor, product);
		if (!want.empty())
		{
			std::string a = name, b = want;
			std::transform(a.begin(), a.end(), a.begin(), ::tolower);
			std::transform(b.begin(), b.end(), b.begin(), ::tolower);
			if (a.find(b) == std::string::npos)
				return DIENUM_CONTINUE;
			s_found = inst->guidInstance;
			s_haveFound = true;
			d.name = name;
			return DIENUM_STOP;
		}
		bool preferred = false;
		for (const auto& id : PREFERRED)
			preferred |= id[0] == vendor && id[1] == product;
		// the first device, unless a preferred wheel turns up later
		if (!s_haveFound || (preferred && !s_foundPreferred))
		{
			s_found = inst->guidInstance;
			s_haveFound = true;
			s_foundPreferred = preferred;
			d.name = name;
		}
		return DIENUM_CONTINUE;
	}

	IDirectInputEffect* CreateEffect(REFGUID type, void* params, DWORD paramSize, DWORD duration)
	{
		DWORD axes[1] = { 0 };   // the X axis, at offset 0 in s_dataFormat
		LONG dir[1] = { 0 };
		DIEFFECT e = { sizeof(DIEFFECT) };
		e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
		e.dwDuration = duration;
		e.dwGain = DI_FFNOMINALMAX;
		e.dwTriggerButton = DIEB_NOTRIGGER;
		e.cAxes = 1;
		e.rgdwAxes = axes;
		e.rglDirection = dir;
		e.cbTypeSpecificParams = paramSize;
		e.lpvTypeSpecificParams = params;
		IDirectInputEffect* fx = nullptr;
		HRESULT hr = d.dev->CreateEffect(type, &e, &fx, nullptr);
		if (FAILED(hr))
		{
			spdlog::error("WheelFFB: CreateEffect failed ({:#x})", uint32_t(hr));
			return nullptr;
		}
		return fx;
	}

	void Close()
	{
		if (d.constant) { d.constant->Stop(); d.constant->Release(); d.constant = nullptr; }
		if (d.damper) { d.damper->Stop(); d.damper->Release(); d.damper = nullptr; }
		if (d.spring) { d.spring->Stop(); d.spring->Release(); d.spring = nullptr; }
		if (d.dev) { d.dev->Unacquire(); d.dev->Release(); d.dev = nullptr; }
	}

	bool Open()
	{
		IDirectInput8A* di = Game::DirectInput8();
		HWND hwnd = Game::GameHwnd();
		if (!di || !hwnd)
			return false;

		s_haveFound = s_foundPreferred = false;
		di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumFFDevice, nullptr, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
		if (!s_haveFound)
		{
			spdlog::warn("WheelFFB: no force feedback device found{}",
				Settings::WheelFFBDevice.get().empty() ? "" : " matching WheelFFBDevice");
			return false;
		}

		HRESULT hr = di->CreateDevice(s_found, &d.dev, nullptr);
		if (FAILED(hr)) { spdlog::error("WheelFFB: CreateDevice failed ({:#x})", uint32_t(hr)); d.dev = nullptr; return false; }
		hr = d.dev->SetDataFormat(&s_dataFormat);
		if (FAILED(hr)) { spdlog::error("WheelFFB: SetDataFormat failed ({:#x})", uint32_t(hr)); Close(); return false; }
		hr = d.dev->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
		if (FAILED(hr)) { spdlog::error("WheelFFB: SetCooperativeLevel failed ({:#x})", uint32_t(hr)); Close(); return false; }

		DIPROPDWORD autocenter = { { sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE }, DIPROPAUTOCENTER_OFF };
		d.dev->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);

		hr = d.dev->Acquire();
		if (FAILED(hr)) { spdlog::error("WheelFFB: Acquire failed ({:#x})", uint32_t(hr)); Close(); return false; }

		DICONDITION cond = {};
		cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
		d.spring = CreateEffect(GUID_Spring, &cond, sizeof cond, INFINITE);
		d.damper = CreateEffect(GUID_Damper, &cond, sizeof cond, INFINITE);
		DICONSTANTFORCE cf = { 0 };
		d.constant = CreateEffect(GUID_ConstantForce, &cf, sizeof cf, 50 * 1000);
		if (!d.spring && !d.constant)
		{
			spdlog::error("WheelFFB: \"{}\" accepted no effects", d.name);
			Close();
			return false;
		}
		if (d.spring) d.spring->Start(1, 0);
		if (d.damper) d.damper->Start(1, 0);
		spdlog::info("WheelFFB: using \"{}\" (spring {}, damper {}, constant force {})", d.name,
			d.spring ? "yes" : "no", d.damper ? "yes" : "no", d.constant ? "yes" : "no");
		return true;
	}

	// A DirectInput call failed because the device was lost: acquire it again and restart the effects
	void Reacquire()
	{
		if (!d.dev || FAILED(d.dev->Acquire()))
			return;
		if (d.spring) d.spring->Start(1, 0);
		if (d.damper) d.damper->Start(1, 0);
	}

	void LogFailure(const char* what, HRESULT hr);

	void SetCondition(IDirectInputEffect* fx, LONG coefficient)
	{
		if (!fx)
			return;
		DWORD axes[1] = { 0 };
		LONG dir[1] = { 0 };
		DICONDITION cond = {};
		cond.lPositiveCoefficient = cond.lNegativeCoefficient = coefficient;
		cond.dwPositiveSaturation = cond.dwNegativeSaturation = DI_FFNOMINALMAX;
		DIEFFECT e = { sizeof(DIEFFECT) };
		e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
		e.cAxes = 1;
		e.rgdwAxes = axes;
		e.rglDirection = dir;
		e.cbTypeSpecificParams = sizeof cond;
		e.lpvTypeSpecificParams = &cond;
		// in place if the driver allows it; else with the restart DirectInput may need (some drivers refuse
		// DIEP_NORESTART outright)
		HRESULT hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART);
		if (FAILED(hr) && hr != DIERR_INPUTLOST && hr != DIERR_NOTACQUIRED && hr != DIERR_NOTEXCLUSIVEACQUIRED)
			hr = fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_START);
		if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED || hr == DIERR_NOTEXCLUSIVEACQUIRED)
			Reacquire();
		if (FAILED(hr))
			LogFailure("a spring/damper update", hr);
	}

	// A pattern step: a constant force for a little longer than a tick (it lapses by itself if the game
	// stops calling, say in a pause)
	void SetConstant(LONG magnitude)
	{
		if (!d.constant)
			return;
		DWORD axes[1] = { 0 };
		LONG dir[1] = { 0 };
		DICONSTANTFORCE cf = { magnitude };
		DIEFFECT e = { sizeof(DIEFFECT) };
		e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
		e.dwDuration = 50 * 1000;
		e.cAxes = 1;
		e.rgdwAxes = axes;
		e.rglDirection = dir;
		e.cbTypeSpecificParams = sizeof cf;
		e.lpvTypeSpecificParams = &cf;
		HRESULT hr = d.constant->SetParameters(&e, DIEP_TYPESPECIFICPARAMS | DIEP_DURATION | DIEP_START);
		if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED || hr == DIERR_NOTEXCLUSIVEACQUIRED)
			Reacquire();
		if (FAILED(hr))
			LogFailure("a pattern step", hr);
	}

	// ---------------------------------------------------------------------------------------------
	// Per tick
	// ---------------------------------------------------------------------------------------------
	int   s_shownLevel16 = 4 * 16;   // the level the device has, in 1/16 levels (the rates move it)
	LONG  s_lastSpring = -1, s_lastDamper = -1;
	int   s_pattern = -1, s_step = 0;
	bool  s_patternMirror = false;
	bool  s_raceActive = false;

	// CabinetCtrl_Main switches the drive board's following on at the arcade's request modes 0xD/0xF and
	// keeps it through the race. On PC the player's car only ticks once the race runs, after the start
	// mode has passed, so here it follows in every mode of a race (the arcade's keep-list and
	// START/RESTART), else the wheel stays at level 4.
	void UpdateRaceActive()
	{
		const int mode = Game::current_mode ? int(*Game::current_mode) : -1;
		bool active = false;
		switch (mode)
		{
		case STATE_START: case STATE_WARP: case STATE_RESTART: case STATE_GAME: case STATE_GIVEUP:
		case STATE_SMPAUSEMENU: case STATE_GOAL: case STATE_TIMEUP: case STATE_LINK_TIMEUP:
		case STATE_TRYAGAIN: case STATE_OUTRUNMILES:
			active = true;
			break;
		default:
			break;
		}
		if (active != s_raceActive)
			spdlog::info("WheelFFB: following the car {} (game mode {:#x})", active ? "on" : "off", mode);
		s_raceActive = active;
	}

	// a failed effect update, logged the first few times
	int s_failures = 0;
	void LogFailure(const char* what, HRESULT hr)
	{
		if (s_failures < 8)
			spdlog::warn("WheelFFB: {} failed ({:#x}){}", what, uint32_t(hr), ++s_failures == 8 ? " (no more of these)" : "");
	}

	void Tick(EVWORK_CAR* carStruct)
	{
		if (!Settings::WheelFFB)
		{
			if (d.dev) Close();
			return;
		}
		if (!d.dev)
		{
			if (d.retryTicks > 0) { d.retryTicks--; return; }
			if (!Open()) { d.retryTicks = 60 * 30; return; }   // look again in about half a minute
		}

		const uint8_t* car = reinterpret_cast<const uint8_t*>(carStruct);
		const uint8_t* work = Module::exe_ptr(Work::Address);

		UpdateRaceActive();
		int target = 4;   // outside a race the arcade leaves the wheel at level 4 (DrCtrlMoveProgSend)
		if (s_raceActive && car && work)
		{
			arc.event = -1;
			DrCtrlDataSet(car, work);
			target = arc.level;
			if (arc.event >= 0)
			{
				spdlog::debug("WheelFFB: event {} -> pattern {}{}", arc.event, PATTERN_OF_EVENT[arc.event], arc.mirror ? " mirrored" : "");
				s_pattern = PATTERN_OF_EVENT[arc.event];
				s_step = 1;
				s_patternMirror = arc.mirror;
			}
		}
		else
		{
			arc = ArcadeState{};
			s_pattern = -1;
		}

		// the level moves towards its target at the arcade's rates (rising slowly, falling fast)
		if (s_shownLevel16 < target * 16) s_shownLevel16 = (std::min)(target * 16, s_shownLevel16 + arc.rateUp);
		else if (s_shownLevel16 > target * 16) s_shownLevel16 = (std::max)(target * 16, s_shownLevel16 - arc.rateDown);

		const float master = Settings::WheelFFBStrength / 100.f;
		const LONG spring = LONG(DI_FFNOMINALMAX * master * (Settings::WheelFFBSpring / 100.f) * (s_shownLevel16 / (15.f * 16.f)));
		const LONG damper = LONG(DI_FFNOMINALMAX * master * (Settings::WheelFFBDamper / 100.f) * (arc.damping / 2.f));
		static int s_ticks = 0;
		if (++s_ticks % 120 == 0)
			spdlog::debug("WheelFFB: mode {:#x} following {} level {} (shown {:.2f}) spring {} damper {} speed {} slip {}",
				Game::current_mode ? int(*Game::current_mode) : -1, s_raceActive, arc.level, s_shownLevel16 / 16.f, spring, damper,
				car ? rd<uint32_t>(car, Car::SpeedKmh) : 0, car ? rd<int16_t>(car, Car::FrontSlipAngle) : 0);
		if (spring != s_lastSpring) { SetCondition(d.spring, spring); s_lastSpring = spring; }
		if (damper != s_lastDamper) { SetCondition(d.damper, damper); s_lastDamper = damper; }

		if (s_pattern >= 0)
		{
			const int value = int(PATTERNS[s_pattern][s_step]) - 0x80;
			float force = std::clamp(value / 127.f, -1.f, 1.f);
			if (s_patternMirror) force = -force;
			if (Settings::WheelFFBInvert) force = -force;
			SetConstant(LONG(DI_FFNOMINALMAX * master * (Settings::WheelFFBEffects / 100.f) * force));
			if (++s_step > 15)
			{
				s_pattern = -1;
				SetConstant(0);
			}
		}
	}
}

// Called by the Vibration hook (hooks_forcefeedback.cpp) once per player car tick, after the game's own
void WheelFFB_Tick(EVWORK_CAR* car)
{
	Tick(car);
}

class WheelFFB : public Hook
{
public:
	std::string_view description() override
	{
		return "WheelFFB";
	}

	void declare_settings() override
	{
		// the strengths are read every tick; the device is looked for again when the name changes
		Settings::WheelFFBDevice.watch([] { Close(); d.retryTicks = 0; });
	}

	bool apply() override
	{
		return true;
	}

	static WheelFFB instance;
};
WheelFFB WheelFFB::instance;
