#ifndef SG_H
#define SG_H

//Ref: https://mynameismjp.wordpress.com/2016/10/09/sg-series-part-2-spherical-gaussians-101/
//Ref: https://github.com/kayru/Probulator

//#include "HLSL_Types.h"

struct SG
{
	float3 Amplitude;
	float3 Axis;
	float Sharpness;
};

#define SG_BASIS_LOBE_COUNT 12

struct SGBasis
{
	SG lobes[SG_BASIS_LOBE_COUNT];
};

struct SGProbe
{
	SGBasis basis;
	float3 position;
};

struct OctreeNode
{
	float3 min, max;
	bool is_leaf;
	int children[2][2][2]; //x,y,z 0 idx is less than 1 idx for given component

	//FCS TODO: Temp payload, replace with SGBasis
	float3 color;
};

#ifndef __cplusplus

inline bool Octree_IsValidPosition(OctreeNode node, float3 position)
{
	return all(position > node.min) && all(position < node.max);
}

inline int Octree_FindRelevantChild(OctreeNode node, float3 position)
{	
	float3 center = (node.min + node.max) / 2.0;
	uint x = position.x < center.x ? 0 : 1;
	uint y = position.y < center.y ? 0 : 1;
	uint z = position.z < center.z ? 0 : 1;
	return node.children[x][y][z];
}

inline OctreeNode Octree_Search(uint octree_bindless_idx, float3 position)
{
	StructuredBuffer<OctreeNode> octree = ResourceDescriptorHeap[octree_bindless_idx];
	OctreeNode current_node = octree[0];

	if (!Octree_IsValidPosition(current_node, position))
	{
		//FCS TODO: better return for outside octree extents...
		OctreeNode dummy_node;
		dummy_node.color = float3(0, 0, 0);
		return dummy_node;
	}

	while (!current_node.is_leaf)
	{
		int child_idx = Octree_FindRelevantChild(current_node, position);
		if (child_idx < 0)
		{
			break; // No child at this position, so terminate at non-leaf parent
		}
		current_node = octree[child_idx];
	}

	return current_node;
}

#define PI 3.141592653589793

float3 SG_Evaluate(SG sg, float3 dir)
{
	float cosAngle = dot(dir, sg.Axis);
	return sg.Amplitude * exp(sg.Sharpness * (cosAngle - 1.0f));
}

//-------------------------------------------------------------------------------------------------
// Computes the vector product of two SG's, which produces a new SG. If the new SG is evaluated,
// with a direction 'v' the result is equal to SGx(v) * SGy(v).
//-------------------------------------------------------------------------------------------------
SG SG_Product(in SG x, in SG y)
{
	float3 um = (x.Sharpness * x.Axis + y.Sharpness * y.Axis) / (x.Sharpness + y.Sharpness);
	float umLength = length(um);
	float lm = x.Sharpness + y.Sharpness;

	SG res;
	res.Axis = um * (1.0f / umLength);
	res.Sharpness = lm * umLength;
	res.Amplitude = x.Amplitude * y.Amplitude * exp(lm * (umLength - 1.0f));

	return res;
}

//-------------------------------------------------------------------------------------------------
// Computes the integral of an SG over the entire sphere
//-------------------------------------------------------------------------------------------------
float3 SG_Integral(in SG sg)
{
	float expTerm = 1.0f - exp(-2.0f * sg.Sharpness);
	return 2 * PI * (sg.Amplitude / sg.Sharpness) * expTerm;
}

//-------------------------------------------------------------------------------------------------
// Computes the approximate integral of an SG over the entire sphere. The error vs. the
// non-approximate version decreases as sharpeness increases.
//-------------------------------------------------------------------------------------------------
float3 SG_Integral_Approximate(in SG sg)
{
	return 2 * PI * (sg.Amplitude / sg.Sharpness);
}

