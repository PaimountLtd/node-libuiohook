/******************************************************************************
    Copyright (C) 2016-2020 by Streamlabs (General Workings Inc)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include "hook.h"

#include <thread>
#include <mutex>
#include <iostream>
#include <sstream>
#include <inttypes.h>
#include <vector>
#include <map>
#include <windows.h>

class Worker: public Napi::AsyncWorker	
{	
    public:	
    Worker(Napi::Function& callback) : AsyncWorker(callback){};	
    virtual ~Worker() {};	

    void Execute() {	
    };	
    void OnOK() {	
        Callback().Call( {} );	
    };	
};

typedef int16_t key_t;

struct HotKey {
	std::vector<std::pair<key_t, bool>> keys;
	std::unique_ptr<Worker> cbDown, cbUp;
	bool wasDown = false;

	static uint32_t Stringify(std::vector<std::pair<key_t, bool>> keys) {
		std::ostringstream os;
		for (size_t idx=0; idx < keys.size(); idx++) {
			os << keys[idx].first << ":" << keys[idx].second << "|";
		}
		std::string outString = os.str();
		return std::hash<std::string>{}(outString);
	};
};

struct ThreadData {
	std::mutex mtx;
	std::thread worker;
	std::map<uint32_t, HotKey> hotkeys;

	bool shutdown = false;
} gThreadData;

static bool isKeyDown(key_t k) {
	return (bool)(GetAsyncKeyState(k) >> 15);
}

static int32_t HotKeyThread(void* arg) {
	ThreadData* td = static_cast<ThreadData*>(arg);

	// Temporarily prevent execution until main is ready.
	{
		std::unique_lock<std::mutex> ulock(td->mtx);
	}

	while (!td->shutdown) {
		// Test each hotkey
		{
			std::unique_lock<std::mutex> ulock(td->mtx);
			for (auto& hk : td->hotkeys) {
				bool allPressed = true;

				for (std::pair<key_t, bool> k : hk.second.keys) {
					bool isBound = k.second;
					bool isPressed = isKeyDown(k.first);

					if (isBound && !isPressed) {
						allPressed = false;
					} else if (!isBound && isPressed) {
						if (!hk.second.wasDown) {
							allPressed = false;
						}
					}
				}

				if (allPressed && !hk.second.wasDown) {
					if (hk.second.cbDown != nullptr)
						hk.second.cbDown->Queue();

					hk.second.wasDown = true;
				} else if (!allPressed && hk.second.wasDown) {
					if (hk.second.cbUp != nullptr)
						hk.second.cbUp->Queue();

					hk.second.wasDown = false;
				}
			}
		}

		// Sleep 1ms (at most). Actual time varies, no hardware or scheduler is perfect.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return 0;
}

template < class ContainerT >
void tokenize(const std::string& str, ContainerT& tokens,
	const std::string& delimiters = " ", bool trimEmpty = false) {
	std::string::size_type pos, lastPos = 0, length = str.length();

	using value_type = typename ContainerT::value_type;
	using size_type = typename ContainerT::size_type;

	while (lastPos < length + 1) {
		pos = str.find_first_of(delimiters, lastPos);
		if (pos == std::string::npos) {
			pos = length;
		}

		if (pos != lastPos || !trimEmpty)
			tokens.push_back(value_type(str.data() + lastPos,
			(size_type)pos - lastPos));

		lastPos = pos + 1;
	}
}

Napi::Value StartHotkeyThreadJS(const Napi::CallbackInfo& info) {
	if (gThreadData.worker.joinable())
		return Napi::Boolean::New(info.Env(), false);

	gThreadData.mtx.lock();
	gThreadData.worker = std::thread(HotKeyThread, &gThreadData);
	gThreadData.mtx.unlock();

	return Napi::Boolean::New(info.Env(), true);
}

Napi::Value StopHotkeyThreadJS(const Napi::CallbackInfo& info) {
	if (!gThreadData.worker.joinable())
		return Napi::Boolean::New(info.Env(), false);

	gThreadData.shutdown = true;
	gThreadData.worker.join();

	return Napi::Boolean::New(info.Env(), true);
}

std::vector<std::pair<key_t, bool>> StringToKeys(std::string keystr, Napi::Object modifiers) {
	static std::map<std::string, key_t> g_KeyMap = {
	#ifdef _WIN32
		// Mouse
		std::make_pair("LeftMouseButton", VK_LBUTTON), std::make_pair("RightMouseButton", VK_RBUTTON),
		std::make_pair("MiddleMouseButton", VK_MBUTTON),
		std::make_pair("X1MouseButton", VK_XBUTTON1), std::make_pair("X2MouseButton", VK_XBUTTON2),
		// Keyboard
		std::make_pair("Backspace", VK_BACK),
		std::make_pair("Tab", VK_TAB),
		std::make_pair("Clear", VK_CLEAR),
		std::make_pair("Enter", VK_RETURN),
		std::make_pair("Shift", VK_SHIFT), std::make_pair("ShiftLeft", VK_LSHIFT), std::make_pair("ShiftRight", VK_RSHIFT),
		std::make_pair("Control", VK_CONTROL), std::make_pair("ControlLeft", VK_LCONTROL), std::make_pair("ControlRight", VK_RCONTROL),
		std::make_pair("Command", VK_CONTROL), std::make_pair("LeftCommand", VK_LCONTROL), std::make_pair("RightCommand", VK_RCONTROL),
		std::make_pair("CommandOrControl", VK_CONTROL), std::make_pair("LeftCommandOrControl", VK_LCONTROL), std::make_pair("RightCommandOrControl", VK_RCONTROL),
		std::make_pair("Alt", VK_MENU), std::make_pair("AltLeft", VK_LMENU), std::make_pair("AltRight", VK_RMENU),
		std::make_pair("Menu", VK_MENU), std::make_pair("LeftMenu", VK_LMENU), std::make_pair("RightMenu", VK_RMENU),
		std::make_pair("OSLeft", VK_LWIN), std::make_pair("OSRight", VK_RWIN),
		std::make_pair("Pause", VK_PAUSE),
		std::make_pair("Capital", VK_CAPITAL), std::make_pair("CapsLock", VK_CAPITAL),
		std::make_pair("NumLock", VK_NUMLOCK),
		std::make_pair("ScrollLock", VK_SCROLL),
		std::make_pair("Escape", VK_ESCAPE),
		std::make_pair("Space", VK_SPACE),
		std::make_pair("PageUp", VK_PRIOR), std::make_pair("PageDown", VK_NEXT),
		std::make_pair("Home", VK_HOME), std::make_pair("End", VK_END),
		std::make_pair("Left", VK_LEFT), std::make_pair("Right", VK_RIGHT),
		std::make_pair("Up", VK_UP), std::make_pair("Down", VK_DOWN),
		std::make_pair("Select", VK_SELECT),
		std::make_pair("Print", VK_PRINT),
		std::make_pair("Execute", VK_EXECUTE),
		std::make_pair("Snapshot", VK_SNAPSHOT),
		std::make_pair("PrintScreen", VK_SNAPSHOT),
		std::make_pair("Insert", VK_INSERT), std::make_pair("Delete", VK_DELETE),
		std::make_pair("Help", VK_HELP),
		std::make_pair("Apps", VK_APPS),
		std::make_pair("Sleep", VK_SLEEP),
		/// Function
		std::make_pair("F1", VK_F1), std::make_pair("F2", VK_F2), std::make_pair("F3", VK_F3),
		std::make_pair("F4", VK_F4), std::make_pair("F5", VK_F5), std::make_pair("F6", VK_F6),
		std::make_pair("F7", VK_F7), std::make_pair("F8", VK_F8), std::make_pair("F9", VK_F9),
		std::make_pair("F10", VK_F10), std::make_pair("F11", VK_F11), std::make_pair("F12", VK_F12),
		std::make_pair("F13", VK_F13), std::make_pair("F14", VK_F14), std::make_pair("F15", VK_F15),
		std::make_pair("F16", VK_F16), std::make_pair("F17", VK_F17), std::make_pair("F18", VK_F18),
		std::make_pair("F19", VK_F19), std::make_pair("F20", VK_F20), std::make_pair("F21", VK_F21),
		std::make_pair("F22", VK_F22), std::make_pair("F23", VK_F23), std::make_pair("F24", VK_F24),
		/// Numeric
		std::make_pair("Digit0", 0x30), std::make_pair("Digit1", 0x31), std::make_pair("Digit2", 0x32),
		std::make_pair("Digit3", 0x33), std::make_pair("Digit4", 0x34), std::make_pair("Digit5", 0x35),
		std::make_pair("Digit6", 0x36), std::make_pair("Digit7", 0x37), std::make_pair("Digit8", 0x38),
		std::make_pair("Digit9", 0x39),
		/// Letters
		std::make_pair("KeyA", 0x41), std::make_pair("KeyB", 0x42), std::make_pair("KeyC", 0x43),
		std::make_pair("KeyD", 0x44), std::make_pair("KeyE", 0x45), std::make_pair("KeyF", 0x46),
		std::make_pair("KeyG", 0x47), std::make_pair("KeyH", 0x48), std::make_pair("KeyI", 0x49),
		std::make_pair("KeyJ", 0x4A), std::make_pair("KeyK", 0x4B), std::make_pair("KeyL", 0x4C),
		std::make_pair("KeyM", 0x4D), std::make_pair("KeyN", 0x4E), std::make_pair("KeyO", 0x4F),
		std::make_pair("KeyP", 0x50), std::make_pair("KeyQ", 0x51), std::make_pair("KeyR", 0x52),
		std::make_pair("KeyS", 0x53), std::make_pair("KeyT", 0x54), std::make_pair("KeyU", 0x55),
		std::make_pair("KeyV", 0x56), std::make_pair("KeyW", 0x57), std::make_pair("KeyX", 0x58),
		std::make_pair("KeyY", 0x59), std::make_pair("KeyZ", 0x5A),
		/// Numeric Pad
		std::make_pair("Numpad0", VK_NUMPAD0), std::make_pair("Numpad1", VK_NUMPAD1),
		std::make_pair("Numpad2", VK_NUMPAD2), std::make_pair("Numpad3", VK_NUMPAD3),
		std::make_pair("Numpad4", VK_NUMPAD4), std::make_pair("Numpad5", VK_NUMPAD5),
		std::make_pair("Numpad6", VK_NUMPAD6), std::make_pair("Numpad7", VK_NUMPAD7),
		std::make_pair("Numpad8", VK_NUMPAD8), std::make_pair("Numpad9", VK_NUMPAD9),
		std::make_pair("NumpadMultiply", VK_MULTIPLY), std::make_pair("NumpadDivide", VK_DIVIDE),
		std::make_pair("NumpadAdd", VK_ADD), std::make_pair("NumpadSubtract", VK_SUBTRACT),
		std::make_pair("Separator", VK_SEPARATOR), std::make_pair("NumpadDecimal", VK_DECIMAL),
		std::make_pair("NumLock", VK_NUMLOCK), std::make_pair("NumpadEnter", VK_RETURN),

		/// OEM Keys
		std::make_pair("Semicolon", VK_OEM_1), std::make_pair("Equal", VK_OEM_PLUS),
		std::make_pair("Comma", VK_OEM_COMMA), std::make_pair("Minus", VK_OEM_MINUS),
		std::make_pair("Period", VK_OEM_PERIOD), std::make_pair("Slash", VK_OEM_2),
		std::make_pair("Backquote", VK_OEM_3), std::make_pair("BracketLeft", VK_OEM_4),
		std::make_pair("Backslash", VK_OEM_5), std::make_pair("BracketRight", VK_OEM_6),
		std::make_pair("Quote", VK_OEM_7),
		// Arrows
		std::make_pair("ArrowUp", VK_UP), std::make_pair("ArrowLeft", VK_LEFT),
		std::make_pair("ArrowRight", VK_RIGHT), std::make_pair("ArrowDown", VK_DOWN),

		/// Media
		std::make_pair("MediaPlayPause", VK_MEDIA_PLAY_PAUSE), std::make_pair("MediaTrackPrevious", VK_MEDIA_PREV_TRACK),
		std::make_pair("MediaTrackNext", VK_MEDIA_NEXT_TRACK), std::make_pair("MediaStop", VK_MEDIA_STOP),
	#else
		std::make_pair("Escape", VC_ESCAPE),
		std::make_pair("F1", VC_F1),
		std::make_pair("F2", VC_F2),
		std::make_pair("F3", VC_F3),
		std::make_pair("F4", VC_F4),
		std::make_pair("F5", VC_F5),
		std::make_pair("F6", VC_F6),
		std::make_pair("F7", VC_F7),
		std::make_pair("F8", VC_F8),
		std::make_pair("F9", VC_F9),
		std::make_pair("F10", VC_F10),
		std::make_pair("F11", VC_F11),
		std::make_pair("F12", VC_F12),
		std::make_pair("F13", VC_F13),
		std::make_pair("F14", VC_F14),
		std::make_pair("F15", VC_F15),
		std::make_pair("F16", VC_F16),
		std::make_pair("F17", VC_F17),
		std::make_pair("F18", VC_F18),
		std::make_pair("F19", VC_F19),
		std::make_pair("F20", VC_F20),
		std::make_pair("F21", VC_F21),
		std::make_pair("F22", VC_F22),
		std::make_pair("F23", VC_F23),
		std::make_pair("F24", VC_F24),
		std::make_pair("1", VC_1),
		std::make_pair("2", VC_2),
		std::make_pair("3", VC_3),
		std::make_pair("4", VC_4),
		std::make_pair("5", VC_5),
		std::make_pair("6", VC_6),
		std::make_pair("7", VC_7),
		std::make_pair("8", VC_8),
		std::make_pair("9", VC_9),
		std::make_pair("0", VC_0),
		std::make_pair("Backspace", VC_BACKSPACE),
		std::make_pair("Tab", VC_TAB),
		std::make_pair("A", VC_A),
		std::make_pair("B", VC_B),
		std::make_pair("C", VC_C),
		std::make_pair("D", VC_D),
		std::make_pair("E", VC_E),
		std::make_pair("F", VC_F),
		std::make_pair("G", VC_G),
		std::make_pair("H", VC_H),
		std::make_pair("I", VC_I),
		std::make_pair("J", VC_J),
		std::make_pair("K", VC_K),
		std::make_pair("L", VC_L),
		std::make_pair("M", VC_M),
		std::make_pair("N", VC_N),
		std::make_pair("O", VC_O),
		std::make_pair("P", VC_P),
		std::make_pair("Q", VC_Q),
		std::make_pair("R", VC_R),
		std::make_pair("S", VC_S),
		std::make_pair("T", VC_T),
		std::make_pair("U", VC_U),
		std::make_pair("V", VC_V),
		std::make_pair("W", VC_W),
		std::make_pair("X", VC_X),
		std::make_pair("Y", VC_Y),
		std::make_pair("Z", VC_Z),
		std::make_pair("Control", 29),
		std::make_pair("CommandOrControl", 29),
		std::make_pair("Command", 29),
		std::make_pair("Alt", 56),
		std::make_pair("Shift", 42),
	#endif
	};

	bool modShift, modCtrl, modMenu, modMeta;
	modShift = modifiers.Get("shift").ToBoolean().Value();
	modCtrl = modifiers.Get("ctrl").ToBoolean().Value();
	modMenu = modifiers.Get("alt").ToBoolean().Value();
	modMeta = modifiers.Get("meta").ToBoolean().Value();


	std::map<std::string, key_t>::iterator it = g_KeyMap.find(keystr);


	std::vector<std::pair<key_t, bool>> keys;

	if (it != g_KeyMap.end()) {
		key_t key = g_KeyMap.at(keystr);

		keys.push_back(std::make_pair(g_KeyMap.at("Shift"), modShift));
		keys.push_back(std::make_pair(g_KeyMap.at("Control"), modCtrl));
		keys.push_back(std::make_pair(g_KeyMap.at("Menu"), modMenu));
		keys.push_back(std::make_pair(g_KeyMap.at("OSLeft"), modMeta));

		keys.push_back(std::make_pair(key, true));
	}

	return std::move(keys);
}

Napi::Value RegisterHotkeyJS(const Napi::CallbackInfo& info) {
	/* interface INodeLibuiohookBinding {
	 *   callback: () => void;
	 *   eventType: TKeyEventType;
	 *   key: string; // Is key code
	 *   modifiers: {
	 *     alt: boolean;
	 *     ctrl: boolean;
	 *     shift: boolean;
	 *     meta: boolean;
	 *   };
	 * }
	 */

	Napi::Object binds = info[0].ToObject();
	std::vector<std::pair<key_t, bool>> keys = StringToKeys(
		binds.Get("key").ToString().Utf8Value(),
		binds.Get("modifiers").ToObject()
	);

	std::string eventString = binds.Get("eventType").ToString().Utf8Value();

	if (keys.size() == 0)
		return Napi::Boolean::New(info.Env(), false);

	uint32_t key = HotKey::Stringify(keys);
	if (gThreadData.hotkeys.count(key)) {
		auto hk = gThreadData.hotkeys.find(key);

		// Lock mutex for modifications

		if (eventString == "registerKeydown") {
			if (!hk->second.cbDown) {
				// Lock mutex for modifications
				std::unique_lock<std::mutex> ulock(gThreadData.mtx);
				hk->second.cbDown = std::make_unique<Worker>(binds.Get("callback").As<Napi::Function>());
				hk->second.cbDown->SuppressDestruct();
			} else {
				return Napi::Boolean::New(info.Env(), false);
			}
		} else if (eventString == "registerKeyup") {
			if (!hk->second.cbUp) {
				// Lock mutex for modifications
				std::unique_lock<std::mutex> ulock(gThreadData.mtx);
				hk->second.cbUp = std::make_unique<Worker>(binds.Get("callback").As<Napi::Function>());
				hk->second.cbUp->SuppressDestruct();
			} else {
				return Napi::Boolean::New(info.Env(), false);
			}
		}
	} else {
		HotKey hk;
		hk.keys = std::move(keys);
		hk.wasDown = false;

		if (eventString == "registerKeydown") {
			hk.cbDown = std::make_unique<Worker>(binds.Get("callback").As<Napi::Function>());
			hk.cbDown->SuppressDestruct();
		} else if (eventString == "registerKeyup") {
			hk.cbUp = std::make_unique<Worker>(binds.Get("callback").As<Napi::Function>());
			hk.cbUp->SuppressDestruct();
		}

		// Lock mutex for modifications
		std::unique_lock<std::mutex> ulock(gThreadData.mtx);
		gThreadData.hotkeys.insert_or_assign(key, std::move(hk));
	}

	return Napi::Boolean::New(info.Env(), true);
}

