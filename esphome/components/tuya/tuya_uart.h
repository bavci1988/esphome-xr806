#pragma once

#include <cinttypes>
#include <memory>
#include <vector>

#include "tuya.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/frame_reader.h"
#include "esphome/core/helpers.h"

#ifdef USE_TUYA_LOW_POWER
#include <array>
#include <initializer_list>

#include "esphome/core/automation.h"
#include "tuya_low_power.h"
#include "tuya_low_power_product.h"
#include "tuya_low_power_autobaud.h"
#include "tuya_low_power_sleep_window.h"
#include "tuya_low_power_product_retry.h"
#include "tuya_low_power_standby_state.h"
#include "tuya_low_power_wire_transactions.h"
#include "tuya_low_power_uart_errors.h"
#include "tuya_low_power_host.h"
#endif  // USE_TUYA_LOW_POWER

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/time.h"
#endif

namespace esphome::tuya {

// Longest frame accepted, as in Tasmota; keeps the reader's positions 8 bit
static constexpr size_t MAX_FRAME_SIZE = 255;

// 55 AA, version, command, payload length (2, BE), payload, checksum
using TuyaLength = PayloadLengthU16BE<LengthField{.offset = 4, .trailer = 1}>;
void warn_too_long(size_t len);
using TuyaFrame = FrameFormat<FrameHeader<0x55, 0xAA>, TuyaLength, WarnIfLongerThan<MAX_FRAME_SIZE, warn_too_long>>;

enum class TuyaCommandType : uint8_t {
  HEARTBEAT = 0x00,
  PRODUCT_QUERY = 0x01,
  CONF_QUERY = 0x02,
  WIFI_STATE = 0x03,
  WIFI_RESET = 0x04,
  WIFI_SELECT = 0x05,
  DATAPOINT_DELIVER = 0x06,
  DATAPOINT_REPORT_ASYNC = 0x07,
  DATAPOINT_QUERY = 0x08,
  GMT_TIME_QUERY = 0x0C,
  WIFI_TEST = 0x0E,
  LOCAL_TIME_QUERY = 0x1C,
  DATAPOINT_REPORT_SYNC = 0x22,
  DATAPOINT_REPORT_ACK = 0x23,
  WIFI_RSSI = 0x24,
  VACUUM_MAP_UPLOAD = 0x28,
  GET_NETWORK_STATUS = 0x2B,
  EXTENDED_SERVICES = 0x34,
};

enum class TuyaExtendedServicesCommandType : uint8_t {
  RESET_NOTIFICATION = 0x04,
  MODULE_RESET = 0x05,
  UPDATE_IN_PROGRESS = 0x0A,
};

enum class TuyaInitState : uint8_t {
  INIT_HEARTBEAT = 0x00,
  INIT_PRODUCT,
  INIT_CONF,
  INIT_WIFI,
  INIT_DATAPOINT,
  INIT_DONE,
};

struct TuyaCommand {
  TuyaCommandType cmd;
  std::vector<uint8_t> payload;
};

/// Tuya MCU controller connected over UART (the standard Tuya serial protocol).
class TuyaUART final : public Tuya, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_status_pin(InternalGPIOPin *status_pin) { this->status_pin_ = status_pin; }
#ifdef USE_TUYA_LOW_POWER
  void set_auto_baud(bool auto_baud) { this->auto_baud_ = auto_baud; }
  void set_mcu_wake_pin(GPIOPin *pin) { this->mcu_wake_pin_ = pin; }
  /// Connects the platform's power management; call before setup(). The calls below follow the contract in
  /// tuya_low_power_host.h and run on the main loop thread.
  void set_low_power_host(TuyaLowPowerHost *host) { this->lp_host_ = host; }
  bool request_low_power_refresh();
  /// Diagnostics: sends only the start of an MCU firmware update (CMD0A, or CMD0D for version-0 frames) for an
  /// image of `image_size` bytes and logs the MCU's answer. No data follows; the running MCU firmware is untouched.
  bool low_power_probe_mcu_update(uint32_t image_size);
  /// Sends an MCU firmware image with the Tuya MCU update commands (start, then packets of offset and data, each
  /// acknowledged, then an empty packet at the end). The MCU checks the image itself before it installs it. `image`
  /// must stay valid until the transfer ends. Restarts the module afterwards, so it detects the MCU anew.
  bool low_power_update_mcu(const uint8_t *image, size_t size);
  /// Firmware version from the MCU's product info ("v"), empty until it was reported.
  const char *low_power_mcu_version() const { return this->lp_product_.version.data(); }
  /// True when the module may enter standby now. Returns false once on purpose right after it sent CMD12/01. After
  /// every false return, call low_power_standby_cancelled() and try again later.
  bool low_power_prepare_for_standby();
  /// The standby attempt was not made; tells the MCU that the module stays awake when it was told otherwise.
  void low_power_standby_cancelled();
  /// After a standby: expected_source is true when one of the configured wake sources woke the module, wake_gpio
  /// when the MCU's wake line did, wake_timer when the wake timer did. The platform clears its own standby request
  /// itself.
  void low_power_standby_resumed(bool expected_source, bool wake_gpio, bool wake_timer = false);
#endif  // USE_TUYA_LOW_POWER
  /// Called once after boot, when the MCU takes commands: after the initialization of the standard protocol,
  /// and for protocol low_power once the MCU reports its changes on its own.
  template<typename F> void add_on_mcu_ready_callback(F &&callback) {
    this->mcu_ready_callback_.add(std::forward<F>(callback));
  }

