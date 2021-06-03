#ifndef CL_RANDOM
#define CL_RANDOM

#include "geom.h"

// http://www.burtleburtle.net/bob/hash/integer.html
inline uint hash(uint seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

// Seed is modified (can be used in next iteration)
inline float rand(uint *seed)
{
    *seed = hash(*seed);
    return (float)(*seed) * (one_4294967296); // 1.0f / 2^32
}

#endif