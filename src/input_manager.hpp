#pragma once

#include <SDL3/SDL.h>
#include <unordered_map>
#include <vector>
#include <memory>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "overlay/overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <istream>
#include "input_names.hpp"

#include "imgui.h"
#include <format>
#include <string>
#include <fstream>

// fixups for SDL3 sillyness
#define SDL_GAMEPAD_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_GAMEPAD_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_GAMEPAD_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_GAMEPAD_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH

enum class ListenState
{
	False = 0,
	WaitForButtonRelease = 1,
	Listening = 2,
	WaitForBindButtonRelease = 3
};

inline ListenState isListeningForInput = ListenState::False;

// Actions belonging to the mod rather than the game.
enum class ModAction
{
	OverlayToggle,
	HudToggle,
	OpenChat,
	MusicNext,
	MusicPrevious,
	Count
};

enum class InputSourceType
{
	GamePad,
	Keyboard
};

struct InputState
{
	float currentValue = 0.0f;
	float previousValue = 0.0f;
	bool isAxis = false;
	InputSourceType lastSourceType;

	void update(float newValue)
	{
		previousValue = currentValue;
		currentValue = newValue;
	}

	bool isNewlyPressed(float threshold = 0.5f) const
	{
		return currentValue >= threshold && previousValue < threshold;
	}

	bool isPressed(float threshold = 0.5f) const
	{
		return currentValue >= threshold;
	}
};

// Non-gamepad SDL joysticks (wheels, pedals, button boxes) are owned by the
// InputManager. A binding names its device by USB vendor/product so that it
// survives the device being replugged or SDL renumbering it; 0 matches the
// first joystick connected.
SDL_Joystick* InputManager_FindJoystick(uint32_t device);
std::string InputManager_JoystickLabel(uint32_t device);

inline uint32_t JoystickDeviceId(SDL_Joystick* joystick)
{
	return (uint32_t(SDL_GetJoystickVendor(joystick)) << 16) | SDL_GetJoystickProduct(joystick);
}

//
// A single bound input: one gamepad button, one gamepad axis, one key, or one
// axis, button or hat direction of a joystick.
//
struct InputBinding
{
	enum class Kind : uint8_t { None, PadButton, PadAxis, Key, JoyAxis, JoyButton, JoyHat };

	// How a joystick axis turns into a value. A wheel rests in the middle of
	// its range; a pedal rests at one end of it, and which end depends on the
	// device, so the binding records the end it was resting at when captured.
	enum class AxisMode : uint8_t
	{
		Centered,    // -1 .. 1, rest at 0
		Full,        //  0 .. 1, rest at the minimum
		FullInverted //  0 .. 1, rest at the maximum
	};

	static constexpr float StickRange = 32768.f;
	static constexpr float RStickDeadzone = 0.7f; // needs a high deadzone or flicks bounce

	// A pedal at rest rarely reads exactly zero. Without this a resting brake
	// pedal can feed the game a brake value of 1/255.
	static constexpr float PedalRestDeadzone = 0.02f;

	Kind kind = Kind::None;
	bool negate = false;
	union
	{
		SDL_GamepadButton button;
		SDL_GamepadAxis axis;
		SDL_Scancode key;
	};

	// Joystick bindings only.
	uint32_t joyDevice = 0;
	int joyIndex = 0;
	uint8_t joyHatDir = 0;
	AxisMode axisMode = AxisMode::Centered;

	InputBinding() : button(SDL_GAMEPAD_BUTTON_INVALID) {}
	InputBinding(SDL_GamepadButton b, bool n = false) : kind(Kind::PadButton), negate(n), button(b) {}
	InputBinding(SDL_GamepadAxis a, bool n = false) : kind(Kind::PadAxis), negate(n), axis(a) {}
	InputBinding(SDL_Scancode k, bool n = false) : kind(Kind::Key), negate(n), key(k) {}

	static InputBinding JoyAxis(uint32_t device, int index, AxisMode mode, bool n = false)
	{
		InputBinding b;
		b.kind = Kind::JoyAxis;
		b.negate = n;
		b.joyDevice = device;
		b.joyIndex = index;
		b.axisMode = mode;
		return b;
	}

	static InputBinding JoyButton(uint32_t device, int index)
	{
		InputBinding b;
		b.kind = Kind::JoyButton;
		b.joyDevice = device;
		b.joyIndex = index;
		return b;
	}

	static InputBinding JoyHat(uint32_t device, int index, uint8_t direction)
	{
		InputBinding b;
		b.kind = Kind::JoyHat;
		b.joyDevice = device;
		b.joyIndex = index;
		b.joyHatDir = direction;
		return b;
	}

	bool isAxis() const { return kind == Kind::PadAxis || kind == Kind::JoyAxis; }
	bool isKeyboard() const { return kind == Kind::Key; }
	bool isGamepad() const { return kind == Kind::PadButton || kind == Kind::PadAxis; }
	bool isJoystick() const { return kind == Kind::JoyAxis || kind == Kind::JoyButton || kind == Kind::JoyHat; }
	bool isNegated() const { return negate; }

	InputSourceType sourceType() const
	{
		return isKeyboard() ? InputSourceType::Keyboard : InputSourceType::GamePad;
	}

	static float readJoyAxis(Sint16 raw, AxisMode mode)
	{
		switch (mode)
		{
		case AxisMode::Full:
		case AxisMode::FullInverted:
		{
			float value = (float(raw) + 32768.f) / 65535.f;
			if (mode == AxisMode::FullInverted)
				value = 1.f - value;
			value = (value - PedalRestDeadzone) / (1.f - PedalRestDeadzone);
			return std::clamp(value, 0.f, 1.f);
		}
		default:
			return std::clamp(float(raw) / StickRange, -1.f, 1.f);
		}
	}

