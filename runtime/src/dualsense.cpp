#include "galaxy/dualsense.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace galaxy::input::dualsense {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Offsets of the shared state block inside an input report.
constexpr std::size_t kStateOffsetUsb = 1;
constexpr std::size_t kStateOffsetBluetooth = 2;
// Layout from the state block start.
constexpr std::size_t kStLeftX = 0;
constexpr std::size_t kStLeftY = 1;
constexpr std::size_t kStRightX = 2;
constexpr std::size_t kStRightY = 3;
constexpr std::size_t kStTriggerLeft = 4;
constexpr std::size_t kStTriggerRight = 5;
constexpr std::size_t kStButtons = 7;   // 3 bytes used of 4
constexpr std::size_t kStGyro = 15;     // pitch, yaw, roll (s16 LE)
constexpr std::size_t kStAccel = 21;    // x, y, z (s16 LE)
constexpr std::size_t kStTimestamp = 27;
constexpr std::size_t kStateBytes = 31;

std::int16_t s16(const std::uint8_t* d) noexcept {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(d[0] | (d[1] << 8)));
}

// 0..255 with 128 at rest to -1..1.
float stick_unit(std::uint8_t raw) noexcept {
    const float centered = static_cast<float>(static_cast<int>(raw) - 128);
    return std::clamp(centered / (centered < 0.0f ? 128.0f : 127.0f), -1.0f, 1.0f);
}

constexpr std::uint32_t crc_step(std::uint32_t crc, std::uint8_t byte) noexcept {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
    }
    return crc;
}

}  // namespace

Calibration::Calibration() noexcept {
    const float gyro = 2000.0f / 32768.0f * kPi / 180.0f;  // +-2000 dps over int16
    gyro_scale = {gyro, gyro, gyro};
    accel_scale = {1.0f / 8192.0f, 1.0f / 8192.0f, 1.0f / 8192.0f};
}

Calibration parse_calibration(std::span<const std::uint8_t> r) noexcept {
    Calibration cal{};
    if (r.size() < 35 || r[0] != kFeatureCalibration) return cal;
    const int speed_2x = s16(&r[19]) + s16(&r[21]);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        cal.gyro_bias[axis] = static_cast<float>(s16(&r[1 + axis * 2]));
        const int plus = s16(&r[7 + axis * 4]);
        const int minus = s16(&r[9 + axis * 4]);
        const int range = plus - minus;
        if (range > 0 && speed_2x > 0) {
            // dps per count = speed_2x / range.
            cal.gyro_scale[axis] = static_cast<float>(speed_2x) / static_cast<float>(range) * kPi / 180.0f;
        }
        const int accel_plus = s16(&r[23 + axis * 4]);
        const int accel_minus = s16(&r[25 + axis * 4]);
        const int accel_range = accel_plus - accel_minus;
        if (accel_range > 0) {
            cal.accel_bias[axis] = static_cast<float>(accel_plus) - static_cast<float>(accel_range) * 0.5f;
            cal.accel_scale[axis] = 2.0f / static_cast<float>(accel_range);  // g per count
        }
    }
    return cal;
}

bool decode_report(std::span<const std::uint8_t> report, const Calibration& cal,
                   switch_hid::DecodedState& out) noexcept {
    std::size_t base = 0;
    if (report.size() >= kInputReportUsbBytes && report[0] == kInputReportUsb) {
        base = kStateOffsetUsb;
    } else if (report.size() >= kInputReportBluetoothBytes && report[0] == kInputReportBluetooth) {
        base = kStateOffsetBluetooth;
    } else {
        return false;
    }
    if (report.size() < base + kStateBytes) return false;
    const std::uint8_t* s = report.data() + base;

    out = {};
    out.normalized = true;
    out.stick_norm = {stick_unit(s[kStLeftX]), -stick_unit(s[kStLeftY]),
                      stick_unit(s[kStRightX]), -stick_unit(s[kStRightY])};
    out.trigger_norm = {static_cast<float>(s[kStTriggerLeft]) / 255.0f,
                        static_cast<float>(s[kStTriggerRight]) / 255.0f};

    const std::uint8_t b0 = s[kStButtons];
    const std::uint8_t b1 = s[kStButtons + 1];
    const std::uint8_t b2 = s[kStButtons + 2];
    std::uint32_t buttons = 0;
    const auto set = [&buttons](bool on, unsigned bit) {
        if (on) buttons |= pad::bit(bit);
    };
    // Hat: 0 up, clockwise to 7 up-left, 8 released.
    const unsigned hat = b0 & 0x0fu;
    set(hat == 0 || hat == 1 || hat == 7, pad::kDpadUpBit);
    set(hat == 3 || hat == 4 || hat == 5, pad::kDpadDownBit);
    set(hat == 5 || hat == 6 || hat == 7, pad::kDpadLeftBit);
    set(hat == 1 || hat == 2 || hat == 3, pad::kDpadRightBit);
    set((b0 & 0x10u) != 0, pad::kWestBit);    // Square
    set((b0 & 0x20u) != 0, pad::kSouthBit);   // Cross
    set((b0 & 0x40u) != 0, pad::kEastBit);    // Circle
    set((b0 & 0x80u) != 0, pad::kNorthBit);   // Triangle
    set((b1 & 0x01u) != 0, pad::kLeftShoulderBit);   // L1
    set((b1 & 0x02u) != 0, pad::kRightShoulderBit);  // R1
    set((b1 & 0x10u) != 0, pad::kBackBit);    // Create
    set((b1 & 0x20u) != 0, pad::kStartBit);   // Options
    set((b1 & 0x40u) != 0, pad::kLeftStickBit);   // L3
    set((b1 & 0x80u) != 0, pad::kRightStickBit);  // R3
    set((b2 & 0x01u) != 0, pad::kHomeBit);     // PS
    set((b2 & 0x02u) != 0, pad::kCaptureBit);  // touchpad click
    out.buttons = buttons;
    out.left_trigger = (b1 & 0x04u) != 0 || out.trigger_norm[0] > 0.5f;
    out.right_trigger = (b1 & 0x08u) != 0 || out.trigger_norm[1] > 0.5f;

    // One IMU sample per report, in the SDL sensor frame the DualSense already uses.
    switch_hid::ImuSample sample{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        sample.gyro[axis] =
            (static_cast<float>(s16(s + kStGyro + axis * 2)) - cal.gyro_bias[axis]) * cal.gyro_scale[axis];
        sample.accel[axis] =
            (static_cast<float>(s16(s + kStAccel + axis * 2)) - cal.accel_bias[axis]) * cal.accel_scale[axis];
    }
    out.imu[0] = sample;
    out.imu_count = 1;
    return true;
}

