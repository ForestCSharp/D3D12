
float remap(float value, float input_min, float input_max, float output_min, float output_max)
{
    return output_min + (value - input_min) * (output_max - output_min) / (input_max - input_min);
}

float remap_clamped(float value, float input_min, float input_max, float output_min, float output_max)
{
    float clamped = clamp(value, input_min, input_max);
    return remap(clamped, input_min, input_max, output_min, output_max);
}

//TODO: Testing new noise
// Jenkins hash function.
uint hash1(uint x) {
    x += (x << 10u);
    x ^= (x >> 6u);
    x += (x << 3u);
    x ^= (x >> 11u);
    x += (x << 15u);
    return x;
}

uint hash1_mut(inout uint h) {
    uint res = h;
    h = hash1(h);
    return res;
}

uint hash_combine2(uint x, uint y) {
    static const uint M = 1664525u, C = 1013904223u;
    uint seed = (x * M + y + C) * M;

    // Tempering (from Matsumoto)
    seed ^= (seed >> 11u);
    seed ^= (seed << 7u) & 0x9d2c5680u;
    seed ^= (seed << 15u) & 0xefc60000u;
    seed ^= (seed >> 18u);
    return seed;
}

uint hash2(uint2 v) {
    return hash_combine2(v.x, hash1(v.y));
}

uint hash3(uint3 v) {
    return hash_combine2(v.x, hash2(v.yz));
}

uint hash4(uint4 v) {
    return hash_combine2(v.x, hash3(v.yzw));
}

float randf(uint seed)
{
    return float(hash1(seed) & uint(0x7fffffffU)) / float(0x7fffffff);
}

float randf_range(float min, float max, uint seed)
{
    return remap_clamped(randf(seed), 0.0f, 1.0f, min, max);
}

float3 randf_range_3d(float min, float max, uint seed)
{
    return float3(
        randf_range(min, max, seed),
        randf_range(min, max, seed + 1),
        randf_range(min, max, seed + 2)
    );
}

float3 randf_in_unit_sphere(uint seed) {
    while (true)
    {
        float3 p = randf_range_3d(-1.0, 1.0, seed);
        if (length(p) >= 1) continue;
        return p;
    }
}

float3 randf_in_hemisphere(const float3 normal, const uint seed) {
    float3 in_unit_sphere = randf_in_unit_sphere(seed);
    return dot(in_unit_sphere, normal) > 0.0 ? in_unit_sphere : -in_unit_sphere;
}