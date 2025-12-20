#pragma pack_matrix(row_major)

struct Instance {
    int      diffuse_tex;
    int      normal_tex;
    int      specular_tex;
    int      emissive_tex;
    float3   base_color;
    float    roughness;
    float3   emissive_color;
    float    metalness;
    int      vbuffer;
    int      ibuffer;
    int      bbuffer;
    int      padding;
    float4x4 transform;
    float4x4 inv_transform;
    float4x4 inv_transpose;
};

struct Vertex {
    float3 position;
    float3 tangeant;
    float3 bitangeant;
    float3 normal;
    float2 uv;
    float2 pad;
};

struct Hit {
    float u;
    float v;
    float w;
    uint  prim_idx;
    uint  instance;
};

struct ShadowHit {
    bool miss;
};

struct Attributes {
    float2 barycentrics;
};

struct UniformData {
    float4x4 inv_view_proj;
    uint2    resolution;
    uint     rng_seed;
    uint     sample_count;
    float3   sun_dir;
};

struct GeometryData {
	float3 position;
	float2 uv;
	float3 geometric_normal;
	float3 normal;
	float3 shading_normal;
	float3 tangeant;
	float3 bitangeant;
};

struct MaterialData {
    float4 base_color;
    float3 emissive_color;
    float  roughness;
    float  metalness;
};

struct BrdfData {
	float3 f0;
	float3 diffuse;
	float3 F;

	float3 v;
	float3 n;
	float3 h;
	float3 l;

	float n_dot_l;
	float n_dot_v;
	float h_dot_l;
	float h_dot_n;
	float h_dot_v;
	
	float roughness;
	float alpha;
	float alpha_squared;

    bool l_backfacing;
    bool v_backfacing;
};

#define INVALID_INSTANCE 0xFFFFFFFF
#define STANDARD_RAY_INDEX 0
#define SHADOW_RAY_INDEX 1

static const float pi32 = 3.1415926535897;
static const float pi32_over_2 = pi32 / 2.0;
static const float one_over_pi32 = 1.0 / pi32;

StructuredBuffer<Instance>      instances   : register(t0);
RaytracingAccelerationStructure BVH         : register(t1);
SamplerState                    tsampler    : register(s0);
RWTexture2D<float4>             out_texture : register(u0);
ConstantBuffer<UniformData>     uniforms    : register(b0);

float3 transform_point(float3 p, float4x4 m) {
    return mul(float4(p, 1), m).xyz;
}

float3 transform_vector(float3 v, float4x4 m) {
    return mul(float4(v, 0), m).xyz;
}

void orthonormal_basis(float3 v, out float3 b0, out float3 b1) {
	float3 u = float3(0, 0, 1);
	if (abs(dot(v,u)) > 0.999) u = float3(0, 1, 0);
	b0 = normalize(cross(u, v));
	b1 = normalize(cross(b0, v));
}

