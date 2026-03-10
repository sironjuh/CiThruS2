#include "StreamPerfStats.h"

#include <atomic>

namespace
{
	std::atomic<uint64_t> GReadbackMicroseconds{ 0 };
	std::atomic<uint64_t> GReadbackSamples{ 0 };
	std::atomic<uint64_t> GRtpSendMicroseconds{ 0 };
	std::atomic<uint64_t> GRtpSendSamples{ 0 };
	std::atomic<uint64_t> GReaderDroppedFrames{ 0 };
	std::atomic<uint64_t> GRtpDroppedFrames{ 0 };

	uint64_t ToMicroseconds(double milliseconds)
	{
		if (milliseconds <= 0.0)
		{
			return 0;
		}

		return static_cast<uint64_t>(milliseconds * 1000.0);
	}
}

void StreamPerfStats::AddReadbackSample(double milliseconds)
{
	GReadbackMicroseconds.fetch_add(ToMicroseconds(milliseconds), std::memory_order_relaxed);
	GReadbackSamples.fetch_add(1, std::memory_order_relaxed);
}

void StreamPerfStats::AddRtpSendSample(double milliseconds)
{
	GRtpSendMicroseconds.fetch_add(ToMicroseconds(milliseconds), std::memory_order_relaxed);
	GRtpSendSamples.fetch_add(1, std::memory_order_relaxed);
}

void StreamPerfStats::AddReaderDrop(uint64_t count)
{
	GReaderDroppedFrames.fetch_add(count, std::memory_order_relaxed);
}

void StreamPerfStats::AddRtpDrop(uint64_t count)
{
	GRtpDroppedFrames.fetch_add(count, std::memory_order_relaxed);
}

FStreamPerfSnapshot StreamPerfStats::Consume()
{
	FStreamPerfSnapshot snapshot;

	const uint64_t readbackMicroseconds = GReadbackMicroseconds.exchange(0, std::memory_order_relaxed);
	snapshot.ReadbackSamples = GReadbackSamples.exchange(0, std::memory_order_relaxed);
	if (snapshot.ReadbackSamples > 0)
	{
		snapshot.ReadbackMs = static_cast<double>(readbackMicroseconds) /
			(static_cast<double>(snapshot.ReadbackSamples) * 1000.0);
	}

	const uint64_t rtpSendMicroseconds = GRtpSendMicroseconds.exchange(0, std::memory_order_relaxed);
	snapshot.RtpSendSamples = GRtpSendSamples.exchange(0, std::memory_order_relaxed);
	if (snapshot.RtpSendSamples > 0)
	{
		snapshot.RtpSendMs = static_cast<double>(rtpSendMicroseconds) /
			(static_cast<double>(snapshot.RtpSendSamples) * 1000.0);
	}

	snapshot.ReaderDroppedFrames = GReaderDroppedFrames.exchange(0, std::memory_order_relaxed);
	snapshot.RtpDroppedFrames = GRtpDroppedFrames.exchange(0, std::memory_order_relaxed);
	return snapshot;
}
