// The frame tests need the low_power protocol, also in clang-tidy builds whose defines.h leaves it out
#ifndef USE_TUYA_LOW_POWER
#define USE_TUYA_LOW_POWER
#endif
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "esphome/components/tuya/tuya_uart.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"

// Drives whole frames through TuyaUART with the low_power protocol: handshake, write queue and recovery.
namespace esphome::tuya::testing {

namespace {

struct Frame {
  uint8_t version;
  uint8_t command;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode(const Frame &f) {
  std::vector<uint8_t> out{0x55,
                           0xAA,
                           f.version,
                           f.command,
                           static_cast<uint8_t>(f.payload.size() >> 8),
                           static_cast<uint8_t>(f.payload.size())};
  out.insert(out.end(), f.payload.begin(), f.payload.end());
  uint8_t sum = 0;
  for (uint8_t b : out)
    sum += b;
  out.push_back(sum);
  return out;
}

std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// Each frame as its command followed by its payload
using Frames = std::vector<std::vector<uint8_t>>;

class MockUART : public uart::UARTComponent {
 public:
  std::vector<uint8_t> rx;
  std::vector<uint8_t> tx;

  void write_array(const uint8_t *data, size_t len) override { this->tx.insert(this->tx.end(), data, data + len); }
  bool read_array(uint8_t *data, size_t len) override {
    if (this->rx.size() < len)
      return false;
    std::copy(this->rx.begin(), this->rx.begin() + len, data);
    this->rx.erase(this->rx.begin(), this->rx.begin() + len);
    return true;
  }
  size_t available() override { return this->rx.size(); }
  bool peek_byte(uint8_t *data) override {
    if (this->rx.empty())
      return false;
    *data = this->rx.front();
    return true;
  }
  uart::UARTFlushResult flush() override { return this->flush_result; }
  uart::UARTFlushResult flush_result{uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS};
  void check_logger_conflict() override {}
#if defined(USE_ESP8266) || defined(USE_ESP32) || defined(USE_LIBRETINY)
  void load_settings(bool dump_config) override {}
#endif

  // The frames sent since the last call
  std::vector<Frame> sent() {
    std::vector<Frame> frames;
    size_t i = 0;
    while (i + 7 <= this->tx.size()) {
      const size_t len = (this->tx[i + 4] << 8) | this->tx[i + 5];
      if (this->tx[i] != 0x55 || this->tx[i + 1] != 0xAA || i + 7 + len > this->tx.size())
        break;
      frames.push_back({this->tx[i + 2], this->tx[i + 3], {this->tx.begin() + i + 6, this->tx.begin() + i + 6 + len}});
      i += 7 + len;
    }
    this->tx.clear();
    return frames;
  }
};

class TuyaLowPowerFrames : public ::testing::Test {
 protected:
  void SetUp() override {
    this->tuya_.set_uart_parent(&this->uart_);
    this->tuya_.add_on_mcu_ready_callback([this]() { this->ready_count_++; });
    this->step_(0);
    this->tuya_.setup();
  }

  // The network status interval (id 1) must not outlive this TuyaUART into a later test
  void TearDown() override {
    App.scheduler.cancel_interval(&this->tuya_, uint32_t{1});
    App.scheduler.call(millis());
  }

  // One loop pass at loop time now. Commands keep a 10 ms gap measured with millis(), so wait that long first.
  void step_(uint32_t now) {
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    LoopBlockingGuard{nullptr, nullptr, now};
    this->tuya_.loop();
  }

  void receive_(uint32_t now, const Frame &frame) {
    const auto raw = encode(frame);
    this->uart_.rx.insert(this->uart_.rx.end(), raw.begin(), raw.end());
    this->step_(now);
  }

  // Product info of a version-0 or version-3 MCU, after the module asked for it
  void handshake_(uint8_t version, const std::string &product = R"({"p":"abcdefgh","v":"1.0.0"})") {
    auto sent = this->uart_.sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].command, 0x01);
    this->receive_(100, {version, 0x01, bytes(product)});
    this->step_(110);
  }

