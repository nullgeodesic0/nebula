#pragma once

// Sony DualSense (and DualSense Edge) over Windows HID, USB and Bluetooth:
// input report decoding, IMU calibration and the output report that drives
// the rumble motors and LEDs. Report layouts follow the Linux hid-playstation
// driver and SDL's zlib-licensed SDL_hidapi_ps5.c; this file is an
// independent implementation.

#include "galaxy/switch_controller.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace galaxy::input::dualsense {

inline constexpr std::uint16_t kSonyVendorId = 0x054c;
inline constexpr std::uint16_t kDualSenseProductId = 0x0ce6;
inline constexpr std::uint16_t kDualSenseEdgeProductId = 0x0df2;

inline constexpr std::uint8_t kInputReportUsb = 0x01;       // 64 bytes
inline constexpr std::uint8_t kInputReportBluetooth = 0x31; // 78 bytes
inline constexpr std::size_t kInputReportUsbBytes = 64;
inline constexpr std::size_t kInputReportBluetoothBytes = 78;
inline constexpr std::uint8_t kOutputReportUsb = 0x02;      // 64 bytes with the id
inline constexpr std::uint8_t kOutputReportBluetooth = 0x31;
inline constexpr std::size_t kOutputReportUsbBytes = 64;
inline constexpr std::size_t kOutputReportBluetoothBytes = 78;
// Feature report holding the IMU calibration. Reading it over Bluetooth also
// switches the controller from the 10-byte simple report to report 0x31.
inline constexpr std::uint8_t kFeatureCalibration = 0x05;
inline constexpr std::size_t kFeatureCalibrationBytes = 41;

[[nodiscard]] constexpr bool is_dualsense_product(std::uint16_t product) noexcept {
    return product == kDualSenseProductId || product == kDualSenseEdgeProductId;
}

// Raw int16 sensor counts to physical units. Defaults are the nominal
// +-2000 dps gyro and 8192 counts per g accelerometer.
struct Calibration {
    std::array<float, 3> gyro_bias{};    // counts
    std::array<float, 3> gyro_scale{};   // rad/s per count
    std::array<float, 3> accel_bias{};   // counts
    std::array<float, 3> accel_scale{};  // g per count
    Calibration() noexcept;
};

// Parses the calibration feature report (byte 0 = report id 0x05). Degenerate
// values (a zero range) keep the nominal defaults for that axis.
[[nodiscard]] Calibration parse_calibration(std::span<const std::uint8_t> report) noexcept;

// Decodes an input report: USB 0x01 (64 bytes) or Bluetooth 0x31 (78 bytes).
// Fills buttons (unified pad bits), sticks, analog triggers and one IMU sample
// in the SDL sensor frame. False for other ids, short reports, or the
// Bluetooth simple report (0x01 of 10 bytes) that precedes the full mode.
[[nodiscard]] bool decode_report(std::span<const std::uint8_t> report, const Calibration& cal,
                                 switch_hid::DecodedState& out) noexcept;

// The 32-bit microsecond/3 sensor timestamp of the last decoded report, for dt.
[[nodiscard]] std::uint32_t report_sensor_timestamp(std::span<const std::uint8_t> report) noexcept;

// Rumble: compatible-vibration motors, 0..255 each. `lights` also sets the
// lightbar and player LED (sent once on connect).
struct OutputState {
    std::uint8_t motor_left = 0;   // strong (low frequency)
    std::uint8_t motor_right = 0;  // weak (high frequency)
    bool set_motors = true;
    bool set_lights = false;
    std::uint8_t player_leds = 0x04;  // centre LED
    std::array<std::uint8_t, 3> lightbar{0, 0, 0x40};
};

// Writes a complete output report into `out` (zero-filled to its size).
// `sequence` is the Bluetooth output sequence (wraps at 16). Returns the
// number of bytes to write: kOutputReport*Bytes, or 0 if `out` is too small.
[[nodiscard]] std::size_t build_output_report(bool bluetooth, std::uint8_t sequence,
                                              const OutputState& state,
                                              std::span<std::uint8_t> out) noexcept;

// CRC-32 (zlib polynomial) of {0xA2, data...}, the Bluetooth output checksum.
[[nodiscard]] std::uint32_t bluetooth_output_crc(std::span<const std::uint8_t> data) noexcept;

}  // namespace galaxy::input::dualsense
