#pragma once

#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/metrics/v3/stats.pb.h"

#include "source/common/json/json_loader.h"

#include "test/integration/http_protocol_integration.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {

class IntegrationAdminTest : public HttpProtocolIntegrationTest {
public:
  void initialize() override {
    config_helper_.addConfigModifier(
        [](envoy::config::bootstrap::v3::Bootstrap& bootstrap) -> void {
          auto listener_config = bootstrap.mutable_static_resources()->mutable_listeners(0);
          auto additional_address = listener_config->add_additional_addresses();
          envoy::config::core::v3::SocketAddress& socket_address =
              *additional_address->mutable_address()->mutable_socket_address();
          socket_address.set_address("127.0.0.2");
          socket_address.set_port_value(0);
          auto& hist_settings =
              *bootstrap.mutable_stats_config()->mutable_histogram_bucket_settings();
          envoy::config::metrics::v3::HistogramBucketSettings* setting = hist_settings.Add();
          setting->mutable_match()->set_suffix("upstream_cx_connect_ms");
          setting->mutable_buckets()->Add(1);
          setting->mutable_buckets()->Add(2);
          setting->mutable_buckets()->Add(3);
          setting->mutable_buckets()->Add(4);
        });
    HttpIntegrationTest::initialize();
  }

  absl::string_view request(const std::string port_key, const std::string method,
                            const std::string endpoint, BufferingStreamDecoderPtr& response) {
    response = IntegrationUtil::makeSingleRequest(lookupPort(port_key), method, endpoint, "",
                                                  downstreamProtocol(), version_);
    EXPECT_TRUE(response->complete());
    return response->headers().getStatusValue();
  }

  /**
   * Validates that the passed in string conforms to output of stats in JSON format.
   */
  void validateStatsJson(const std::string& stats_json, const uint64_t expected_hist_count) {
    Json::ObjectSharedPtr statsjson = Json::Factory::loadFromString(stats_json).value();
    EXPECT_TRUE(statsjson->hasObject("stats"));
    uint64_t histogram_count = 0;
    auto array = *statsjson->getObjectArray("stats");
    for (const Json::ObjectSharedPtr& obj_ptr : array) {
      if (obj_ptr->hasObject("histograms")) {
        histogram_count++;
        const Json::ObjectSharedPtr histograms_ptr = *obj_ptr->getObject("histograms");
        // Validate that both supported_quantiles and computed_quantiles are present in JSON.
        EXPECT_TRUE(histograms_ptr->hasObject("supported_quantiles"));
        EXPECT_TRUE(histograms_ptr->hasObject("computed_quantiles"));

        const std::vector<Json::ObjectSharedPtr> computed_quantiles =
            *histograms_ptr->getObjectArray("computed_quantiles");
        EXPECT_GT(computed_quantiles.size(), 0);

        // Validate that each computed_quantile has name and value objects.
        EXPECT_TRUE(computed_quantiles[0]->hasObject("name"));
        EXPECT_TRUE(computed_quantiles[0]->hasObject("values"));

        // Validate that supported and computed quantiles are of the same size.
        EXPECT_EQ(histograms_ptr->getObjectArray("supported_quantiles")->size(),
                  computed_quantiles[0]->getObjectArray("values")->size());
      }
    }

    // Validate that the stats JSON has expected histograms element.
    EXPECT_EQ(expected_hist_count, histogram_count) << stats_json;
  }
};

} // namespace Envoy
