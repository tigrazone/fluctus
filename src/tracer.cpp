#include <ctime>
#include "tracer.hpp"

#include "window.hpp"
#include "progressview.hpp"
#include "clcontext.hpp"
#include "settings.hpp"
#include "utils.h"
#include "geom.h"

double STARTtime;
size_t Niteration;

namespace fr = FireRays;

Tracer::Tracer(int width, int height) : useWavefront(Settings::getInstance().getUseWavefront())
{
    resetParams(width, height);

    // For getting build options from program state
    clt::Kernel::setUserPointer(static_cast<void*>(this));

    // Before UI
    initCamera();
    initPostProcessing();
    initAreaLight();

    scene.reset(new Scene());

    // done only once (VS debugging stops working if context is recreated)
    window = new PTWindow(width, height, this); // this = glfw user pointer
    window->setShowFPS(true);
#ifdef WITH_OPTIX
    denoiser.bindBuffers(window);
#endif
    clctx = new CLContext();
    window->setCLContextPtr(clctx);
    window->setupGUI();
    clctx->setup(window);
    setupToolbar();
}

void Tracer::resetParams(int width, int height)
{
    auto& s = Settings::getInstance();
    const float renderScale = s.getRenderScale();

    // not passed to GPU
    maxRenderTime = Settings::getInstance().getMaxRenderTime();

    params.width = static_cast<unsigned int>(width * renderScale);
    params.height = static_cast<unsigned int>(height * renderScale);
    // env map will be overriden after scene load if it is present
    params.useEnvMap = cl_uint(s.getUseEnvMap());
    params.useAreaLight = cl_uint(s.getUseAreaLight());
    params.envMapStrength = 1.0f;
    params.maxBounces = s.getMaxPathDepth();
    params.sampleImpl = cl_uint(s.getSampleImplicit());
    params.sampleExpl = cl_uint(s.getSampleExplicit());
    params.useRoulette = cl_uint(s.getUseRussianRoulette());
    params.wfSeparateQueues = cl_uint(s.getUseSeparateQueues());
    params.maxSpp = cl_uint(s.getMaxSpp());
    params.width1 = 1.0f /(float) params.width;
    params.height1 = 1.0f /(float) params.height;
}

// Run whenever a scene is loaded
void Tracer::init(int width, int height, std::string sceneFile)
{
    resetParams(width, height);

    window->showMessage("Loading scene");
    selectScene(sceneFile);
    loadState();
    window->showMessage("Creating BVH");
    initHierarchy();

    // Diagonal gives maximum ray length within the scene
    const AABB_t bounds = bvh->getSceneBounds();
    params.worldRadius = cl_float(length(bounds.max - bounds.min) * 0.5f);

    window->showMessage("Uploading scene data");
    clctx->uploadSceneData(bvh, scene.get());

    // Data uploaded to GPU => no longer needed
    delete bvh;

    // Setup GUI sliders with correct values
    updateGUI();

    // Hide status message
    window->hideMessage();
}

// Render interactive preview
void Tracer::renderInteractive()
{
    // Show toolbar
    toggleGUI();

    while (running())
    {
        update();
    }
}

// Final frame render with predefined spp
void Tracer::renderSingle(int spp, bool denoise)
{
    const bool drawProgress = true;

    // Currently only MK can guarantee given spp for every pixel
    if (useWavefront)
        toggleRenderer();

    // Setup
    if (params.useRoulette)
    {
        std::cout << "Turning off russian roulette" << std::endl;
        params.useRoulette = false;
    }

    if (denoise)
    {
        useDenoiser = true;
        clctx->recompileKernels(false);
    }

    clctx->updateParams(params);
    clctx->enqueueResetKernel(params);

    std::cout << "Rendering " << spp << " spp at " << params.maxBounces << " bounces" << std::endl;

    // Render loop
    int sample = 0;
    while(sample < spp && running())
    {
        if (drawProgress)
        {
            window->draw();
            glFinish();
        }
        
        clctx->enqueueRayGenKernel(params);

        for (int bounce = 0; bounce < params.maxBounces + 1; bounce++)
        {
            clctx->enqueueNextVertexKernel(params);
            clctx->enqueueBsdfSampleKernel(params);
        }

        // Splat results
        clctx->enqueueSplatKernel(params); // transitions all to raygen

        // Postprocess
        clctx->enqueuePostprocessKernel(params);

        // Finish command queue
        clctx->finishQueue();

        // Check for exit etc.
        glfwPollEvents();

        if (sample % 10 == 0)
            std::cout << "\rRendered: " << sample << "/" << spp << std::flush;

        sample++;
    }

    // Export result
    clctx->saveImage("output_" + std::to_string(sample) + ".png", params);
#ifdef WITH_OPTIX
    if (denoise)
    {
        std::cout << "Initializing denoiser..." << std::endl;
        denoiser.denoise();
        clctx->saveImage("output_" + std::to_string(sample) + "_denoised.png", params);
    }
#else
    (void)denoise;
#endif
}

inline void printStats(CLContext *ctx)
{
    static double lastPrinted = 0;
    double now = glfwGetTime();
    double delta = now - lastPrinted;
    if (delta > 1.0)
    {
        lastPrinted = now;
        ctx->updateRenderPerf(delta); // updated perf can now be accessed from anywhere
        PerfNumbers perf = ctx->getRenderPerf();
        printf("pass %d, %ds | %.1fM primary, %.1fM extension, %.1fM shadow, %.1fM samples, total: %.1fMRays/s\r",
            Niteration + 1, (size_t)(now - STARTtime), perf.primary, perf.extension, perf.shadow, perf.samples, perf.total);

        // Reset stat counters (synchronously...)
        ctx->resetStats();
    }
}