static uint rng_state;
uint rand_pcg() {
    uint state = rng_state;
    rng_state = rng_state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random_float() {
    return rand_pcg() * (1.0 / 4294967296.0);
}

float2 random_float2() {
    return float2(random_float(), random_float());
}

float2 sample_uniform_disk(float2 u) {
	u = 2 * u - float2(1, 1);
	if (u.x == 0.0 && u.y == 0.0) return float2(0, 0);

	float theta, r;
	if (abs(u.x) > abs(u.y)) {
		r = u.x;
		theta = pi32_over_2 * 0.5 * (u.y / u.x);
	} else {
		r = u.y;
		theta = pi32_over_2 - pi32_over_2 * 0.5 * (u.x / u.y);
	}
	return r * float2(cos(theta), sin(theta));
}

float3 sample_cosine_hemisphere(float2 u) {
	float2 d = sample_uniform_disk(u);
	float z = 1 - d.x * d.x - d.y * d.y;
	z = sqrt(max(0.0, z));
	return float3(d.x, d.y, z);
}

float3 sample_cone(float2 u, float3 direction, float theta) {
    float cos_theta = lerp(1.0, cos(theta), u.x);
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
    float phi = 2.0 * pi32 * u.y;

    float3 local = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

    float3 x, y, z;
    z = direction;
    orthonormal_basis(z, x, y);

    return normalize(local.x * x + local.y * y + local.z * z);
}

float luminance(float3 rgb) {
	return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float3 unpack_normal_map(float4 s) {
	float2 n = s.xy * 2.0 - 1.0;
	float  z = sqrt(saturate(1.0 - dot(n,n)));
	return float3(n.x, n.y, z);
}

GeometryData load_geometry_data(Hit hit, float3 rd) {
	GeometryData data;

    const Instance instance = instances[hit.instance];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vbuffer)];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.ibuffer)];
    const float4x4 transform = instance.transform;
    const float4x4 inv_transpose = instance.inv_transpose;
    const Vertex v0 = vertices[indices[hit.prim_idx * 3]];
    const Vertex v1 = vertices[indices[hit.prim_idx * 3 + 1]];
    const Vertex v2 = vertices[indices[hit.prim_idx * 3 + 2]];

    data.position = transform_point(v0.position * hit.w + v1.position * hit.u + v2.position * hit.v, transform);
    data.uv = v0.uv * hit.w + v1.uv * hit.u + v2.uv * hit.v;
	data.geometric_normal = normalize(transform_vector(normalize(cross(v1.position - v0.position, v2.position - v0.position)), inv_transpose));
    data.normal = normalize(transform_vector(normalize(v0.normal * hit.w + v1.normal * hit.u + v2.normal * hit.v), inv_transpose));
    data.tangeant = normalize(transform_vector(normalize(v0.tangeant * hit.w + v1.tangeant * hit.u + v2.tangeant * hit.v), inv_transpose));
    data.bitangeant = normalize(transform_vector(normalize(v0.bitangeant * hit.w + v1.bitangeant * hit.u + v2.bitangeant * hit.v), inv_transpose));
    if (dot(rd, data.geometric_normal) > 0.0f) {
        data.geometric_normal *= -1;
		data.normal *= -1;
    }

	data.shading_normal = data.normal;
	if (instance.normal_tex != -1) {
        Texture2D tex  = ResourceDescriptorHeap[NonUniformResourceIndex(instance.normal_tex)];
		float3    norm = unpack_normal_map(tex.SampleLevel(tsampler, data.uv, 0));
		data.shading_normal = normalize(data.tangeant * norm.x + data.bitangeant * norm.y + data.normal * norm.z);	
	}

	return data;
}

MaterialData load_material_data(Hit hit, GeometryData geo) {
	MaterialData data;

    const Instance instance = instances[hit.instance];
	
	data.base_color = float4(instance.base_color, 1.0f);
	if (instance.diffuse_tex != -1) {
        Texture2D tex   = ResourceDescriptorHeap[NonUniformResourceIndex(instance.diffuse_tex)];
        data.base_color = data.base_color * tex.SampleLevel(tsampler, geo.uv, 0);
	}

	data.emissive_color = float3(instance.emissive_color);
	if (instance.emissive_tex != -1) {
        Texture2D tex       = ResourceDescriptorHeap[NonUniformResourceIndex(instance.emissive_tex)];
        data.emissive_color = data.emissive_color * tex.SampleLevel(tsampler, geo.uv, 0).rgb;
	}

	data.roughness = instance.roughness;
	data.metalness = instance.metalness;
	if (instance.specular_tex != -1) {
        Texture2D tex   = ResourceDescriptorHeap[NonUniformResourceIndex(instance.specular_tex)];
        float4    orm   = tex.SampleLevel(tsampler, geo.uv, 0);
		data.roughness *= orm.y;
		data.metalness *= orm.z;
	}

	return data;
}

