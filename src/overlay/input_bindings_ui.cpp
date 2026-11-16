#include "input_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

//
// Binding editor.
//
// An action holds a list of bindings and fires on whichever of them reads
// highest, so several inputs per action can be assigned.
// This screen displays each action on the left, and selecting one will
// display and allow changing the bindings for it on the right.
//
class InputBindingsUI : public OverlayWindow
{
public:
	// A modal that in-game code can raise on its own, so it has to be drawn
	// whether or not the overlay is open.
	Kind kind() const override { return Kind::Hud; }
	const char* name() const override { return "Input Bindings"; }

private:
	// One action, either an analog channel or a digital switch. Held by value
	// rather than by pointer because the prompt outlives a frame, and
	// setupDefaultBindings()/readBindingIni() replace the binding vectors.
	struct Selection
	{
		using Kind = InputManager::ActionKind;

		Kind kind = Kind::Volume;
		int index = 0;

		bool operator==(const Selection& other) const
		{
			return kind == other.kind && index == other.index;
		}

		bool isVolume() const { return kind == Kind::Volume; }
	};

	// The order actions are listed in, grouped by when they get used.
	struct ActionListEntry
	{
		const char* group;
		Selection::Kind kind;
		int index;
	};

	static constexpr auto Vol = Selection::Kind::Volume;
	static constexpr auto Sw = Selection::Kind::Switch;
	static constexpr auto Mod = Selection::Kind::Mod;

	static inline const ActionListEntry ActionList[] = {
		{ "Driving", Vol, int(ADChannel::Steering)        },
		{ "Driving", Vol, int(ADChannel::Acceleration)    },
		{ "Driving", Vol, int(ADChannel::Brake)           },
		{ "Driving", Sw,  int(SwitchId::GearUp)           },
		{ "Driving", Sw,  int(SwitchId::GearDown)         },
		{ "Driving", Sw,  int(SwitchId::ChangeView)       },

		{ "Menus",   Sw,  int(SwitchId::Start)            },
		{ "Menus",   Sw,  int(SwitchId::Back)             },
		{ "Menus",   Sw,  int(SwitchId::A)                },
		{ "Menus",   Sw,  int(SwitchId::B)                },
		{ "Menus",   Sw,  int(SwitchId::X)                },
		{ "Menus",   Sw,  int(SwitchId::Y)                },
		{ "Menus",   Sw,  int(SwitchId::SelectionUp)      },
		{ "Menus",   Sw,  int(SwitchId::SelectionDown)    },
		{ "Menus",   Sw,  int(SwitchId::SelectionLeft)    },
		{ "Menus",   Sw,  int(SwitchId::SelectionRight)   },

		{ "Online",  Sw,  int(SwitchId::License)          },
		{ "Online",  Sw,  int(SwitchId::SignIn)           },

		{ "Tweaks",  Mod, int(ModAction::OverlayToggle)   },
		{ "Tweaks",  Mod, int(ModAction::HudToggle)       },
		{ "Tweaks",  Mod, int(ModAction::OpenChat)        },
		{ "Tweaks",  Mod, int(ModAction::MusicNext)       },
		{ "Tweaks",  Mod, int(ModAction::MusicPrevious)   },
	};

	// How far an analog action has to move from rest before its name lights up.
	// Digital actions use the game's own threshold instead, via
	// InputState::isPressed.
	static constexpr float ActiveThreshold = 0.25f;

	Selection selected{ Selection::Kind::Volume, int(ADChannel::Steering) };

	// What the listening prompt will write into. -1 appends a binding instead of
	// replacing one.
	Selection bindTarget;
	int bindIndex = -1;
	std::string bindingName;

