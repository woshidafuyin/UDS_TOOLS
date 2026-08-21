#include "app/diagnostic_request_service.hpp"

#include "core/asc_trace.hpp"
#include "core/hex.hpp"
#include "core/isotp.hpp"
#include "core/uds_client.hpp"
#include "drivers/can/can_bus_provider.hpp"

#include <chrono>
#include <stdexcept>

namespace uds::app {
namespace {
using namespace std::chrono_literals;
constexpr std::string_view kCancelled = "operation cancelled by user";
}

DiagnosticRequestService::DiagnosticRequestService(BusFactory bus_factory)
    : bus_factory_(std::move(bus_factory)) {
  if (!bus_factory_) {
    bus_factory_ = [](const DiagnosticRequest& request) {
      return default_can_bus_provider()->create(
          {"", request.channel, request.profile.nominal_bitrate,
           request.profile.data_bitrate, request.profile.can_fd,
           L"UDSToolDiagnosticRequest"});
    };
  }
}

DiagnosticRequestResult DiagnosticRequestService::run(
    const DiagnosticRequest& request, std::stop_token stop) const {
  DiagnosticRequestResult result;
  try {
    if (request.payload.empty()) throw std::runtime_error("UDS request is empty");
    if (stop.stop_requested()) throw std::runtime_error(kCancelled.data());
    auto bus = bus_factory_(request);
    if (!bus) throw std::runtime_error("diagnostic bus factory returned null");
    if (!request.trace_file.empty()) {
      auto trace =
          std::make_shared<AscTraceWriter>(request.trace_file, request.channel);
      bus = std::make_unique<TracingCanBus>(std::move(bus), std::move(trace));
    }
    bus->open();
    IsoTpConfig config{request.tx_id, request.rx_id, request.profile.padding, 0,
                       request.profile.isotp_st_min, 1000ms, 1000ms,
                       request.profile.extended_id,
                       request.profile.extended_id, request.profile.uds_fd,
                       request.profile.uds_brs};
    IsoTpSession transport(*bus, config);
    UdsClient client(transport, {}, stop);
    const auto response = client.request(
        request.payload, std::chrono::milliseconds(request.timeout_ms),
        std::chrono::milliseconds(request.timeout_ms), stop);
    result.success = response.success;
    result.request_hex = to_hex(response.request);
    result.response_hex = to_hex(response.response);
    result.elapsed_ms = static_cast<unsigned>(response.elapsed.count());
    result.nrc = response.nrc;
    result.message = response.detail.empty()
                         ? (response.success ? "diagnostic request succeeded"
                                             : "diagnostic request failed")
                         : response.detail;
  } catch (const std::exception& error) {
    result.cancelled = std::string_view(error.what()).find(kCancelled) !=
                       std::string_view::npos;
    result.message = error.what();
  }
  return result;
}

} // namespace uds::app
