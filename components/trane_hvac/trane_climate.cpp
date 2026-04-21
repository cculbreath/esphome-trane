#include "trane_climate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace trane_hvac {

static const char *const TAG = "trane_hvac.climate";

void TraneClimate::dump_config() {
  LOG_CLIMATE("", "Trane HVAC Climate", this);
}

climate::ClimateTraits TraneClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.set_supports_current_temperature(true);
  traits.set_supports_two_point_target_temperature(true);
  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_HEAT,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT_COOL,
    climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_presets({
    climate::CLIMATE_PRESET_HOME,
    climate::CLIMATE_PRESET_AWAY,
    climate::CLIMATE_PRESET_SLEEP,
    climate::CLIMATE_PRESET_BOOST,
  });
  traits.set_visual_min_temperature(15.5f);   // 60°F
  traits.set_visual_max_temperature(29.4f);   // 85°F
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void TraneClimate::setup() {
  // Current temperature
  if (current_temp_sensor_ != nullptr) {
    current_temp_sensor_->add_on_state_callback([this](float state) {
      if (!std::isnan(state)) {
        this->current_temperature = (state - 32.0f) * 5.0f / 9.0f;
        this->publish_state();
      }
    });
  }

  // Heat setpoint
  heat_setpoint_sensor_->add_on_state_callback([this](float state) {
    if (!std::isnan(state)) {
      this->target_temperature_low = (state - 32.0f) * 5.0f / 9.0f;
      this->publish_state();
    }
  });

  // Cool setpoint
  cool_setpoint_sensor_->add_on_state_callback([this](float state) {
    if (!std::isnan(state)) {
      this->target_temperature_high = (state - 32.0f) * 5.0f / 9.0f;
      this->publish_state();
    }
  });

  // Mode text sensor — maps ZoneSettings.ZoneMode (as text) to climate modes.
  // ZoneMode is the configured mode (set by user), not the operating state.
  mode_sensor_->add_on_state_callback([this](const std::string &state) {
    if (state == "heat")
      this->mode = climate::CLIMATE_MODE_HEAT;
    else if (state == "cool")
      this->mode = climate::CLIMATE_MODE_COOL;
    else if (state == "heat_cool" || state == "auto")
      this->mode = climate::CLIMATE_MODE_HEAT_COOL;
    else if (state == "off")
      this->mode = climate::CLIMATE_MODE_OFF;
    this->publish_state();
  });

  // Demand stage sensor — maps SystemOpStatus.C to ClimateAction
  // Known stage strings from CAN bus observations:
  //   "--"          = idle between cycles
  //   "HP Stage 1"  = heat pump low stage
  //   "HP Stage 2"  = heat pump high stage
  //   "HP1+ID1"     = heat pump stage 1 + induced draft stage 1
  //   "HP1+ID2"     = heat pump stage 1 + induced draft stage 2
  //   "HP2+ID1"     = heat pump stage 2 + induced draft stage 1 (defrost/aux)
  //   "HP2+ID2"     = heat pump stage 2 + induced draft stage 2 (defrost/aux)
  //   "ID Stage 1"  = induced draft / gas stage 1 (furnace only)
  //   "ID Stage 2"  = induced draft / gas stage 2 (furnace only)
  if (demand_sensor_ != nullptr) {
    demand_sensor_->add_on_state_callback([this](const std::string &state) {
      if (this->mode == climate::CLIMATE_MODE_OFF) {
        this->action = climate::CLIMATE_ACTION_OFF;
      } else if (state == "--" || state.empty()) {
        // "--" means compressor idle — could still be fan running
        // action will be updated by indoor_unit_state if blower is active
        this->action = climate::CLIMATE_ACTION_IDLE;
      } else if (state.find("Cool") != std::string::npos) {
        this->action = climate::CLIMATE_ACTION_COOLING;
      } else if (state.find("HP") != std::string::npos ||
                 state.find("ID") != std::string::npos) {
        // HP Stage 1, HP Stage 2, HP1+ID1, HP1+ID2, HP2+ID1, HP2+ID2, ID Stage 1, ID Stage 2
        this->action = (this->mode == climate::CLIMATE_MODE_COOL)
                         ? climate::CLIMATE_ACTION_COOLING
                         : climate::CLIMATE_ACTION_HEATING;
      } else {
        this->action = climate::CLIMATE_ACTION_IDLE;
      }
      this->publish_state();
    });
  }

  // Subscribe to indoor unit state for fan action detection
  // IndoorStatus.D = "B" means blower running, "A" means transition/off
  // When demand stage is "--" but blower is running = fan post-cycle state
  if (indoor_unit_state_sensor_ != nullptr) {
    indoor_unit_state_sensor_->add_on_state_callback([this](const std::string &state) {
      if (state == "B" && this->action == climate::CLIMATE_ACTION_IDLE) {
        this->action = climate::CLIMATE_ACTION_FAN;
        this->publish_state();
      } else if (state == "A" && this->action == climate::CLIMATE_ACTION_FAN) {
        this->action = climate::CLIMATE_ACTION_IDLE;
        this->publish_state();
      }
    });
  }

  // Seed initial values from current sensor states
  if (current_temp_sensor_ != nullptr && !std::isnan(current_temp_sensor_->state))
    this->current_temperature = (current_temp_sensor_->state - 32.0f) * 5.0f / 9.0f;
  if (!std::isnan(heat_setpoint_sensor_->state))
    this->target_temperature_low = (heat_setpoint_sensor_->state - 32.0f) * 5.0f / 9.0f;
  if (!std::isnan(cool_setpoint_sensor_->state))
    this->target_temperature_high = (cool_setpoint_sensor_->state - 32.0f) * 5.0f / 9.0f;
}

void TraneClimate::control(const climate::ClimateCall &call) {
  // --- Mode change ----------------------------------------------------------
  if (call.get_mode().has_value()) {
    climate::ClimateMode new_mode = *call.get_mode();
    this->mode = new_mode;
    this->publish_state();
    mode_trigger_.trigger(new_mode);
  }

  // --- Setpoint change ------------------------------------------------------
  if (call.get_target_temperature_low().has_value() ||
      call.get_target_temperature_high().has_value()) {
    float hsp_c = this->target_temperature_low;
    float csp_c = this->target_temperature_high;
    if (call.get_target_temperature_low().has_value())
      hsp_c = *call.get_target_temperature_low();
    if (call.get_target_temperature_high().has_value())
      csp_c = *call.get_target_temperature_high();

    this->target_temperature_low  = hsp_c;
    this->target_temperature_high = csp_c;
    this->publish_state();

    // Convert °C → °F for CAN transmission
    float hsp_f = hsp_c * 9.0f / 5.0f + 32.0f;
    float csp_f = csp_c * 9.0f / 5.0f + 32.0f;
    ESP_LOGI(TAG, "Setpoint: Hsp=%.0f°F Csp=%.0f°F", hsp_f, csp_f);
    temperature_trigger_.trigger(hsp_f, csp_f);
  }

  // --- Preset change --------------------------------------------------------
  if (call.get_preset().has_value()) {
    climate::ClimatePreset new_preset = *call.get_preset();
    this->preset = new_preset;
    this->publish_state();
    preset_trigger_.trigger(new_preset);
  }
}

}  // namespace trane_hvac
}  // namespace esphome
