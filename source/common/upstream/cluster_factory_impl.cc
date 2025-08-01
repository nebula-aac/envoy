#include "source/common/upstream/cluster_factory_impl.h"

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/server/options.h"

#include "source/common/http/utility.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/dns_resolver/dns_factory_util.h"
#include "source/common/network/socket_option_factory.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/upstream/health_checker_impl.h"
#include "source/server/transport_socket_config_impl.h"

namespace Envoy {
namespace Upstream {

absl::StatusOr<std::pair<ClusterSharedPtr, ThreadAwareLoadBalancerPtr>>
ClusterFactoryImplBase::create(const envoy::config::cluster::v3::Cluster& cluster,
                               Server::Configuration::ServerFactoryContext& server_context,
                               LazyCreateDnsResolver dns_resolver_fn,
                               Outlier::EventLoggerSharedPtr outlier_event_logger,
                               bool added_via_api) {
  std::string cluster_name;
  std::string cluster_config_type_name;

  ClusterFactory* factory;
  // try to look up by typed_config
  if (cluster.has_cluster_type() && cluster.cluster_type().has_typed_config() &&
      (TypeUtil::typeUrlToDescriptorFullName(cluster.cluster_type().typed_config().type_url()) !=
       ProtobufWkt::Struct::GetDescriptor()->full_name())) {
    cluster_config_type_name =
        TypeUtil::typeUrlToDescriptorFullName(cluster.cluster_type().typed_config().type_url());
    factory = Registry::FactoryRegistry<ClusterFactory>::getFactoryByType(cluster_config_type_name);
    if (factory == nullptr) {
      return absl::InvalidArgumentError(
          fmt::format("Didn't find a registered cluster factory implementation for type: '{}'",
                      cluster_config_type_name));
    }
  } else {
    if (!cluster.has_cluster_type()) {
      switch (cluster.type()) {
        PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
      case envoy::config::cluster::v3::Cluster::STATIC:
        cluster_name = "envoy.cluster.static";
        break;
      case envoy::config::cluster::v3::Cluster::STRICT_DNS:
        cluster_name = "envoy.cluster.strict_dns";
        break;
      case envoy::config::cluster::v3::Cluster::LOGICAL_DNS:
        cluster_name = "envoy.cluster.logical_dns";
        break;
      case envoy::config::cluster::v3::Cluster::ORIGINAL_DST:
        cluster_name = "envoy.cluster.original_dst";
        break;
      case envoy::config::cluster::v3::Cluster::EDS:
        cluster_name = "envoy.cluster.eds";
        break;
      }
    } else {
      cluster_name = cluster.cluster_type().name();
    }
    factory = Registry::FactoryRegistry<ClusterFactory>::getFactory(cluster_name);
    if (factory == nullptr) {
      return absl::InvalidArgumentError(fmt::format(
          "Didn't find a registered cluster factory implementation for name: '{}'", cluster_name));
    }
  }

  if (cluster.common_lb_config().has_consistent_hashing_lb_config() &&
      cluster.common_lb_config().consistent_hashing_lb_config().use_hostname_for_hashing() &&
      cluster.type() != envoy::config::cluster::v3::Cluster::STRICT_DNS) {
    return absl::InvalidArgumentError(fmt::format(
        "Cannot use hostname for consistent hashing loadbalancing for cluster of type: '{}'",
        cluster_name));
  }

  ClusterFactoryContextImpl context(server_context, dns_resolver_fn,
                                    std::move(outlier_event_logger), added_via_api);
  return factory->create(cluster, context);
}

absl::StatusOr<Network::DnsResolverSharedPtr>
ClusterFactoryImplBase::selectDnsResolver(const envoy::config::cluster::v3::Cluster& cluster,
                                          ClusterFactoryContext& context) {
  // We make this a shared pointer to deal with the distinct ownership
  // scenarios that can exist: in one case, we pass in the "default"
  // DNS resolver that is owned by the Server::Instance. In the case
  // where 'dns_resolvers' is specified, we have per-cluster DNS
  // resolvers that are created here but ownership resides with
  // StrictDnsClusterImpl/LogicalDnsCluster.
  if ((cluster.has_typed_dns_resolver_config() &&
       !(cluster.typed_dns_resolver_config().typed_config().type_url().empty())) ||
      (cluster.has_dns_resolution_config() &&
       !cluster.dns_resolution_config().resolvers().empty()) ||
      !cluster.dns_resolvers().empty()) {

    envoy::config::core::v3::TypedExtensionConfig typed_dns_resolver_config;
    Network::DnsResolverFactory& dns_resolver_factory =
        Network::createDnsResolverFactoryFromProto(cluster, typed_dns_resolver_config);
    auto& server_context = context.serverFactoryContext();
    return dns_resolver_factory.createDnsResolver(server_context.mainThreadDispatcher(),
                                                  server_context.api(), typed_dns_resolver_config);
  }

  return context.dnsResolver();
}

absl::StatusOr<std::pair<ClusterSharedPtr, ThreadAwareLoadBalancerPtr>>
ClusterFactoryImplBase::create(const envoy::config::cluster::v3::Cluster& cluster,
                               ClusterFactoryContext& context) {

  absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
      status_or_cluster = createClusterImpl(cluster, context);
  RETURN_IF_NOT_OK_REF(status_or_cluster.status());
  std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>& new_cluster_pair =
      status_or_cluster.value();

  auto& server_context = context.serverFactoryContext();

  if (!cluster.health_checks().empty()) {
    // TODO(htuch): Need to support multiple health checks in v2.
    if (cluster.health_checks().size() != 1) {
      return absl::InvalidArgumentError("Multiple health checks not supported");
    } else {
      auto checker_or_error = HealthCheckerFactory::create(cluster.health_checks()[0],
                                                           *new_cluster_pair.first, server_context);
      RETURN_IF_NOT_OK_REF(checker_or_error.status());
      new_cluster_pair.first->setHealthChecker(checker_or_error.value());
    }
  }

  auto detector_or_error = Outlier::DetectorImplFactory::createForCluster(
      *new_cluster_pair.first, cluster, server_context.mainThreadDispatcher(),
      server_context.runtime(), context.outlierEventLogger(),
      server_context.api().randomGenerator());
  RETURN_IF_NOT_OK_REF(detector_or_error.status());
  new_cluster_pair.first->setOutlierDetector(detector_or_error.value());

  return status_or_cluster;
}

} // namespace Upstream
} // namespace Envoy
