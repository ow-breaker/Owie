#include <algorithm>
#include <cmath>
#include <cstdint>

#include "bms_relay.h"
#include "packet.h"

namespace {

template <class T>
inline T clamp(T x, T lower, T upper) {
  return std::min(upper, std::max(x, lower));
}

inline int16_t int16FromNetworkOrder(const void* const p) {
  uint8_t* const charPointer = (uint8_t* const)p;
  return ((uint16_t)(*charPointer)) << 8 | *(charPointer + 1);
}

int openCircuitSocFromVoltage(float voltageVolts) {
  // kindly provided by biell@ in https://github.com/lolwheel/Owie/issues/1
  return clamp((int)(99.9 / (0.8 + pow(1.29, (54 - voltageVolts))) - 10), 0,
               100);
}

}  // namespace

void BmsRelay::batteryPercentageParser(Packet& p) {
  if (p.getType() != 3) {
    return;
  }
  // 0x3 message is just one byte containing battery percentage.
  bms_soc_percent_ = *(int8_t*)p.data();
  if (filtered_total_voltage_millivolts_ == 0) {
    // If we don't have voltage, swallow the packet
    p.setShouldForward(false);
    return;
  }
  overridden_soc_percent_ =
      openCircuitSocFromVoltage(filtered_total_voltage_millivolts_ / 1000.0);
  p.data()[0] = overridden_soc_percent_;
}

void BmsRelay::currentParser(Packet& p) {
  if (p.getType() != 5) {
    return;
  }
  p.setShouldForward(isCharging());

  // 0x5 message encodes current as signed int16.
  // The scaling factor (tested on a Pint) seems to be 0.055
  // i.e. 1 in the data message below corresponds to 0.055 Amps.
  current_ = int16FromNetworkOrder(p.data());

  const unsigned long now = millis_();
  if (last_current_message_millis_ == 0) {
    last_current_ = current_;
    last_current_message_millis_ = now;
    return;
  }
  const int32_t millisElapsed = (int32_t)(now - last_current_message_millis_);
  last_current_message_millis_ = now;
  const int32_t current_times_milliseconds =
      (last_current_ + current_) * millisElapsed / 2;
  last_current_ = current_;
  if (current_times_milliseconds > 0) {
    current_times_milliseconds_used_ += current_times_milliseconds;
  } else {
    current_times_milliseconds_regenerated_ -= current_times_milliseconds;
  }
}

void BmsRelay::bmsSerialParser(Packet& p) {
  if (p.getType() != 6) {
    return;
  }
  // 0x6 message has the BMS serial number encoded inside of it.
  // It is the last seven digits from the sticker on the back of the BMS.
  if (captured_serial_ == 0) {
    for (int i = 0; i < 4; i++) {
      captured_serial_ |= p.data()[i] << (8 * (3 - i));
    }
  }
  if (serial_override_ == 0) {
    return;
  }
  uint32_t serial_override_copy = serial_override_;
  for (int i = 3; i >= 0; i--) {
    p.data()[i] = serial_override_copy & 0xFF;
    serial_override_copy >>= 8;
  }
}

void BmsRelay::cellVoltageParser(Packet& p) {
  if (p.getType() != 2) {
    return;
  }
  // The data in this packet is 16 int16-s. First 15 of them is
  // individual cell voltages in millivolts. The last value is mysterious.
  const uint8_t* const data = p.data();
  uint16_t total_voltage = 0;
  for (int i = 0; i < 15; i++) {
    uint16_t cellVoltage = int16FromNetworkOrder(data + (i << 1));
    total_voltage += cellVoltage;
    cell_millivolts_[i] = cellVoltage;
  }
  total_voltage_millivolts_ = total_voltage;
  if (filtered_total_voltage_millivolts_ == 0) {
    total_voltage_filter_.setTo(total_voltage);
  }
  filtered_total_voltage_millivolts_ =
      total_voltage_filter_.step(total_voltage);
}

void BmsRelay::temperatureParser(Packet& p) {
  if (p.getType() != 4) {
    return;
  }
  int8_t* const temperatures = (int8_t*)p.data();
  int16_t temperature_sum = 0;
  for (int i = 0; i < 5; i++) {
    int8_t temp = temperatures[i];
    temperature_sum += temp;
    temperatures_celsius_[i] = temp;
  }
  avg_temperature_celsius_ = temperature_sum / 5;
}

void BmsRelay::powerOffParser(Packet& p) {
  if (p.getType() != 11) {
    return;
  }
  // We're about to shut down.
  if (powerOffCallback_) {
    powerOffCallback_();
  }
}

void BmsRelay::bmsStatusParser(Packet& p) {
  if (p.getType() != 0) {
    return;
  }
  last_status_byte_ = p.data()[0];

  // Forwarding the status packet during normal operation seems to drive
  // an event loop in the controller that tallies used Ah and eventually
  // makes the board throw Error 23. Swallowing the packet does the trick.
  p.setShouldForward(isCharging() || isBatteryEmpty() ||
                     isBatteryTempOutOfRange() || isBatteryOvercharged());
}