  // A version-3 MCU: working mode, then the full status that its CMD08 asks for
  void sync_version_three_(const std::string &product) {
    this->handshake_(3, product);
    this->receive_(200, {0x03, 0x02, {}});
    this->receive_(300, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00}});
    this->step_(800);
    ASSERT_EQ(this->ready_count_, 1);
    this->uart_.sent();
  }

  // The frames sent since the last call
  Frames sent_() {
    Frames frames;
    for (const auto &f : this->uart_.sent()) {
      frames.push_back({f.command});
      frames.back().insert(frames.back().end(), f.payload.begin(), f.payload.end());
    }
    return frames;
  }

  // Loop passes until the module has sent count frames, for example once a wake pulse is over
  Frames wait_for_frames_(uint32_t now, size_t count = 1) {
    Frames sent;
    for (int i = 0; i < 20 && sent.size() < count; i++) {
      this->step_(now);
      for (auto &frame : this->sent_())
        sent.push_back(std::move(frame));
    }
    return sent;
  }

  // Runs the network status interval (every second, in real time) until the module sends something. The host
  // scheduler takes its time from the clock and sets the loop time to now.
  Frames network_interval_(uint32_t now) {
    for (int i = 0; i < 150; i++) {
      App.scheduler.call(now);
      this->step_(now);
      auto sent = this->sent_();
      if (!sent.empty())
        return sent;
    }
    return {};
  }

  MockUART uart_;
  TuyaUART tuya_;
  int ready_count_{0};
};

// The MCU's wake line wk=20 is the host's wake input 5
const std::string V0_SLEEP_PRODUCT = R"({"p":"abcdefgh","v":"1.3.1","wk":20,"mwk":28,"sm":1})";
const std::string V3_SLEEP_PRODUCT = R"({"p":"abcdefgh","v":"1.0.9","sm":2,"wk":20,"mwk":16})";

class FakeHost final : public TuyaLowPowerHost {
 public:
  void set_mcu_synchronized(bool synchronized) override { this->synchronized = synchronized; }
  bool ota_busy() const override { return this->ota; }
  bool standby_supported() const override { return true; }
  int standby_wake_pin() const override { return 5; }
  bool standby_requires_external_sync() const override { return true; }
  bool standby_may_quiesce() const override { return true; }
  bool standby_services_quiesced() const override { return this->quiesced; }
  void request_preflight(bool requested) override { this->preflight = requested; }
  bool preflight_requested() const override { return this->preflight; }
  void request_standby(bool requested) override { this->standby = requested; }
  bool standby_requested() const override { return this->standby; }
  bool network_quiescing() const override { return false; }
  bool standby_keeps_network() const override { return this->keeps_network; }
  int configure_wake_lines(int wake_gpio, int /*mcu_wake_gpio*/, bool /*mcu_wake_active_high*/,
                           bool create_mcu_wake_output) override {
    this->output_created = create_mcu_wake_output;
    return wake_gpio == 20 ? 5 : -1;
  }
  // Like a host that learns the wake line from the product info only
  GPIOPin *mcu_wake_output() override { return this->output_created ? this->wake_output : nullptr; }

  GPIOPin *wake_output{nullptr};
  bool output_created{false};

  bool synchronized{false};
  bool ota{false};
  bool quiesced{true};
  bool preflight{false};
  bool standby{false};
  bool keeps_network{false};
};

// Records the level of the wake line to the MCU
class FakePin final : public GPIOPin {
 public:
  void setup() override {}
  void pin_mode(gpio::Flags flags) override {}
  gpio::Flags get_flags() const override { return gpio::FLAG_OUTPUT; }
  bool digital_read() override { return this->level; }
  void digital_write(bool value) override {
    this->level = value;
    this->pulses += value ? 1 : 0;
  }
  bool level{false};
  int pulses{0};
};

class TuyaLowPowerSleep : public TuyaLowPowerFrames {
 protected:
  void SetUp() override {
    this->tuya_.set_low_power_host(&this->host_);
    TuyaLowPowerFrames::SetUp();
  }