float3 fresnel_schlick(float3 f0, float n_dot_s) {
	const float3 f90 = (1.0).xxx;
	return f0 + (f90 - f0) * pow(saturate(1.0 - n_dot_s), 5.0);	
}

BrdfData load_brdf_data(MaterialData mat, float3 n, float3 l, float3 v) {
	BrdfData data;

	data.f0 = lerp((0.04).xxx, mat.base_color.rgb, mat.metalness);
	data.diffuse = mat.base_color.rgb * (1.0 - mat.metalness);
	data.v = v;
	data.n = n;
	data.h = normalize(l + v);
	data.l = l;

    float n_dot_l = dot(n, l);
    float n_dot_v = dot(n, v);
    data.l_backfacing = n_dot_l <= 0.0;
    data.v_backfacing = n_dot_v <= 0.0;

	data.n_dot_l = min(max(0.0001, n_dot_l), 1.0);
	data.n_dot_v = min(max(0.0001, n_dot_v), 1.0);
	data.h_dot_l = min(max(0.0001, dot(l, data.h)), 1.0);
	data.h_dot_n = min(max(0.0001, dot(n, data.h)), 1.0);
	data.h_dot_v = min(max(0.0001, dot(v, data.h)), 1.0);
	data.roughness = max(0.01, mat.roughness);
	data.alpha = data.roughness * data.roughness;
	data.alpha_squared = data.alpha * data.alpha;
	data.F = fresnel_schlick(data.f0, data.h_dot_v);

	return data;
}

float specular_probability(MaterialData mat, float3 n, float3 v) {
	float f0 = luminance(lerp((0.04).xxx, mat.base_color.rgb, mat.metalness));
	float diffuse = luminance(mat.base_color.rgb * (1.0 - mat.metalness));
	float f = saturate(luminance(fresnel_schlick(f0.xxx, max(0.0, dot(v, n)))));

	float s = f;
	float d = diffuse * (1.0 - f);
	return max(0.1, min(0.9, s / max(0.0001, s + d)));
}

// Samples a microfacet normal for the GGX distribution using VNDF method.
// Source: "Sampling the GGX Distribution of Visible Normals" by Heitz
// See also https://hal.inria.0fr/hal-00996995v1/document and http://jcgt.org/published/0007/04/01/
// Random variables 'u' must be in <0;1) interval
// PDF is 'G1(NdotV) * D'
float3 sample_ggx_vndf(float3 Ve, float2 alpha2, float2 rng) {
	// Section 3.2: transforming the view direction to the hemisphere configuration
	float3 Vh = normalize(float3(alpha2.x * Ve.x, alpha2.y * Ve.y, Ve.z));

	// Section 4.1: orthonormal basis (with special case if cross product is zero)
	float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * (1.0 / sqrt(lensq)) : float3(1.0, 0.0, 0.0);
	float3 T2 = cross(Vh, T1);

	// Section 4.2: parameterization of the projected area
	float r = sqrt(rng.x);
	float phi = 2 * pi32 * rng.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5 * (1.0 + Vh.z);
	t2 = lerp(sqrt(1.0 - t1 * t1), t2, s);

	// Section 4.3: reprojection onto hemisphere
	float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

	// Section 3.4: transforming the normal back to the ellipsoid configuration
	return normalize(float3(alpha2.x * Nh.x, alpha2.y * Nh.y, max(0.0, Nh.z)));
}

float smith_g2_over_g1_height_correlated(float alpha_squared, float n_dot_s_squared) {
	return 2.0 / (sqrt(((alpha_squared * (1.0 - n_dot_s_squared)) + n_dot_s_squared) / n_dot_s_squared) + 1.0);
}

float smith_g_a(float alpha, float n_dot_s) {
    return n_dot_s / (max(0.00001, alpha) * sqrt(1.0 - min(0.99999, n_dot_s * n_dot_s)));
}

