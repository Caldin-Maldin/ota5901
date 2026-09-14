import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_ID, CONF_LAMBDA
from esphome.core import CORE
from esphome import pins

AUTO_LOAD = ["display"]
DEPENDENCIES = ["web_server"]

CONF_SD_PIN = "sd_pin"
CONF_SCLK_PIN = "sclk_pin"
CONF_CS_PIN = "cs_pin"
CONF_RESET_PIN = "reset_pin"
CONF_XCLK_PIN = "xclk_pin"
CONF_TE_PIN = "te_pin"
CONF_XCLK_FREQUENCY = "xclk_frequency"
CONF_SPI_HALF_PERIOD_US = "spi_half_period_us"
CONF_READY_TIMEOUT_MS = "ready_timeout_ms"
CONF_TE_DELAY_US = "te_delay_us"
CONF_SKIP_UNCHANGED = "skip_unchanged"

ota5901_ns = cg.esphome_ns.namespace("ota5901")
OTA5901Display = ota5901_ns.class_(
    "OTA5901Display", cg.PollingComponent, display.DisplayBuffer
)


def _validate_target(config):
    if not (CORE.is_esp8266 or CORE.is_esp32):
        raise cv.Invalid(
            "OTA5901 external component supports ESP8266 and ESP32 only"
        )
    if CORE.is_esp8266 and config.get(CONF_TE_PIN) == 16:
        raise cv.Invalid(
            "OTA5901 on ESP8266 requires an interrupt-capable TE pin; "
            "GPIO16/D0 cannot be used. Move TE to GPIO3/RX (recommended)."
        )
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(OTA5901Display),
            cv.Required(CONF_SD_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_SCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_XCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_TE_PIN): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_XCLK_FREQUENCY, default=28800): cv.int_range(
                min=28000, max=200000
            ),
            cv.Optional(CONF_SPI_HALF_PERIOD_US, default=1): cv.int_range(
                min=1, max=20
            ),
            cv.Optional(CONF_READY_TIMEOUT_MS, default=5000): cv.int_range(
                min=250, max=30000
            ),
            cv.Optional(CONF_TE_DELAY_US, default=50): cv.int_range(
                min=0, max=10000
            ),
            cv.Optional(CONF_SKIP_UNCHANGED, default=True): cv.boolean,
        }
    ).extend(cv.polling_component_schema("60s")),
    _validate_target,
)


async def to_code(config):
    if CORE.is_esp8266:
        # ESPHome 2026.1+ removes ESP8266 waveform support from the build
        # unless a component explicitly requests it. XCLK is generated with
        # startWaveformClockCycles() on ESP8266 only.
        from esphome.components.esp8266.const import require_waveform
        require_waveform()

    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    cg.add(var.set_sd_pin(config[CONF_SD_PIN]))
    cg.add(var.set_sclk_pin(config[CONF_SCLK_PIN]))
    cg.add(var.set_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    cg.add(var.set_xclk_pin(config[CONF_XCLK_PIN]))
    if CONF_TE_PIN in config:
        cg.add(var.set_te_pin(config[CONF_TE_PIN]))

    cg.add(var.set_xclk_frequency(config[CONF_XCLK_FREQUENCY]))
    cg.add(var.set_spi_half_period_us(config[CONF_SPI_HALF_PERIOD_US]))
    cg.add(var.set_ready_timeout_ms(config[CONF_READY_TIMEOUT_MS]))
    cg.add(var.set_te_delay_us(config[CONF_TE_DELAY_US]))
    cg.add(var.set_skip_unchanged(config[CONF_SKIP_UNCHANGED]))



    # --- FIX: attach the YAML lambda so the display actually renders ---
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA],
            [(display.Display.operator("ref"), "it")],
            return_type=cg.void,
        )
        cg.add(var.set_writer(lambda_))