	float read(SDL_Gamepad* gamepad) const
	{
		float value = 0.0f;

		switch (kind)
		{
		case Kind::Key:
		{
			const bool* state_array = SDL_GetKeyboardState(nullptr);
			value = float(state_array[key]);
			break;
		}
		case Kind::PadButton:
			if (!gamepad)
				return 0.0f;
			value = float(SDL_GetGamepadButton(gamepad, button));
			break;
		case Kind::PadAxis:
		{
			if (!gamepad)
				return 0.0f;

			Sint16 raw = SDL_GetGamepadAxis(gamepad, axis);

			int deadzone = 0;
			if (axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY)
				deadzone = int(StickRange * Settings::SteeringDeadZone);
			else if (axis == SDL_GAMEPAD_AXIS_RIGHTX || axis == SDL_GAMEPAD_AXIS_RIGHTY)
				deadzone = int(StickRange * RStickDeadzone);
			else
				deadzone = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

			if (abs(raw) < deadzone)
				raw = 0;

			value = raw / StickRange;
			break;
		}
		case Kind::JoyAxis:
		{
			SDL_Joystick* joystick = InputManager_FindJoystick(joyDevice);
			if (!joystick)
				return 0.0f;
			// No deadzone on a centred axis: a wheel has no stick drift to hide,
			// and a deadzone would put a flat spot in the middle of the lock.
			value = readJoyAxis(SDL_GetJoystickAxis(joystick, joyIndex), axisMode);
			break;
		}
		case Kind::JoyButton:
		{
			SDL_Joystick* joystick = InputManager_FindJoystick(joyDevice);
			if (!joystick)
				return 0.0f;
			value = float(SDL_GetJoystickButton(joystick, joyIndex));
			break;
		}
		case Kind::JoyHat:
		{
			SDL_Joystick* joystick = InputManager_FindJoystick(joyDevice);
			if (!joystick)
				return 0.0f;
			value = (SDL_GetJoystickHat(joystick, joyIndex) & joyHatDir) ? 1.0f : 0.0f;
			break;
		}
		default:
			return 0.0f;
		}

		return negate ? -value : value;
	}

	static const char* hatDirName(uint8_t direction)
	{
		switch (direction)
		{
		case SDL_HAT_UP:    return "Up";
		case SDL_HAT_DOWN:  return "Down";
		case SDL_HAT_LEFT:  return "Left";
		case SDL_HAT_RIGHT: return "Right";
		default:            return "";
		}
	}

	static std::optional<uint8_t> hatDirFromName(std::string_view name)
	{
		static constexpr uint8_t Directions[] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
		const std::string text(name);
		for (uint8_t direction : Directions)
			if (!_stricmp(text.c_str(), hatDirName(direction)))
				return direction;
		return std::nullopt;
	}

	std::string joystickDisplayName() const
	{
		const std::string device = InputManager_JoystickLabel(joyDevice);
		switch (kind)
		{
		case Kind::JoyAxis:
		{
			const char* mode = axisMode == AxisMode::Centered ? "" : " (pedal)";
			return std::format("{} Axis {}{}", device, joyIndex, mode);
		}
		case Kind::JoyButton:
			return std::format("{} Button {}", device, joyIndex);
		case Kind::JoyHat:
			return std::format("{} D-Pad {} {}", device, joyIndex, hatDirName(joyHatDir));
		default:
			return "";
		}
	}

	// "046d:c272 Axis0", "046d:c272 Axis2 FullInv", "046d:c272 Button4",
	// "046d:c272 Hat0 Up". "*" in place of the device matches any joystick.
	std::string joystickIniName() const
	{
		const std::string device = joyDevice
			? std::format("{:04x}:{:04x}", joyDevice >> 16, joyDevice & 0xFFFF)
			: std::string("*");

		switch (kind)
		{
		case Kind::JoyAxis:
		{
			const char* mode = axisMode == AxisMode::Full ? " Full"
				: axisMode == AxisMode::FullInverted ? " FullInv" : "";
			return std::format("{} Axis{}{}", device, joyIndex, mode);
		}
		case Kind::JoyButton:
			return std::format("{} Button{}", device, joyIndex);
		case Kind::JoyHat:
			return std::format("{} Hat{} {}", device, joyIndex, hatDirName(joyHatDir));
		default:
			return "";
		}
	}

	static std::optional<InputBinding> parseJoystickIni(const std::string& value)
	{
		std::vector<std::string> words;
		{
			std::string word;
			for (char c : value)
			{
				if (c == ' ' || c == '\t')
				{
					if (!word.empty())
						words.push_back(word);
					word.clear();
				}
				else
					word += c;
			}
			if (!word.empty())
				words.push_back(word);
		}
		if (words.size() < 2)
			return std::nullopt;

		uint32_t device = 0;
		if (words[0] != "*")
		{
			unsigned vid = 0, pid = 0;
			if (sscanf_s(words[0].c_str(), "%x:%x", &vid, &pid) != 2 || vid > 0xFFFF || pid > 0xFFFF)
				return std::nullopt;
			device = (vid << 16) | pid;
		}

		auto indexAfter = [&](const char* prefix) -> std::optional<int>
		{
			const size_t len = strlen(prefix);
			if (words[1].size() <= len || _strnicmp(words[1].c_str(), prefix, len))
				return std::nullopt;
			char* end = nullptr;
			long index = strtol(words[1].c_str() + len, &end, 10);
			if (*end || index < 0 || index > 255)
				return std::nullopt;
			return int(index);
		};

		if (auto index = indexAfter("Axis"))
		{
			AxisMode mode = AxisMode::Centered;
			if (words.size() >= 3)
			{
				if (!_stricmp(words[2].c_str(), "Full"))
					mode = AxisMode::Full;
				else if (!_stricmp(words[2].c_str(), "FullInv"))
					mode = AxisMode::FullInverted;
				else
					return std::nullopt;
			}
			return JoyAxis(device, *index, mode);
		}
		if (auto index = indexAfter("Button"))
			return JoyButton(device, *index);
		if (auto index = indexAfter("Hat"))
		{
			if (words.size() < 3)
				return std::nullopt;
			auto direction = hatDirFromName(words[2]);
			if (!direction)
				return std::nullopt;
			return JoyHat(device, *index, *direction);
		}

		return std::nullopt;
	}

	std::string displayName(SDL_GamepadType padType = SDL_GAMEPAD_TYPE_UNKNOWN, bool isSteerAction = false) const
	{
		switch (kind)
		{
		case Kind::Key:       return SDL_GetScancodeName(key);
		case Kind::PadAxis:   return InputNames::displayNameForAxis(axis, padType, negate, isSteerAction);
		case Kind::PadButton: return InputNames::displayNameForButton(button, padType);
		case Kind::JoyAxis:
		case Kind::JoyButton:
		case Kind::JoyHat:    return joystickDisplayName();
		default:              return "";
		}
	}

	std::string iniName() const
	{
		switch (kind)
		{
		case Kind::Key:       return SDL_GetScancodeName(key);
		case Kind::PadAxis:   return InputNames::iniNameForAxis(axis);
		case Kind::PadButton: return InputNames::iniNameForButton(button);
		case Kind::JoyAxis:
		case Kind::JoyButton:
		case Kind::JoyHat:    return joystickIniName();
		default:              return "";
		}
	}
};

class InputAction
{
	std::vector<InputBinding> bindings_;
	InputState state_;

	// Pedals only go one way. A centred joystick axis bound to one of them
	// (a combined-pedal axis, say) reads negative for half its travel, which
	// would otherwise outrank a positive reading from another binding.
	bool unsigned_ = false;

public:
	void setUnsigned(bool isUnsigned) { unsigned_ = isUnsigned; }

