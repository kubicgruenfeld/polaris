/**
 * @file src/platform/linux/input/inputtino_ei_virtual_input.cpp
 * @brief Emulated-input routing into a Polaris-owned gamescope.
 */
// local includes
#include "inputtino_ei_virtual_input.h"

#include "src/config.h"
#include "src/logging.h"

#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
  // standard includes
  #include <algorithm>
  #include <cmath>
  #include <cstdlib>
  #include <mutex>
  #include <optional>
  #include <poll.h>
  #include <string>

  // lib includes
  #include <inputtino/keyboard.hpp>
  #include <libei.h>
  #include <linux/input-event-codes.h>
#endif

using namespace std::literals;

namespace platf {

#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT

  namespace {
    /**
     * @brief Map a Moonlight mouse button to its evdev code.
     *
     * gamescope hands EIS_EVENT_BUTTON_BUTTON straight to wlserver_mousebutton,
     * which expects evdev, so no translation layer is needed.
     */
    int mouse_button_to_evdev(int button) {
      switch (button) {
        case BUTTON_LEFT:
          return BTN_LEFT;
        case BUTTON_MIDDLE:
          return BTN_MIDDLE;
        case BUTTON_RIGHT:
          return BTN_RIGHT;
        case BUTTON_X1:
          return BTN_SIDE;
        case BUTTON_X2:
          return BTN_EXTRA;
        default:
          return -1;
      }
    }

    /**
     * @brief Map a Moonlight key to its evdev scancode.
     *
     * libei keyboard codes are evdev scancodes, the same thing the Wayland
     * route sends, so the mapping table is shared and needs no offset.
     */
    int moonlight_key_to_evdev(std::uint16_t modcode) {
      auto key = inputtino::keyboard::key_mappings.find(static_cast<short>(modcode));
      if (key == inputtino::keyboard::key_mappings.end()) {
        return -1;
      }
      return key->second.linux_code;
    }

    // Moonlight sends 120 units per notch, the same convention as WHEEL_DELTA.
    constexpr double k_scroll_units_per_notch = 120.0;
  }  // namespace

  struct ei_virtual_input_t::impl_t {
    std::mutex mutex;
    ei *ctx = nullptr;
    ei_device *device = nullptr;
    bool emulating = false;
    std::uint32_t sequence = 0;
    bool logged_ready = false;
    bool logged_unavailable = false;
    /// Last absolute position, so absolute input can be sent as relative motion.
    std::optional<std::pair<double, double>> last_absolute;

    ~impl_t() {
      disconnect();
    }

    void release_device() {
      if (device) {
        ei_device_unref(device);
        device = nullptr;
      }
      emulating = false;
      last_absolute.reset();
    }

    void disconnect() {
      release_device();
      if (ctx) {
        ei_unref(ctx);
        ctx = nullptr;
      }
      logged_ready = false;
    }

    /**
     * @brief Whether this host streams through a Polaris-owned gamescope.
     *
     * Every other mode either has the Wayland route or wants host uinput.
     */
    bool gamescope_runtime_active() const {
      return config::video.linux_display.stream_mode == "gamescope_stream"sv;
    }

    /**
     * @brief The EIS socket gamescope listens on.
     *
     * gamescope names it after its own Wayland display ("%s-ei") and exports it
     * as LIBEI_SOCKET for its children. Polaris is gamescope's parent, so it
     * derives the same name instead of inheriting it. libei resolves a relative
     * name against XDG_RUNTIME_DIR, which is where gamescope puts it.
     */
    std::string socket_name() const {
      if (const char *inherited = std::getenv("LIBEI_SOCKET"); inherited && *inherited) {
        return inherited;
      }
      const char *display = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
      return std::string {display && *display ? display : "gamescope-0"} + "-ei";
    }

    /**
     * @brief Drain pending EIS events, optionally waiting for the handshake.
     *
     * The connection takes a few round trips before a device exists, so the
     * first call after connecting waits briefly rather than dropping the input
     * that triggered it.
     */
    void pump(int timeout_ms) {
      if (!ctx) {
        return;
      }

      const int fd = ei_get_fd(ctx);
      for (;;) {
        if (fd >= 0 && timeout_ms >= 0) {
          pollfd pfd {fd, POLLIN, 0};
          if (poll(&pfd, 1, timeout_ms) <= 0) {
            return;
          }
        }
        timeout_ms = 0;

        ei_dispatch(ctx);

        bool handled_any = false;
        while (ei_event *event = ei_get_event(ctx)) {
          handled_any = true;
          switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
              ei_seat_bind_capabilities(
                ei_event_get_seat(event),
                EI_DEVICE_CAP_POINTER,
                EI_DEVICE_CAP_POINTER_ABSOLUTE,
                EI_DEVICE_CAP_BUTTON,
                EI_DEVICE_CAP_SCROLL,
                EI_DEVICE_CAP_KEYBOARD,
                nullptr
              );
              break;
            case EI_EVENT_DEVICE_ADDED:
              release_device();
              device = ei_device_ref(ei_event_get_device(event));
              break;
            case EI_EVENT_DEVICE_RESUMED:
              if (device) {
                ei_device_start_emulating(device, ++sequence);
                emulating = true;
                if (!logged_ready) {
                  BOOST_LOG(info) << "EI virtual input: routing mouse and keyboard to the gamescope session on ["sv
                                  << socket_name() << ']';
                  logged_ready = true;
                }
              }
              break;
            case EI_EVENT_DEVICE_PAUSED:
              emulating = false;
              break;
            case EI_EVENT_DEVICE_REMOVED:
              release_device();
              break;
            case EI_EVENT_DISCONNECT:
              BOOST_LOG(info) << "EI virtual input: gamescope closed the input socket; "sv
                              << "reconnecting on the next input event"sv;
              ei_event_unref(event);
              disconnect();
              return;
            default:
              break;
          }
          ei_event_unref(event);
        }

        if (!handled_any || emulating) {
          return;
        }
      }
    }

