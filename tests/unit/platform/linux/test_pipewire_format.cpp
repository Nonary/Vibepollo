/**
 * @file tests/unit/platform/linux/test_pipewire_format.cpp
 * @brief Exercise capture format compatibility using PipeWire's SPA filter.
 */
#include "../../../tests_common.h"

#include <array>
#include <spa/param/video/format-utils.h>
#include <spa/pod/filter.h>
#include <src/platform/linux/pipewire_format.h>

namespace {
  struct format_pod_t {
    std::array<uint8_t, 1024> storage {};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_pod_frame frame {};

    format_pod_t() {
      const auto size = SPA_RECTANGLE(2560, 1440);
      spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(&builder,
                          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                          SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
                          SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    }

    const spa_pod *finish() {
      return static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
    }

    void add_producer_rates(uint32_t minimum, uint32_t maximum) {
      const auto variable_rate = SPA_FRACTION(0, 1);
      const auto minimum_rate = SPA_FRACTION(minimum, 1);
      const auto maximum_rate = SPA_FRACTION(maximum, 1);
      spa_pod_builder_add(&builder,
                          SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable_rate),
                          SPA_FORMAT_VIDEO_maxFramerate,
                          SPA_POD_CHOICE_RANGE_Fraction(&maximum_rate, &minimum_rate, &maximum_rate), 0);
    }
  };

  int negotiate(const spa_pod *producer, const spa_pod *consumer, spa_video_info_raw &format) {
    std::array<uint8_t, 2048> storage {};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    spa_pod *result = nullptr;
    const int status = spa_pod_filter(&builder, &result, producer, consumer);
    if (status < 0) {
      return status;
    }
    spa_pod_fixate(result);
    return spa_format_video_raw_parse(result, &format);
  }
}  // namespace

TEST(PipewireFormat, AcceptsKwinFiniteMaximumWithVariableFrameRate) {
  // Native SteamOS KWin offers these rates for its 2560x1440, 119 Hz output.
  format_pod_t producer;
  producer.add_producer_rates(1, 119);
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, true);
  spa_video_info_raw format {};
  ASSERT_GE(negotiate(producer.finish(), consumer.finish(), format), 0);
  EXPECT_EQ(format.framerate.num, 0u);
  EXPECT_EQ(format.framerate.denom, 1u);
  EXPECT_EQ(format.max_framerate.num, 119u);
  EXPECT_EQ(format.max_framerate.denom, 1u);
}

TEST(PipewireFormat, PreservesUnpacedProducerSupport) {
  format_pod_t producer;
  producer.add_producer_rates(0, 0);
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, true);
  spa_video_info_raw format {};
  ASSERT_GE(negotiate(producer.finish(), consumer.finish(), format), 0);
  EXPECT_EQ(format.framerate.num, 0u);
  EXPECT_EQ(format.max_framerate.num, 0u);
}

TEST(PipewireFormat, CanOmitOptionalMaximumForGamescope) {
  format_pod_t consumer;
  pipewire::add_framerate_parameters(&consumer.builder, false);
  const auto *format = consumer.finish();
  EXPECT_NE(spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_framerate), nullptr);
  EXPECT_EQ(spa_pod_find_prop(format, nullptr, SPA_FORMAT_VIDEO_maxFramerate), nullptr);
}

TEST(PipewireFormat, ZeroOnlyMaximumReproducesKwinNegotiationFailure) {
  format_pod_t producer;
  producer.add_producer_rates(1, 119);
  format_pod_t consumer;
  consumer.add_producer_rates(0, 0);
  spa_video_info_raw format {};
  EXPECT_LT(negotiate(producer.finish(), consumer.finish(), format), 0);
}
