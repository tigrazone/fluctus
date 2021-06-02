#ifndef CL_BXDF_IDEAL_DIELECTRIC
#define CL_BXDF_IDEAL_DIELECTRIC

#include "utils.cl"
#include "fresnel.cl"

// Ideal dielectric
// Check PBRT 8.2 (p.516)

float3 sampleIdealDielectric(Hit *hit, Material *material, bool backface, global TexDescriptor *textures, global uchar *texData, float3 dirIn, float3 *dirOut, float *pdfW, uint *randSeed)
{
	float3 bsdf = (float3)(1.0f, 1.0f, 1.0f);

	float cosI = - dot((dirIn), hit->N);
	float n1 = 1.0f, n2 = material->Ni;
	if (backface) swap_m(n1, n2, float); // inside of material
	float eta = n1 / n2;
	
	float cosThetaT;

	float fr = fresnelDielectric1(cosI, n1, n2, &cosThetaT);
	if (rand(randSeed) < fr)
	{
		// Reflection
		*dirOut = reflect((dirIn), hit->N, &cosI);
	}
	else
	{
		// Refraction
		*dirOut = refract1((dirIn), hit->N, eta, &cosI, &cosThetaT);
		bsdf *= eta * eta; // eta^2 applied in case of radiance transport (16.1.3)
		
		// Simulate absorption
		float3 Ks = matGetFloat3(material->Ks, hit->uvTex, material->map_Ks, textures, texData);
		bsdf *= Ks;
	}

	// (1-fr) or (fr) in pdf and BSDF cancel out
	*pdfW = 1.0f;

	// PBRT eq. 8.8
	// cosTh of geometry term needs to be cancelled out
	float cosO = dot(normalize(*dirOut), hit->N);
	return bsdf / cosO;
}

// BSDF (dirac delta) is non-zero with zero probability for two given directions
float3 evalIdealDielectric()
{
	return (float3)(0.0f, 0.0f, 0.0f);
}

// Probability of supplying a correct refl/refr direction pair is zero
float pdfIdealDielectric()
{
	return 0.0f;
}

#endif