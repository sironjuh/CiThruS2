#pragma once

#include <cstdint>

struct FStreamPerfSnapshot
{
	double ReadbackMs = 0.0;
	uint64_t ReadbackSamples = 0;
	double RtpSendMs = 0.0;
	uint64_t RtpSendSamples = 0;
	uint64_t ReaderDroppedFrames = 0;
	uint64_t RtpDroppedFrames = 0;
};

class StreamPerfStats
{
public:
	static void AddReadbackSample(double milliseconds);
	static void AddRtpSendSample(double milliseconds);
	static void AddReaderDrop(uint64_t count = 1);
	static void AddRtpDrop(uint64_t count = 1);
	static FStreamPerfSnapshot Consume();
};