  FakeHost host_;
};

}  // namespace

TEST_F(TuyaLowPowerFrames, VersionZeroRecoversAfterAckTimeout) {
  this->handshake_(0);
  // The host build is online but has no API client, so the module reports 0x03 (router, not cloud)
  auto sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x02);
  EXPECT_EQ(sent[0].payload, std::vector<uint8_t>{0x03});

  // No ACK: after the timeout the module asks for the product info directly, 500 ms later
  this->step_(110 + 1500);
  EXPECT_TRUE(this->uart_.sent().empty());
  this->step_(110 + 1500 + 500);
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x01);

  // The MCU answers: the network state goes out again and its ACK ends the recovery
  this->receive_(2200, {0x00, 0x01, bytes(R"({"p":"abcdefgh","v":"1.0.0"})")});
  this->step_(2210);
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x02);
  this->receive_(2300, {0x00, 0x02, {}});
  EXPECT_TRUE(this->uart_.sent().empty());
  // Without status 0x04 the MCU does not send its full status, so the module does not become ready
  this->step_(4000);
  EXPECT_EQ(this->ready_count_, 0);
}

TEST_F(TuyaLowPowerFrames, VersionZeroAcksOnlyValidReports) {
  this->handshake_(0);
  this->uart_.sent();
  // A bitmask datapoint must be 1, 2 or 4 bytes: the whole frame gets no ACK
  this->receive_(200, {0x00, 0x05, {0x07, 0x05, 0x00, 0x03, 0x01, 0x02, 0x03}});
  EXPECT_TRUE(this->uart_.sent().empty());
  // Each datapoint of a valid report is acknowledged on its own
  this->receive_(300, {0x00, 0x05, {0x01, 0x01, 0x00, 0x01, 0x01, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05}});
  auto sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].command, 0x05);
  EXPECT_EQ(sent[0].payload, (std::vector<uint8_t>{0x00, 0x01}));
  EXPECT_EQ(sent[1].payload, (std::vector<uint8_t>{0x00, 0x02}));
}

TEST_F(TuyaLowPowerFrames, VersionThreeHandshakeAndWrites) {
  this->handshake_(3);
  // Version 3: CMD02 is the empty working-mode query; its answer may carry two pin bytes
  auto sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x02);
  EXPECT_TRUE(sent[0].payload.empty());
  this->receive_(200, {0x03, 0x02, {0x0E, 0x0F}});
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].command, 0x03);
  EXPECT_EQ(sent[1].command, 0x08);

  // The full status arrives: the MCU is ready, once
  this->receive_(300, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05}});
  this->step_(800);  // The status is complete once its frames have been quiet for 500 ms
  EXPECT_EQ(this->ready_count_, 1);

  // Two writes: the second waits until the MCU's CMD07 confirms the first
  this->tuya_.set_boolean_datapoint_value(1, true);
  this->tuya_.set_integer_datapoint_value(2, 7);
  this->step_(1400);
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x06);
  EXPECT_EQ(sent[0].payload, (std::vector<uint8_t>{0x01, 0x01, 0x00, 0x01, 0x01}));
  this->step_(1500);
  EXPECT_TRUE(this->uart_.sent().empty());
  this->receive_(1600, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x01}});
  this->step_(1610);
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x06);
  EXPECT_EQ(sent[0].payload[0], 0x02);
  this->receive_(1700, {0x03, 0x07, {0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x07}});
  EXPECT_EQ(this->ready_count_, 1);

  // A restarted MCU gets the working-mode handshake again
  this->receive_(1800, {0x03, 0x00, {0x00}});
  this->step_(1810);
  bool mode_query = false;
  for (const auto &f : this->uart_.sent())
    mode_query |= f.command == 0x02;
  EXPECT_TRUE(mode_query);
}

