#include "FrameRateLimiter.h"

FrameRateLimiter::FrameRateLimiter(uint32_t maxFps)
	: maxFps_(maxFps)
	, hasForwardedFrame_(false)
	, lastForwardTime_(std::chrono::steady_clock::time_point::min())
{
	GetInputPin<0>().AcceptAnyFormat();
	GetOutputPin<0>().SetData(nullptr);
	GetOutputPin<0>().SetSize(0);
}

void FrameRateLimiter::Process()
{
	const uint8_t* inputData = GetInputPin<0>().GetConnectedPin().GetData();
	const uint32_t inputSize = GetInputPin<0>().GetConnectedPin().GetSize();

	if (!inputData || inputSize == 0)
	{
		GetOutputPin<0>().SetData(nullptr);
		GetOutputPin<0>().SetSize(0);
		return;
	}

	if (maxFps_ > 0)
	{
		const auto now = std::chrono::steady_clock::now();
		const auto frameInterval = std::chrono::duration<double>(1.0 / static_cast<double>(maxFps_));
		if (hasForwardedFrame_ && (now - lastForwardTime_) < frameInterval)
		{
			GetOutputPin<0>().SetData(nullptr);
			GetOutputPin<0>().SetSize(0);
			return;
		}

		lastForwardTime_ = now;
		hasForwardedFrame_ = true;
	}

	GetOutputPin<0>().SetData(inputData);
	GetOutputPin<0>().SetSize(inputSize);
}

void FrameRateLimiter::OnInputPinsConnected()
{
	GetOutputPin<0>().SetFormat(GetInputPin<0>().GetConnectedPin().GetFormat());
}