void Tracer::update()
{
    // Calculate time since last update
    double newT = glfwGetTime();
    float deltaT = (float)std::min(newT - lastUpdate, 0.1); // seconds
    lastUpdate = newT;
    
    // React to key presses
    glfwPollEvents();
    pollKeys(deltaT);

    glFinish(); // locks execution to refresh rate of display (GL)

    // Update RenderParams in GPU memory if needed
    if(paramsUpdatePending)
    {
        // Recompile kernels (conservatively!)
        clctx->recompileKernels(false); // no need to set arguments

        // Update render dimensions
        const float renderScale = Settings::getInstance().getRenderScale();
        window->getFBSize(params.width, params.height);
        params.width = static_cast<unsigned int>(params.width * renderScale);
        params.height = static_cast<unsigned int>(params.height * renderScale);
		
		params.width1 = 1.0f /(float) params.width;
		params.height1 = 1.0f /(float) params.height;

        updateGUI();
        clctx->updateParams(params);
        paramsUpdatePending = false;
        iteration = 0; // accumulation reset
		
        renderTimeStart = newT;		
		
		//global info
        Niteration = 0;
		STARTtime = glfwGetTime();
    }

    if(maxRenderTime > 0 && newT >= renderTimeStart + maxRenderTime)
    {
        window->draw();
        return;
    }

    QueueCounters cnt = {};
    
    if (useWavefront)
    {
        // Aila-style WF
        cl_uint maxBounces = params.maxBounces;
        int N = 1;
        
        if (iteration == 0)
        {
            // Set to 2-bounce for preview
            params.maxBounces = std::min((cl_uint)2, maxBounces);
            clctx->updateParams(params);
            N = 3;

            // Create and trace primary rays
            clctx->resetPixelIndex();
            clctx->enqueueWfResetKernel(params); // puts all in raygen queue
            clctx->enqueueWfRaygenKernel(params);
            clctx->enqueueWfExtRayKernel(params);
            clctx->enqueueClearWfQueues();
        }

        // Advance wavefront N segments
        for (int i = 0; i < N; i++)
        {
            // Fill queues
            clctx->enqueueWfLogicKernel(params, iteration == 0);

            // Operate on queues
            clctx->enqueueWfRaygenKernel(params);
            clctx->enqueueWfMaterialKernels(params);
            clctx->enqueueGetCounters(&cnt); // the subsequent kernels don't grow the queues
            clctx->enqueueWfExtRayKernel(params);
            clctx->enqueueWfShadowRayKernel(params);

            // Clear queues
            clctx->enqueueClearWfQueues();
        }

        // Reset bounces
        if (iteration == 0)
        {
            params.maxBounces = maxBounces;
            clctx->updateParams(params);
        }
    }
    else
    {
        // Luxrender-style microkernels
        if (iteration == 0)
        {
            // Interactive preview: 1 bounce indirect
            clctx->enqueueResetKernel(params);
            clctx->enqueueRayGenKernel(params);

            // Two segments
            clctx->enqueueNextVertexKernel(params);
            clctx->enqueueBsdfSampleKernel(params);
            clctx->enqueueNextVertexKernel(params);
            clctx->enqueueBsdfSampleKernel(params);

            // Preview => also splat incomplete paths
            clctx->enqueueSplatPreviewKernel(params);
        }
        else
        {
            // Generate new camera rays
            clctx->enqueueRayGenKernel(params);

            // Trace rays
            clctx->enqueueNextVertexKernel(params);

            // Direct lighting + environment map IS
            clctx->enqueueBsdfSampleKernel(params);

            // Splat results
            clctx->enqueueSplatKernel(params);
        }
    }

    // Postprocess
    clctx->enqueuePostprocessKernel(params);

    // Finish command queue
    clctx->finishQueue();

    // Enqueue WF pixel index update
    clctx->updatePixelIndex(params.width * params.height, cnt.raygenQueue);

    // Denoise and draw preview
#ifdef WITH_OPTIX
    const int threshold = 10;
    if (!useDenoiser || iteration < threshold || denoiserStrength == 0.0f)
    {
        // Don't run denoiser (yet)
        window->draw();
    }
    else if (iteration % threshold == 0) 
    {
        // Denoise sparingly (expensive!)
        denoiser.denoise();
        window->drawDenoised();
    }
    else
    {
        // Display cached result, keep UI responsive
        window->displayDenoised();
    }
#else
    window->draw();
#endif
    
    if (useWavefront)
    {
        // Update statsAsync based on queues
        clctx->statsAsync.extensionRays += cnt.extensionQueue;
        clctx->statsAsync.shadowRays += cnt.shadowQueue;
        clctx->statsAsync.primaryRays += cnt.raygenQueue;
        clctx->statsAsync.samples += (iteration > 0) ? cnt.raygenQueue : 0;
    }
    else
    {
        // Explicit atomic render stats only on MK
        clctx->fetchStatsAsync();
    }

    // Calculate tracing performance without overhead
    //clctx->checkTracingPerf();

    // Display render statistics (MRays/s)
    printStats(clctx);

    // Update iteration counter
    iteration++;
    Niteration++;

    if (iteration % 1000 == 0)
        saveImage();
}

