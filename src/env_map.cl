#include "utils.cl"

/* Utilities for evaluating environment map Li's and pdf's */
/* HDRIs are typically stored in linear space, no gamma correction needed! */

// integer UVs
constant sampler_t samplerInt = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

// floating point UVs in range [0,1]
constant sampler_t samplerFloat = CLK_NORMALIZED_COORDS_TRUE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

// Mapping from http://gl.ict.usc.edu/Data/HighResProbes/
// U mapped from [0,2] to [0,1]
inline float2 directionToUV(float3 dir)
{
	return (float2)(atan2pi(dir.x, -dir.z)*0.5f + 0.5f, acospi(dir.y)); // remove *M_INV_PI
}

// Mapping from http://gl.ict.usc.edu/Data/HighResProbes/
// U mapped from [0,1] to [0,2]
inline float3 UVToDirection(float u, float v, float *sinPhi)
{
	/*
    float phi = v * M_PI_F;
    float theta = u * M_2PI_F - M_PI_F;
	*/
	
    float cosPhi, cosTh;	
	
    *sinPhi = sincos(v * M_PI_F, &cosPhi);
	
    float sinTh = sincos(u * M_2PI_F - M_PI_F, &cosTh);
	
    return (float3)(*sinPhi*sinTh, cosPhi, -*sinPhi*cosTh);
}

inline float3 evalEnvMapDir(read_only image2d_t envMap, float3 dir)
{
    float2 uv = directionToUV(dir);
    return read_imagef(envMap, samplerFloat, uv).xyz;
}

inline float3 evalEnvMapUVfloat(read_only image2d_t envMap, float u, float v)
{
    return read_imagef(envMap, samplerFloat, (float2)(u, v)).xyz;
}

inline float3 evalEnvMapUVint(read_only image2d_t envMap, int u, int v)
{
    return read_imagef(envMap, samplerInt, (int2)(u, v)).xyz;
}

typedef struct
{
    const int width;
    const int height;
	global const float *pdfTable;
    global const float *probTable;
    global const int *aliasTable;
} EnvMapContext;

// Uses the Alias Method
inline void sampleEnvMapAlias(float rnd, float3 *L, float *pdfW, EnvMapContext ctx)
{
    const int width = ctx.width;
    const int height = ctx.height;
	
	const int wh = width * height;

	// Sample 1D distribution over whole image
	float rand = rnd * wh;
	int i = min((int)floor(rand), wh - 1);
    float mProb = ctx.probTable[i];
    int uvInd = (rand - i < mProb) ? i : ctx.aliasTable[i];
    //float pdf_uv = ctx.pdfTable[uvInd];

    // Compute outgoing dir
	//int uInd = uvInd % width;
	// int vInd = uvInd / width;
    float u = ((float)(uvInd % width) + 0.5f) * native_recip((float)width);
    float v = ((float)uvInd + 0.5f) * native_recip((float)wh);
	
	float sinTh;
	
    *L = UVToDirection(u, v, &sinTh);

    // Compute pdf
    if (sinTh != 0.0f)
        *pdfW = ctx.pdfTable[uvInd] * native_recip(sinTh);
    else
        *pdfW = 0.0f;
}

// Get pdf of sampling 'direction', used in MIS
float envMapPdf(int width, int height, global float *pdfTable, float3 direction)
{
    if (direction.y > 0.99f)
        return 0.0f;
	
    float2 uv = directionToUV(direction);
    //float sinTh = sin(uv.y * M_PI_F);
	
    //float sinTh = rsqrt (1.0f - direction.y*direction.y);

    int iu = min((int)floor(uv.x * width), width - 1);
    int iv = min((int)floor(uv.y * height), height - 1);

    return pdfTable[iv * width + iu] * rsqrt (1.0f - direction.y*direction.y);
}