TEST_F(TuyaLowPowerFrames, VersionThreeWriteWithoutReportNeedsNoResync) {
  this->handshake_(3);
  this->uart_.sent();
  this->receive_(200, {0x03, 0x02, {}});
  this->receive_(300, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05}});
  this->step_(800);  // The status is complete once its frames have been quiet for 500 ms
  this->uart_.sent();

  this->tuya_.set_boolean_datapoint_value(1, true);
  this->step_(1400);
  ASSERT_EQ(this->uart_.sent().size(), 1u);
  // A second write waits while the first one waits for its report
  this->tuya_.set_integer_datapoint_value(2, 9);
  this->step_(1500);
  EXPECT_TRUE(this->uart_.sent().empty());
  // MCU firmware 1.0.9 applies the write but sends no CMD07: the module asks for the state, no product query
  this->step_(1400 + 1500);
  auto sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x08);
  // The answer shows the new value; then the waiting write goes out
  this->receive_(3000, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x01, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05}});
  this->step_(3010);
  sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x06);
  EXPECT_EQ(sent[0].payload[0], 0x02);
  EXPECT_EQ(this->ready_count_, 1);
}

TEST_F(TuyaLowPowerFrames, RequestBackToTheReportedValueReplacesAQueuedWrite) {
  this->handshake_(3);
  this->uart_.sent();
  this->receive_(200, {0x03, 0x02, {}});
  this->receive_(300, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05}});
  this->step_(800);  // The status is complete once its frames have been quiet for 500 ms
  this->uart_.sent();

  // A write to DP2 is in flight, so the DP1 write waits in the queue
  this->tuya_.set_integer_datapoint_value(2, 9);
  this->step_(1400);
  ASSERT_EQ(this->uart_.sent().size(), 1u);
  this->tuya_.set_boolean_datapoint_value(1, true);
  // Back to the reported value (off) before the queued write went out: it replaces the queued "on"
  this->tuya_.set_boolean_datapoint_value(1, false);
  this->receive_(1500, {0x03, 0x07, {0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x09}});
  this->step_(1510);
  auto sent = this->uart_.sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].command, 0x06);
  EXPECT_EQ(sent[0].payload, (std::vector<uint8_t>{0x01, 0x01, 0x00, 0x01, 0x00}));
}

TEST_F(TuyaLowPowerFrames, VersionThreeStatusInSeveralFramesIsCompleteWhenQuiet) {
  this->handshake_(3);
  this->uart_.sent();
  this->receive_(200, {0x03, 0x02, {}});
  // MCU firmware 1.0.9 answers CMD08 with three CMD07 frames and a CMD25 between them
  this->receive_(300, {0x03, 0x07, {0x1F, 0x00, 0x00, 0x04, 0x06, 0x00, 0x00, 0xC8}});
  this->receive_(310, {0x03, 0x25, {}});
  this->receive_(320, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00, 0x03, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00}});
  EXPECT_EQ(this->ready_count_, 0);
  this->receive_(330, {0x03, 0x07, {0x14, 0x01, 0x00, 0x01, 0x00}});
  this->step_(829);
  EXPECT_EQ(this->ready_count_, 0);
  this->step_(830);
  EXPECT_EQ(this->ready_count_, 1);
}

// The version-0 tests below stop before the full status: it needs CMD02 0x04, which this build without an API
// client never sends, so the module stays unsynchronized and every grant is refused.
TEST_F(TuyaLowPowerFrames, SleepRequestsWithoutHostAreBusy) {
  this->handshake_(0, V0_SLEEP_PRODUCT);
  this->sent_();
  this->receive_(6000, {0x00, 0x38, {0x00}});
  EXPECT_EQ(this->sent_(), (Frames{{0x38, 0x00, 0x01}}));
  // No later grant either
  this->step_(20000);
  EXPECT_TRUE(this->sent_().empty());
}

TEST_F(TuyaLowPowerFrames, VersionThreeSleepRequestsWithoutHostAreBusy) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(20000);
  EXPECT_TRUE(this->sent_().empty());  // No sleep offer
  this->receive_(20100, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x02}}));
}

