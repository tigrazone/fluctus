#ifndef CL_BXDF
#define CL_BXDF

#include "bxdf_types.h"
#include "diffuse.cl"
#include "ideal_dielectric.cl"
#include "ideal_reflection.cl"
#include "ggx.cl"
#include "glossy.cl"

// Placeholder
typedef float3 Spectrum;

// Note: In these formulas, dirIn points towards the surface, not away from it!

// Generate outgoing direction and pdf given invoming direction
Spectrum bxdfSample(
	Hit *hit,
	Material *material,
	bool backface, // indicates that normal has been flipped (affects refraction)
	global TexDescriptor *textures,
	global uchar *texData,
	float3 dirIn,
	float3 *dirOut,
	float *pdfW,
	uint *randSeed)
{	
	cl_int mtype = material->type;
	
	if(mtype == BXDF_MIXED) {
		mtype = BXDF_DIFFUSE;
	}
	
	if(mtype == BXDF_MIXED) {
		float3 one_three_float3 = ((float3) (1.0f, 1.0f, 1.0f)) / 3.0f;		

		float cosI = - dot(normalize(dirIn), hit->N);
		float n1 = 1.0f, n2 = material->Ni;
		if (backface) swap_m(n1, n2, float); // inside of material

		float fr = schlickDielectric(cosI, n1, n2);		
		
		float3 Kd = matGetAlbedo(material->Kd, hit->uvTex, material->map_Kd, textures, texData);
		float3 Ks = matGetFloat3(material->Ks, hit->uvTex, material->map_Ks, textures, texData);
		float3 Kt = material->Kt; 
		float3 Ke = material->Ke; 
		
          // Compute probabilities for each surface interaction.
          // Specular is just regular reflectiveness * fresnel.
          float rhoS = dot(one_three_float3,  Ks) * fr;
          // If we don't have a specular reflection, choose either diffuse or
          // transmissive
          // Mix them based on the dissolve value of the material
          float rhoD = dot(one_three_float3, Kd) *
                       (1.0f - fr) * (1.0f - material->d);
          float rhoR = dot(one_three_float3, Kt) *
                       (1.0f - fr) * material->d;

          float rhoE = dot(one_three_float3, Ke);

          // Normalize probabilities so they sum to 1.0
          float totalrho = rhoS + rhoD + rhoR + rhoE;
          float totalrho1 = native_recip(totalrho);		  

          rhoS *= totalrho1;
          rhoD *= totalrho1;
          rhoR *= totalrho1;
          rhoE *= totalrho1;
		  
		  float rnd = rand(randSeed);
		  
          // REFLECT glossy
          if (rnd < rhoS) {
			  mtype = BXDF_IDEAL_REFLECTION;
		  }
          // REFLECT diffuse
          else if (rnd < rhoS + rhoD) {
			mtype = BXDF_DIFFUSE;
          }
          // REFRACT
          else if (rnd < rhoD + rhoS + rhoR) {
			  mtype = BXDF_IDEAL_DIELECTRIC;
			  }
          // EMIT
          else {
			  mtype = BXDF_EMISSIVE;
		  }
	}
	
	switch(mtype)
	{
		case BXDF_DIFFUSE:
			return sampleDiffuse(hit, material, textures, texData, dirOut, pdfW, randSeed);
		case BXDF_GLOSSY:
			return sampleGlossy(hit, material, backface, textures, texData, dirIn, dirOut, pdfW, randSeed);
		case BXDF_GGX_ROUGH_REFLECTION:
			return sampleGGXReflect(hit, material, textures, texData, dirIn, dirOut, pdfW, randSeed);
		case BXDF_IDEAL_REFLECTION:
			return sampleIdealReflection(hit, material, backface, textures, texData, dirIn, dirOut, pdfW, randSeed);
		case BXDF_GGX_ROUGH_DIELECTRIC:
			return sampleGGXRefract(hit, material, backface, textures, texData, dirIn, dirOut, pdfW, randSeed);
		case BXDF_IDEAL_DIELECTRIC:
			return sampleIdealDielectric(hit, material, backface, textures, texData, dirIn, dirOut, pdfW, randSeed);
		case BXDF_EMISSIVE:
			return (float3)(1.0f, 1.0f, 1.0f);
	}

	return (float3)(0.0f, 0.0f, 0.0f);
}

