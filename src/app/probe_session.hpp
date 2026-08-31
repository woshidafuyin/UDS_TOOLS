#pragma once

#include "app/probe_plan.hpp"

#include <string>

namespace uds::app::probe_detail {

class ProbePreconditions;

std::string execute_probe_session(
    const ProbePlan& plan, ICanBus& bus, ProbePreconditions& preconditions,
    const ProbeServiceCallbacks& callbacks, std::stop_token stop);

} // namespace uds::app::probe_detail
