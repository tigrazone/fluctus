#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_HPP_TARGET_OPENCL_VERSION 120

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "cl2.hpp"
#include "window.hpp"
#include "tracer.hpp"
#include "math/float3.hpp"

int main(int argc, char* argv[])
{
    // Initial size of window
    int width = (argc > 1) ? atoi(argv[1]) : 800;
    int height = (argc > 2) ? atoi(argv[2]) : 600;

    if (!glfwInit())
    {
        exit(EXIT_FAILURE);
    }

    Tracer tracer(width, height);

    /*
    std::cout << std::endl;
    std::cout << "sizeof(FireRays::float4): " << sizeof(FireRays::float4) << std::endl;
    std::cout << "alignof(FireRays::float4): " << alignof(FireRays::float4) << std::endl;
    std::cout << "sizeof(cl_float4): " << sizeof(cl_float4) << std::endl;
    std::cout << "alignof(cl_float4): " << alignof(cl_float4) << std::endl;
    std::cout << std::endl;
     */

    int iter = 0;

    // Main loop
    while(tracer.running())
    {
        // Do stuff
        tracer.update();
        if(++iter % 50 == 0) std::cout << "\rIteration " << iter << std::flush;
        //std::cout << "Press any key to continue..." << std::endl; system("read");
    }

    glfwTerminate(); // in tracer destructor?

    return 0;
}