	const InputState& update(SDL_Gamepad* primary_pad)
	{
		float maxValue = 0.0f;
		bool isAxisInput = false;
		InputSourceType lastSource = state_.lastSourceType;

		// Read all bindings and take the highest absolute value
		for (const auto& binding : bindings_)
		{
			float currentValue = binding.read(primary_pad);
			if (unsigned_ && currentValue < 0.0f)
				currentValue = 0.0f;
			if (std::abs(currentValue) > std::abs(maxValue))
			{
				maxValue = currentValue;
				isAxisInput = binding.isAxis();
				lastSource = binding.sourceType();
			}
		}

		state_.lastSourceType = lastSource;
		state_.isAxis = isAxisInput;
		state_.update(maxValue);
		return state_;
	}

	void add(const InputBinding& binding) { bindings_.push_back(binding); }

	void clear() { bindings_.clear(); }

	std::vector<InputBinding>& bindings() { return bindings_; }
	const std::vector<InputBinding>& bindings() const { return bindings_; }

	const InputState& getState() const { return state_; }
	void setState(const InputState& state) { this->state_ = state; }
};

// ReadSwitch translates the raw DirectInput button mask into the SwitchId bits
// the rest of the game uses. Code that reads the raw mask instead needs to see
// the same presses, so this is that translation table, inverted.
struct RawButtonMapping
{
	uint32_t rawBit;
	SwitchId switchId;
};
inline constexpr RawButtonMapping RawButtonMap[] = {
	{ 0x00000001, SwitchId::Start          },
	{ 0x00000200, SwitchId::Back           },
	{ 0x00000002, SwitchId::A              },
	{ 0x00000004, SwitchId::B              },
	{ 0x00000008, SwitchId::X              },
	{ 0x00000010, SwitchId::Y              },
	{ 0x00000040, SwitchId::SelectionUp    },
	{ 0x00000020, SwitchId::SelectionDown  },
	{ 0x00000100, SwitchId::SelectionLeft  },
	{ 0x00000080, SwitchId::SelectionRight },
	{ 0x00000800, SwitchId::License        },
	{ 0x00000400, SwitchId::SignIn         },
	{ 0x08000000, SwitchId::Unknown0x200   },
	{ 0x00100000, SwitchId::Unknown0x100   },
};

// Two buttons that ReadSwitch has no entry for, so they reach the game only
// through the raw mask. A DirectInput pad reports the triggers here, and the
// Sumo car select screen toggles between its two car lists on the right one.
inline constexpr uint32_t RawTriggerLeft = 0x1000;
inline constexpr uint32_t RawTriggerRight = 0x2000;
inline constexpr float RawTriggerThreshold = 0.5f;

class InputManager
{
public:
	// Which of the three binding tables an action lives in.
	enum class ActionKind { Volume, Switch, Mod };

private:
	std::array<InputAction, size_t(ADChannel::Count)> volumeBindings;
	std::array<InputAction, size_t(SwitchId::Count)> switchBindings;
	std::array<InputAction, size_t(ModAction::Count)> modBindings;

	std::mutex mtx;
	std::vector<SDL_Gamepad*> controllers;
	int primaryControllerIndex = -1;

	// Joysticks SDL has no gamepad mapping for: wheels, pedals, button boxes.
	// All of them are read at once; each binding names its own device.
	std::vector<SDL_Joystick*> joysticks;

	// Axis positions when the binding prompt started listening, so a capture
	// sees which axis moved rather than which one is away from centre. A pedal
	// at rest sits at one end of its range and would otherwise bind instantly.
	std::vector<std::pair<SDL_JoystickID, std::vector<Sint16>>> captureBaseline;

	SDL_Window* window = nullptr;

	// cached values as of last update call
	std::array<InputState, size_t(ADChannel::Count)> volumes;
	std::array<InputState, size_t(ModAction::Count)> modStates;

	// Mod actions are readable while the overlay is up, since the overlay toggle
	// has to be able to close it again, but not while the binding dialog owns
	// every input.
	bool modActionsDeaf = false;

	// Switch bitmasks. switch_current/_previous are what the game sees;
	// switch_overlay is the same data before game-side suppression, so the
	// overlay can still be driven while the game is deaf.
	uint32_t switch_current;
	uint32_t switch_previous;
	uint32_t switch_overlay;

	// Mirror of the raw DirectInput masks. Edges are tracked here rather than
	// read back out of the game's copy, because DInputUpdate rewrites the same
	// fields whenever a device is still being polled.
	uint32_t raw_buttons = 0;
	uint32_t raw_pressed = 0;
	uint32_t raw_released = 0;

	// Latched until the user releases everything - see update().
	// Previously function-local statics inside update().
	bool suppressOverlayUntilRelease = false;
	bool suppressGameUntilRelease = false;
	InputSourceType lastInputSource_ = InputSourceType::GamePad;

private:
	static inline const std::string volumeNames[] = {
		"Steering",
		"Acceleration",
		"Brake"
	};

	static inline const std::string switchNames[] = {
		"Start",
		"Back",
		"A",
		"B",
		"X",
		"Y",
		"Gear Down",
		"Gear Up",
		"Unk0x100",
		"Unk0x200",
		"Selection Up",
		"Selection Down",
		"Selection Left",
		"Selection Right",
		"License",
		"Sign In",
		"Unk0x10000",
		"Unk0x20000",
		"Change View"
	};

	static inline const std::string modNames[] = {
		"Overlay",
		"HUD Toggle",
		"Open Chat",
		"Music Next",
		"Music Previous"
	};
	static_assert(std::size(modNames) == size_t(ModAction::Count));

	// switchNames must stay 1:1 with SwitchId - the ini reader/writer index both
	// by the same value. (volumeNames deliberately does NOT match ADChannel:
	// the enum reserves 4 unused AD channels, so always bound by std::size.)
	static_assert(std::size(switchNames) == size_t(SwitchId::Count));

	static int Sumo_CalcSteerSensitivity_wrapper(int a1, int a2)
	{
		int returnValue;
		__asm {
			push ebx

			mov eax, a1
			mov ebx, a2

			call Game::Sumo_CalcSteerSensitivity

			mov returnValue, eax

			pop ebx
		}
		return returnValue;
	}

	void setupGamepad(SDL_Gamepad* controller)
	{
		Game::CurrentPadType = Game::GamepadType::Xbox;
		auto type = SDL_GetGamepadType(controller);
		switch (type)
		{
		case SDL_GAMEPAD_TYPE_PS3:
		case SDL_GAMEPAD_TYPE_PS4:
		case SDL_GAMEPAD_TYPE_PS5:
			Game::CurrentPadType = Game::GamepadType::PS;
			break;
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
			Game::CurrentPadType = Game::GamepadType::Switch;
			break;
		};
	}

