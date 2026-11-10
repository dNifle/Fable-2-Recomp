// fable_2 - Cross-platform host key/mouse state for the input bridges.
//
// The Win32 build polls raw host input with GetAsyncKeyState (global key
// state, readable from the guest input thread). There is no portable
// equivalent, so on other platforms the state is mirrored from the SDK's
// window input events (rex::ui::WindowInputListener) - the same pattern the
// SDK's own mnk driver uses (see thirdparty/rexglue-sdk/src/input/mnk).
// KeyboardGamepadDriver attaches the tracker to the game window; the query
// helpers below are used by the keyboard gamepad (keyboard_gamepad.h), the
// F5 external-Lua runner (fable2_f5_lua.h) and the debug console
// (fable2_console.h).
//
// Event callbacks run on the UI thread, the queries on the guest input
// thread, hence the mutex. On focus loss all held state is cleared so no key
// or button sticks after alt-tabbing away.

#pragma once

#include <cstdint>

#ifdef _WIN32

#include <windows.h>

#include <rex/runtime.h>
#include <rex/ui/window.h>

namespace fable2::hostkeys {

inline bool KeyHeld(uint16_t vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
inline void Attach(rex::ui::Window*, size_t = 0) {}
inline void Detach(rex::ui::Window*) {}
// The Win32 mouse-look path warps the cursor and diffs positions itself.
inline void TakeMouseDelta(float&, float&) {}
inline bool GameWindowFocused() {
  // The game window is the foreground (top-level) window of this process.
  auto* rt = rex::Runtime::instance();
  rex::ui::Window* win = rt ? rt->display_window() : nullptr;
  HWND hwnd = win ? reinterpret_cast<HWND>(win->GetNativeWindowHandle()) : nullptr;
  return hwnd != nullptr && (GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT));
}

}  // namespace fable2::hostkeys

#else  // !_WIN32

#include <cstring>
#include <mutex>

#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace fable2::hostkeys {

class Tracker final : public rex::ui::WindowInputListener,
                      public rex::ui::WindowListener {
 public:
  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    SetKey(static_cast<uint16_t>(e.virtual_key()), true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    SetKey(static_cast<uint16_t>(e.virtual_key()), false);
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    SetKey(ButtonVk(e.button()), true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    SetKey(ButtonVk(e.button()), false);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    // Fractional motion; still reported under SetRelativeMouseMode.
    std::lock_guard<std::mutex> l(mutex_);
    mouse_dx_ += e.dx();
    mouse_dy_ += e.dy();
  }

  // WindowListener
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    // No stuck keys/buttons after alt-tabbing away.
    std::lock_guard<std::mutex> l(mutex_);
    std::memset(key_down_, 0, sizeof key_down_);
  }

  bool KeyHeld(uint16_t vk) {
    std::lock_guard<std::mutex> l(mutex_);
    return vk < 256 && key_down_[vk];
  }
  void TakeMouseDelta(float& dx, float& dy) {
    std::lock_guard<std::mutex> l(mutex_);
    dx = mouse_dx_;
    dy = mouse_dy_;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
  }

 private:
  static uint16_t ButtonVk(rex::ui::MouseEvent::Button b) {
    using MB = rex::ui::MouseEvent::Button;
    switch (b) {
      case MB::kLeft:   return 0x01;  // VK_LBUTTON
      case MB::kRight:  return 0x02;  // VK_RBUTTON
      case MB::kMiddle: return 0x04;  // VK_MBUTTON
      case MB::kX1:     return 0x05;  // VK_XBUTTON1
      case MB::kX2:     return 0x06;  // VK_XBUTTON2
      default:          return 0;
    }
  }
  void SetKey(uint16_t vk, bool down) {
    if (vk >= 256) return;
    std::lock_guard<std::mutex> l(mutex_);
    key_down_[vk] = down;
  }

  std::mutex mutex_;
  bool key_down_[256] = {};
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;
};

inline Tracker& GetTracker() {
  static Tracker tracker;
  return tracker;
}
inline bool KeyHeld(uint16_t vk) { return GetTracker().KeyHeld(vk); }
inline void TakeMouseDelta(float& dx, float& dy) {
  GetTracker().TakeMouseDelta(dx, dy);
}
inline void Attach(rex::ui::Window* win, size_t z_order = 0) {
  if (!win) return;
  win->AddInputListener(&GetTracker(), z_order);
  win->AddListener(&GetTracker());
}
inline void Detach(rex::ui::Window* win) {
  if (!win) return;
  win->RemoveInputListener(&GetTracker());
  win->RemoveListener(&GetTracker());
}
inline bool GameWindowFocused() {
  auto* rt = rex::Runtime::instance();
  rex::ui::Window* win = rt ? rt->display_window() : nullptr;
  return win != nullptr && win->HasFocus();
}

}  // namespace fable2::hostkeys

#endif  // _WIN32