// Evaluate bxdf value given invoming and outgoing directions.
Spectrum bxdfEval(
	Hit *hit,
	Material *material,
	bool backface,
	global TexDescriptor *textures,
	global uchar *texData,
	float3 dirIn,
	float3 dirOut,
	uint *randSeed)
{	
	cl_int mtype = material->type;
	if(mtype == BXDF_MIXED) {
		mtype = BXDF_DIFFUSE;
	}	
	
	if(mtype == BXDF_MIXED) {
		float3 one_three_float3 = ((float3) (1.0f, 1.0f, 1.0f)) / 3.0f;		

		float cosI = - dot(normalize(dirIn), hit->N);
		float n1 = 1.0f, n2 = material->Ni;
		if (backface) swap_m(n1, n2, float); // inside of material

		float fr = schlickDielectric(cosI, n1, n2);		
		
		float3 Kd = matGetAlbedo(material->Kd, hit->uvTex, material->map_Kd, textures, texData);
		float3 Ks = matGetFloat3(material->Ks, hit->uvTex, material->map_Ks, textures, texData);
		float3 Kt = material->Kt; 
		float3 Ke = material->Ke; 
		
          // Compute probabilities for each surface interaction.
          // Specular is just regular reflectiveness * fresnel.
          float rhoS = dot(one_three_float3,  Ks) * fr;
          // If we don't have a specular reflection, choose either diffuse or
          // transmissive
          // Mix them based on the dissolve value of the material
          float rhoD = dot(one_three_float3, Kd) *
                       (1.0f - fr) * (1.0f - material->d);
          float rhoR = dot(one_three_float3, Kt) *
                       (1.0f - fr) * material->d;

          float rhoE = dot(one_three_float3, Ke);

          // Normalize probabilities so they sum to 1.0
          float totalrho = rhoS + rhoD + rhoR + rhoE;
          float totalrho1 = native_recip(totalrho);		  

          rhoS *= totalrho1;
          rhoD *= totalrho1;
          rhoR *= totalrho1;
          rhoE *= totalrho1;
		  
		  float rnd = rand(randSeed);
		  
          // REFLECT glossy
          if (rnd < rhoS) {
			  mtype = BXDF_IDEAL_REFLECTION;
		  }
          // REFLECT diffuse
          else if (rnd < rhoS + rhoD) {
			mtype = BXDF_DIFFUSE;
          }
          // REFRACT
          else if (rnd < rhoD + rhoS + rhoR) {
			  mtype = BXDF_IDEAL_DIELECTRIC;
			  }
          // EMIT
          else {
			  mtype = BXDF_EMISSIVE;
		  }
	}
	
	switch(mtype)
	{
		case BXDF_DIFFUSE:
			return evalDiffuse(hit, material, textures, texData, dirIn, dirOut);
		case BXDF_GLOSSY:
			return evalGlossy(hit, material, backface, textures, texData, dirIn, dirOut);
		case BXDF_GGX_ROUGH_REFLECTION:
			return evalGGXReflect(hit, material, textures, texData, dirIn, dirOut);
		case BXDF_IDEAL_REFLECTION:
			return evalIdealReflection();
		case BXDF_GGX_ROUGH_DIELECTRIC:
			return evalGGXRefract(hit, material, backface, textures, texData, dirIn, dirOut);
		case BXDF_IDEAL_DIELECTRIC:
			return evalIdealDielectric();
		case BXDF_EMISSIVE:
			return material->Ke;
			return (float3)(1.0f, 1.0f, 1.0f);
	}
	
	return (float3)(0.0f, 0.0f, 0.0f);
}

// Get pdf given incoming, outgoing directions (mainly for MIS)
float bxdfPdf(
	Hit *hit,
	Material *material,
	bool backface,
	global TexDescriptor *textures,
	global uchar *texData,
	float3 dirIn,
	float3 dirOut,
	uint *randSeed)
{	
	cl_int mtype = material->type;
	if(mtype == BXDF_MIXED) {
		mtype = BXDF_DIFFUSE;
	}
	
	if(mtype == BXDF_MIXED) {
		float3 one_three_float3 = ((float3) (1.0f, 1.0f, 1.0f)) / 3.0f;		

		float cosI = - dot(normalize(dirIn), hit->N);
		float n1 = 1.0f, n2 = material->Ni;
		if (backface) swap_m(n1, n2, float); // inside of material

		float fr = schlickDielectric(cosI, n1, n2);		
		
		float3 Kd = matGetAlbedo(material->Kd, hit->uvTex, material->map_Kd, textures, texData);
		float3 Ks = matGetFloat3(material->Ks, hit->uvTex, material->map_Ks, textures, texData);
		float3 Kt = material->Kt; 
		float3 Ke = material->Ke; 
		
          // Compute probabilities for each surface interaction.
          // Specular is just regular reflectiveness * fresnel.
          float rhoS = dot(one_three_float3,  Ks) * fr;
          // If we don't have a specular reflection, choose either diffuse or
          // transmissive
          // Mix them based on the dissolve value of the material
          float rhoD = dot(one_three_float3, Kd) *
                       (1.0f - fr) * (1.0f - material->d);
          float rhoR = dot(one_three_float3, Kt) *
                       (1.0f - fr) * material->d;

          float rhoE = dot(one_three_float3, Ke);

          // Normalize probabilities so they sum to 1.0
          float totalrho = rhoS + rhoD + rhoR + rhoE;
          float totalrho1 = native_recip(totalrho);		  

          rhoS *= totalrho1;
          rhoD *= totalrho1;
          rhoR *= totalrho1;
          rhoE *= totalrho1;
		  
		  float rnd = rand(randSeed);
		  
          // REFLECT glossy
          if (rnd < rhoS) {
			  mtype = BXDF_IDEAL_REFLECTION;
		  }
          // REFLECT diffuse
          else if (rnd < rhoS + rhoD) {
			mtype = BXDF_DIFFUSE;
          }
          // REFRACT
          else if (rnd < rhoD + rhoS + rhoR) {
			  mtype = BXDF_IDEAL_DIELECTRIC;
			  }
          // EMIT
          else {
			  mtype = BXDF_EMISSIVE;
		  }
	}
	
	switch(mtype)
	{
		case BXDF_DIFFUSE:
			return pdfDiffuse(hit, dirOut);
		case BXDF_GLOSSY:
			return pdfGlossy(hit, material, textures, texData, backface, dirIn, dirOut);
		case BXDF_GGX_ROUGH_REFLECTION:
			return pdfGGXReflect(hit, material, dirIn, dirOut);
		case BXDF_IDEAL_REFLECTION:
			return pdfIdealReflection();
		case BXDF_GGX_ROUGH_DIELECTRIC:
			return pdfGGXRefract(hit, material, backface, dirIn, dirOut);
		case BXDF_IDEAL_DIELECTRIC:
			return pdfIdealDielectric();
		case BXDF_EMISSIVE:
			return 0.0f;
	}

	return 0.0f;
}

#endif