  TuyaInitState get_init_state();
#ifdef USE_TIME
  void set_time_id(time::RealTimeClock *time_id) { this->time_id_ = time_id; }
  void set_force_time_sync(bool force_time_sync) { this->force_time_sync_ = force_time_sync; }
#endif

 protected:
  void dump_mcu_config_();
  /// Returns false for a frame that fails the checksum
  bool handle_frame_(const uint8_t *frame, size_t len);
  void handle_datapoints_(const uint8_t *buffer, size_t len);

  void handle_command_(uint8_t command, uint8_t version, const uint8_t *buffer, size_t len);
  void notify_mcu_ready_() {
    if (this->mcu_ready_)
      return;
    this->mcu_ready_ = true;
    this->mcu_ready_callback_.call();
  }
#ifdef USE_TUYA_LOW_POWER
  // Entry points called from the standard Tuya code paths in tuya_uart.cpp.
  void setup_low_power_();
  void loop_low_power_();
  bool check_low_power_uart_rx_errors_();
  uint32_t low_power_uart_rx_error_count_() const;
  void dump_low_power_config_();
  void handle_low_power_frame_(uint8_t command, uint8_t version, const uint8_t *buffer, size_t len);
  void send_low_power_raw_command_(const TuyaCommand &command);
  bool begin_low_power_command_(const TuyaCommand &command);
  void send_low_power_datapoint_(uint8_t datapoint_id, TuyaDatapointType datapoint_type, const uint8_t *data,
                                 size_t len);
  void handle_low_power_command_(uint8_t command, uint8_t version, const uint8_t *data, size_t len);
  void send_low_power_direct_(uint8_t command, const uint8_t *data, size_t len);
  void remember_queued_value_(uint8_t datapoint_id, TuyaDatapointType datapoint_type, const uint8_t *data, size_t len);
  // The list lives on the stack, so short replies need no heap
  void send_low_power_direct_(uint8_t command, std::initializer_list<uint8_t> payload) {
    this->send_low_power_direct_(command, payload.begin(), payload.size());
  }
  void send_low_power_time_(uint8_t command);
  void send_low_power_network_();
  void poll_low_power_autobaud_();
  void send_low_power_product_query_(bool direct_probe);
  void send_product_query_now_(bool direct_probe);
  void poll_low_power_product_probe_();
  void begin_mcu_wake_pulse_();
  bool mcu_wake_pulse_running_();
  void finish_low_power_sync_();
  void poll_low_power_v3_status_();
  void process_low_power_queue_();
  void poll_low_power_product_recovery_();
  void invalidate_low_power_mode_epoch_();
  // The sleep handshake, in tuya_low_power_sleep.cpp
  void setup_low_power_sleep_();
  void poll_low_power_sleep_();
  // A byte or frame arrived: stops a pending sleep handshake
  void on_low_power_rx_byte_();
  void on_low_power_frame_(uint8_t command, const uint8_t *data, size_t len);
  void stop_low_power_sleep_();
  void preempt_low_power_sleep_();
  bool low_power_network_held_() const;
  void configure_low_power_wake_lines_();
  void handle_low_power_sleep_request_(const uint8_t *data, size_t len);
  void handle_low_power_v3_sleep_(const uint8_t *data, size_t len);
  void poll_low_power_v3_sleep_();
  const LogString *low_power_sleep_blocker_(bool in_frame_handler = false) const;
  const LogString *low_power_link_blocker_(bool in_frame_handler = false) const;
  void log_low_power_sleep_blocker_(const LogString *blocker);
  const LogString *low_power_standby_blocker_() const;
  void update_low_power_sleep_lock_();
  bool flush_low_power_();
  void fail_low_power_transport_(const LogString *reason);
  uint8_t grant_low_power_sleep_(bool in_frame_handler);
  void send_low_power_consent_(uint8_t command, uint8_t subcommand);
  void poll_low_power_sleep_preflight_();
  void cancel_low_power_preflight_(bool notify_mcu);
  void poll_low_power_deferred_sleep_();
  void cancel_low_power_standby_();
  void send_low_power_awake_notification_();
  void send_low_power_awake_frame_();
  void wake_low_power_mcu_();
#endif  // USE_TUYA_LOW_POWER
  void send_raw_command_(TuyaCommand command);
  void process_command_queue_();
  void send_command_(TuyaCommand command);
  void send_empty_command_(TuyaCommandType command);
  void send_datapoint_command(uint8_t datapoint_id, TuyaDatapointType datapoint_type, const uint8_t *data,
                              size_t len) override;
  void set_status_pin_();
  void send_wifi_status_();
  uint8_t get_wifi_status_code_();
  uint8_t get_wifi_rssi_();

#ifdef USE_TIME
  void register_local_time_sync_callback_();
  void check_force_time_sync_();
  void send_local_time_(bool periodic = false);
  void send_gmt_time_();
  time::RealTimeClock *time_id_{nullptr};
  bool force_time_sync_{false};
  uint8_t last_force_time_sync_minute_{0xFF};  // 0xFF: no minute seen yet
  bool time_sync_callback_registered_{false};
  bool gmt_time_sync_callback_registered_{false};
#endif
#ifdef USE_TUYA_LOW_POWER
  bool auto_baud_{false};
  GPIOPin *mcu_wake_pin_{nullptr};
  TuyaLowPowerHost *lp_host_{nullptr};
  TuyaLowPowerAutoBaud lp_autobaud_{};
  TuyaLowPowerUartErrors lp_uart_rx_errors_{};
  TuyaLowPowerSession lp_session_{};
  TuyaLowPowerProductInfo lp_product_{};
  // CMD01 JSON copy for diagnostics: the frame reader delivers no longer payload
  std::array<char, MAX_FRAME_SIZE + 1> lp_product_text_{};
  TuyaLowPowerSleepWindow lp_sleep_window_{};
  TuyaLowPowerProductRetry lp_product_retry_{};
  bool lp_mode_requery_pending_{false};
  bool lp_initialized_callback_called_{false};
  // The MCU frames with protocol version 3: status via CMD08/CMD07, writes via CMD06.
  bool lp_v3_{false};
  uint32_t lp_v3_last_poll_ms_{0};
  bool lp_v3_mode_acked_{false};
  // A CMD07 of the full status arrived; the status is complete once the frames are quiet
  bool lp_v3_report_pending_{false};
  // The MCU sent a report nobody asked for: it pushes changes, so the status poll is not needed.
  bool lp_v3_pushes_{false};
  enum class UpdateProbe : uint8_t { UPDATE_PROBE_IDLE, UPDATE_PROBE_PENDING, UPDATE_PROBE_SENT };
  UpdateProbe lp_update_probe_{UpdateProbe::UPDATE_PROBE_IDLE};
  uint32_t lp_update_probe_size_{0};
  uint32_t lp_update_probe_sent_ms_{0};
  uint8_t low_power_update_start_cmd_() const { return this->lp_v3_ ? 0x0A : 0x0D; }
  uint8_t low_power_update_data_cmd_() const { return this->lp_v3_ ? 0x0B : 0x0E; }
  // Version-0 MCUs (HC32) clear the staging area only on a separate erase command (0x0C, subcommand 0),
  // which blocks for the whole erase without an answer. Version-3 MCUs erase on the start command.
  static constexpr uint8_t LOW_POWER_UPDATE_ERASE_CMD = 0x0C;
  static constexpr uint32_t LOW_POWER_UPDATE_ERASE_MS = 1000;
  void poll_low_power_update_probe_();
  enum class McuUpdate : uint8_t {
    MCU_UPDATE_IDLE,
    MCU_UPDATE_START_PENDING,
    MCU_UPDATE_START_SENT,
    MCU_UPDATE_ERASE_WAIT,
    MCU_UPDATE_SEND_PACKET,
    MCU_UPDATE_PACKET_SENT,
    MCU_UPDATE_FINAL_SENT,
  };
  void poll_low_power_mcu_update_();
  bool handle_low_power_mcu_update_(uint8_t command, const uint8_t *data, size_t len);
  void send_low_power_update_packet_();
  void fail_low_power_mcu_update_(const char *reason);
  McuUpdate lp_mcu_update_{McuUpdate::MCU_UPDATE_IDLE};
  const uint8_t *lp_mcu_update_image_{nullptr};
  uint32_t lp_mcu_update_size_{0};
  uint32_t lp_mcu_update_offset_{0};
  uint16_t lp_mcu_update_packet_{256};
  uint16_t lp_mcu_update_chunk_{0};
  uint8_t lp_mcu_update_retries_{0};
  uint8_t lp_mcu_update_percent_{0};
  uint32_t lp_mcu_update_sent_ms_{0};
  uint32_t lp_mode_requery_after_ms_{0};
  uint32_t lp_mcu_wake_pulse_until_{0};
  uint32_t lp_mcu_woken_ms_{0};      // End of the last wake pulse
  uint32_t lp_mcu_heard_ms_{0};      // Last valid frame from the MCU
  uint32_t lp_product_probe_ms_{0};  // Last product query of a recovery
  enum class ProductProbe : uint8_t { PRODUCT_PROBE_NONE, PRODUCT_PROBE_QUEUED, PRODUCT_PROBE_DIRECT };
  // A product query waiting for the wake pulse to end.
  ProductProbe lp_probe_pending_{ProductProbe::PRODUCT_PROBE_NONE};
  bool lp_mcu_wake_pulse_active_{false};
  uint8_t lp_network_status_{0xFF};
  int8_t lp_wake_input_{-1};  // wk in the host's pin numbering
  TuyaLowPowerWireTransactions lp_wire_{};
  // Why sleep was last refused, nullptr when allowed; logged when it changes
  const LogString *lp_sleep_blocked_{nullptr};
  uint32_t lp_wake_guard_ms_{0};  // A wake or a new network state; no sleep for LP_WAKE_GUARD_MS after it
  uint32_t lp_cmd38_preflight_ms_{0};
  uint32_t lp_cmd38_cooldown_ms_{0};  // Last busy answer for a cancelled preflight
  uint32_t lp_cmd38_retry_ms_{0};     // Last deferred grant that had to stop the network services
  uint32_t lp_v3_offer_ms_{0};
  uint32_t lp_v3_idle_since_ms_{0};  // Last byte in either direction
  TuyaLowPowerStandbyState lp_standby_{};
  // Network services are being stopped before a sleep request is answered
  bool lp_cmd38_pending_ : 1 {false};
  // A version-0 MCU asked for sleep and was told busy; it does not ask again until its next activity
  bool lp_cmd38_deferred_ : 1 {false};
  bool lp_mcu_wake_fresh_ : 1 {false};  // The last wake pulse is not answered yet
  bool lp_awake_notification_pending_ : 1 {false};
  bool lp_v3_offer_pending_ : 1 {false};
  bool lp_wake_lines_configured_ : 1 {false};
  // The wake output belongs to the host, not to the configuration
  bool lp_mcu_wake_from_host_ : 1 {false};
#endif  // USE_TUYA_LOW_POWER
  CallbackManager<void()> mcu_ready_callback_{};
  bool mcu_ready_{false};
  TuyaInitState init_state_ = TuyaInitState::INIT_HEARTBEAT;
  bool init_failed_{false};
  int init_retries_{0};
  uint8_t protocol_version_ = -1;
  InternalGPIOPin *status_pin_{nullptr};
  int status_pin_reported_ = -1;
  int reset_pin_reported_ = -1;
  uint32_t last_command_timestamp_ = 0;
  uint32_t last_rx_char_timestamp_ = 0;
  std::unique_ptr<char[]> product_;
  std::vector<TuyaCommand> command_queue_;
  optional<TuyaCommandType> expected_response_{};
  uint8_t wifi_status_ = -1;
  bool product_invalid_{false};
  // Holds commands back while a received frame is handled
  bool handling_frame_{false};
  LengthFrameReader<MAX_FRAME_SIZE, TuyaFrame> reader_;
};

#ifdef USE_TUYA_LOW_POWER
// Explicit DP106 refresh for automation; no unsolicited boot refresh.
template<typename... Ts> class TuyaRefreshAction : public Action<Ts...> {
 public:
  explicit TuyaRefreshAction(TuyaUART *parent) : parent_(parent) {}

 protected:
  void play(const Ts &...x) override { (void) this->parent_->request_low_power_refresh(); }

  TuyaUART *parent_;
};
#endif

}  // namespace esphome::tuya