    /**
     * @brief Connect and wait for gamescope to hand us an emulating device.
     */
    bool ensure_ready() {
      if (!gamescope_runtime_active()) {
        disconnect();
        return false;
      }

      if (ctx && emulating) {
        pump(0);
      }
      if (ctx && emulating) {
        return true;
      }

      if (!ctx) {
        ctx = ei_new_sender(nullptr);
        if (!ctx) {
          return false;
        }
        ei_configure_name(ctx, "polaris");

        const auto socket = socket_name();
        if (const int rc = ei_setup_backend_socket(ctx, socket.c_str()); rc != 0) {
          if (!logged_unavailable) {
            BOOST_LOG(warning) << "EI virtual input: cannot reach gamescope's input socket ["sv << socket
                               << "]: "sv << strerror(-rc) << "; mouse and keyboard will use host uinput"sv;
            logged_unavailable = true;
          }
          ei_unref(ctx);
          ctx = nullptr;
          return false;
        }
        logged_unavailable = false;
      }

      // A fresh connection needs a few round trips before a device exists.
      pump(ctx && !emulating ? 250 : 0);
      return ctx != nullptr && emulating;
    }

    /**
     * @brief Close the event group so gamescope applies it.
     */
    bool frame() {
      if (!device) {
        return false;
      }
      ei_device_frame(device, ei_now(ctx));
      return true;
    }

    bool send_scroll(double dx, double dy) {
      if (!ensure_ready()) {
        return false;
      }
      ei_device_scroll_delta(device, dx, dy);
      return frame();
    }
  };

#else

  struct ei_virtual_input_t::impl_t {};

#endif

  ei_virtual_input_t::ei_virtual_input_t():
      impl(std::make_unique<impl_t>()) {
  }

  ei_virtual_input_t::~ei_virtual_input_t() = default;

  void ei_virtual_input_t::reset() {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    impl->disconnect();
#endif
  }

  bool ei_virtual_input_t::should_block_host_fallback() {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    // Host uinput cannot reach a headless gamescope, and letting it through
    // would drive the host session instead.
    return impl->gamescope_runtime_active();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::move(int delta_x, int delta_y) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_ready()) {
      return false;
    }
    ei_device_pointer_motion(impl->device, delta_x, delta_y);
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::move_abs(const touch_port_t &touch_port, float x, float y) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    if (!impl->ensure_ready() || touch_port.width <= 0 || touch_port.height <= 0) {
      return false;
    }

    // gamescope maps absolute motion to wlserver_mousewarp() with
    // bSynthetic = true, and that path deliberately leaves bCursorHidden alone
    // — an absolute-only client would move the pointer without ever revealing
    // the cursor. Relative motion goes to wlserver_mousemotion(), which does
    // reveal it, so carry absolute input as deltas once there is a previous
    // position to subtract.
    const double target_x = std::clamp(double(x), 0.0, double(touch_port.width));
    const double target_y = std::clamp(double(y), 0.0, double(touch_port.height));

    if (impl->last_absolute) {
      const auto [previous_x, previous_y] = *impl->last_absolute;
      impl->last_absolute = {target_x, target_y};
      ei_device_pointer_motion(impl->device, target_x - previous_x, target_y - previous_y);
    } else {
      impl->last_absolute = {target_x, target_y};
      ei_device_pointer_motion_absolute(impl->device, target_x, target_y);
    }
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::button(int button, bool release) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    const auto evdev_button = mouse_button_to_evdev(button);
    if (evdev_button < 0) {
      BOOST_LOG(warning) << "EI virtual input: unknown mouse button: "sv << button;
      // Claim it anyway: falling through would send it to the host session.
      return impl->gamescope_runtime_active();
    }
    if (!impl->ensure_ready()) {
      return false;
    }
    ei_device_button_button(impl->device, static_cast<std::uint32_t>(evdev_button), !release);
    return impl->frame();
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::scroll(int high_res_distance) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    // Scroll deltas are continuous here, so a sub-notch flick is carried
    // rather than rounded away as it would be with discrete wheel clicks.
    return impl->send_scroll(0.0, -high_res_distance / k_scroll_units_per_notch);
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::hscroll(int high_res_distance) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    return impl->send_scroll(high_res_distance / k_scroll_units_per_notch, 0.0);
#else
    return false;
#endif
  }

  bool ei_virtual_input_t::keyboard_update(std::uint16_t modcode, bool release) {
#ifdef POLARIS_BUILD_EI_VIRTUAL_INPUT
    std::scoped_lock lock(impl->mutex);
    const auto evdev_keycode = moonlight_key_to_evdev(modcode);
    if (evdev_keycode < 0) {
      return impl->gamescope_runtime_active();
    }
    if (!impl->ensure_ready()) {
      return false;
    }
    ei_device_keyboard_key(impl->device, static_cast<std::uint32_t>(evdev_keycode), !release);
    return impl->frame();
#else
    return false;
#endif
  }

}  // namespace platf