TEST_F(TuyaLowPowerFrames, VersionZeroCrossedNetworkStateIsSentAgain) {
  this->handshake_(0);
  EXPECT_EQ(this->sent_(), (Frames{{0x02, 0x03}}));
  // The MCU asks to sleep instead of answering CMD02 and sleeps: no ACK timeout, no product query
  this->receive_(200, {0x00, 0x38, {0x00}});
  EXPECT_EQ(this->sent_(), (Frames{{0x38, 0x00, 0x01}}));
  this->step_(2500);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_EQ(this->network_interval_(2600), (Frames{{0x02, 0x03}}));
}

TEST_F(TuyaLowPowerSleep, VersionZeroWakesTheMcuForEachCommand) {
  FakePin pin;
  this->host_.wake_output = &pin;
  this->handshake_(0, V0_SLEEP_PRODUCT);
  // The wake line comes with the product info. The MCU may sleep right after its answer, so CMD02 needs a pulse
  EXPECT_TRUE(pin.level);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_EQ(this->wait_for_frames_(120), (Frames{{0x02, 0x03}}));
  EXPECT_FALSE(pin.level);
  EXPECT_EQ(pin.pulses, 1);
  // The MCU answered that pulse; a restart heartbeat right after it queues CMD02 again, which needs a new pulse
  this->receive_(200, {0x00, 0x02, {}});
  this->receive_(300, {0x00, 0x00, {0x00}});
  EXPECT_TRUE(pin.level);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_EQ(this->wait_for_frames_(310), (Frames{{0x02, 0x03}}));
  EXPECT_EQ(pin.pulses, 2);
}

TEST_F(TuyaLowPowerSleep, VersionThreeOfferRequestAndWake) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  // The module offers sleep after 5 s without a byte
  this->step_(5299);
  EXPECT_TRUE(this->sent_().empty());
  this->step_(5300);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_TRUE(this->host_.standby);
  EXPECT_TRUE(this->tuya_.low_power_prepare_for_standby());  // The answer is the whole handshake
  // Every wake ends the consent with 91 01 01
  this->tuya_.low_power_standby_resumed(true, false);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x01, 0x01}}));
  this->host_.standby = false;

  // The MCU's own request: busy within the wake guard, granted after it
  this->receive_(5500, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x02}}));
  this->receive_(10500, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x00}}));
  EXPECT_TRUE(this->host_.standby);
  // A report ends the consent, and the MCU is told
  this->receive_(10600, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x01}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x01, 0x01}}));
  EXPECT_FALSE(this->host_.standby);

  // A declined offer, then one without an answer
  this->step_(15600);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->receive_(15700, {0x03, 0x91, {0x00, 0x00}});
  EXPECT_FALSE(this->host_.standby);
  this->step_(20700);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->step_(21700);
  this->step_(26699);
  EXPECT_TRUE(this->sent_().empty());
  this->step_(26700);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));

  // During an update the MCU's request is busy with the update flag
  this->host_.ota = true;
  this->receive_(26800, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x04}}));
}

TEST_F(TuyaLowPowerSleep, VersionThreeWakeNoticeFollowsAWakePulse) {
  FakePin pin;
  this->host_.wake_output = &pin;
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(5300);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  const int pulses = pin.pulses;
  // The sleeping MCU listens only after a pulse on its wake line
  this->tuya_.low_power_standby_resumed(true, true);
  EXPECT_TRUE(pin.level);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_EQ(this->wait_for_frames_(5410), (Frames{{0x91, 0x01, 0x01}}));
  EXPECT_FALSE(pin.level);
  EXPECT_EQ(pin.pulses, pulses + 1);
}