	void onControllerAdded(SDL_JoystickID instanceId)
	{
		spdlog::debug(__FUNCTION__ "({})", instanceId);

		SDL_Gamepad* controller = SDL_OpenGamepad(instanceId);
		if (!controller)
		{
			spdlog::error(__FUNCTION__ "({}): !controller", instanceId);
			return;
		}

		std::lock_guard<std::mutex> lock(mtx);

		// check if we've already seen this controller, SDL sometimes sends two controller added events some reason
		if (std::find(controllers.begin(), controllers.end(), controller) != controllers.end())
		{
			spdlog::warn(__FUNCTION__ "({}): dupe, ignored", instanceId);
			SDL_CloseGamepad(controller);
			return;
		}

		controllers.push_back(controller);

		// If we don't have primary already, set it as this
		if (primaryControllerIndex == -1)
			setPrimaryGamepad(controllers.size() - 1);
	}

	void onControllerRemoved(SDL_JoystickID instanceId)
	{
		spdlog::debug(__FUNCTION__ "(instance {})", instanceId);

		std::lock_guard<std::mutex> lock(mtx);

		auto it = std::find_if(controllers.begin(), controllers.end(), [instanceId](SDL_Gamepad* controller)
			{
				return SDL_GetGamepadID(controller) == instanceId;
			});

		if (it != controllers.end())
		{
			Game::CurrentPadType = Game::GamepadType::PC;

			SDL_CloseGamepad(*it);
			controllers.erase(it);

			spdlog::debug(__FUNCTION__ "(instance {}): removed instance", instanceId);

			if (primaryControllerIndex >= controllers.size())
				setPrimaryGamepad(controllers.empty() ? -1 : 0);
		}
	}

	static const char* joystickTypeName(SDL_JoystickType type)
	{
		switch (type)
		{
		case SDL_JOYSTICK_TYPE_GAMEPAD:      return "gamepad";
		case SDL_JOYSTICK_TYPE_WHEEL:        return "wheel";
		case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade stick";
		case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight stick";
		case SDL_JOYSTICK_TYPE_DANCE_PAD:    return "dance pad";
		case SDL_JOYSTICK_TYPE_GUITAR:       return "guitar";
		case SDL_JOYSTICK_TYPE_DRUM_KIT:     return "drum kit";
		case SDL_JOYSTICK_TYPE_ARCADE_PAD:   return "arcade pad";
		case SDL_JOYSTICK_TYPE_THROTTLE:     return "throttle";
		default:                             return "unknown";
		}
	}

	// Gamepads arrive as both a joystick and a gamepad event. Only the ones SDL
	// cannot map as a gamepad are taken here; the rest go through
	// onControllerAdded as before.
	void onJoystickAdded(SDL_JoystickID instanceId)
	{
		if (SDL_IsGamepad(instanceId))
			return;

		SDL_Joystick* joystick = SDL_OpenJoystick(instanceId);
		if (!joystick)
		{
			spdlog::error("InputManager: failed to open joystick {}: {}", instanceId, SDL_GetError());
			return;
		}

		std::lock_guard<std::mutex> lock(mtx);

		if (std::find(joysticks.begin(), joysticks.end(), joystick) != joysticks.end())
			return; // SDL reference-counts opens, so this one is already held

		joysticks.push_back(joystick);

		const char* name = SDL_GetJoystickName(joystick);
		spdlog::info("InputManager: joystick \"{}\" ({:04x}:{:04x}, {}): {} axes, {} buttons, {} hats",
			name ? name : "", SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick),
			joystickTypeName(SDL_GetJoystickType(joystick)), SDL_GetNumJoystickAxes(joystick),
			SDL_GetNumJoystickButtons(joystick), SDL_GetNumJoystickHats(joystick));
	}

	void onJoystickRemoved(SDL_JoystickID instanceId)
	{
		std::lock_guard<std::mutex> lock(mtx);

		auto it = std::find_if(joysticks.begin(), joysticks.end(), [instanceId](SDL_Joystick* joystick)
			{
				return SDL_GetJoystickID(joystick) == instanceId;
			});

		if (it == joysticks.end())
			return;

		spdlog::info("InputManager: joystick {:04x}:{:04x} removed",
			SDL_GetJoystickVendor(*it), SDL_GetJoystickProduct(*it));

		SDL_CloseJoystick(*it);
		joysticks.erase(it);
	}