// Runs benchmark on conference, egyptcat and kitchen (30s each)
// Generates csv (stats over time) or txt (averages)
void Tracer::runBenchmark()
{
    // Setup renderer state for benchmarking
    params.width = 1024;
    params.height = 1024;
    Settings::getInstance().setRenderScale(1.0f);
    window->setSize(params.width, params.height);
	
    params.width1 = 1.0f /(float) params.width;
    params.height1 = 1.0f /(float) params.height;
	
    updateGUI();

    // Called when scene changes
    auto resetRenderer = [&]()
    {
        iteration = 0;
		Niteration = 0;
		STARTtime = glfwGetTime();
        glFinish();
        clctx->updateParams(params);
        clctx->enqueueResetKernel(params);
        clctx->enqueueWfResetKernel(params);
        clctx->enqueueClearWfQueues();
        clctx->finishQueue();
        clctx->resetStats();
    };
    
    std::vector<const char*> scenes =
    { 
        "assets/egyptcat/egyptcat.obj",
        "assets/conference/conference.obj",
        "assets/country_kitchen/Country-Kitchen.obj",
    };

    std::stringstream simpleReport;
    std::stringstream csvReport;
    csvReport << "scene;time;primary;extension;shadow;total;samples\n";

    // Stats include time dimension
    std::vector<RenderStats> statsLog;
    double lastLogTime = 0;

    auto logStats = [&](const char* scene, double elapsed, double deltaT)
    {
        RenderStats stats = clctx->getStats();
        statsLog.push_back(stats);
        clctx->resetStats();
        lastLogTime = glfwGetTime();
        double s = 1e6 * deltaT;
        csvReport << scene << ";" << elapsed << ";" << stats.primaryRays / s << ";"
            << stats.extensionRays / s << ";" << stats.shadowRays / s << ";"
            << (stats.primaryRays + stats.extensionRays + stats.shadowRays) / s << ";"
            << stats.samples / s << "\n";
    };

    toggleGUI();
    window->setShowFPS(false);
    auto prg = window->getProgressView();
    
    const double RENDER_LEN = 30.0;
    for (int i = 0; i < scenes.size(); i++) {
        std::string counter = std::to_string(i + 1) + "/" + std::to_string(scenes.size());
        init(params.width, params.height, scenes[i]);
        resetRenderer();

        double startT = glfwGetTime();
        double currT = startT;
        while (currT - startT < RENDER_LEN)
        {
            QueueCounters cnt;

            glfwPollEvents();
            if (!window->available()) exit(0); // react to exit button

            if (useWavefront)
            {
                clctx->enqueueWfLogicKernel(params, false);
                clctx->enqueueWfRaygenKernel(params);
                clctx->enqueueWfMaterialKernels(params);
                clctx->enqueueGetCounters(&cnt);
                clctx->enqueueWfExtRayKernel(params);
                clctx->enqueueWfShadowRayKernel(params);
                clctx->enqueueClearWfQueues();
            }
            else
            {
                clctx->enqueueRayGenKernel(params);
                clctx->enqueueNextVertexKernel(params);
                clctx->enqueueBsdfSampleKernel(params);
                clctx->enqueueSplatKernel(params);
            }

            clctx->enqueuePostprocessKernel(params);

            // Synchronize
            clctx->finishQueue();

            // Update statistics
            if (useWavefront)
            {
                // Update statsAsync based on queues
                clctx->statsAsync.extensionRays += cnt.extensionQueue;
                clctx->statsAsync.shadowRays += cnt.shadowQueue;
                clctx->statsAsync.primaryRays += cnt.raygenQueue;
                clctx->statsAsync.samples += (iteration > 0) ? cnt.raygenQueue : 0;

                // Update index of next pixel to shade
                clctx->updatePixelIndex(params.width * params.height, cnt.raygenQueue);
            }
            else
            {
                // Fetch explicit stats from device
                clctx->fetchStatsAsync();
            }


            // Draw image + loading bar
            prg->showMessage("Running benchmark " + counter, (currT - startT) / RENDER_LEN);

            // Save statistics every half a second to log for further processing
            double deltaT = currT - lastLogTime;
            if (deltaT > 0.5)
                logStats(scenes[i], currT - startT, deltaT);

            iteration++;
			Niteration++;
            currT = glfwGetTime();
        }

        // Process statistics for current scene
        logStats(scenes[scenes.size() - 1], currT - startT, currT - lastLogTime);
        double time = currT - startT;
        unsigned long long sums[] = { 0, 0, 0, 0 };
        for (RenderStats &s : statsLog)
        {
            sums[0] += s.primaryRays;
            sums[1] += s.extensionRays;
            sums[2] += s.shadowRays;
            sums[3] += s.samples;
        }

        double scale = 1e6 * time;
        double prim = sums[0] / scale;
        double ext = sums[1] / scale;
        double shdw = sums[2] / scale;
        double samp = sums[3] / scale;
        
        char statistics[512];
        sprintf(statistics, "%s: %.1fM primary, %.2fM extension, %.2fM shadow, %.2fM samples, total: %.2fM rays/s", scenes[i], prim, ext, shdw, samp, prim + ext + shdw);
        std::cout << statistics << std::endl;
        simpleReport << statistics << std::endl;
        statsLog.clear();
    }

    prg->hide();
    toggleGUI();
    window->setShowFPS(true);

    // Output report
    std::string outpath = saveFileDialog("Save results", "", { "*.txt", "*.csv" });
    if (!outpath.empty())
    {
        if (!endsWith(outpath, ".csv") && !endsWith(outpath, ".txt")) outpath += ".csv";
        std::ofstream outfile(outpath);

        if (!outfile.good()) {
            std::cout << "Failed to write benchmark report!" << std::endl;
            return;
        }

        std::string contents = (endsWith(outpath, ".csv")) ? csvReport.str() : simpleReport.str();
        outfile << contents;
    }
}

