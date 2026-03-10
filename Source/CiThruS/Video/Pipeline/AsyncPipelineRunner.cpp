#include "AsyncPipelineRunner.h"
#include "Pipeline.h"
#include "Misc/Debug.h"

#include <stdexcept>
#include <chrono>

AsyncPipelineRunner::AsyncPipelineRunner(Pipeline* pipeline)
	: wantsStop_(false),
	  thread_(std::thread(&AsyncPipelineRunner::RunPipeline, this, pipeline))
{

}

AsyncPipelineRunner::~AsyncPipelineRunner()
{
	wantsStop_ = true;

	thread_.join();
}

void AsyncPipelineRunner::RunPipeline(Pipeline* pipeline)
{
	try
	{
		while (!wantsStop_)
		{
			auto loopStart = std::chrono::steady_clock::now();
			pipeline->Run();
			auto loopEnd = std::chrono::steady_clock::now();
			auto loopDuration = loopEnd - loopStart;
			if (loopDuration < std::chrono::milliseconds(1))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1) - loopDuration);
			}
		}
	}
	catch (const std::exception& exception)
	{
		Debug::Log("Pipeline crashed: " + std::string(exception.what()));
	}

	delete pipeline;
}