public:
	~InputManager()
	{
		for (auto controller : controllers)
			SDL_CloseGamepad(controller);
		for (auto joystick : joysticks)
			SDL_CloseJoystick(joystick);
	}

	SDL_Joystick* findJoystick(uint32_t device)
	{
		for (auto* joystick : joysticks)
			if (!device || JoystickDeviceId(joystick) == device)
				return joystick;
		return nullptr;
	}

	std::string joystickLabel(uint32_t device)
	{
		if (auto* joystick = findJoystick(device))
			if (const char* name = SDL_GetJoystickName(joystick); name && *name)
				return name;

		if (!device)
			return "Joystick";
		return std::format("Joystick {:04x}:{:04x}", device >> 16, device & 0xFFFF);
	}

	const std::vector<SDL_Joystick*>& connectedJoysticks() const { return joysticks; }

	// A joystick axis counts as moved once it has gone a quarter of its full
	// travel from where it started: half a pedal stroke, or half the lock of a
	// wheel to one side.
	static constexpr int JoystickAxisMoved = 16384;

	// Records every joystick axis as it stands, for detectJoystickInput.
	void snapshotJoystickAxes()
	{
		captureBaseline.clear();
		for (auto* joystick : joysticks)
		{
			std::vector<Sint16> axes(std::max(SDL_GetNumJoystickAxes(joystick), 0));
			for (int i = 0; i < int(axes.size()); i++)
				axes[i] = SDL_GetJoystickAxis(joystick, i);
			captureBaseline.emplace_back(SDL_GetJoystickID(joystick), std::move(axes));
		}
	}

	// The first joystick input pressed or moved since snapshotJoystickAxes, as
	// a binding. An axis that was resting near one end of its range is taken to
	// be a pedal and gets a 0..1 binding from that end; anything else is a
	// centred axis whose positive direction is the way it was moved.
	std::optional<InputBinding> detectJoystickInput()
	{
		for (auto* joystick : joysticks)
		{
			const uint32_t device = JoystickDeviceId(joystick);

			for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); i++)
				if (SDL_GetJoystickButton(joystick, i))
					return InputBinding::JoyButton(device, i);

			for (int i = 0; i < SDL_GetNumJoystickHats(joystick); i++)
			{
				const Uint8 hat = SDL_GetJoystickHat(joystick, i);
				// A diagonal could mean either direction, so wait for a clean one.
				if (hat == SDL_HAT_UP || hat == SDL_HAT_DOWN || hat == SDL_HAT_LEFT || hat == SDL_HAT_RIGHT)
					return InputBinding::JoyHat(device, i, hat);
			}

			const std::vector<Sint16>* baseline = nullptr;
			for (const auto& [id, axes] : captureBaseline)
				if (id == SDL_GetJoystickID(joystick))
					baseline = &axes;
			if (!baseline)
				continue; // connected mid-capture; it gets a baseline next time

			for (int i = 0; i < int(baseline->size()); i++)
			{
				const int rest = (*baseline)[i];
				const int delta = int(SDL_GetJoystickAxis(joystick, i)) - rest;
				if (std::abs(delta) < JoystickAxisMoved)
					continue;

				if (rest <= -24576)
					return InputBinding::JoyAxis(device, i, InputBinding::AxisMode::Full);
				if (rest >= 24576)
					return InputBinding::JoyAxis(device, i, InputBinding::AxisMode::FullInverted);
				return InputBinding::JoyAxis(device, i, InputBinding::AxisMode::Centered, delta < 0);
			}
		}

		return std::nullopt;
	}

	// Whether any joystick input is held. Axes are measured against the last
	// capture's snapshot, the only rest position known for certain: a pedal
	// rests at the end of its range, so distance from centre says nothing.
	// Before any capture there is no snapshot and axes are not counted, which
	// is fine for the only use, waiting for whatever opened a prompt to be let
	// go of; after a capture it holds the next prompt until a pedal just bound
	// has come back up.
	bool anyJoystickInputPressed()
	{
		for (auto* joystick : joysticks)
		{
			for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); i++)
				if (SDL_GetJoystickButton(joystick, i))
					return true;

			for (int i = 0; i < SDL_GetNumJoystickHats(joystick); i++)
				if (SDL_GetJoystickHat(joystick, i) != SDL_HAT_CENTERED)
					return true;

			for (const auto& [id, axes] : captureBaseline)
			{
				if (id != SDL_GetJoystickID(joystick))
					continue;
				for (int i = 0; i < int(axes.size()); i++)
					if (std::abs(int(SDL_GetJoystickAxis(joystick, i)) - int(axes[i])) >= JoystickAxisMoved)
						return true;
			}
		}
		return false;
	}

	SDL_Gamepad* getPrimaryGamepad()
	{
		if (primaryControllerIndex < 0)
			return nullptr;
		if (primaryControllerIndex >= controllers.size())
			return nullptr;
		return controllers[primaryControllerIndex];
	}

	void setPrimaryGamepad(int index)
	{
		if (index < 0 || index >= controllers.size())
			primaryControllerIndex = -1;
		else
			primaryControllerIndex = index;

		spdlog::debug("InputManager::primaryControllerIndex = {}", primaryControllerIndex);

		if (auto* pad = getPrimaryGamepad())
			setupGamepad(pad);
	}

	void init(HWND hwnd);

	// An ini written before an action existed has no lines for it, so it loads
	// unbound. That is recoverable for every action except the overlay toggle:
	// without it there is no way to reach the UI that would rebind it, so it
	// always gets its default back.
	void ensureOverlayBindable()
	{
		InputAction& overlay = modBindings[size_t(ModAction::OverlayToggle)];
		if (!overlay.bindings().empty())
			return;

		spdlog::warn(__FUNCTION__ ": overlay toggle had no bindings, restoring F11");
		addModBinding(ModAction::OverlayToggle, SDL_SCANCODE_F11);
	}

	void setupDefaultBindings()
	{
		// Remove any previous bindings
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();
		for (auto& binding : modBindings)
			binding.clear();

		// Keyboard
		addVolumeBinding(ADChannel::Steering, SDL_SCANCODE_LEFT, true);
		addVolumeBinding(ADChannel::Steering, SDL_SCANCODE_RIGHT);
		addVolumeBinding(ADChannel::Acceleration, SDL_SCANCODE_UP);
		addVolumeBinding(ADChannel::Brake, SDL_SCANCODE_DOWN);

		addSwitchBinding(SwitchId::GearUp, SDL_SCANCODE_W);
		addSwitchBinding(SwitchId::GearDown, SDL_SCANCODE_D);

		addSwitchBinding(SwitchId::Start, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::Back, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::A, SDL_SCANCODE_RETURN);
		addSwitchBinding(SwitchId::B, SDL_SCANCODE_ESCAPE);
		addSwitchBinding(SwitchId::X, SDL_SCANCODE_E);
		addSwitchBinding(SwitchId::Y, SDL_SCANCODE_F);

		addSwitchBinding(SwitchId::ChangeView, SDL_SCANCODE_F);

		addSwitchBinding(SwitchId::SelectionUp, SDL_SCANCODE_UP);
		addSwitchBinding(SwitchId::SelectionDown, SDL_SCANCODE_DOWN);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_SCANCODE_LEFT);
		addSwitchBinding(SwitchId::SelectionRight, SDL_SCANCODE_RIGHT);

		addSwitchBinding(SwitchId::SignIn, SDL_SCANCODE_F1);
		addSwitchBinding(SwitchId::License, SDL_SCANCODE_F2);
		addSwitchBinding(SwitchId::X, SDL_SCANCODE_F1);
		addSwitchBinding(SwitchId::Y, SDL_SCANCODE_F2);

		// Gamepad
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_AXIS_LEFTX);
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
		addVolumeBinding(ADChannel::Steering, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
		addVolumeBinding(ADChannel::Acceleration, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		addVolumeBinding(ADChannel::Brake, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);

		addSwitchBinding(SwitchId::GearUp, SDL_GAMEPAD_AXIS_RIGHTY);
		addSwitchBinding(SwitchId::GearDown, SDL_GAMEPAD_AXIS_RIGHTY, true);
		addSwitchBinding(SwitchId::GearUp, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding(SwitchId::GearDown, SDL_GAMEPAD_BUTTON_B);

		addSwitchBinding(SwitchId::Start, SDL_GAMEPAD_BUTTON_START);
		addSwitchBinding(SwitchId::Back, SDL_GAMEPAD_BUTTON_BACK);
		addSwitchBinding(SwitchId::A, SDL_GAMEPAD_BUTTON_A);
		addSwitchBinding(SwitchId::B, SDL_GAMEPAD_BUTTON_B);
		addSwitchBinding(SwitchId::X, SDL_GAMEPAD_BUTTON_X);
		addSwitchBinding(SwitchId::Y, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding(SwitchId::ChangeView, SDL_GAMEPAD_BUTTON_Y);

		addSwitchBinding(SwitchId::SelectionUp, SDL_GAMEPAD_AXIS_LEFTY, true);
		addSwitchBinding(SwitchId::SelectionDown, SDL_GAMEPAD_AXIS_LEFTY, false);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_GAMEPAD_AXIS_LEFTX, true);
		addSwitchBinding(SwitchId::SelectionRight, SDL_GAMEPAD_AXIS_LEFTX, false);
		addSwitchBinding(SwitchId::SelectionUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
		addSwitchBinding(SwitchId::SelectionDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		addSwitchBinding(SwitchId::SelectionLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
		addSwitchBinding(SwitchId::SelectionRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

		// Some reason signin/license need both X/Y and SignIn/License bound, odd
		addSwitchBinding(SwitchId::SignIn, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding(SwitchId::License, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
		addSwitchBinding(SwitchId::X, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
		addSwitchBinding(SwitchId::Y, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);

		// Mod actions.
		addModBinding(ModAction::OverlayToggle, SDL_SCANCODE_F11);
		addModBinding(ModAction::OpenChat, SDL_SCANCODE_Y);
		addModBinding(ModAction::MusicNext, SDL_SCANCODE_X);
		addModBinding(ModAction::MusicNext, SDL_GAMEPAD_BUTTON_BACK);
		addModBinding(ModAction::MusicPrevious, SDL_SCANCODE_Z);
	}

	//
	// INI parsing
	//
	struct IniEntry
	{
		std::string section;
		std::string key;
		std::string value;
	};

	// text -> entries. Knows nothing about actions or bindings.
	static std::vector<IniEntry> parseIniLines(std::istream& in)
	{
		std::vector<IniEntry> entries;
		std::string section;
		std::string line;

		while (std::getline(in, line))
		{
			line = Util::trim(line);
			if (line.empty() || line.front() == '#' || line.front() == ';')
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				section = line.substr(1, line.size() - 2);
				continue;
			}

			auto delim = line.find('=');
			if (delim == std::string::npos)
				continue;

			entries.push_back({ section,
				Util::trim(line.substr(0, delim)),
				Util::trim(line.substr(delim + 1)) });
		}

		return entries;
	}

	struct ActionRef
	{
		using Kind = ActionKind;

		Kind kind = Kind::Switch;
		int index = -1;
		bool negate = false;
	};

	// "Steering-" -> { volume, 0, negated }
	static std::optional<ActionRef> parseActionName(std::string_view name)
	{
		ActionRef ref;

		if (!name.empty() && name.back() == '-')
		{
			ref.negate = true;
			name.remove_suffix(1);
		}

		const std::string trimmed(name);

		for (int i = 0; i < int(std::size(volumeNames)); i++)
			if (!stricmp(trimmed.c_str(), volumeNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Volume;
				ref.index = i;
				return ref;
			}

		for (int i = 0; i < int(SwitchId::Count); i++)
			if (!stricmp(trimmed.c_str(), switchNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Switch;
				ref.index = i;
				return ref;
			}

		for (int i = 0; i < int(ModAction::Count); i++)
			if (!stricmp(trimmed.c_str(), modNames[i].c_str()))
			{
				ref.kind = ActionRef::Kind::Mod;
				ref.index = i;
				return ref;
			}

		return std::nullopt;
	}

	// "LS-X" / "Return" -> InputBinding (negate applied by the caller)
	static std::optional<InputBinding> parseBindingValue(const std::string& value, bool keyboard)
	{
		if (keyboard)
		{
			SDL_Scancode scancode = SDL_GetScancodeFromName(value.c_str());
			if (scancode == SDL_SCANCODE_UNKNOWN)
				return std::nullopt;
			return InputBinding(scancode);
		}

		if (auto axis = InputNames::axisFromIni(value))
			return InputBinding(*axis);
		if (auto button = InputNames::buttonFromIni(value))
			return InputBinding(*button);

		return std::nullopt;
	}

	void addBinding(const ActionRef& ref, const InputBinding& binding)
	{
		switch (ref.kind)
		{
		case ActionRef::Kind::Volume: volumeBindings[ref.index].add(binding); break;
		case ActionRef::Kind::Mod:    modBindings[ref.index].add(binding); break;
		default:                      switchBindings[ref.index].add(binding); break;
		}
	}

	bool readBindingIni(const std::filesystem::path& iniPath)
	{
		if (!std::filesystem::exists(iniPath))
			return false;

		spdlog::info(__FUNCTION__ " - reading INI from {}", iniPath.string());

		std::ifstream file(iniPath);
		if (!file || !file.is_open())
		{
			spdlog::error(__FUNCTION__ " - failed to read INI, using defaults");
			return false;
		}

		const auto entries = parseIniLines(file);
		file.close();

		int key_binds = 0;
		int pad_binds = 0;
		int joy_binds = 0;
		for (const auto& entry : entries)
		{
			if (!stricmp(entry.section.c_str(), "Keyboard"))
				key_binds++;
			else if (!stricmp(entry.section.c_str(), "Gamepad"))
				pad_binds++;
			else if (!stricmp(entry.section.c_str(), "Joystick"))
				joy_binds++;
		}

		if (key_binds <= 0 && pad_binds <= 0 && joy_binds <= 0)
		{
			spdlog::error(__FUNCTION__ " - failed to read binds from INI, using defaults");
			return false;
		}

		spdlog::info(__FUNCTION__ " - {} key binds, {} pad binds, {} joystick binds", key_binds, pad_binds, joy_binds);

		// we have binds, reset any of our defaults
		for (auto& binding : volumeBindings)
			binding.clear();
		for (auto& binding : switchBindings)
			binding.clear();
		for (auto& binding : modBindings)
			binding.clear();

		for (const auto& entry : entries)
		{
			const bool keyboard = !stricmp(entry.section.c_str(), "Keyboard");
			const bool joystick = !stricmp(entry.section.c_str(), "Joystick");
			if (!keyboard && !joystick && stricmp(entry.section.c_str(), "Gamepad"))
				continue; // unknown section

			auto action = parseActionName(entry.key);
			if (!action)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding action for {} = {}", entry.key, entry.value);
				continue;
			}

			auto binding = joystick ? InputBinding::parseJoystickIni(entry.value)
				: parseBindingValue(entry.value, keyboard);
			if (!binding)
			{
				spdlog::error(__FUNCTION__ ": failed to parse binding for {} = {}", entry.key, entry.value);
				continue;
			}

			binding->negate = action->negate;
			addBinding(*action, *binding);
		}

		return true;
	}

	enum class BindingSection { Gamepad, Joystick, Keyboard };

	static BindingSection sectionOf(const InputBinding& binding)
	{
		if (binding.isKeyboard())
			return BindingSection::Keyboard;
		if (binding.isJoystick())
			return BindingSection::Joystick;
		return BindingSection::Gamepad;
	}

	// Writes every binding of one source kind for one section.
	void writeBindingSection(std::ostream& file, BindingSection section)
	{
		auto writeAction = [&](const std::string& name, const InputAction& action)
		{
			for (const auto& bind : action.bindings())
			{
				if (sectionOf(bind) != section)
					continue;

				const std::string direction = bind.isNegated() ? "-" : "";
				file << name << direction << " = " << bind.iniName() << "\n";
			}
		};

		for (int i = 0; i < int(std::size(volumeNames)); ++i)
			writeAction(volumeNames[i], volumeBindings[i]);

		for (int i = 0; i < int(SwitchId::Count); ++i)
			writeAction(switchNames[i], switchBindings[i]);

		for (int i = 0; i < int(ModAction::Count); ++i)
			writeAction(modNames[i], modBindings[i]);
	}

	bool saveBindingIni(const std::filesystem::path& iniPath)
	{
		std::ofstream file(iniPath, std::ios::out | std::ios::trunc);
		if (!file.is_open())
		{
			spdlog::error(__FUNCTION__ ": failed to open file for writing: {}", iniPath.string());
			return false;
		}

		file << "# These bindings are used when UseNewInput is enabled inside OutRun2006Tweaks.ini\n";
		file << "# With that enabled, you can use in-game Controls > Configuration dialog to change these during gameplay\n";
		file << "# (editing this file manually can allow more advanced config, such as binding multiple inputs to a single action)\n";
		file << "# If this file doesn't exist or is empty, bindings will be reset to default.\n";
		file << "#\n";
		file << "# Actions with a negative symbol after them ('Steering-') either treat the input as a negative value, or only trigger the action on negative inputs\n";
		file << "# Analog actions bound to digital inputs, eg. 'Steering- = DPad-Left', will make DPad-Left send a negative Steering value, making it move to the left\n";
		file << "# Digital actions bound to analog inputs, eg. 'Gear Down- = RS-Y', will only trigger the action when RS-Y is negative\n";
		file << "# Analog -> analog actions can also be inverted by adding a negative to them\n";
		file << "#\n";
		file << "# [Joystick] holds wheels, pedals and other devices SDL has no gamepad mapping for, named by USB vendor:product\n";
		file << "# ('*' matches any). 'Axis2' is a centred axis such as a wheel; 'Axis2 Full' reads 0..1 from the axis minimum\n";
		file << "# and 'Axis2 FullInv' from its maximum, for pedals. Buttons are 'Button4', D-pads are 'Hat0 Up' (Down/Left/Right).\n\n";

		file << "[Gamepad]\n";
		writeBindingSection(file, BindingSection::Gamepad);

		file << "\n[Joystick]\n";
		writeBindingSection(file, BindingSection::Joystick);

		file << "\n[Keyboard]\n";
		writeBindingSection(file, BindingSection::Keyboard);

		file.close();
		spdlog::info(__FUNCTION__": saved to INI file: {}", iniPath.string());

		return true;
	}

	void pumpSdlEvents()
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
			switch (event.type)
			{
			case SDL_EVENT_GAMEPAD_ADDED:
				onControllerAdded(event.gdevice.which);
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				onControllerRemoved(event.gdevice.which);
				break;
			case SDL_EVENT_JOYSTICK_ADDED:
				onJoystickAdded(event.jdevice.which);
				break;
			case SDL_EVENT_JOYSTICK_REMOVED:
				onJoystickRemoved(event.jdevice.which);
				break;
			case SDL_EVENT_QUIT:
				PostQuitMessage(0);
				break;
			}
	}

	// Reads every volume binding into the cache the game later queries.
	// Skipped entirely while the binding dialog is up, so a stick being waved
	// around to pick a bind doesn't steer the car.
	void updateVolumes(SDL_Gamepad* gamepad)
	{
		for (size_t i = 0; i < volumeBindings.size(); ++i)
		{
			auto& vol = volumeBindings[i].update(gamepad);
			if (Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]
				continue;

			volumes[i] = vol;

			// Steering runs through the game's own sensitivity curve so that
			// the in-game sensitivity setting still applies.
			if (i == 0 && !Settings::BypassGameSensitivity)
			{
				int cur = ceil(volumes[i].currentValue * 127.0f);
				int prev = ceil(volumes[i].previousValue * 127.0f);
				volumes[i].currentValue = Sumo_CalcSteerSensitivity_wrapper(cur, prev) / 127.0f;
				volumeBindings[i].setState(volumes[i]);
			}
		}
	}

	// Collapses the switch bindings into a bitmask, one bit per SwitchId.
	uint32_t readSwitchMask(SDL_Gamepad* gamepad)
	{
		uint32_t mask = 0;
		for (size_t i = 0; i < switchBindings.size(); ++i)
		{
			auto& switchState = switchBindings[i].update(gamepad);
			if (switchState.isPressed())
			{
				mask |= (1 << i);
				lastInputSource_ = switchState.lastSourceType;
			}
		}
		return mask;
	}

	void update()
	{
		pumpSdlEvents();

		auto* gamepad = getPrimaryGamepad();

		switch_previous = switch_current;

		// Whatever opened the binding dialog (or started a listen) is still
		// physically held down right now. Passing that press on would instantly
		// re-trigger whatever we just opened, so latch a suppression flag and
		// swallow input until the user has released everything.
		if (isListeningForInput == ListenState::Listening)
			suppressOverlayUntilRelease = true;
		if (Overlay::IsBindingDialogActive)
			suppressGameUntilRelease = true;

		updateVolumes(gamepad);
		switch_current = readSwitchMask(gamepad);

		// Everything released - safe to start passing input on again.
		if (switch_current == 0) [[likely]]
		{
			suppressOverlayUntilRelease = false;
			suppressGameUntilRelease = false;
		}

		// Two consumers, suppressed independently. The overlay is masked first,
		// so anything hidden from the overlay is hidden from the game as well.
		if (suppressOverlayUntilRelease) [[unlikely]]
			switch_current = 0;

		switch_overlay = switch_current;

		// The overlay being open used to stop update() running at all, which is
		// what kept the game from seeing input through it. Mod actions have to be
		// read while it is open, so it runs either way now and the game is held
		// off here instead.
		if (suppressGameUntilRelease || Overlay::IsBindingDialogActive || Overlay::IsActive) [[unlikely]]
			switch_current = 0;

		updateModActions(gamepad);

		updateRawDInputState();
	}

	// Mod actions take the overlay's suppression but not the game's, so the
	// overlay toggle can still close the overlay. The binding dialog owns every
	// input while it is up, so nothing fires under it. States keep updating
	// regardless, so a key held across the dialog closing reads as held rather
	// than newly pressed.
	void updateModActions(SDL_Gamepad* gamepad)
	{
		modActionsDeaf = suppressOverlayUntilRelease || Overlay::IsBindingDialogActive;

		for (size_t i = 0; i < modBindings.size(); ++i)
			modStates[i] = modBindings[i].update(gamepad);
	}

	// Rebuilds the raw DirectInput masks from the bindings. Called once per
	// update so that a held button produces a single press edge.
	void updateRawDInputState()
	{
		uint32_t buttons = 0;
		for (const auto& mapping : RawButtonMap)
			if (switch_current & (1u << int(mapping.switchId)))
				buttons |= mapping.rawBit;

		if (volumes[int(ADChannel::Acceleration)].currentValue >= RawTriggerThreshold)
			buttons |= RawTriggerRight;
		if (volumes[int(ADChannel::Brake)].currentValue >= RawTriggerThreshold)
			buttons |= RawTriggerLeft;

		raw_pressed = buttons & ~raw_buttons;
		raw_released = raw_buttons & ~buttons;
		raw_buttons = buttons;

		applyRawDInputState();
	}

	// Copies the cached masks into the game's state. Also called after ReadIO,
	// which rebuilds them from whatever device DInputUpdate polled. Copying
	// rather than recomputing means running twice in a frame cannot swallow a
	// press edge.
	void applyRawDInputState()
	{
		if (!Game::dinput_state)
			return;

		Game::dinput_state->buttons_4 = raw_buttons;
		Game::dinput_state->pressed_8 = raw_pressed;
		Game::dinput_state->released_C = raw_released;
	}

	// Table lookups by kind, for the bindings UI which walks all three.
	InputAction& actionFor(ActionKind kind, int index)
	{
		switch (kind)
		{
		case ActionKind::Volume: return volumeBindings[index];
		case ActionKind::Mod:    return modBindings[index];
		default:                 return switchBindings[index];
		}
	}

	static const std::string& actionName(ActionKind kind, int index)
	{
		switch (kind)
		{
		case ActionKind::Volume: return volumeNames[index];
		case ActionKind::Mod:    return modNames[index];
		default:                 return switchNames[index];
		}
	}

	// What to call a mod action's binding in a prompt. Prefers a keyboard one:
	// that is what a reader can press without a pad plugged in, and the pad
	// labels depend on which pad is connected.
	std::string modActionDisplayName(ModAction action)
	{
		const auto& bindings = modBindings[size_t(action)].bindings();
		if (bindings.empty())
			return "(unbound)";

		const InputBinding* pick = &bindings.front();
		for (const auto& binding : bindings)
			if (binding.isKeyboard())
			{
				pick = &binding;
				break;
			}

		auto padType = SDL_GAMEPAD_TYPE_XBOX360;
		if (auto* primary = getPrimaryGamepad())
			padType = SDL_GetGamepadType(primary);

		return pick->displayName(padType);
	}

	// Whether a mod action's binding is down right now. Deliberately not an edge:
	// this updates once per game tick while callers read on their own cadence,
	// which for the overlay is once per rendered frame, so an edge taken here
	// would be seen twice above tick rate and missed below it. Callers compare
	// against their own previous value instead, which is right at any rate.
	//
	// False while the binding dialog is up, so a key being bound never also fires
	// what it is bound to.
	bool modActionHeld(ModAction action) const
	{
		if (modActionsDeaf) [[unlikely]]
			return false;

		return modStates[size_t(action)].isPressed();
	}

	const InputAction& modAction(ModAction action) const { return modBindings[size_t(action)]; }
	InputAction& modAction(ModAction action) { return modBindings[size_t(action)]; }
	static const std::string& modActionName(ModAction action) { return modNames[size_t(action)]; }

	void setVibration(WORD left, WORD right)
	{
		auto* controller = getPrimaryGamepad();
		if (!controller)
			return;

		std::lock_guard<std::mutex> lock(mtx);
		SDL_RumbleGamepad(controller, left, right, 1000);

		// TODO: SDL_RumbleGamepadTriggers doesn't appear to work with any backend?
		// Disabling this code for now, we'll rely on the old ImpulseVibration / DetourDeviceIoControl method instead.
#if 0
		if (Settings::ImpulseVibrationMode != 0)
		{
			int impulseLeft = float(left);
			int impulseRight = float(right);

			if (Settings::ImpulseVibrationMode == 2) // Swap L/R
			{
				impulseLeft = float(right);
				impulseRight = float(left);
			}
			else if (Settings::ImpulseVibrationMode == 3) // Merge L/R by using whichever is highest
			{
				impulseLeft = impulseRight = max(left, right);
			}
			impulseLeft = impulseLeft * Settings::ImpulseVibrationLeftMultiplier;
			impulseRight = impulseRight * Settings::ImpulseVibrationRightMultiplier;
			SDL_RumbleGamepadTriggers(controller, Uint16(ceil(impulseLeft)), Uint16(ceil(impulseRight)), 1000);
		}
#endif
	}

	// Add sources to bindings
	template <typename... Args>
	void addVolumeBinding(ADChannel id, Args&&... args)
	{
		volumeBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
	}

	template <typename... Args>
	void addSwitchBinding(SwitchId id, Args&&... args)
	{
		switchBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
	}
	template <typename... Args>
	void addModBinding(ModAction id, Args&&... args)
	{
		modBindings[int(id)].add(InputBinding(std::forward<Args>(args)...));
	}

	bool anyInputPressed()
	{
		int key_count = 0;
		const bool* key_state = SDL_GetKeyboardState(&key_count);
		for (int i = 0; i < key_count; i++)
			if (key_state[i])
				return true;

		if (anyJoystickInputPressed())
			return true;

		auto* controller = getPrimaryGamepad();
		if (!controller)
			return false;

		// Check all possible buttons
		for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
			if (SDL_GetGamepadButton(controller, static_cast<SDL_GamepadButton>(i)))
				return true;

		// Check all possible axes
		for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
		{
			float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
			if (std::abs(value) > 0.5f)
				return true;
		}

		return false;
	}

	InputSourceType lastInputSource() { return lastInputSource_; }

	//
	// Handlers for games original input functions
	//
	int GetVolume(ADChannel volumeId)
	{
		const auto& state = volumes[int(volumeId)];
		if (volumeId == ADChannel::Steering)
			return int(ceil(state.currentValue * 127.0f));

		return int(ceil(state.currentValue * 255.0f));
	}

	int GetVolumeOld(ADChannel volumeId)
	{
		const auto& state = volumes[int(volumeId)];
		if (volumeId == ADChannel::Steering)
			return int(ceil(state.previousValue * 127.0f));

		return int(ceil(state.previousValue * 255.0f));
	}

	bool SwitchOn(uint32_t switches)
	{
		return (switch_previous & switches) != switches && (switch_current & switches) == switches;
	}

	bool SwitchNow(uint32_t switches)
	{
		return (switch_current & switches) == switches;
	}

	friend class InputBindingsUI;

	static InputManager instance;
};

constexpr uint32_t StartSwitchMask = 1 << int(SwitchId::Start);

void InputManager_Update();
bool InputManager_ModActionHeld(ModAction action);
std::string InputManager_ModActionDisplayName(ModAction action);
void InputManager_SetVibration(WORD left, WORD right);