	// Wheel setup: a run of prompts, one per action, that replaces each
	// action's joystick bindings with whatever is pressed. Empty when not
	// running.
	static inline const Selection WheelSetupSteps[] = {
		{ Vol, int(ADChannel::Steering)      },
		{ Vol, int(ADChannel::Acceleration)  },
		{ Vol, int(ADChannel::Brake)         },
		{ Sw,  int(SwitchId::GearUp)         },
		{ Sw,  int(SwitchId::GearDown)       },
		{ Sw,  int(SwitchId::Start)          },
		{ Sw,  int(SwitchId::A)              },
		{ Sw,  int(SwitchId::B)              },
		{ Sw,  int(SwitchId::SelectionUp)    },
		{ Sw,  int(SwitchId::SelectionDown)  },
		{ Sw,  int(SwitchId::SelectionLeft)  },
		{ Sw,  int(SwitchId::SelectionRight) },
		{ Sw,  int(SwitchId::Y)              },
	};
	int wheelSetupStep = -1;

	bool wheelSetupRunning() const { return wheelSetupStep >= 0; }

	void wheel_setup_next()
	{
		wheelSetupStep++;
		if (wheelSetupStep >= int(std::size(WheelSetupSteps)))
		{
			wheelSetupStep = -1;
			isListeningForInput = ListenState::False;
			return;
		}

		const Selection& step = WheelSetupSteps[wheelSetupStep];
		selected = step;
		begin_listening(step, -1);
	}

	// Track binding changes (options tab are handled differently)
	bool unsavedChanges = false;
	bool confirmingReset = false;

	std::vector<Settings::SettingBase*> pendingSettings;

	static InputAction& action_for(const Selection& selection)
	{
		return InputManager::instance.actionFor(selection.kind, selection.index);
	}

	static const std::string& name_for(const Selection& selection)
	{
		return InputManager::actionName(selection.kind, selection.index);
	}

	// Steering reads as a signed axis, and its bindings are named differently
	// because of it (left/right rather than negated/not).
	static bool is_steering(const Selection& selection)
	{
		return selection.isVolume() && selection.index == int(ADChannel::Steering);
	}

	void begin_listening(const Selection& target, int index)
	{
		// Waits for the click that got here to be let go of first, otherwise it
		// binds the mouse button or whatever key triggered it.
		isListeningForInput = ListenState::WaitForButtonRelease;
		bindTarget = target;
		bindIndex = index;
		bindingName = name_for(target);
	}

public:
	void init() override {}