void Tracer::runBenchmarkFromFile(std::string filename)
{
    const std::string SETTINGS_KEY = "settings";
    const std::string SKIP_PP_KEY = "skipPP";

    Settings& settings = Settings::getInstance();
    std::string baseFolder = getUnixFolderPath(filename, true);

    std::ifstream fileStream(filename);
    if (!fileStream)
    {
        std::cout << "Could not open file: " << filename << ", aborting benchmark..." << std::endl;
        return;
    }
    json base;
    fileStream >> base;

    // Default Values
    params.width = 1024;
    params.height = 1024;
    Settings::getInstance().setRenderScale(1.0f);
    bool skipPostProcess = false;

    auto preprocessSettings = [&](json& jsonFile)
    {
        if (!json_contains(jsonFile, SETTINGS_KEY))
            return;
        json& settingsFile = jsonFile[SETTINGS_KEY];
        if (json_contains(settingsFile, "envMap") && !isAbsolutePath(settingsFile["envMap"].get<std::string>()))
        {
            settingsFile["envMap"] = baseFolder + settingsFile["envMap"].get<std::string>();
        }
    };

    auto importSettings = [&](const json& baseJson)
    {
        if (json_contains(baseJson, SETTINGS_KEY))
        {
            const json& settingsJson = baseJson[SETTINGS_KEY];
            settings.import(settingsJson);
            if (json_contains(settingsJson, SKIP_PP_KEY))
            {
                skipPostProcess = settingsJson[SKIP_PP_KEY].get<bool>();
            }
        }
    };

    auto importDefaultSettings = [&]()
    {
        importSettings(base);
    };

    auto initSettings = [&](const json& sceneJson)
    {
        importDefaultSettings();
        importSettings(sceneJson);
        window->setSize(settings.getWindowWidth(), settings.getWindowHeight());
        updateGUI();
        resetParams(settings.getWindowWidth(), settings.getWindowHeight());
        useWavefront = settings.getUseWavefront();
        initCamera();
        initPostProcessing();
        initAreaLight();
    };

    // Called when scene changes
    auto resetRenderer = [&]()
    {
        clctx->recompileKernels(false);
        iteration = 0;
		Niteration = 0;
		STARTtime = glfwGetTime();
        glFinish();
        clctx->updateParams(params);
        clctx->enqueueResetKernel(params);
        clctx->enqueueWfResetKernel(params);
        clctx->enqueueClearWfQueues();
        clctx->finishQueue();
        clctx->resetStats();
    };

    // Load Default Settings for all Benchmarks if provided
    preprocessSettings(base);
    importDefaultSettings();

    std::string outputFolder = baseFolder;
    if (json_contains(base, "outputFolder"))
    {
        const std::string outputFolderBase = getUnixFolderPath(base["outputFolder"].get<std::string>(), false);
        outputFolder = isAbsolutePath(outputFolderBase) ? outputFolderBase : baseFolder + outputFolderBase;
    }
    createPath(outputFolder);

    json scenes = base["scenes"];

    toggleGUI();
    window->setShowFPS(false);
    auto prg = window->getProgressView();

    int currentSceneNumber = 0;
    for (json sceneJson : scenes)
    {
        // show progress bar
        std::string progressTitle = "Running benchmark " + std::to_string(currentSceneNumber + 1) + "/" + std::to_string(scenes.size());
        prg->showMessage(progressTitle, 0);

        // Init Logging for the current scene
        std::stringstream simpleReport;
        std::stringstream csvReport;
        csvReport << "time;primary;extension;shadow;total;samples\n";
        // Stats include time dimension
        std::vector<RenderStats> statsLog;
        double lastLogTime = 0;
        auto logStats = [&](double elapsed, double deltaTime)
        {
            RenderStats stats = clctx->getStats();
            statsLog.push_back(stats);
            clctx->resetStats();
            lastLogTime = glfwGetTime();
            double s = 1e6 * deltaTime;
            csvReport << elapsed << ";" << stats.primaryRays / s << ";"
                << stats.extensionRays / s << ";" << stats.shadowRays / s << ";"
                << (stats.primaryRays + stats.extensionRays + stats.shadowRays) / s << ";"
                << stats.samples / s << "\n";
        };

        // process all custom settings
        preprocessSettings(sceneJson);
        initSettings(sceneJson);

        // load scene
        std::string sceneFile = baseFolder + sceneJson["file"].get<std::string>();
        init(params.width, params.height, sceneFile);

        // reset render state
        resetRenderer();

        double maxRenderTime = settings.getMaxRenderTime();
        if (settings.getMaxRenderTime() == 0 && settings.getMaxSpp() == 0)
        {
            // no boundary condition given, use 30s as default
            maxRenderTime = 30;
        }

        double startTime = glfwGetTime();
        unsigned long long sampleCount = 0L;
        unsigned long long maxSampleCount = static_cast<unsigned long long>(params.width * params.height) * params.maxSpp;
        auto getProgress = [&](double currentTime)
        {
            const float timeProgress = maxRenderTime != 0 ? float((currentTime - startTime) / maxRenderTime) : 0.0f;
            // Extra check for equality, not sure with long to double conversion and then dividing
            const float sppProgress = maxSampleCount == 0 ? 0 : sampleCount == maxSampleCount ? 1.0f : std::min(float(double(sampleCount) / maxSampleCount), 0.9999f);
            return std::max(timeProgress, sppProgress);
        };
        // if maxRenderTime is 0, then render till maxSpp Condition is reached
        // if both are given, we stop once one of them is reached (although maxSpp doesn't exit immediately due to cheks during logging only (every 0.5s))
        double currentTime = startTime;
        // Wavefront Segment Start
        if (useWavefront)
        {
            clctx->enqueueWfRaygenKernel(params);
            clctx->enqueueWfExtRayKernel(params);
        }
        while ((maxRenderTime == 0 || currentTime - startTime < maxRenderTime) && (maxSampleCount == 0 || sampleCount < maxSampleCount))
        {
            QueueCounters cnt;

            glfwPollEvents();
            if (!window->available()) exit(0); // react to exit button

            if (useWavefront)
            {
                clctx->enqueueWfLogicKernel(params, false);
                clctx->enqueueWfRaygenKernel(params);
                clctx->enqueueWfMaterialKernels(params);
                clctx->enqueueGetCounters(&cnt);
                clctx->enqueueWfExtRayKernel(params);
                clctx->enqueueWfShadowRayKernel(params);
                clctx->enqueueClearWfQueues();
            }
            else
            {
                clctx->enqueueRayGenKernel(params);
                clctx->enqueueNextVertexKernel(params);
                clctx->enqueueBsdfSampleKernel(params);
                clctx->enqueueSplatKernel(params);
            }

            // Possible Skip PP during Benchmarking
            if (!skipPostProcess)
            {
                // Postprocess
                clctx->enqueuePostprocessKernel(params);
            }

            // Synchronize
            clctx->finishQueue();

            // Update statistics
            if (useWavefront)
            {
                // Update statsAsync based on queues
                clctx->statsAsync.extensionRays += cnt.extensionQueue;
                clctx->statsAsync.shadowRays += cnt.shadowQueue;
                clctx->statsAsync.primaryRays += cnt.raygenQueue;
                clctx->statsAsync.samples += (iteration > 0) ? cnt.raygenQueue : 0;
                sampleCount += cnt.splattedSamples;

                // Update index of next pixel to shade
                // only needed for WF
                clctx->updatePixelIndex(params.width * params.height, cnt.raygenQueue);
            }
            else
            {
                // Fetch explicit stats from device
                clctx->fetchStatsAsync();
            }

            iteration++;
            Niteration++;
            currentTime = glfwGetTime();
            // Save statistics every half a second to log for further processing
            double deltaTime = currentTime - lastLogTime;
            if (deltaTime > 0.5)
            {
                sampleCount += clctx->getStats().samples;
                logStats(currentTime - startTime, deltaTime);
            }
            prg->showMessage(progressTitle, getProgress(currentTime));
        }

        // If Skipped Before, PP before saving
        if (skipPostProcess)
        {
            // Postprocess
            clctx->enqueuePostprocessKernel(params);
            clctx->finishQueue();
        }

        // Save final Image
        std::string outputFile = outputFolder + sceneJson["outputFile"].get<std::string>();
        clctx->saveImage(outputFile + ".png", params);
        clctx->saveImage(outputFile + ".hdr", params);

        // Process statistics for current scene
        logStats(currentTime - startTime, currentTime - lastLogTime);

        auto fillSimpleReport = [&]()
        {
            double time = currentTime - startTime;
            unsigned long long sums[] = { 0, 0, 0, 0 };
            for (RenderStats& stats : statsLog)
            {
                sums[0] += stats.primaryRays;
                sums[1] += stats.extensionRays;
                sums[2] += stats.shadowRays;
                sums[3] += stats.samples;
            }

            double scale = 1e6 * time;
            double prim = sums[0] / scale;
            double ext = sums[1] / scale;
            double shdw = sums[2] / scale;
            double samp = sums[3] / scale;

            char statistics[512];
            sprintf(statistics, "%.1fM primary, %.2fM extension, %.2fM shadow, %.2fM samples, total: %.2fM rays/s", prim, ext, shdw, samp, prim + ext + shdw);
            std::cout << statistics << std::endl;
            simpleReport << statistics << std::endl;
            statsLog.clear();
        };

        auto saveStats = [&]()
        {
            // save stats!
            std::ofstream csvFile(outputFile + ".csv");
            if (csvFile.good())
            {
                csvFile << csvReport.str();
            }
            else
            {
                std::cout << "Failed to write CSV benchmark report!" << std::endl;
            }

            std::ofstream txtFile(outputFile + ".txt");
            if (txtFile.good())
            {
                txtFile << simpleReport.str();
            }
            else
            {
                std::cout << "Failed to write TXT benchmark report!" << std::endl;
            }
        };

        fillSimpleReport();
        saveStats();
        currentSceneNumber++;
    }

    prg->hide();
    importDefaultSettings();
    resetRenderer();
    toggleGUI();
    window->setShowFPS(true);
}