std::uint32_t report_sensor_timestamp(std::span<const std::uint8_t> report) noexcept {
    std::size_t base = 0;
    if (report.size() >= kInputReportUsbBytes && report[0] == kInputReportUsb) {
        base = kStateOffsetUsb;
    } else if (report.size() >= kInputReportBluetoothBytes && report[0] == kInputReportBluetooth) {
        base = kStateOffsetBluetooth;
    } else {
        return 0;
    }
    if (report.size() < base + kStateBytes) return 0;
    const std::uint8_t* t = report.data() + base + kStTimestamp;
    return static_cast<std::uint32_t>(t[0]) | (static_cast<std::uint32_t>(t[1]) << 8) |
           (static_cast<std::uint32_t>(t[2]) << 16) | (static_cast<std::uint32_t>(t[3]) << 24);
}

std::uint32_t bluetooth_output_crc(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffu;
    crc = crc_step(crc, 0xa2);
    for (const std::uint8_t byte : data) crc = crc_step(crc, byte);
    return ~crc;
}

std::size_t build_output_report(bool bluetooth, std::uint8_t sequence, const OutputState& state,
                                std::span<std::uint8_t> out) noexcept {
    const std::size_t total = bluetooth ? kOutputReportBluetoothBytes : kOutputReportUsbBytes;
    if (out.size() < total) return 0;
    std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(total), std::uint8_t{0});
    std::size_t common = 1;  // first byte of the shared block
    if (bluetooth) {
        out[0] = kOutputReportBluetooth;
        out[1] = static_cast<std::uint8_t>((sequence & 0x0fu) << 4);
        out[2] = 0x10;  // output tag
        common = 3;
    } else {
        out[0] = kOutputReportUsb;
    }
    std::uint8_t* c = out.data() + common;
    // valid_flag0: bit 0 compatible vibration, bit 1 haptics select.
    if (state.set_motors) c[0] |= 0x03;
    // valid_flag1: bit 2 lightbar control, bit 4 player LED control.
    if (state.set_lights) c[1] |= 0x04 | 0x10;
    c[2] = state.motor_right;
    c[3] = state.motor_left;
    // valid_flag2: bit 1 lightbar setup control (stops the connect animation).
    if (state.set_lights) {
        c[39] |= 0x02;
        c[42] = 0x02;  // lightbar setup: light out, so the colour below applies
        c[44] = state.player_leds;
        c[45] = state.lightbar[0];
        c[46] = state.lightbar[1];
        c[47] = state.lightbar[2];
    }
    if (bluetooth) {
        const std::uint32_t crc = bluetooth_output_crc(
            std::span<const std::uint8_t>(out.data(), kOutputReportBluetoothBytes - 4));
        out[kOutputReportBluetoothBytes - 4] = static_cast<std::uint8_t>(crc);
        out[kOutputReportBluetoothBytes - 3] = static_cast<std::uint8_t>(crc >> 8);
        out[kOutputReportBluetoothBytes - 2] = static_cast<std::uint8_t>(crc >> 16);
        out[kOutputReportBluetoothBytes - 1] = static_cast<std::uint8_t>(crc >> 24);
    }
    return total;
}

}  // namespace galaxy::input::dualsense