float smith_g1_ggx(float a) {
    float a2 = a * a;
    return 2.0 / (sqrt(a2 + 1.0) / a2 + 1.0);
}

float smith_g2_separable(float alpha, float n_dot_l, float n_dot_v) {
    float al = smith_g_a(alpha, n_dot_l);
    float av= smith_g_a(alpha, n_dot_v);
    return smith_g1_ggx(al) * smith_g1_ggx(av);
}

float sample_weight_ggx_vndf(float alpha_squared, float n_dot_l, float n_dot_v){
	float g1v = smith_g2_over_g1_height_correlated(alpha_squared, n_dot_v * n_dot_v);
	float g1l = smith_g2_over_g1_height_correlated(alpha_squared, n_dot_l * n_dot_l);
	return g1l / (g1v + g1l - g1v * g1l);
}

float v_smith_ggx_correlated(float n_dot_l, float n_dot_v, float alpha_squared) {
    float a = n_dot_l * sqrt((-n_dot_v * alpha_squared + n_dot_v) * n_dot_v + alpha_squared);
    float b = n_dot_v * sqrt((-n_dot_l * alpha_squared + n_dot_l) * n_dot_l + alpha_squared);

    return 0.5 / (a + b);
}

float ggx_d(float n_dot_h, float alpha_squared) {
    float f = (n_dot_h * alpha_squared - n_dot_h) * n_dot_h + 1;
    return alpha_squared / (f * f);
}

float3 eval_specular(BrdfData data) {
    float D  = ggx_d(data.h_dot_n, max(0.00001, data.alpha_squared));
    //float G2 = v_smith_ggx_correlated(data.n_dot_l, data.n_dot_v, data.alpha_squared);
    float G2 = smith_g2_separable(data.alpha, data.n_dot_l, data.n_dot_v);

    //return min(10.0, data.F * G2 * D * data.n_dot_l);
    return min(10.0, ((data.F * G2 * D) / (4.0 * data.n_dot_l * data.n_dot_v)) * data.n_dot_l);
}

float diffuse_term(BrdfData data) {
    float energy_bias   = 0.5 * data.roughness;
    float energy_factor = lerp(1.0, 1.0 / 1.51, data.roughness);
    float fd90_minus_one = energy_bias + 2.0 * data.h_dot_l * data.h_dot_l * data.roughness - 1.0;
    float fdl = 1.0 + (fd90_minus_one * pow(1.0 - data.n_dot_l, 5.0));
    float fdv = 1.0 + (fd90_minus_one * pow(1.0 - data.n_dot_v, 5.0));
    return fdl * fdv * energy_factor;
}

float3 eval_diffuse(BrdfData data) {
    return data.diffuse * diffuse_term(data) * one_over_pi32 * data.n_dot_l;
}

float3 eval_brdf(GeometryData geo, MaterialData mat, float3 v, float3 l) {
    BrdfData data = load_brdf_data(mat, geo.shading_normal, l, v);
    if (data.l_backfacing || data.v_backfacing) {
        return (0.0).xxx;
    }

    float3 specular = eval_specular(data);
    float3 diffuse = eval_diffuse(data);

    float3 kd = (1.0).xxx - data.F;
    return kd * diffuse + specular;
}