Napi::Value UnregisterHotkeyJS(const Napi::CallbackInfo& info) {
	Napi::Object binds = info[0].ToObject();
	std::vector<std::pair<key_t, bool>> keys = StringToKeys(
		binds.Get("key").ToString().Utf8Value(),
		binds.Get("modifiers").ToObject()
	);
	std::string eventString = binds.Get("eventType").ToString().Utf8Value();

	if (keys.size() == 0)
		return Napi::Boolean::New(info.Env(), false);

	uint32_t key = HotKey::Stringify(keys);
	if (!gThreadData.hotkeys.count(key)) {
		std::cout << "Cannot find key " << key << std::endl;
		return Napi::Boolean::New(info.Env(), false);
	}

	// Lock mutex for modifications
	std::unique_lock<std::mutex> ulock(gThreadData.mtx);

	auto hk = gThreadData.hotkeys.find(key);

	if (eventString == "registerKeydown") {
		if (hk->second.cbDown) {
			hk->second.cbDown = nullptr;
		} else {
			return Napi::Boolean::New(info.Env(), false);
		}
	} else if (eventString == "registerKeyup") {
		if (hk->second.cbUp) {
			hk->second.cbDown = nullptr;
		} else {
			return Napi::Boolean::New(info.Env(), false);
		}
	}

	// If both callbacks were removed, don't bother keeping the object around.
	if ((hk->second.cbUp == nullptr) && (hk->second.cbDown == nullptr)) {
		gThreadData.hotkeys.erase(key);
	}
	return Napi::Boolean::New(info.Env(), true);
}

Napi::Value UnregisterHotkeysJS(const Napi::CallbackInfo& info) {
	std::unique_lock<std::mutex> ulock(gThreadData.mtx);
	gThreadData.hotkeys.clear();

	return info.Env().Undefined();
}