	//
	// Listens for any input at all rather than being told up front whether to
	// expect a key or a pad: the binding records which it was. Escape cancels
	// and Delete clears, in every case, so there is no separate keyboard timeout
	// and the two halves behave the same.
	//
	bool HandleNewBinding()
	{
		InputAction& action = action_for(bindTarget);
		auto& bindings = action.bindings();

		if (wheelSetupRunning())
		{
			// Escape skips one step, Delete abandons the rest.
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				ImGui::CloseCurrentPopup();
				wheel_setup_next();
				return false;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
			{
				wheelSetupStep = -1;
				isListeningForInput = ListenState::False;
				ImGui::CloseCurrentPopup();
				return false;
			}
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			isListeningForInput = ListenState::False;
			ImGui::CloseCurrentPopup();
			return false;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
		{
			const bool removed = bindIndex >= 0 && bindIndex < int(bindings.size());
			if (removed)
				bindings.erase(bindings.begin() + bindIndex);

			isListeningForInput = ListenState::False;
			ImGui::CloseCurrentPopup();
			return removed;
		}

		// Replaces the binding that was clicked, or appends when the add button
		// was. Appending is the whole point of the list, so nothing here clears
		// what is already bound.
		const auto commit = [&](const InputBinding& binding)
		{
			// The wheel setup replaces what the wheel had for this action, so
			// running it twice doesn't leave two of everything.
			if (wheelSetupRunning())
				std::erase_if(bindings, [](const InputBinding& b) { return b.isJoystick(); });

			if (bindIndex >= 0 && bindIndex < int(bindings.size()))
				bindings[bindIndex] = binding;
			else
				action.add(binding);

			isListeningForInput = ListenState::WaitForBindButtonRelease;
			ImGui::CloseCurrentPopup();
		};

		// Wheels, pedals and anything else SDL has no gamepad mapping for. Checked
		// first during the wheel setup, so a key held on the side can't take a
		// step meant for the wheel.
		if (wheelSetupRunning())
			if (auto binding = InputManager::instance.detectJoystickInput())
			{
				commit(*binding);
				return true;
			}

		// Keyboard
		{
			int keyCount = 0;
			const bool* keyState = SDL_GetKeyboardState(&keyCount);
			for (int i = 0; i < keyCount; i++)
			{
				// Reserved above, so they can't be bound to anything.
				if (i == SDL_SCANCODE_ESCAPE || i == SDL_SCANCODE_DELETE || i == SDL_SCANCODE_BACKSPACE)
					continue;

				if (keyState[i])
				{
					commit(InputBinding(static_cast<SDL_Scancode>(i)));
					return true;
				}
			}
		}

		// Controller
		if (auto* controller = InputManager::instance.getPrimaryGamepad())
		{
			for (int i = SDL_GAMEPAD_BUTTON_SOUTH; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
			{
				if (SDL_GetGamepadButton(controller, static_cast<SDL_GamepadButton>(i)))
				{
					commit(InputBinding(static_cast<SDL_GamepadButton>(i)));
					return true;
				}
			}

			for (int i = SDL_GAMEPAD_AXIS_LEFTX; i < SDL_GAMEPAD_AXIS_COUNT; i++)
			{
				const float value = SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(i)) / 32768.0f;
				if (std::abs(value) > 0.5f)
				{
					// The direction it was pushed becomes the binding's, which
					// the invert toggle can flip afterwards.
					commit(InputBinding(static_cast<SDL_GamepadAxis>(i), value < 0));
					return true;
				}
			}
		}

		if (auto binding = InputManager::instance.detectJoystickInput())
		{
			commit(*binding);
			return true;
		}

		return false;
	}

private:
	// Left pane: every action, grouped, with the ones currently reading input
	// picked out. Watching a name light up is how a binding gets verified
	// without leaving the screen.
	void draw_action_list()
	{
		const ImVec4 activeColour = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];

		const char* group = nullptr;
		for (const ActionListEntry& entry : ActionList)
		{
			if (!group || std::strcmp(group, entry.group) != 0)
			{
				group = entry.group;
				ImGui::SeparatorText(group);
			}

			const Selection action{ entry.kind, entry.index };
			const InputState& state = action_for(action).getState();

			// A binding's value keeps its sign, and negating a binding flips it,
			// so an axis bound to two opposing digital actions gives one of them
			// a positive value and the other a negative one. Only the positive
			// side fires, which is what isPressed tests for - taking the
			// magnitude instead would light up both ends of the same stick.
			// An analog action uses the sign for direction rather than for on
			// and off, so there either end of the range counts as movement.
			const bool active = action.isVolume()
				? std::abs(state.currentValue) >= ActiveThreshold
				: state.isPressed();

			if (active)
				ImGui::PushStyleColor(ImGuiCol_Text, activeColour);

			if (ImGui::Selectable(name_for(action).c_str(), selected == action))
				selected = action;

			if (active)
				ImGui::PopStyleColor();
		}
	}