bool eval_indirect_brdf(float2 rng, GeometryData geo, MaterialData mat, float3 v, bool specular, out float3 out_ray_direction, out float3 out_sample_weight) {
	float3x3 to_local, to_world;
	to_world[2] = geo.shading_normal;
	orthonormal_basis(to_world[2], to_world[0], to_world[1]);
	to_local = transpose(to_world);
	float3 v_local = normalize(mul(v, to_local));

	float3 n_local = float3(0, 0, 1);
	float3 ray_direction_local = 0.xxx;
	float3 sample_weight = 0.xxx;

	if (specular) {
		BrdfData data = load_brdf_data(mat, n_local, float3(0,0,1), v_local);
		float3   h_local = float3(0, 0, 1);
		
		if (data.alpha != 0.0) {
			h_local = sample_ggx_vndf(v_local, data.alpha.xx, rng);
		}
		ray_direction_local = normalize(reflect(-v_local, h_local));

		float  h_dot_v = max(0.00001, min(1.0, dot(h_local, v_local)));
		float  n_dot_l = max(0.00001, min(1.0, dot(n_local, ray_direction_local)));
		float  n_dot_v = max(0.00001, min(1.0, dot(n_local, v_local)));
		float3 f = fresnel_schlick(data.f0, h_dot_v);
		sample_weight = f * sample_weight_ggx_vndf(data.alpha_squared, n_dot_l, n_dot_v);
    } else {
        ray_direction_local = sample_cosine_hemisphere(rng);
        BrdfData data = load_brdf_data(mat, n_local, ray_direction_local, v_local);
        float3   h_specular = float3(0, 0, 1);

        if (data.alpha != 0.0f) {
            h_specular = sample_ggx_vndf(v_local, data.alpha.xx, rng);
        }

        float  h_dot_v = max(0.00001, min(1.0, dot(v_local, h_specular)));
        float3 kd = (1.0).xxx - fresnel_schlick(data.f0, h_dot_v);
        sample_weight = data.diffuse * diffuse_term(data) * kd;
    }

    if (luminance(sample_weight) == 0.0) {
		return false;
	}

    
    float3 wi = normalize(mul(ray_direction_local, to_world));
    //if (dot(geo.normal, wi) <= 0.0001) {
    //    to_world[2] = geo.normal;
    //    orthonormal_basis(to_world[2], to_world[0], to_world[1]);
    //    wi = normalize(mul(ray_direction_local, to_world));
    //    if (dot(geo.normal, wi) <= 0.0001) {
    //        to_world[2] = geo.geometric_normal;
    //        orthonormal_basis(to_world[2], to_world[0], to_world[1]);
    //        wi = normalize(mul(ray_direction_local, to_world));
    //    }
    //}

    out_ray_direction = wi;
    out_sample_weight = sample_weight;
    return true;
}

bool hit_is_transparent(uint instance_idx, uint prim_idx, float2 barycentrics) {
    const Instance instance = instances[instance_idx];
    if (instance.diffuse_tex == -1) {
        return true;
    }

    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vbuffer)];
    StructuredBuffer<uint>   indices  = ResourceDescriptorHeap[NonUniformResourceIndex(instance.ibuffer)];
    const Vertex             v0       = vertices[indices[prim_idx * 3]];
    const Vertex             v1       = vertices[indices[prim_idx * 3 + 1]];
    const Vertex             v2       = vertices[indices[prim_idx * 3 + 2]];
    
    float  w  = 1.0 - (barycentrics.x + barycentrics.y);
    float2 uv = v0.uv * w + v1.uv * barycentrics.x + v2.uv * barycentrics.y;

    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(instance.diffuse_tex)];
    return tex.SampleLevel(tsampler, uv, 0).a < 0.5;
}

[shader("closesthit")]
void closesthit(inout Hit hit, Attributes attributes) {
    hit.u = attributes.barycentrics.x;
    hit.v = attributes.barycentrics.y;
    hit.w = 1.0 - (hit.u + hit.v);
    hit.prim_idx = PrimitiveIndex();
    hit.instance = InstanceID();
}

[shader("anyhit")]
void anyhit(inout Hit hit, Attributes attributes) {
    if (hit_is_transparent(InstanceID(), PrimitiveIndex(), attributes.barycentrics)) {
        IgnoreHit();
    }
}

[shader("anyhit")]
void shadow_anyhit(inout ShadowHit hit, Attributes attributes) {
    if (hit_is_transparent(InstanceID(), PrimitiveIndex(), attributes.barycentrics)) {
        IgnoreHit();
    }
}

[shader("miss")]
void miss(inout Hit hit) {
    hit.instance = INVALID_INSTANCE;
}