// Empty file name means scene selector is opened
void Tracer::selectScene(std::string file)
{
    if (file.empty())
    {
        const std::string selected = openFileDialog("Select a scene file", "assets/", { "*.obj", "*.ply", "*.pbf", "*.pbrt", "*.sc.json" });
        file = (!selected.empty()) ? selected : "assets/egyptcat/egyptcat.obj";
    }

    scene.reset(new Scene());
    scene->loadModel(file, window->getProgressView());
	
	if(scene->updateCamera) 
	{
		params.camera = scene->cam;
		scene->updateCamera = false;
		cameraRotation.x = 0; 
		cameraRotation.y = 0;
		cameraSpeed = 1.0f;
        // updateCamera();
		printf("*** camera updated from file\n");
	}
	
    if (envMap)
    {
        scene->setEnvMap(envMap);
        params.useEnvMap = cl_int(true);
    }

    sceneHash = scene->hashString();

    const std::string envMapName = Settings::getInstance().getEnvMapName();
    if (envMapName.empty())
        return;

    if (!envMap || envMap->getName() != envMapName)
    {
        envMap.reset(new EnvironmentMap(envMapName));
        scene->setEnvMap(envMap);
        initEnvMap();
    }
    else
    {
        std::cout << "Reusing environment map" << std::endl;
    }
}

void Tracer::initEnvMap()
{
    // Bool operator => check if ptr is empty
    if (envMap && envMap->valid())
    {
        params.useEnvMap = cl_int(true);
        this->hasEnvMap = true;
        clctx->createEnvMap(envMap.get());
    }
}

// Check if old hierarchy can be reused
void Tracer::initHierarchy()
{
    const std::string hashFile = "data/hierarchies/hierarchy_" + sceneHash + ".bin";
    const std::ifstream input(hashFile, std::ios::in);

    if (input.good())
    {
		std::cout << "Trinagles: " << scene->getTriangles().size() << std::endl;
        std::cout << "Reusing BVH..." << std::endl;
        loadHierarchy(hashFile, scene->getTriangles());
    }
    else
    {
		std::cout << "Trinagles: " << scene->getTriangles().size() << std::endl;
        std::cout << "Building BVH..." << std::endl;
        constructHierarchy(scene->getTriangles(), SplitMode::SAH, window->getProgressView());
        saveHierarchy(hashFile);
    }
}

Tracer::~Tracer()
{
    delete window;
    delete clctx;
}

bool Tracer::running()
{
    return window->available();
}

