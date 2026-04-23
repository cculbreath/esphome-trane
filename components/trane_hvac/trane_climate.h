#pragma once
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace trane_hvac {

// Forward-declare trigger types so climate.py can bind automations to them
class TraneModeTrigger;
class TraneTemperatureTrigger;
class TranePresetTrigger;

class TraneClimate : public climate::Climate, public Component {
 public:
  void setup() override;
  void dump_config() override;

  // Sensor wiring
  void set_current_temperature_sensor(sensor::Sensor *s) { current_temp_sensor_ = s; }
  void set_heat_setpoint_sensor(sensor::Sensor *s) { heat_setpoint_sensor_ = s; }
  void set_cool_setpoint_sensor(sensor::Sensor *s) { cool_setpoint_sensor_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s) { mode_sensor_ = s; }
  void set_demand_stage_sensor(text_sensor::TextSensor *s) { demand_sensor_ = s; }
  void set_indoor_unit_state_sensor(text_sensor::TextSensor *s) { indoor_unit_state_sensor_ = s; }

  // Trigger accessors for climate.py automation binding
  Trigger<climate::ClimateMode> *get_mode_trigger() { return &mode_trigger_; }
  Trigger<float, float> *get_temperature_trigger() { return &temperature_trigger_; }
  Trigger<climate::ClimatePreset> *get_preset_trigger() { return &preset_trigger_; }

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  // Computes the ClimateAction for a given configured mode and the
  // demand-stage text. Shared by the demand callback (reacts to CAN pushes)
  // and the mode callback (re-seeds after mode change without waiting for
  // the next demand broadcast — see the comment in setup()).
  climate::ClimateAction compute_action_(climate::ClimateMode mode,
                                         const std::string &demand) const;

  sensor::Sensor *current_temp_sensor_{nullptr};
  sensor::Sensor *heat_setpoint_sensor_{nullptr};
  sensor::Sensor *cool_setpoint_sensor_{nullptr};
  text_sensor::TextSensor *mode_sensor_{nullptr};
  text_sensor::TextSensor *demand_sensor_{nullptr};
  text_sensor::TextSensor *indoor_unit_state_sensor_{nullptr};

  Trigger<climate::ClimateMode> mode_trigger_;
  Trigger<float, float> temperature_trigger_;   // args: hsp_f, csp_f (°F)
  Trigger<climate::ClimatePreset> preset_trigger_;
};

}  // namespace trane_hvac
}  // namespace esphome
