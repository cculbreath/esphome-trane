import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, text_sensor
from esphome.components.climate import ClimateMode, ClimatePreset
from esphome.const import CONF_ID
from esphome import automation

trane_hvac_ns = cg.esphome_ns.namespace("trane_hvac")
TraneClimate = trane_hvac_ns.class_(
    "TraneClimate", climate.Climate, cg.Component
)

CONF_CURRENT_TEMPERATURE_SENSOR = "current_temperature_sensor"
CONF_HEAT_SETPOINT_SENSOR = "heat_setpoint_sensor"
CONF_COOL_SETPOINT_SENSOR = "cool_setpoint_sensor"
CONF_MODE_TEXT_SENSOR = "mode_text_sensor"
CONF_DEMAND_STAGE_SENSOR = "demand_stage_sensor"
CONF_SET_MODE_ACTION = "set_mode_action"
CONF_SET_TEMPERATURE_ACTION = "set_temperature_action"
CONF_SET_PRESET_ACTION = "set_preset_action"

CONFIG_SCHEMA = climate.climate_schema(TraneClimate).extend({
    cv.Optional(CONF_CURRENT_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
    cv.Required(CONF_HEAT_SETPOINT_SENSOR): cv.use_id(sensor.Sensor),
    cv.Required(CONF_COOL_SETPOINT_SENSOR): cv.use_id(sensor.Sensor),
    cv.Required(CONF_MODE_TEXT_SENSOR): cv.use_id(text_sensor.TextSensor),
    cv.Optional(CONF_DEMAND_STAGE_SENSOR): cv.use_id(text_sensor.TextSensor),
    cv.Optional(CONF_SET_MODE_ACTION): automation.validate_automation(single=True),
    cv.Optional(CONF_SET_TEMPERATURE_ACTION): automation.validate_automation(single=True),
    cv.Optional(CONF_SET_PRESET_ACTION): automation.validate_automation(single=True),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    if CONF_CURRENT_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_SENSOR])
        cg.add(var.set_current_temperature_sensor(sens))

    heat = await cg.get_variable(config[CONF_HEAT_SETPOINT_SENSOR])
    cg.add(var.set_heat_setpoint_sensor(heat))

    cool = await cg.get_variable(config[CONF_COOL_SETPOINT_SENSOR])
    cg.add(var.set_cool_setpoint_sensor(cool))

    mode = await cg.get_variable(config[CONF_MODE_TEXT_SENSOR])
    cg.add(var.set_mode_sensor(mode))

    if CONF_DEMAND_STAGE_SENSOR in config:
        demand = await cg.get_variable(config[CONF_DEMAND_STAGE_SENSOR])
        cg.add(var.set_demand_stage_sensor(demand))

    if CONF_SET_MODE_ACTION in config:
        await automation.build_automation(
            var.get_mode_trigger(),
            [(ClimateMode, "mode")],
            config[CONF_SET_MODE_ACTION],
        )

    if CONF_SET_TEMPERATURE_ACTION in config:
        await automation.build_automation(
            var.get_temperature_trigger(),
            [(cg.float_, "hsp_f"), (cg.float_, "csp_f")],
            config[CONF_SET_TEMPERATURE_ACTION],
        )

    if CONF_SET_PRESET_ACTION in config:
        await automation.build_automation(
            var.get_preset_trigger(),
            [(ClimatePreset, "preset")],
            config[CONF_SET_PRESET_ACTION],
        )