// Callback for when the window size changes
void Tracer::resizeBuffers(int width, int height)
{
    window->getScreen()->resizeCallbackEvent(width, height);
    ProgressView *pv = window->getProgressView();
    if (pv) pv->center();

    //window->createTextures();
    window->createPBOs();
    clctx->setupPixelStorage(window);
#ifdef WITH_OPTIX
    denoiser.resizeBuffers(window);
#endif
    paramsUpdatePending = true;
}

inline void writeVec(std::fstream &out, FireRays::float3 &vec)
{
    write(out, vec.x);
    write(out, vec.y);
    write(out, vec.z);
}

inline void readVec(std::fstream &in, FireRays::float3 &vec)
{
	read(in, vec.x);
	read(in, vec.y);
	read(in, vec.z);
}

// Shared method for read/write => no forgotten members
void Tracer::iterateStateItems(StateIO mode)
{
	#define rw(item) if(mode == StateIO::WRITE) write(stream, item); else read(stream, item);
	#define rwVec(item) if(mode == StateIO::WRITE) writeVec(stream, item); else readVec(stream, item);

	auto fileMode = std::ios::binary | ((mode == StateIO::WRITE) ? std::ios::out : std::ios::in);
	std::fstream stream("data/states/state_" + sceneHash + ".dat", fileMode);

	if (stream.good())
	{
		// Camera
		rw(cameraRotation.x);
		rw(cameraRotation.y);
		rw(cameraSpeed);
		rw(params.camera.fov);
		
		if(mode == StateIO::READ) {
			params.camera.fovSCALE = tan(toRad(0.5f * params.camera.fov));
		}
		
        rw(params.camera.focalDist);
        rw(params.camera.apertureSize);
		rwVec(params.camera.dir);
		rwVec(params.camera.pos);
		rwVec(params.camera.right);
		rwVec(params.camera.up);

		// Lights
		rwVec(params.areaLight.N);
		rwVec(params.areaLight.pos);
		rwVec(params.areaLight.right);
		rwVec(params.areaLight.up);
		rwVec(params.areaLight.E);
		rw(params.areaLight.size.x);
		rw(params.areaLight.size.y);
		rw(params.envMapStrength);

		// Sampling parameters
		rw(params.maxBounces);
		rw(params.useAreaLight);
		rw(params.useEnvMap);
		rw(params.sampleExpl);
		rw(params.sampleImpl);
        rw(params.useRoulette);

        // Post processing
        rw(params.ppParams.exposure);
        rw(params.ppParams.tmOperator);

		std::cout << ((mode == StateIO::WRITE) ? "State dumped" : "State imported") << std::endl;
	}
	else
	{
		std::cout << "Could not open state file" << std::endl;
	}

	#undef rw
	#undef rwVec
}

Hit Tracer::pickSingle()
{
    // Position relative to upper-left
    double xpos, ypos;
    glfwGetCursorPos(window->glfwWindowPtr(), &xpos, &ypos);

    int width, height;
    glfwGetWindowSize(window->glfwWindowPtr(), &width, &height);

    // Ignores FB scaling
    float NDCx = xpos / width;
    float NDCy = (height - ypos) / height;

    return clctx->pickSingle(NDCx, NDCy);
}

// Set DoF depth based on hit distance
void Tracer::pickDofDepth()
{
    Hit hit = pickSingle();
    
    printf("Pick result: i = %d, dist = %.2f\n\n", hit.i, hit.t);

    // If scene hit, set focal distance
    if (hit.i > -1)
    {
        params.camera.focalDist = hit.t;
        paramsUpdatePending = true;
    }
}

void Tracer::saveState()
{
	iterateStateItems(StateIO::WRITE);
}

void Tracer::loadState()
{
	iterateStateItems(StateIO::READ);
}

void Tracer::saveImage()
{
    std::time_t epoch = std::time(nullptr);
    std::string fileName = "output_" + std::to_string(epoch) + ".png";
#ifdef WITH_OPTIX
    if (useDenoiser)
        denoiser.denoise();
#endif
    clctx->saveImage(fileName, params);
}

void Tracer::loadHierarchy(const std::string filename, std::vector<RTTriangle>& triangles)
{
    m_triangles = &triangles;
    params.n_tris = (cl_uint)m_triangles->size();
    bvh = new SBVH(m_triangles, filename);
}

void Tracer::saveHierarchy(const std::string filename)
{
    bvh->exportTo(filename);
}

void Tracer::constructHierarchy(std::vector<RTTriangle>& triangles, SplitMode splitMode, ProgressView *progress)
{
    m_triangles = &triangles;
    params.n_tris = (cl_uint)m_triangles->size();
    bvh = new SBVH(m_triangles, splitMode, progress);
}

void Tracer::initCamera()
{
    auto s = Settings::getInstance().getCameraSettings();
    Camera cam;
    cam.pos = s.pos;
    cam.right = s.right;
    cam.up = s.up;
    cam.dir = s.dir;
    cam.fov = s.fov;
	cam.fovSCALE = tan(toRad(0.5f * cam.fov));
    cam.apertureSize = s.apertureSize;
    cam.focalDist = s.focalDist;

    cameraRotation = s.cameraRotation;
    cameraSpeed = 1.0f;

    params.camera = cam;

    paramsUpdatePending = true;
}

void Tracer::initPostProcessing()
{
    PostProcessParams p;
    p.exposure = 1.0f;
    p.tmOperator = Settings::getInstance().getTonemap();

    params.ppParams = p;
    paramsUpdatePending = true;
}

void Tracer::initAreaLight()
{
    const auto s = Settings::getInstance().getAreaLightSettings();
    params.areaLight.E = s.E;
    params.areaLight.right = s.right;
    params.areaLight.up = s.up;
    params.areaLight.N = s.N;
    params.areaLight.pos = s.pos;
    params.areaLight.size = s.size;
    paramsUpdatePending = true;
}

