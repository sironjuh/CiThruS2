#pragma once

#include "PipelineFilter.h"

#include <chrono>
#include <cstdint>

class CITHRUS_API FrameRateLimiter : public PipelineFilter<1, 1>
{
public:
	FrameRateLimiter(uint32_t maxFps = 30);
	virtual void Process() override;
	virtual void OnInputPinsConnected() override;

protected:
	uint32_t maxFps_;
	bool hasForwardedFrame_;
	std::chrono::steady_clock::time_point lastForwardTime_;
};
