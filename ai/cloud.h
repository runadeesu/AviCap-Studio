#pragma once
// Opt-in cloud assistant: turns an editing instruction into a Plan with
// Claude (Anthropic Messages API). Used only when the user has selected the
// Claude provider and given consent in the AI panel / settings.
//
// What is sent: the instruction text and describeTimeline() (track layout,
// clip file names and times, title/subtitle text, markers). Never video or
// audio data and never file paths. The reply is constrained to a JSON schema
// and then validated by planFromJson() like any other plan, so the model can
// only request the operations the local engine supports.

#include <string>

#include "ai/plan.h"
#include "core/jobs.h"
#include "core/net.h"

namespace avc::ai {

struct CloudOptions {
    std::string apiKey;
    std::string model = "claude-opus-5-5";
    std::string effort = "medium";
    int maxTokens = 16000;
    std::string endpoint = "https://api.anthropic.com/v1/messages";
};

// The pieces are public so they can be unit tested without a network.
Json cloudPlanSchema();
std::string cloudSystemPrompt();
Json buildCloudRequest(const std::string& instruction, const Json& timeline, const CloudOptions& opt);
// Parses a Messages API response body (status = HTTP status).
Result<Plan> parseCloudResponse(int status, const std::string& body);

Result<Plan> requestCloudPlan(const std::string& instruction, const Json& timeline, const CloudOptions& opt,
                              const HttpTransport& transport, const CancelToken& cancel = {});

}  // namespace avc::ai