[shader("miss")]
void shadow_miss(inout ShadowHit hit) {
    hit.miss = true;
}

[shader("raygeneration")]
void ray_gen() {
    uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= uniforms.resolution.x || pixel.y >= uniforms.resolution.y) {
        return;
    }
    
    rng_state = uniforms.rng_seed + pixel.y * uniforms.resolution.x + pixel.x;

    float2 fpixel = pixel + 0.5 + 0.75 * sample_uniform_disk(float2(random_float(), random_float()));
    float  ndc_x = (2.0f * fpixel.x / float(uniforms.resolution.x)) - 1.0f;
    float  ndc_y = 1.0f - (2.0f * fpixel.y / float(uniforms.resolution.y));
    float4 near_clip = float4(ndc_x, ndc_y, 0.0f, 1.0f);
    float4 far_clip = float4(ndc_x, ndc_y, 1.0f, 1.0f);
    float4 near_world = mul(near_clip, uniforms.inv_view_proj);
    float4 far_world = mul(far_clip, uniforms.inv_view_proj);
    near_world /= near_world.w;
    far_world /= far_world.w;

    RayDesc ray;
    ray.Origin = near_world.xyz;
    ray.TMin = 0.0;
    ray.Direction = normalize(far_world.xyz - ray.Origin);
    ray.TMax = 1000.0;
     
    //float3    sky_color = float3(0.729, 0.807, 0.921) * 2.0f;
    float3    sky_color = float3(0.729, 0.807, 0.921) * 0.01f;
    float3    throughput = float3(1.0, 1.0, 1.0);
    float3    radiance = float3(0.0, 0.0, 0.0);
    float     maxd = 1000.0;
    const int max_bounce = 2;
    for (int bounce = 0; bounce <= max_bounce; bounce += 1) {
        Hit hit;
        TraceRay(BVH, RAY_FLAG_NONE, 0xFF, STANDARD_RAY_INDEX, 0, STANDARD_RAY_INDEX, ray, hit);

        if (hit.instance == INVALID_INSTANCE) {
            radiance += sky_color * throughput;
            break;
        }
        
		float2 rng = float2(random_float(), random_float());
		float3 v = -ray.Direction;
		GeometryData geo = load_geometry_data(hit, ray.Direction);
		MaterialData mat = load_material_data(hit, geo);
		radiance += mat.emissive_color * throughput;

		ray.Origin = geo.position + geo.geometric_normal * 0.001f;
        ray.Direction = sample_cone(rng, uniforms.sun_dir, pi32 * 0.005);
        ShadowHit shadow_hit;
        shadow_hit.miss = false;
        TraceRay(BVH, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, SHADOW_RAY_INDEX, 0, SHADOW_RAY_INDEX, ray, shadow_hit);
        if (shadow_hit.miss) {
            //radiance += 1.0 * float3(1, 0.8, 0.5) * max(0, dot(sun, geo.shading_normal)) * throughput;
            //float3 light_intensity = 15.0 * float3(1.0, 0.8, 0.5);
            float3 light_intensity = 1.0 * float3(0.67, 0.67, 0.75);
            radiance += eval_brdf(geo, mat, v, uniforms.sun_dir) * light_intensity * throughput;
        }

		bool   eval_specular;
		float  spec_p = specular_probability(mat, geo.shading_normal, v);
		if (random_float() < spec_p) {
			eval_specular = true;
			throughput = throughput / spec_p;
		} else {
			eval_specular = false;
			throughput = throughput / (1.f - spec_p);
		}

        float3 brdf_weight;
		if (eval_indirect_brdf(rng, geo, mat, v, eval_specular, ray.Direction, brdf_weight) == false) {
			break;
		}

		throughput = brdf_weight * throughput;
    }

    out_texture[pixel] = lerp(out_texture[pixel], float4(radiance, 1.0), 1.0 / uniforms.sample_count);
}