// "The rows of R represent the coordinates in the original space of unit vectors along the
//  coordinate axes of the rotated space." (https://www.fastgraph.com/makegames/3drotation/)
void Tracer::updateCamera()
{
    if(cameraRotation.x < 0) cameraRotation.x += 360.0f;
    if(cameraRotation.x > 360.0f) cameraRotation.x -= 360.0f;
    if (cameraRotation.y < -90) cameraRotation.y = -90;
    if (cameraRotation.y > 90) cameraRotation.y = 90;

    const auto right = scene->getWorldRight();
    const auto up = scene->getWorldUp();

    const fr::matrix rot = rotation(right, toRad(cameraRotation.y)) * rotation(up, toRad(cameraRotation.x));

    params.camera.right = fr::float3(rot.m00, rot.m01, rot.m02);
    params.camera.up =    fr::float3(rot.m10, rot.m11, rot.m12);
    params.camera.dir =  -fr::float3(rot.m20, rot.m21, rot.m22); // camera points in the negative z-direction
}

void Tracer::updateAreaLight()
{
    params.areaLight.right = params.camera.right;
    params.areaLight.up = params.camera.up;
    params.areaLight.N = params.camera.dir;
    params.areaLight.pos = params.camera.pos - 0.01f * params.camera.dir;
}

// Load a scene with keys 1-5 based on shortcuts in settings.json
void Tracer::quickLoadScene(unsigned int key)
{
    auto mapping = Settings::getInstance().getShortcuts();
    auto it = mapping.find(key);
    if (it != mapping.end()) init(params.width, params.height, it->second);
}

// Controls the way light sources are sampled in path tracing
void Tracer::toggleSamplingMode()
{
    if (params.sampleImpl && params.sampleExpl) // both => expl
    {
        params.sampleImpl = false;
        std::cout << std::endl << "Sampling mode: explicit" << std::endl;
    }
    else if (params.sampleExpl) // expl => impl
    {
        params.sampleExpl = false;
        params.sampleImpl = true;
        std::cout << std::endl << "Sampling mode: implicit" << std::endl;
    }
    else // impl => both
    {
        params.sampleExpl = true;
        std::cout << std::endl << "Sampling mode: MIS" << std::endl;
    }
}

void Tracer::toggleLightSourceMode()
{
    if (!hasEnvMap)
    {
        std::cout << std::endl << "No environment map loaded!" << std::endl;
    }
    else if (params.useAreaLight && params.useEnvMap) // both => env
    {
        params.useAreaLight = false;
        std::cout << std::endl << "Light mode: environment" << std::endl;
    }
    else if (params.useEnvMap) // env => area
    {
        params.useEnvMap = false;
        params.useAreaLight = true;
        std::cout << std::endl << "Light mode: area light" << std::endl;
    }
    else // area => both
    {
        params.useEnvMap = true;
        std::cout << std::endl << "Light mode: both" << std::endl;
    }
}

void Tracer::toggleRenderer()
{
    useWavefront = !useWavefront;
    window->setRenderMethod((useWavefront) ? PTWindow::RenderMethod::WAVEFRONT : PTWindow::RenderMethod::MICROKERNEL);
}

void Tracer::toggleDenoiserVisibility()
{
#ifdef WITH_OPTIX
    if (!useDenoiser)
    {
        useDenoiser = true;
        clctx->recompileKernels(false);
        denoiserStrength = 0.0f;
    }

    denoiserStrength = (denoiserStrength > 0.5f) ? 0.0f : 1.0f;
    denoiser.setBlend(1.0f - denoiserStrength);
    updateGUI();
#endif
}

void Tracer::handleChar(unsigned int codepoint)
{
    window->getScreen()->charCallbackEvent(codepoint);
}

void Tracer::handleFileDrop(int count, const char **filenames)
{
    if (window->getScreen()->dropCallbackEvent(count, filenames)) return;

    for (int i = 0; i < count; i++)
    {
        std::string file(filenames[i]);
        if (endsWithAny(file, { ".obj", ".ply", ".pbf", ".pbrt", ".sc.json" }))
        {
            init(params.width, params.height, file);
            paramsUpdatePending = true;
            return;
        }
        if (endsWith(file, ".hdr"))
        {
            if (!envMap || envMap->getName() != file)
            {
                Settings::getInstance().setEnvMapName(file);
                envMap.reset(new EnvironmentMap(file));
                scene->setEnvMap(envMap);
                initEnvMap();
                paramsUpdatePending = true;
            }
            
            return;
        }
        if (endsWith(file, ".bm.json"))
        {
            runBenchmarkFromFile(file);
            return;
        }
    }

    std::cout << "Unknown file format" << std::endl;
}

#define cam_pos params.camera.pos
#define cam_center (params.camera.pos + params.camera.dir)
void Tracer::printDebug()
{
    printf("\r\nCamera Position: %f, %f, %f\r\n", cam_pos.x, cam_pos.y, cam_pos.z);
    printf("Camera Look At: %f, %f, %f\r\n", cam_center.x, cam_center.y, cam_center.z);
}
#undef cam_center
#undef cam_pos

