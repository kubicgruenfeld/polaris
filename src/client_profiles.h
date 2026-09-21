/**
 * @file src/client_profiles.h
 * @brief Declarations for per-client display profile support.
 *
 * Client profiles allow different streaming clients (e.g. handhelds, TVs) to
 * automatically apply client-specific display settings such as output name,
 * color range, and HDR overrides. Profiles are loaded from a JSON file in the
 * application data directory.
 */
#pragma once

// standard includes
#include <optional>
#include <string>

namespace client_profiles {

  /// Accepted range for client_profile_t::sdr_nits. Below the floor SDR is
  /// indistinguishable from black; the ceiling is well past any display's
  /// sustained full-screen output.
  constexpr int k_min_sdr_nits = 1;
  constexpr int k_max_sdr_nits = 10000;

  /**
   * @brief Per-client display profile.
   *
   * Each field is optional except output_name. When a field has a value, it
   * overrides the corresponding global config setting for that client's session.
   */
  struct client_profile_t {
    std::string output_name;            ///< Which display output to use (e.g. "HDMI-A-1")
    std::optional<int> color_range;     ///< Override color_range: 0 = client, 1 = limited, 2 = full
    std::optional<bool> hdr;            ///< Override HDR enable/disable for this client
    /**
     * @brief Override the SDR reference white, in nits, for this client.
     *
     * Reaches the nested gamescope as --hdr-sdr-content-nits, which decides how
     * bright SDR content is rendered inside an HDR output. Unset means the host
     * setting stands. The host default of 203 is BT.2408 reference white, a
     * mastering-environment figure: a TV in a lit room usually wants less, a
     * handheld often wants the default, and the same host streams to both.
     */
    std::optional<int> sdr_nits;
    std::string mac_address;            ///< MAC address for Wake-on-LAN (e.g. "AA:BB:CC:DD:EE:FF")
  };

  /**
   * @brief Load client profiles from the JSON configuration file.
   *
   * Reads profiles from `<appdata>/client_profiles.json`. This should be
   * called once at startup. If the file does not exist or is invalid, an
   * empty profile map is used and a warning is logged.
   */
  void load();

  /**
   * @brief Look up a client profile by device name.
   * @param device_name The device name reported by the streaming client.
   * @return The matching profile, or std::nullopt if no profile exists for this client.
   */
  std::optional<client_profile_t> get_client_profile(const std::string &device_name);

  /**
   * @brief Get all client profiles as a JSON object.
   * @return JSON string with all profiles keyed by device name.
   */
  std::string get_all_profiles_json();

  /**
   * @brief Save or update a client profile.
   * @param device_name The device name to save the profile for.
   * @param profile The profile data to save.
   */
  void save_client_profile(const std::string &device_name, const client_profile_t &profile);

  /**
   * @brief Delete a client profile.
   * @param device_name The device name whose profile to remove.
   * @return true if a profile was deleted, false if it didn't exist.
   */
  bool delete_client_profile(const std::string &device_name);

}  // namespace client_profiles
