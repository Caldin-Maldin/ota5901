#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/component.h"

namespace esphome {
namespace ota5901 {

class OTA5901Display : public display::DisplayBuffer {
 public:
  void set_sd_pin(uint8_t pin) { this->sd_pin_ = pin; }
  void set_sclk_pin(uint8_t pin) { this->sclk_pin_ = pin; }
  void set_cs_pin(uint8_t pin) { this->cs_pin_ = pin; }
  void set_reset_pin(uint8_t pin) { this->reset_pin_ = pin; }
  void set_xclk_pin(uint8_t pin) { this->xclk_pin_ = pin; }
  void set_te_pin(uint8_t pin) { this->te_pin_ = pin; this->has_te_pin_ = true; }
  void set_xclk_frequency(uint32_t hz) { this->xclk_frequency_ = hz; }
  void set_spi_half_period_us(uint8_t us) { this->spi_half_period_us_ = us; }
  void set_ready_timeout_ms(uint32_t ms) { this->ready_timeout_ms_ = ms; }
  void set_te_delay_us(uint32_t us) { this->te_delay_us_ = us; }
  void set_skip_unchanged(bool skip) { this->skip_unchanged_ = skip; }
  void set_upload_timeout_ms(uint32_t ms) { this->upload_timeout_ms_ = ms; }

  // Публичный приём сырого 4096-байтового кадра (можно звать из lambda YAML).
  bool apply_frame_raw(const uint8_t *data, size_t len, bool invert);

  // --- Debug helpers ---
  bool debug_render_lambda();
  bool debug_fill_white();
  bool debug_fill_black();
  bool debug_checkerboard(uint8_t cell = 8);
  bool debug_test_pattern();
  bool debug_raw_fill(uint8_t value);
  bool debug_reinit_current();
  uint16_t debug_read_status();

  // --- Браузерный загрузчик (POST /ota5901/frame) ---
  bool web_begin_frame_upload(size_t total);
  bool web_write_frame_chunk(size_t index, const uint8_t *data, size_t len, size_t total);
  bool web_finish_frame_upload();
  void web_abort_frame_upload();

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override;

  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_BINARY;
  }

 protected:
  static constexpr int WIDTH = 256;
  static constexpr int HEIGHT = 128;
  static constexpr size_t FRAMEBUFFER_SIZE = WIDTH * HEIGHT / 8;

  int get_width_internal() override { return WIDTH; }
  int get_height_internal() override { return HEIGHT; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void setup_pins_();
  bool start_xclk_();
  inline void spi_delay_();
  void send9_no_cs_(bool a0, uint8_t value);
  void write_word_(bool a0, uint8_t value);
  inline void cmd_(uint8_t value) { this->write_word_(false, value); }
  inline void data_(uint8_t value) { this->write_word_(true, value); }
  void cmd_data_(uint8_t command, const uint8_t *data, size_t len);

  uint16_t read_reg16_(uint8_t command);
  uint16_t read_status_4a_();
  bool wait_ready_();

  void set_ram_address_(uint16_t address);
  void write_ram_repeat_(uint16_t address, uint8_t value, size_t len);

  void minimal_init_();
  bool ensure_initialized_();
  void attach_te_interrupt_();
  static void te_isr_trampoline_();
  void queue_refresh_();
  void service_refresh_();
  void write_frame_();

  uint32_t framebuffer_hash_() const;

  uint8_t sd_pin_{4};
  uint8_t sclk_pin_{14};
  uint8_t cs_pin_{12};
  uint8_t reset_pin_{13};
  uint8_t xclk_pin_{5};
  uint8_t te_pin_{3};
  bool has_te_pin_{false};

  uint32_t xclk_frequency_{28800};
  uint8_t spi_half_period_us_{1};
  uint32_t ready_timeout_ms_{5000};
  uint32_t te_delay_us_{50};
  bool skip_unchanged_{true};
  uint32_t upload_timeout_ms_{5000};

  std::atomic<bool> web_upload_in_progress_{false};
  std::atomic<size_t> web_upload_received_{0};
  uint32_t web_upload_started_ms_{0};

  volatile uint32_t te_event_counter_{0};
  uint32_t te_wait_sequence_{0};
  bool te_interrupt_attached_{false};

  std::atomic<bool> refresh_pending_{false};
  uint8_t refresh_retry_count_{0};

  bool xclk_started_{false};
  bool controller_initialized_{false};
  bool have_last_hash_{false};
  uint32_t last_hash_{0};

  bool web_handler_registered_{false};

#ifdef USE_ESP32
  int xclk_ledc_channel_{-1};
#endif

  static OTA5901Display *te_isr_instance_;
};

}  // namespace ota5901
}  // namespace esphome