TEST_F(TuyaLowPowerSleep, VersionThreeCommandFollowsTheWakeNotice) {
  FakePin pin;
  this->host_.wake_output = &pin;
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(5300);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  const int pulses = pin.pulses;
  // The MCU answered just now, but sleeps: the write waits for the pulse and the wake notice
  this->tuya_.set_boolean_datapoint_value(1, true);
  this->step_(5410);
  EXPECT_TRUE(pin.level);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_FALSE(this->host_.standby);
  EXPECT_EQ(this->wait_for_frames_(5420, 2), (Frames{{0x91, 0x01, 0x01}, {0x06, 0x01, 0x01, 0x00, 0x01, 0x01}}));
  EXPECT_EQ(pin.pulses, pulses + 1);
}

TEST_F(TuyaLowPowerSleep, VersionThreeUnexpectedWakeReadsTheStateAgain) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(5300);
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  this->sent_();
  // A version-3 MCU has no boot heartbeat: it is told and its state is read again
  this->tuya_.low_power_standby_resumed(false, false);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x01, 0x01}}));
  this->host_.standby = false;
  this->step_(5500);
  EXPECT_EQ(this->sent_(), (Frames{{0x08}}));
  // A report alone does not end the recovery; the complete status does
  this->receive_(5600, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00}});
  this->receive_(5700, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x02}}));
  this->step_(6200);
  this->receive_(10500, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x00}}));
}

TEST_F(TuyaLowPowerSleep, VersionThreeConsentWhileBlockedWakesTheMcu) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(5300);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  // An update started after the offer: the MCU already sleeps, so it is told that the module stays awake
  this->host_.ota = true;
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x01, 0x01}}));
  EXPECT_FALSE(this->host_.standby);
}

TEST_F(TuyaLowPowerSleep, VersionZeroUnsynchronizedMcuIsNotGranted) {
  this->handshake_(0, V0_SLEEP_PRODUCT);
  this->sent_();
  this->receive_(6000, {0x00, 0x38, {0x00}});
  EXPECT_EQ(this->sent_(), (Frames{{0x38, 0x00, 0x01}}));
  EXPECT_FALSE(this->host_.standby);
  EXPECT_FALSE(this->tuya_.low_power_prepare_for_standby());
}

TEST_F(TuyaLowPowerSleep, FlushFailureBlocksSleep) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->uart_.flush_result = uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
  this->receive_(6000, {0x03, 0x91, {0x02}});
  // The grant may not have left the UART: no standby, and the MCU's state is read again
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x00}, {0x08}}));
  EXPECT_FALSE(this->host_.standby);
  this->uart_.flush_result = uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  this->receive_(6100, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x02}}));
  // The complete status ends the recovery
  this->receive_(6200, {0x03, 0x07, {0x01, 0x01, 0x00, 0x01, 0x00}});
  this->step_(6700);
  this->receive_(7000, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x00}}));
  EXPECT_TRUE(this->host_.standby);
}

TEST_F(TuyaLowPowerSleep, AssumedFlushCountsAsSent) {
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->uart_.flush_result = uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
  this->receive_(6000, {0x03, 0x91, {0x02}});
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x02, 0x00}}));
  EXPECT_TRUE(this->host_.standby);
}

TEST_F(TuyaLowPowerSleep, KeptConsentSleepsAgainAfterATimerWake) {
  this->host_.keeps_network = true;
  this->sync_version_three_(V3_SLEEP_PRODUCT);
  this->step_(5300);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x00}}));
  this->receive_(5400, {0x03, 0x91, {0x00, 0x01}});
  EXPECT_TRUE(this->tuya_.low_power_prepare_for_standby());
  // The platform clears its own request; with the network kept up a timer wake asks for the next standby at once
  this->host_.standby = false;
  this->tuya_.low_power_standby_resumed(true, false, true);
  EXPECT_TRUE(this->sent_().empty());
  EXPECT_TRUE(this->host_.standby);
  EXPECT_TRUE(this->tuya_.low_power_prepare_for_standby());
  // The MCU's own wake ends the consent
  this->host_.standby = false;
  this->tuya_.low_power_standby_resumed(true, true, false);
  EXPECT_EQ(this->sent_(), (Frames{{0x91, 0x01, 0x01}}));
  EXPECT_FALSE(this->host_.standby);
}

}  // namespace esphome::tuya::testing