// Functional keys that need to be triggered only once per press
#define matchInit(key, expr) case key: expr; paramsUpdatePending = true; break;
#define matchKeep(key, expr) case key: expr; break;
void Tracer::handleKeypress(int key, int scancode, int action, int mods)
{
    if (window->getScreen()->keyCallbackEvent(key, scancode, action, mods)) return;

    switch (key)
    {
        // Force init
        matchInit(GLFW_KEY_1,           quickLoadScene(1));
        matchInit(GLFW_KEY_2,           quickLoadScene(2));
        matchInit(GLFW_KEY_3,           quickLoadScene(3));
        matchInit(GLFW_KEY_4,           quickLoadScene(4));
        matchInit(GLFW_KEY_5,           quickLoadScene(5));
        matchInit(GLFW_KEY_6,           quickLoadScene(6));
        matchInit(GLFW_KEY_L,           init(params.width, params.height));  // opens scene selector
        matchInit(GLFW_KEY_H,           toggleLightSourceMode());
        matchInit(GLFW_KEY_7,           toggleRenderer());
        matchInit(GLFW_KEY_F1,          initCamera());
        matchInit(GLFW_KEY_F3,          loadState());
        matchInit(GLFW_KEY_SPACE,       updateAreaLight());
        matchInit(GLFW_KEY_I,           params.maxBounces += 1);
        matchInit(GLFW_KEY_K,           params.maxBounces = std::max(1u, params.maxBounces) - 1);
        matchInit(GLFW_KEY_M,           toggleSamplingMode());
        matchInit(GLFW_KEY_C,           params.wfSeparateQueues = 1 - params.wfSeparateQueues; printf("\nSeparate queues: %u\n", params.wfSeparateQueues));

        // Don't force init
        matchKeep(GLFW_KEY_F2,          saveState());
        matchKeep(GLFW_KEY_F5,          saveImage());
        matchKeep(GLFW_KEY_F6,          toggleDenoiserVisibility(););
        matchKeep(GLFW_KEY_U,           toggleGUI());
        matchKeep(GLFW_KEY_P,           printDebug());
    }
}
#undef matchInit
#undef matchKeep

// Instant and simultaneous key presses (movement etc.)
#define check(key, expr) if(window->keyPressed(key)) { expr; paramsUpdatePending = true; }
void Tracer::pollKeys(float deltaT)
{
    if (shouldSkipPoll()) return;
    
    Camera &cam = params.camera;

    check(GLFW_KEY_W, cam.pos += deltaT * cameraSpeed * 10 * cam.dir;printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
    check(GLFW_KEY_A,           cam.pos -= deltaT * cameraSpeed * 10 * cam.right; printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
    check(GLFW_KEY_S,           cam.pos -= deltaT * cameraSpeed * 10 * cam.dir; printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
    check(GLFW_KEY_D,           cam.pos += deltaT * cameraSpeed * 10 * cam.right; printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
    check(GLFW_KEY_R,           cam.pos += deltaT * cameraSpeed * 10 * cam.up; printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
    check(GLFW_KEY_F,           cam.pos -= deltaT * cameraSpeed * 10 * cam.up; printf("pos x=%.4f y=%.4f z=%.4f\n", cam.pos.x, cam.pos.y, cam.pos.z));
	
	/*
	check(GLFW_KEY_UP,          cameraRotation.y -= 75 * deltaT;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_DOWN,        cameraRotation.y += 75 * deltaT;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_LEFT,        cameraRotation.x -= 75 * deltaT;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_RIGHT,       cameraRotation.x += 75 * deltaT;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
	*/
	
	check(GLFW_KEY_UP,          cameraRotation.y -= 45;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_DOWN,        cameraRotation.y += 45;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_LEFT,        cameraRotation.x -= 45;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
    check(GLFW_KEY_RIGHT,       cameraRotation.x += 45;printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y));
	
    check(GLFW_KEY_PERIOD,      cam.fov = cl_float(std::min(cam.fov + 70 * deltaT, 175.0f)));
    check(GLFW_KEY_COMMA,       cam.fov = cl_float(std::max(cam.fov - 70 * deltaT, 5.0f)));
    check(GLFW_KEY_8,           params.areaLight.size /= cl_float(1 + 5 * deltaT));
    check(GLFW_KEY_9,           params.areaLight.size *= cl_float(1 + 5 * deltaT));
    check(GLFW_KEY_PAGE_DOWN,   params.areaLight.E /= cl_float(1 + 10 * deltaT));
    check(GLFW_KEY_PAGE_UP,     params.areaLight.E *= cl_float(1 + 10 * deltaT));
    check(GLFW_KEY_X,           params.envMapStrength *= cl_float(1 + 5 * deltaT));
    check(GLFW_KEY_Z,           params.envMapStrength /= cl_float(1 + 5 * deltaT));

    if(paramsUpdatePending)
    {
        updateCamera();
    }
}
#undef check

void Tracer::handleMouseButton(int key, int action, int mods)
{
    if (window->getScreen()->mouseButtonCallbackEvent(key, action, mods)) return;

    switch(key)
    {
        case GLFW_MOUSE_BUTTON_LEFT:
            if(action == GLFW_PRESS)
            {
                lastCursorPos = window->getCursorPos();
                mouseButtonState[0] = true;
                //std::cout << "Left mouse button pressed" << std::endl;
            }
            if(action == GLFW_RELEASE)
            {
                mouseButtonState[0] = false;
                //std::cout << "Left mouse button released" << std::endl;
            }
            break;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            if(action == GLFW_PRESS) mouseButtonState[1] = true;
            if(action == GLFW_RELEASE) mouseButtonState[1] = false;
            break;
        case GLFW_MOUSE_BUTTON_RIGHT:
            if(action == GLFW_PRESS) mouseButtonState[2] = true;
            if (action == GLFW_RELEASE)
            {
                mouseButtonState[2] = false;
                pickDofDepth();
            }
            break;
    }
}

void Tracer::handleCursorPos(double x, double y)
{
    if (window->getScreen()->cursorPosCallbackEvent(x, y)) return;

    if(mouseButtonState[0])
    {
        fr::float2 newPos = fr::float2((float)x, (float)y);
        fr::float2 delta =  newPos - lastCursorPos;

        // std::cout << "Mouse delta: " << delta.x <<  ", " << delta.y << std::endl;

        cameraRotation += delta;
		printf("cameraRotation x=%.2f y=%.2f\n", cameraRotation.x, cameraRotation.y);
        lastCursorPos = newPos;

        updateCamera();
        paramsUpdatePending = true;
    }
}

void Tracer::handleMouseScroll(double yoffset)
{
    float newSpeed = (yoffset > 0) ? cameraSpeed * 1.2f : cameraSpeed / 1.2f;
    cameraSpeed = std::max(1e-3f, std::min(1e6f, newSpeed));
    updateGUI();
}