//-------------------------------------------------------------------------------------------------
// Computes the inner product of two SG's, which is equal to Integrate(SGx(v) * SGy(v) * dv).
//-------------------------------------------------------------------------------------------------
float3 SG_InnerProduct(in SG x, in SG y)
{
	float umLength = length(x.Sharpness * x.Axis + y.Sharpness * y.Axis);
	float3 expo = exp(umLength - x.Sharpness - y.Sharpness) * x.Amplitude * y.Amplitude;
	float other = 1.0f - exp(-2.0f * umLength);
	return (2.0f * PI * expo * other) / umLength;
}

//-------------------------------------------------------------------------------------------------
// Returns an approximation of the clamped cosine lobe represented as an SG
//-------------------------------------------------------------------------------------------------
SG CosineLobeSG(in float3 direction)
{
	SG cosineLobe;
	cosineLobe.Axis = direction;
	cosineLobe.Sharpness = 2.133f;
	cosineLobe.Amplitude = 1.17f;

	return cosineLobe;
}

//-------------------------------------------------------------------------------------------------
// Returns an SG approximation of the GGX NDF used in the specular BRDF. For a single-lobe
// approximation, the resulting NDF actually more closely resembles a Beckmann NDF.
//-------------------------------------------------------------------------------------------------
SG DistributionTermSG(in float3 direction, in float roughness)
{
	SG distribution;
	distribution.Axis = direction;
	float m2 = roughness * roughness;
	distribution.Sharpness = 2 / m2;
	distribution.Amplitude = 1.0f / (PI * m2);

	return distribution;
}

//-------------------------------------------------------------------------------------------------
// Computes the approximate incident irradiance from a single SG lobe containing incoming radiance.
// The clamped cosine lobe is approximated as an SG, and convolved with the incoming radiance
// lobe using an SG inner product
//-------------------------------------------------------------------------------------------------
float3 SG_IrradianceInnerProduct(in SG lightingLobe, in float3 normal)
{
	SG cosineLobe = CosineLobeSG(normal);
	return max(SG_InnerProduct(lightingLobe, cosineLobe), 0.0f);
}

//-------------------------------------------------------------------------------------------------
// Computes the approximate incident irradiance from a single SG lobe containing incoming radiance.
// The SG is treated as a punctual light, with intensity equal to the integral of the SG.
//-------------------------------------------------------------------------------------------------
float3 SG_IrradiancePunctual(in SG lightingLobe, in float3 normal)
{
	float cosineTerm = saturate(dot(lightingLobe.Axis, normal));
	return cosineTerm * 2.0f * PI * (lightingLobe.Amplitude) / lightingLobe.Sharpness;
}

//-------------------------------------------------------------------------------------------------
// Computes the approximate incident irradiance from a single SG lobe containing incoming radiance.
// The irradiance is computed using a fitted approximation polynomial. This approximation
// and its implementation were provided by Stephen Hill.
//-------------------------------------------------------------------------------------------------
float3 SG_IrradianceFitted(in SG lightingLobe, in float3 normal)
{
	const float muDotN = dot(lightingLobe.Axis, normal);
	const float lambda = lightingLobe.Sharpness;

	const float c0 = 0.36f;
	const float c1 = 1.0f / (4.0f * c0);

	float eml  = exp(-lambda);
	float em2l = eml * eml;
	float rl   = rcp(lambda);

	float scale = 1.0f + 2.0f * em2l - rl;
	float bias  = (eml - em2l) * rl - em2l;

	float x  = sqrt(1.0f - scale);
	float x0 = c0 * muDotN;
	float x1 = c1 * x;

	float n = x0 + x1;

	float y = (abs(x0) <= x1) ? n * n / x : saturate(muDotN);

	float normalizedIrradiance = scale * y + bias;

	return normalizedIrradiance * SG_Integral_Approximate(lightingLobe);
}

float3 SGBasis_Evaluate(SGBasis basis, float3 dir)
{
	float3 result = float3(0,0,0);
	for (int i=0; i < SG_BASIS_LOBE_COUNT; ++i)
	{
		result += SG_Evaluate(basis.lobes[i], dir);	
	}
	return result;
}

//FCS TODO: Look at probulator again

// Naive V1: (this should be set up in C++ and passed as uniforms...)
// 100 x 100 x 100 grid with cell size of 1000 unit
// assume probe is in grid cell center 

#endif //#ifndef __cplusplus

#endif