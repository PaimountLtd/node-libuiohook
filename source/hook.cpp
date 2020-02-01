
/******************************************************************************
    Copyright (C) 2016-2019 by Streamlabs (General Workings Inc)
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
#include <map>
#include <mutex>
#include <list>
#include <string>
#include <thread>
#include <vector>
#include <inttypes.h>
#include "uiohook.h"
#include <algorithm>

#if defined(__APPLE__) && defined(__MACH__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#define UIOHOOK_ERROR_THREAD_CREATE 0x10

class ForeignWorker {
	private:
	uv_async_t * async;

	static void AsyncClose(uv_handle_t *handle) {
		ForeignWorker *worker =
			reinterpret_cast<ForeignWorker*>(handle->data);

		worker->Destroy();
	}

	static NAUV_WORK_CB(AsyncCallback) {
		ForeignWorker *worker =
			reinterpret_cast<ForeignWorker*>(async->data);
		worker->Execute();
		uv_close(reinterpret_cast<uv_handle_t*>(async), ForeignWorker::AsyncClose);
	}

	protected:
	Nan::Callback *callback;

	v8::Local<v8::Value> Call(int argc = 0, v8::Local<v8::Value> params[] = 0) {
		return callback->Call(argc, params);
	}

	public:
	ForeignWorker(Nan::Callback *callback) {
		async = new uv_async_t;

		uv_async_init(
			uv_default_loop()
			, async
			, AsyncCallback
		);

		async->data = this;
		this->callback = callback;
	}

	void Send() {
		uv_async_send(async);
	}

	virtual void Execute() = 0;
	virtual void Destroy() {
		delete this;
	};

	virtual ~ForeignWorker() {
		delete async;
	}
};

class Worker : public ForeignWorker {
	public:
	Worker(Nan::Callback *callback)
		: ForeignWorker(callback) {}

	virtual void Execute() {
		Call(0, 0);
	}

	virtual void Destroy() {
		delete this;
	}
};

#ifdef WIN32
typedef int16_t key_t;

static uint32_t jenkings_one_at_a_time(const std::pair<uint8_t, bool>* key, size_t sz) {
	size_t p = 0; uint32_t hash = 0;
	while (p < sz) {
		hash += key[p++].first;
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

struct HotKey {
	std::vector<std::pair<key_t, bool>> keys;
	std::unique_ptr<Nan::Callback> cbDown, cbUp;
	bool wasDown = false;

	static uint32_t Stringify(std::vector<std::pair<key_t, bool>> keys) {
		return jenkings_one_at_a_time(reinterpret_cast<std::pair<uint8_t, bool>*>(keys.data()), keys.size() * (sizeof(key_t) + sizeof(bool)));
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
					if (hk.second.cbDown != nullptr) {
						Worker *worker = new Worker(hk.second.cbDown.get());
						worker->Send();
					}

					hk.second.wasDown = true;
				} else if (!allPressed && hk.second.wasDown) {
					if (hk.second.cbUp != nullptr) {
						Worker *worker = new Worker(hk.second.cbUp.get());
						worker->Send();
					}

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

void StartHotkeyThreadJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	if (gThreadData.worker.joinable()) {
		args.GetReturnValue().Set(false);
		return;
	}

	gThreadData.mtx.lock();
	gThreadData.worker = std::thread(HotKeyThread, &gThreadData);
	gThreadData.mtx.unlock();

	args.GetReturnValue().Set(true);
	return;
}

void StopHotkeyThreadJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	if (!gThreadData.worker.joinable()) {
		args.GetReturnValue().Set(false);
		return;
	}

	gThreadData.shutdown = true;
	gThreadData.worker.join();

	args.GetReturnValue().Set(true);
	return;
}

std::vector<std::pair<key_t, bool>> StringToKeys(std::string keystr, v8::Local<v8::Object> modifiers) {
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
	modShift = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "shift").ToLocalChecked())->ToBoolean()->BooleanValue();
	modCtrl = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "ctrl").ToLocalChecked())->ToBoolean()->BooleanValue();
	modMenu = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "alt").ToLocalChecked())->ToBoolean()->BooleanValue();
	modMeta = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "meta").ToLocalChecked())->ToBoolean()->BooleanValue();


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

void RegisterHotkeyJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
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

	v8::Local<v8::Object> binds = args[0]->ToObject();
	std::vector<std::pair<key_t, bool>> keys = StringToKeys(
		std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "key").ToLocalChecked()))),
		binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "modifiers").ToLocalChecked())->ToObject()
	);
	std::string eventString = std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(),
		"eventType").ToLocalChecked())));

	if (keys.size() == 0)
		return;

	uint32_t key = HotKey::Stringify(keys);
	if (gThreadData.hotkeys.count(key)) {
		auto hk = gThreadData.hotkeys.find(key);

		// Lock mutex for modifications

		if (eventString == "registerKeydown") {
			if (!hk->second.cbDown) {
				// Lock mutex for modifications
				std::unique_lock<std::mutex> ulock(gThreadData.mtx);
				hk->second.cbDown = std::make_unique<Nan::Callback>(binds->Get(v8::String::NewFromUtf8(
					args.GetIsolate(), "callback").ToLocalChecked()).As<v8::Function>());
			} else {
				args.GetReturnValue().Set(false);
				return;
			}
		} else if (eventString == "registerKeyup") {
			if (!hk->second.cbUp) {
				// Lock mutex for modifications
				std::unique_lock<std::mutex> ulock(gThreadData.mtx);
				hk->second.cbUp = std::make_unique<Nan::Callback>(binds->Get(v8::String::NewFromUtf8(
					args.GetIsolate(), "callback").ToLocalChecked()).As<v8::Function>());
			} else {
				args.GetReturnValue().Set(false);
				return;
			}
		}
	} else {
		HotKey hk;
		hk.keys = std::move(keys);
		hk.wasDown = false;

		if (eventString == "registerKeydown") {
			hk.cbDown = std::make_unique<Nan::Callback>(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(),
				"callback").ToLocalChecked()).As<v8::Function>());
		} else if (eventString == "registerKeyup") {
			hk.cbUp = std::make_unique<Nan::Callback>(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(),
				"callback").ToLocalChecked()).As<v8::Function>());
		}

		// Lock mutex for modifications
		std::unique_lock<std::mutex> ulock(gThreadData.mtx);
		gThreadData.hotkeys.insert_or_assign(key, std::move(hk));
	}

	args.GetReturnValue().Set(true);
	return;
}

void UnregisterHotkeyJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Local<v8::Object> binds = args[0]->ToObject();
	std::vector<std::pair<key_t, bool>> keys = StringToKeys(
		std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "key").ToLocalChecked()))),
		binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "modifiers").ToLocalChecked())->ToObject()
	);
	std::string eventString = std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(),
		"eventType").ToLocalChecked())));

	if (keys.size() == 0)
		return;

	uint32_t key = HotKey::Stringify(keys);
	if (!gThreadData.hotkeys.count(key)) {
		args.GetReturnValue().Set(false);
		return;
	}

	// Lock mutex for modifications
	std::unique_lock<std::mutex> ulock(gThreadData.mtx);

	auto hk = gThreadData.hotkeys.find(key);

	if (eventString == "registerKeydown") {
		if (hk->second.cbDown) {
			hk->second.cbDown = nullptr;
		} else {
			args.GetReturnValue().Set(false);
			return;
		}
	} else if (eventString == "registerKeyup") {
		if (hk->second.cbUp) {
			hk->second.cbDown = nullptr;
		} else {
			args.GetReturnValue().Set(false);
			return;
		}
	}

	// If both callbacks were removed, don't bother keeping the object around.
	if ((hk->second.cbUp == nullptr) && (hk->second.cbDown == nullptr)) {
		gThreadData.hotkeys.erase(key);
	}

	args.GetReturnValue().Set(true);
	return;
}

void UnregisterHotkeysJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::unique_lock<std::mutex> ulock(gThreadData.mtx);
	gThreadData.hotkeys.clear();
}
#endif

std::map<std::string, int> keyCodesArray;
std::map<uint16_t, _event_type> modifiers;

struct KeyData {
	int code;
};

struct Event {
	uint16_t key;
	std::map< uint16_t, _event_type> modifiers;
};

struct Action {
	_event_type m_event;
	Event m_codeEvent;
	_event_type m_currentState;
	Nan::Callback *m_js_callBack;
};

std::vector<Action*> pressedKeyEventCallbacks;
std::vector<Action*> releasedKeyEventCallbacks;

// int64_t currentModifierCode = 0;
// int64_t currentModifierState = EVENT_KEY_RELEASED;

// Thread and mutex variables.
#ifdef _WIN32
static HANDLE hook_thread;

static HANDLE hook_running_mutex;
static HANDLE hook_control_mutex;
static HANDLE hook_control_cond;
#else
static pthread_t hook_thread;

static pthread_mutex_t hook_running_mutex;
static pthread_mutex_t hook_control_mutex;
static pthread_cond_t hook_control_cond;
#endif

void updateModifierState(uint16_t key, _event_type state) {
	if (key == VC_SHIFT_L || key == VC_SHIFT_R) {
		auto left = modifiers.find(VC_SHIFT_L);
		if (left != modifiers.end())
			left->second = state;

		auto right = modifiers.find(VC_SHIFT_R);
		if (right != modifiers.end())
			right->second = state;
	}
	if (key == VC_CONTROL_L || key == VC_CONTROL_R) {
		auto left = modifiers.find(VC_CONTROL_L);
		if (left != modifiers.end())
			left->second = state;

		auto right = modifiers.find(VC_CONTROL_R);
		if (right != modifiers.end())
			right->second = state;
	}
	if (key == VC_ALT_L || key == VC_ALT_R) {
		auto left = modifiers.find(VC_ALT_L);
		if (left != modifiers.end())
			left->second = state;

		auto right = modifiers.find(VC_ALT_R);
		if (right != modifiers.end())
			right->second = state;
	}
	if (key == VC_META_L || key == VC_META_R) {
		auto left = modifiers.find(VC_META_L);
		if (left != modifiers.end())
			left->second = state;

		auto right = modifiers.find(VC_META_R);
		if (right != modifiers.end())
			right->second = state;
	}
}

void storeStringKeyCodes(void) {
	keyCodesArray = {
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
		std::make_pair("KeyA", VC_A),
		std::make_pair("KeyB", VC_B),
		std::make_pair("KeyC", VC_C),
		std::make_pair("KeyD", VC_D),
		std::make_pair("KeyE", VC_E),
		std::make_pair("KeyF", VC_F),
		std::make_pair("KeyG", VC_G),
		std::make_pair("KeyH", VC_H),
		std::make_pair("KeyI", VC_I),
		std::make_pair("KeyJ", VC_J),
		std::make_pair("KeyK", VC_K),
		std::make_pair("KeyL", VC_L),
		std::make_pair("KeyM", VC_M),
		std::make_pair("KeyN", VC_N),
		std::make_pair("KeyO", VC_O),
		std::make_pair("KeyP", VC_P),
		std::make_pair("KeyQ", VC_Q),
		std::make_pair("KeyR", VC_R),
		std::make_pair("KeyS", VC_S),
		std::make_pair("KeyT", VC_T),
		std::make_pair("KeyU", VC_U),
		std::make_pair("KeyV", VC_V),
		std::make_pair("KeyW", VC_W),
		std::make_pair("KeyX", VC_X),
		std::make_pair("KeyY", VC_Y),
		std::make_pair("KeyZ", VC_Z),
		std::make_pair("Control", MASK_CTRL),
		std::make_pair("CommandOrControl", MASK_META),
		std::make_pair("Command", MASK_META),
		std::make_pair("Alt", MASK_ALT),
		std::make_pair("Shift", MASK_SHIFT)
	};

	modifiers = {
		std::make_pair(VC_SHIFT_L, EVENT_KEY_RELEASED),
		std::make_pair(VC_SHIFT_R, EVENT_KEY_RELEASED),
		std::make_pair(VC_CONTROL_L, EVENT_KEY_RELEASED),
		std::make_pair(VC_CONTROL_R, EVENT_KEY_RELEASED),
		std::make_pair(VC_ALT_L, EVENT_KEY_RELEASED),
		std::make_pair(VC_ALT_R, EVENT_KEY_RELEASED),
		std::make_pair(VC_META_L, EVENT_KEY_RELEASED),
		std::make_pair(VC_META_R, EVENT_KEY_RELEASED),
	};
}

void dispatch_procB(uiohook_event * const event) {
	switch (event->type) {
		case EVENT_HOOK_ENABLED:
			// Lock the running mutex so we know if the hook is enabled.
			#ifdef _WIN32
			WaitForSingleObject(hook_running_mutex, INFINITE);
			#else
			pthread_mutex_lock(&hook_running_mutex);
			#endif


			#ifdef _WIN32
			// Signal the control event.
			SetEvent(hook_control_cond);
			#else
			// Unlock the control mutex so hook_enable() can continue.
			pthread_cond_signal(&hook_control_cond);
			pthread_mutex_unlock(&hook_control_mutex);
			#endif
			break;

		case EVENT_HOOK_DISABLED:
			// Lock the control mutex until we exit.
			#ifdef _WIN32
			WaitForSingleObject(hook_control_mutex, INFINITE);
			#else
			pthread_mutex_lock(&hook_control_mutex);
			#endif

			// Unlock the running mutex so we know if the hook is disabled.
			#ifdef _WIN32
			ReleaseMutex(hook_running_mutex);
			ResetEvent(hook_control_cond);
			#else
			#if defined(__APPLE__) && defined(__MACH__)
			// Stop the main runloop so that this program ends.
			CFRunLoopStop(CFRunLoopGetMain());
			#endif

			pthread_mutex_unlock(&hook_running_mutex);
			#endif
			break;

		case EVENT_KEY_PRESSED: {
			// std::cout << "key code " << event->data.keyboard.keycode << std::endl;
			for (int i = 0; i < pressedKeyEventCallbacks.size(); i++) {
				if (//If the associated event is an EVENT_KEY_PRESSED type
					pressedKeyEventCallbacks.at(i)->m_event == EVENT_KEY_PRESSED &&
					//If the current key pressed is associated with an element in the vector
					event->data.keyboard.keycode == pressedKeyEventCallbacks.at(i)->m_codeEvent.key &&
					//If the key is not already pressed
					pressedKeyEventCallbacks.at(i)->m_currentState != EVENT_KEY_PRESSED) {
					bool hasModifiers = pressedKeyEventCallbacks.at(i)->m_codeEvent.modifiers.empty();
					bool modifiersPressed = false;

					for (auto modifier: pressedKeyEventCallbacks.at(i)->m_codeEvent.modifiers) {
						if (modifier.second != EVENT_KEY_PRESSED) {
							modifiersPressed = false;
							break;
						}
						modifiersPressed = true;
					}

					if (hasModifiers == modifiersPressed) {
						Worker *worker = new Worker(pressedKeyEventCallbacks.at(i)->m_js_callBack);
						worker->Send();

						pressedKeyEventCallbacks.at(i)->m_currentState = EVENT_KEY_PRESSED;
						break;
					}
				}
			}

			auto mod_it = modifiers.find(event->data.keyboard.keycode);
			if (mod_it != modifiers.end())
				updateModifierState(event->data.keyboard.keycode, EVENT_KEY_PRESSED);

			break;
		}
		case EVENT_KEY_RELEASED: {
			for (int i = 0; i < releasedKeyEventCallbacks.size(); i++) {
				if (//If the associated event is an EVENT_KEY_RELEASED type
					releasedKeyEventCallbacks.at(i)->m_event == EVENT_KEY_RELEASED &&
					//If the current key pressed is associated with an element in the vector
					event->data.keyboard.keycode == releasedKeyEventCallbacks.at(i)->m_codeEvent.key) {
						Worker *worker = new Worker(releasedKeyEventCallbacks.at(i)->m_js_callBack);
						worker->Send();

						break;
				}
			}

			for (int i = 0; i < pressedKeyEventCallbacks.size(); i++) {
				if (//If the associated event is an EVENT_KEY_PRESSED type
					pressedKeyEventCallbacks.at(i)->m_event == EVENT_KEY_PRESSED &&
					//If the key is already pressed
					pressedKeyEventCallbacks.at(i)->m_currentState == EVENT_KEY_PRESSED) {
					pressedKeyEventCallbacks.at(i)->m_currentState = EVENT_KEY_RELEASED;
					break;
				}
			}

			auto mod_it = modifiers.find(event->data.keyboard.keycode);
			if (mod_it != modifiers.end())
				updateModifierState(event->data.keyboard.keycode, EVENT_KEY_RELEASED);

			break;
		}
		case EVENT_KEY_TYPED:
		case EVENT_MOUSE_PRESSED:
		case EVENT_MOUSE_RELEASED:
		case EVENT_MOUSE_CLICKED:
		case EVENT_MOUSE_MOVED:
		case EVENT_MOUSE_DRAGGED:
		case EVENT_MOUSE_WHEEL:
		default:
			break;
	}
}

void *hook_thread_proc(void *arg) {
	// Set the hook status.
	int status = hook_run();
	if (status != UIOHOOK_SUCCESS) {
		#ifdef _WIN32
		*(DWORD *)arg = status;
		#else
		*(int *)arg = status;
		#endif
	}

	// Make sure we signal that we have passed any exception throwing code for
	// the waiting hook_enable().
	#ifdef _WIN32
	SetEvent(hook_control_cond);

	return status;
	#else
	// Make sure we signal that we have passed any exception throwing code for
	// the waiting hook_enable().
	pthread_cond_signal(&hook_control_cond);
	pthread_mutex_unlock(&hook_control_mutex);

	return arg;
	#endif
}

int hook_enable() {
	// Lock the thread control mutex.  This will be unlocked when the
	// thread has finished starting, or when it has fully stopped.
	#ifdef _WIN32
	WaitForSingleObject(hook_control_mutex, INFINITE);
	#else
	pthread_mutex_lock(&hook_control_mutex);
	#endif

	// Set the initial status.
	int status = UIOHOOK_FAILURE;

	#ifndef _WIN32
	// Create the thread attribute.
	pthread_attr_t hook_thread_attr;
	pthread_attr_init(&hook_thread_attr);

	// Get the policy and priority for the thread attr.
	int policy;
	pthread_attr_getschedpolicy(&hook_thread_attr, &policy);
	int priority = sched_get_priority_max(policy);
	#endif

	#if defined(_WIN32)
	DWORD hook_thread_id;
	DWORD *hook_thread_status = (DWORD *)malloc(sizeof(DWORD));
	hook_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)hook_thread_proc, hook_thread_status, 0, &hook_thread_id);
	if (hook_thread != INVALID_HANDLE_VALUE) {
		#else
	int *hook_thread_status = (int*)malloc(sizeof(int));
	if (pthread_create(&hook_thread, &hook_thread_attr, hook_thread_proc, hook_thread_status) == 0) {
		#endif
		#if defined(_WIN32)
		// Attempt to set the thread priority to time critical.
		if (SetThreadPriority(hook_thread, THREAD_PRIORITY_TIME_CRITICAL) == 0) {
		}
		#elif (defined(__APPLE__) && defined(__MACH__)) || _POSIX_C_SOURCE >= 200112L
		// Some POSIX revisions do not support pthread_setschedprio so we will 
		// use pthread_setschedparam instead.
		struct sched_param param = { .sched_priority = priority };
		if (pthread_setschedparam(hook_thread, SCHED_OTHER, &param) != 0) {
		}
		#else
		// Raise the thread priority using glibc pthread_setschedprio.
		if (pthread_setschedprio(hook_thread, priority) != 0) {
		}
		#endif


		// Wait for the thread to indicate that it has passed the 
		// initialization portion by blocking until either a EVENT_HOOK_ENABLED 
		// event is received or the thread terminates.
		// NOTE This unlocks the hook_control_mutex while we wait.
		#ifdef _WIN32
		WaitForSingleObject(hook_control_cond, INFINITE);
		#else
		pthread_cond_wait(&hook_control_cond, &hook_control_mutex);
		#endif

		#ifdef _WIN32
		if (WaitForSingleObject(hook_running_mutex, 0) != WAIT_TIMEOUT) {
			#else
		if (pthread_mutex_trylock(&hook_running_mutex) == 0) {
			#endif
			// Lock Successful; The hook is not running but the hook_control_cond 
			// was signaled!  This indicates that there was a startup problem!

			// Get the status back from the thread.
			#ifdef _WIN32
			WaitForSingleObject(hook_thread, INFINITE);
			GetExitCodeThread(hook_thread, hook_thread_status);
			#else
			pthread_join(hook_thread, (void **)&hook_thread_status);
			status = *hook_thread_status;
			#endif
		} else {
			// Lock Failure; The hook is currently running and wait was signaled
			// indicating that we have passed all possible start checks.  We can 
			// always assume a successful startup at this point.
			status = UIOHOOK_SUCCESS;
		}

		free(hook_thread_status);

		} else {
		status = UIOHOOK_ERROR_THREAD_CREATE;
		}

	// Make sure the control mutex is unlocked.
	#ifdef _WIN32
	ReleaseMutex(hook_control_mutex);
	#else
	pthread_mutex_unlock(&hook_control_mutex);
	#endif

	return status;
}

bool logger_proc(unsigned int level, const char *format, ...) {
	bool status = false;

	return status;
}

void StartHotkeyThreadJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::cout << "start hotkey" << std::endl;
	
	storeStringKeyCodes();
	// Lock the thread control mutex.  This will be unlocked when the
	// thread has finished starting, or when it has fully stopped.
	pthread_mutex_init(&hook_running_mutex, NULL);
	pthread_mutex_init(&hook_control_mutex, NULL);
	pthread_cond_init(&hook_control_cond, NULL);

	// Set the logger callback for library output.
	hook_set_logger_proc(&logger_proc);

	// Set the event callback for uiohook events.
	hook_set_dispatch_proc(&dispatch_procB);

	// Start the hook and block.
	// NOTE If EVENT_HOOK_ENABLED was delivered, the status will always succeed.
	int status = hook_enable();
}

void StopHotkeyThreadJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::cout << "stop hotkey" << std::endl;
	hook_stop();
	pthread_mutex_destroy(&hook_running_mutex);
	pthread_mutex_destroy(&hook_control_mutex);
	pthread_cond_destroy(&hook_control_cond);
}

/*

std::vector<std::pair<std::string, int>> keyCodesArray;

struct KeyData {
	std::string name;
	int code;
};

struct Event {
	KeyData key;
	KeyData modifier;

};

struct Action {
	_event_type m_event;
	Event m_codeEvent;
	_event_type m_currentState;
	Nan::Callback *m_js_callBack;
};

std::vector<Action*> pressedKeyEventCallbacks;
std::vector<Action*> releasedKeyEventCallbacks;

*/

void RegisterHotkeyJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::cout << "register hotkey" << std::endl;

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

	Action *action = new Action();

	v8::Local<v8::Object> binds = args[0]->ToObject();
	Event event;

	std::string key_str = std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "key").ToLocalChecked())));
	auto key_it = keyCodesArray.find(key_str);
	
	if (key_it == keyCodesArray.end()) {
		std::cout << "Key not found!, key received: " << key_str.c_str() << std::endl;
		args.GetReturnValue().Set(false);
		return;
	}

	event.key = key_it->second;
	
	bool modShift, modCtrl, modAlt, modMeta;
	v8::Local<v8::Object> modifiers = binds->Get(v8::String::NewFromUtf8(args.GetIsolate(), "modifiers").ToLocalChecked())->ToObject();
	modShift = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "shift").ToLocalChecked())->ToBoolean()->BooleanValue();
	modCtrl = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "ctrl").ToLocalChecked())->ToBoolean()->BooleanValue();
	modAlt = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "alt").ToLocalChecked())->ToBoolean()->BooleanValue();
	modMeta = modifiers->Get(v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), "meta").ToLocalChecked())->ToBoolean()->BooleanValue();

	if(modShift) {
		event.modifiers.emplace(std::make_pair(VC_SHIFT_L, EVENT_KEY_RELEASED));
		event.modifiers.emplace(std::make_pair(VC_SHIFT_R, EVENT_KEY_RELEASED));
	}

	if(modCtrl) {
		event.modifiers.emplace(std::make_pair(VC_CONTROL_L, EVENT_KEY_RELEASED));
		event.modifiers.emplace(std::make_pair(VC_CONTROL_R, EVENT_KEY_RELEASED));
	}

	if(modAlt) {
		event.modifiers.emplace(std::make_pair(VC_ALT_L, EVENT_KEY_RELEASED));
		event.modifiers.emplace(std::make_pair(VC_ALT_R, EVENT_KEY_RELEASED));
	}

	if(modMeta) {
		event.modifiers.emplace(std::make_pair(VC_META_L, EVENT_KEY_RELEASED));
		event.modifiers.emplace(std::make_pair(VC_META_R, EVENT_KEY_RELEASED));
	}

	action->m_codeEvent = event;

	std::string eventString = std::string(*v8::String::Utf8Value(binds->Get(v8::String::NewFromUtf8(args.GetIsolate(),
		"eventType").ToLocalChecked())));

	action->m_currentState = EVENT_KEY_RELEASED;
	action->m_js_callBack = new Nan::Callback(binds->Get(v8::String::NewFromUtf8(
					args.GetIsolate(), "callback").ToLocalChecked()).As<v8::Function>());

	if (eventString.compare("registerKeydown") == 0) {
		action->m_event = EVENT_KEY_PRESSED;
		pressedKeyEventCallbacks.push_back(action);
	} else if (eventString.compare("registerKeyup") == 0) {
		action->m_event = EVENT_KEY_RELEASED;
		releasedKeyEventCallbacks.push_back(action);
	} else {
		std::cout << "Invalid event receive: " << eventString.c_str() << std::endl;
		args.GetReturnValue().Set(false);
		return;
	}

	std::cout << "Hotkey correctly registered" << std::endl;
	args.GetReturnValue().Set(true);
	return;
}

void UnregisterHotkeyJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::cout << "unregister hotkey" << std::endl;
}

void UnregisterHotkeysJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
	std::cout << "unregister hotkeys" << std::endl;
}