	// Right pane: the selected action's bindings, one row each.
	void draw_binding_editor(SDL_GamepadType padType)
	{
		InputAction& action = action_for(selected);
		auto& bindings = action.bindings();
		const bool steering = is_steering(selected);

		ImGui::SeparatorText(name_for(selected).c_str());

		int removeIndex = -1;

		if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##invert", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed);

			for (int i = 0; i < int(bindings.size()); i++)
			{
				InputBinding& binding = bindings[i];

				ImGui::PushID(i);
				ImGui::TableNextRow();

				// The binding's own name is the rebind button, so there is no
				// separate control for the most common thing to want.
				ImGui::TableNextColumn();
				const std::string label = std::format("{}  ({})",
					binding.displayName(padType, steering),
					binding.isKeyboard() ? "keyboard" : binding.isJoystick() ? "wheel/joystick" : "controller");

				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)))
					begin_listening(selected, i);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Rebind this input");

				// Inverting is the only way to reach the '-' suffix the INI
				// format has always had: it sends an analog action the opposite
				// direction, and makes a digital action fire on negative input.
				ImGui::TableNextColumn();
				if (ImGui::Checkbox("##invert", &binding.negate))
					unsavedChanges = true;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(steering
						? "Steer the other way with this input"
						: "Invert this input");

				ImGui::TableNextColumn();
				if (ImGui::Button("X"))
					removeIndex = i;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Remove this binding");

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		if (removeIndex >= 0)
		{
			bindings.erase(bindings.begin() + removeIndex);
			unsavedChanges = true;
		}

		if (bindings.empty())
			ImGui::TextDisabled("Nothing bound.");

		if (ImGui::Button("+ Add binding"))
			begin_listening(selected, -1);

		// Live value of the selected action.
		const float value = action.getState().currentValue;
		const float filled = selected.isVolume() ? std::abs(value) : value;

		ImGui::Spacing();
		ImGui::TextDisabled("Reading");
		ImGui::ProgressBar(std::clamp(filled, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0),
			std::format("{:.2f}", value).c_str());
	}

	void draw_controllers()
	{
		auto& manager = InputManager::instance;

		ImGui::SeparatorText("Gamepads");

		if (manager.controllers.empty())
			ImGui::TextDisabled("No gamepads detected.");

		for (size_t i = 0; i < manager.controllers.size(); i++)
		{
			auto* controller = manager.controllers[i];
			const bool primary = int(i) == manager.primaryControllerIndex;

			ImGui::PushID(int(i));
			if (ImGui::RadioButton(SDL_GetGamepadName(controller), primary))
				manager.setPrimaryGamepad(i);
			ImGui::PopID();
		}

		if (!manager.controllers.empty())
			ImGui::TextDisabled("Gamepad bindings apply to whichever gamepad is selected.");

		draw_joysticks();
	}

	// Every wheel/joystick with what it reads right now, so it is possible to
	// see which axis is which before binding anything.
	void draw_joysticks()
	{
		auto& manager = InputManager::instance;
		const auto& joysticks = manager.connectedJoysticks();

		ImGui::SeparatorText("Wheels and joysticks");

		if (joysticks.empty())
		{
			ImGui::TextDisabled("None detected. If a wheel is plugged in, try another InputBackend.");
			return;
		}

		if (ImGui::Button("Set up wheel..."))
		{
			wheelSetupStep = -1;
			wheel_setup_next();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Asks for steering, pedals, gears and menu buttons in turn.\n"
				"Replaces the wheel bindings of each action it asks about; keyboard and gamepad bindings stay.");

		for (size_t j = 0; j < joysticks.size(); j++)
		{
			SDL_Joystick* joystick = joysticks[j];
			const char* name = SDL_GetJoystickName(joystick);

			ImGui::PushID(int(j));
			const std::string header = std::format("{} ({:04x}:{:04x})###joy",
				name ? name : "Joystick", SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick));

			if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			{
				const int axes = SDL_GetNumJoystickAxes(joystick);
				for (int i = 0; i < axes; i++)
				{
					const Sint16 raw = SDL_GetJoystickAxis(joystick, i);
					ImGui::ProgressBar((float(raw) + 32768.f) / 65535.f, ImVec2(-FLT_MIN, 0),
						std::format("Axis {}: {}", i, raw).c_str());
				}

				std::string held;
				const int buttons = SDL_GetNumJoystickButtons(joystick);
				for (int i = 0; i < buttons; i++)
					if (SDL_GetJoystickButton(joystick, i))
						held += std::format("{}{}", held.empty() ? "" : ", ", i);
				ImGui::Text("Buttons held: %s", held.empty() ? "none" : held.c_str());

				const int hats = SDL_GetNumJoystickHats(joystick);
				for (int i = 0; i < hats; i++)
				{
					const Uint8 hat = SDL_GetJoystickHat(joystick, i);
					std::string dirs;
					for (Uint8 dir : { Uint8(SDL_HAT_UP), Uint8(SDL_HAT_DOWN), Uint8(SDL_HAT_LEFT), Uint8(SDL_HAT_RIGHT) })
						if (hat & dir)
							dirs += std::format("{}{}", dirs.empty() ? "" : "+", InputBinding::hatDirName(dir));
					ImGui::Text("D-Pad %d: %s", i, dirs.empty() ? "centred" : dirs.c_str());
				}

				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	// These are tweaks settings rather than bindings, so they go to the tweaks INI
	// the moment they are changed, the way the settings window writes them. Save
	// and its unsaved marker stay about bindings alone.
	void setting_changed(Settings::SettingBase& setting)
	{
		if (std::find(pendingSettings.begin(), pendingSettings.end(), &setting) == pendingSettings.end())
			pendingSettings.emplace_back(&setting);
	}

	// Held back until the control being dragged is let go of, so a slider doesn't
	// write the INI on every frame of the drag.
	void flush_settings()
	{
		if (pendingSettings.empty() || ImGui::IsAnyItemActive())
			return;

		for (Settings::SettingBase* setting : pendingSettings)
			setting->notify();
		pendingSettings.clear();

		Settings::write(Module::UserIniPath);
	}

	void draw_options()
	{
		auto& manager = InputManager::instance;

		const char* vibrationModes[] = { "Disabled", "Enabled", "Swap L/R", "Merge L/R" };
		if (ImGui::Combo("Vibration Mode", Settings::VibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::VibrationMode);
		if (ImGui::SliderInt("Vibration Strength", Settings::VibrationStrength.ptr(), 0, 10))
			setting_changed(Settings::VibrationStrength);
		if (ImGui::Combo("Impulse Vibration", Settings::ImpulseVibrationMode.ptr(), vibrationModes, IM_ARRAYSIZE(vibrationModes)))
			setting_changed(Settings::ImpulseVibrationMode);

		int deadzonePercent = int(Settings::SteeringDeadZone * 100.f);
		if (ImGui::SliderInt("Steering Deadzone", &deadzonePercent, 5, 20, "%d%%"))
		{
			Settings::SteeringDeadZone = float(deadzonePercent) / 100.f;
			setting_changed(Settings::SteeringDeadZone);
		}

		if (ImGui::Checkbox("Bypass Sensitivity", Settings::BypassGameSensitivity.ptr()))
			setting_changed(Settings::BypassGameSensitivity);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Passes steering input to the game directly, allows for more sensitive controls.\n"
				"Only used when UseNewInput is enabled.");
	}

	// The prompt shown while an input is being waited on.
	void draw_listening_popup()
	{
		if (isListeningForInput == ListenState::False)
			return;

		auto& manager = InputManager::instance;

		if (isListeningForInput == ListenState::WaitForButtonRelease)
		{
			if (!manager.anyInputPressed())
			{
				// Taken once everything is let go, so it records the wheel
				// centred and the pedals up.
				manager.snapshotJoystickAxes();
				isListeningForInput = ListenState::Listening;
			}
			return;
		}

		if (isListeningForInput == ListenState::WaitForBindButtonRelease)
		{
			if (!manager.anyInputPressed())
			{
				if (wheelSetupRunning())
					wheel_setup_next();
				else
					isListeningForInput = ListenState::False;
			}
			return;
		}

		ImGui::OpenPopup("Listening for Input");
		if (ImGui::BeginPopupModal("Listening for Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (wheelSetupRunning())
				ImGui::TextDisabled("Wheel setup, step %d of %d", wheelSetupStep + 1, int(std::size(WheelSetupSteps)));

			if (is_steering(bindTarget))
				ImGui::Text("Turn the wheel (or push the stick) to the right to bind %s", bindingName.c_str());
			else if (bindTarget.isVolume())
				ImGui::Text("Press the pedal (or input) to bind to %s", bindingName.c_str());
			else
				ImGui::Text("Press any input to bind to %s", bindingName.c_str());
			ImGui::Spacing();
			if (wheelSetupRunning())
				ImGui::TextDisabled("Escape to skip this one, Delete to stop the setup");
			else
				ImGui::TextDisabled("Escape to cancel, Delete to clear");

			if (HandleNewBinding())
				unsavedChanges = true;

			ImGui::EndPopup();
		}
	}

public:
	void render(bool overlayEnabled) override
	{
		if (!Overlay::IsBindingDialogActive)
			return;

		auto& manager = InputManager::instance;

		auto padType = SDL_GAMEPAD_TYPE_XBOX360;
		if (auto* primary = manager.getPrimaryGamepad())
			padType = SDL_GetGamepadType(primary);

		bool dialogOpen = true;

		// Sized from the font rather than from the screen, so it tracks what is
		// in it instead of how large the monitor is. The action list is the tall
		// part and scrolls; nothing else grows.
		const Overlay::ContentRect content = Overlay::content_rect();
		const float fontSize = ImGui::GetFontSize();

		float width = fontSize * 27.0f;
		float height = fontSize * 26.0f;

		if (width > content.width * 0.9f)
			width = content.width * 0.9f;
		if (height > content.height * 0.9f)
			height = content.height * 0.9f;

		ImGui::SetNextWindowPos(ImVec2(content.x + (content.width * 0.5f), content.y + (content.height * 0.5f)),
			ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

		ImGui::OpenPopup("Input Bindings");
		if (ImGui::BeginPopupModal("Input Bindings", &dialogOpen, ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove))
		{
			if (ImGui::Button(unsavedChanges ? "Save bindings*##save" : "Save bindings##save"))
				if (manager.saveBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			ImGui::SameLine();

			if (ImGui::Button("Load bindings"))
				if (manager.readBindingIni(Module::BindingsIniPath))
					unsavedChanges = false;

			// Two lines are reserved below: the unsaved note and the button row.
			const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();

			ImGui::BeginChild("##body", ImVec2(0, -footerHeight));
			if (ImGui::BeginTabBar("##sections"))
			{
				if (ImGui::BeginTabItem("Bindings"))
				{
					// The action list is the only part that can outgrow the
					// dialog, so it is the only part that scrolls.
					if (ImGui::BeginChild("##actions", ImVec2(fontSize * 9.0f, 0), ImGuiChildFlags_Borders))
						draw_action_list();
					ImGui::EndChild();

					ImGui::SameLine();

					if (ImGui::BeginChild("##editor", ImVec2(0, 0)))
						draw_binding_editor(padType);
					ImGui::EndChild();

					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Controllers"))
				{
					draw_controllers();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Options"))
				{
					draw_options();
					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}
			ImGui::EndChild();

			// Kept on its own line whether or not there are changes, so the
			// buttons below don't shift as it appears.
			ImGui::TextDisabled("%s", unsavedChanges ? "Note: you have unsaved changes!" : "");

			if (ImGui::Button("Return to game"))
				dialogOpen = false;

			ImGui::SameLine();

			if (ImGui::Button(!confirmingReset ? "Reset to default##clear" : "Are you sure?##clear"))
			{
				if (!confirmingReset)
				{
					confirmingReset = true;
				}
				else
				{
					unsavedChanges = true;
					Settings::SteeringDeadZone = 0.2f;
					setting_changed(Settings::SteeringDeadZone);
					Settings::BypassGameSensitivity = false;
					setting_changed(Settings::BypassGameSensitivity);
					manager.setupDefaultBindings();
					if (auto* controller = manager.getPrimaryGamepad())
						manager.setupGamepad(controller);
					confirmingReset = false;
				}
			}

			// Back/B leaves the dialog, but only while nothing is being bound -
			// otherwise the press meant for a binding closes the screen instead.
			if (isListeningForInput == ListenState::False)
			{
				if ((manager.switch_overlay & (1 << int(SwitchId::Back) | 1 << int(SwitchId::B))) != 0)
					dialogOpen = false;
			}

			draw_listening_popup();

			ImGui::EndPopup();
		}

		// Outside the popup so a change still reaches the INI on the frame the
		// dialog is closed.
		flush_settings();

		if (!dialogOpen)
		{
			wheelSetupStep = -1;
			isListeningForInput = ListenState::False;
			Overlay::IsBindingDialogActive = false;
			Overlay::RequestMouseHide = true;
			confirmingReset = false;
		}
	}

	static InputBindingsUI instance;
};
InputBindingsUI InputBindingsUI::instance;
