#pragma pack_matrix(row_major)

struct TlasNode {
    float3 lmin;
    uint   left;
    float3 lmax;
    uint   right;
    float3 rmin;
    uint   tri_count;
    float3 rmax;
    uint   first_tri;
};

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
    float distance;
    float u;
    float v;
    uint  prim_idx;
    uint  instance;
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

static const float pi32 = 3.1415926535897;
static const float pi32_over_2 = pi32 / 2.0;
static const float one_over_pi32 = 1.0 / pi32;

#define STACK_SIZE 64
#define STRIDE     3

StructuredBuffer<TlasNode>  tlas_nodes   : register(t0);
StructuredBuffer<uint>      tlas_indices : register(t1);
StructuredBuffer<Instance>  instances    : register(t2);
SamplerState                tsampler     : register(s0);
RWTexture2D<float4>         out_texture  : register(u0);
ConstantBuffer<UniformData> uniforms     : register(b0);

float4 intersect_triangle(uint vertex_idx, float3 ro, float3 rd, uint blas_idx) {
    StructuredBuffer<float4> blas = ResourceDescriptorHeap[NonUniformResourceIndex(blas_idx)];
    float4 hit = 0.xxxx;

    float4 v0 = blas[vertex_idx];
    float4 edge1 = blas[vertex_idx + 1];
    float4 edge2 = blas[vertex_idx + 2];
    float3 h = cross(rd, edge2.xyz);
    float  a = dot(edge1.xyz, h);
    if (abs(a) < 0.000001f) {
        return hit;
    }

    float  f = 1.0 / a;
    float3 s = ro - v0.xyz;
    float  u = f * dot(s, h);
    float3 q = cross(s, edge1.xyz);
    float  v = f * dot(rd, q);
    if (u < 0 || v < 0 || u + v > 1) {
        return hit;
    }

    float d = f * dot(edge2.xyz, q);
    if (d > 0) {
        hit = float4(d, u, v, v0.w);
    }

    return hit;
}

float4 decode_char4(uint v) {
    return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
}

bool any_hit(uint instance_idx, float4 hit) {
    const Instance instance = instances[instance_idx];
    uint           prim_idx = asuint(hit.w);
    if (instance.diffuse_tex == -1) {
        return true;
    }

    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vbuffer)];
    StructuredBuffer<uint>   indices  = ResourceDescriptorHeap[NonUniformResourceIndex(instance.ibuffer)];
    const Vertex             v0       = vertices[indices[prim_idx * 3]];
    const Vertex             v1       = vertices[indices[prim_idx * 3 + 1]];
    const Vertex             v2       = vertices[indices[prim_idx * 3 + 2]];
    
    float  w  = 1.0 - (hit.y + hit.z);
    float2 uv = v0.uv * w + v1.uv * hit.y + v2.uv * hit.z;

    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(instance.diffuse_tex)];
    return tex.SampleLevel(tsampler, uv, 0).a > 0.5;
}

#define SWAP(A,B,C,D) tmp=A,A=B,B=tmp,tmp2=C,C=D,D=tmp2;
float4 traverse_blas(uint instance_idx, uint blas_idx, float3 ro, float3 rd, float maxd) {
    float4 hit;
    hit.x = maxd;

    float3 one_over_rd = 1.0 / rd;
    float4 zero4 = 0.xxxx;
    uint   stack[STACK_SIZE];
    uint   stack_ptr = 0;
    uint   offset = 0;
    uint   tmp2;

    StructuredBuffer<float4> blas = ResourceDescriptorHeap[NonUniformResourceIndex(blas_idx)];
    for (;;) {
		float4 data0 = blas[offset + 0], data1 = blas[offset + 1];
		float4 data2 = blas[offset + 2], data3 = blas[offset + 3];
		float3 bmin = data0.xyz, extent = data1.xyz; 
		float4 d0 = decode_char4( asuint(data0.w) ), d1 = decode_char4( asuint(data1.w) ), d2 = decode_char4( asuint(data2.x) );
		float4 d3 = decode_char4( asuint(data2.y) ), d4 = decode_char4( asuint(data2.z) ), d5 = decode_char4( asuint(data2.w) );
		float3 c0min = bmin + extent * float3( d0.x, d2.x, d4.x ), c0max = bmin + extent * float3( d1.x, d3.x, d5.x );
		float3 c1min = bmin + extent * float3( d0.y, d2.y, d4.y ), c1max = bmin + extent * float3( d1.y, d3.y, d5.y );
		float3 c2min = bmin + extent * float3( d0.z, d2.z, d4.z ), c2max = bmin + extent * float3( d1.z, d3.z, d5.z );
		float3 c3min = bmin + extent * float3( d0.w, d2.w, d4.w ), c3max = bmin + extent * float3( d1.w, d3.w, d5.w );
		float3 t1a = (c0min - ro) * one_over_rd, t2a = (c0max - ro) * one_over_rd;
		float3 t1b = (c1min - ro) * one_over_rd, t2b = (c1max - ro) * one_over_rd;
		float3 t1c = (c2min - ro) * one_over_rd, t2c = (c2max - ro) * one_over_rd;
		float3 t1d = (c3min - ro) * one_over_rd, t2d = (c3max - ro) * one_over_rd;
		float3 minta = min( t1a, t2a ), maxta = max( t1a, t2a );
		float3 mintb = min( t1b, t2b ), maxtb = max( t1b, t2b );
		float3 mintc = min( t1c, t2c ), maxtc = max( t1c, t2c );
		float3 mintd = min( t1d, t2d ), maxtd = max( t1d, t2d );
		float tmina = max( max( max( minta.x, minta.y ), minta.z ), 0.0f );
		float tminb = max( max( max( mintb.x, mintb.y ), mintb.z ), 0.0f );
		float tminc = max( max( max( mintc.x, mintc.y ), mintc.z ), 0.0f );
		float tmind = max( max( max( mintd.x, mintd.y ), mintd.z ), 0.0f );
		float tmaxa = min( min( min( maxta.x, maxta.y ), maxta.z ), hit.x );
		float tmaxb = min( min( min( maxtb.x, maxtb.y ), maxtb.z ), hit.x );
		float tmaxc = min( min( min( maxtc.x, maxtc.y ), maxtc.z ), hit.x );
		float tmaxd = min( min( min( maxtd.x, maxtd.y ), maxtd.z ), hit.x );
		float dist0 = tmina > tmaxa ? 1e30f : tmina, dist1 = tminb > tmaxb ? 1e30f : tminb;
		float dist2 = tminc > tmaxc ? 1e30f : tminc, dist3 = tmind > tmaxd ? 1e30f : tmind, tmp;
		uint32_t c0info = asuint( data3.x ), c1info = asuint( data3.y );
		uint32_t c2info = asuint( data3.z ), c3info = asuint( data3.w );
		if (dist0 < dist2) SWAP( dist0, dist2, c0info, c2info );
		if (dist1 < dist3) SWAP( dist1, dist3, c1info, c3info );
		if (dist0 < dist1) SWAP( dist0, dist1, c0info, c1info );
		if (dist2 < dist3) SWAP( dist2, dist3, c2info, c3info );
		if (dist1 < dist2) SWAP( dist1, dist2, c1info, c2info );
		uint32_t leaf[4] = { 0, 0, 0, 0 }, leafs = 0;
		if (dist0 < 1e30f) {
			if (c0info & 0x80000000) leaf[leafs++] = c0info; else if (c0info) stack[stack_ptr++] = c0info;
		}
		if (dist1 < 1e30f) {
			if (c1info & 0x80000000) leaf[leafs++] = c1info; else if (c1info) stack[stack_ptr++] = c1info;
		}
		if (dist2 < 1e30f) {
			if (c2info & 0x80000000) leaf[leafs++] = c2info; else if (c2info) stack[stack_ptr++] = c2info;
		}
		if (dist3 < 1e30f) {
			if (c3info & 0x80000000) leaf[leafs++] = c3info; else if (c3info) stack[stack_ptr++] = c3info;
		}
		for (uint i = 0; i < leafs; i++) {
			const uint N = (leaf[i] >> 16) & 0x7fff;
			uint tri_start = offset + (leaf[i] & 0xffff);
			for (uint j = 0; j < N; j++, tri_start += 3) {
                float4 triangle_hit = intersect_triangle(tri_start, ro, rd, blas_idx);
                if (triangle_hit.x > 0 && triangle_hit.x < hit.x) {
                    if (any_hit(instance_idx, triangle_hit)) {
                        hit = triangle_hit;
                    }
                }
			}
		}
	
    	if (!stack_ptr) break;
		offset = stack[--stack_ptr];
    }

    return hit;
}

bool is_occluded_blas(uint instance_idx, uint blas_idx, float3 ro, float3 rd, float dist) {
    float3 one_over_rd = 1.0 / rd;
    float4 zero4 = 0.xxxx;
    uint   stack[STACK_SIZE];
    uint   stack_ptr = 0;
    uint   offset = 0;
    uint   tmp2;

    StructuredBuffer<float4> blas = ResourceDescriptorHeap[NonUniformResourceIndex(blas_idx)];
    for (;;) {
		float4 data0 = blas[offset + 0], data1 = blas[offset + 1];
		float4 data2 = blas[offset + 2], data3 = blas[offset + 3];
		float3 bmin = data0.xyz, extent = data1.xyz; 
		float4 d0 = decode_char4( asuint(data0.w) ), d1 = decode_char4( asuint(data1.w) ), d2 = decode_char4( asuint(data2.x) );
		float4 d3 = decode_char4( asuint(data2.y) ), d4 = decode_char4( asuint(data2.z) ), d5 = decode_char4( asuint(data2.w) );
		float3 c0min = bmin + extent * float3( d0.x, d2.x, d4.x ), c0max = bmin + extent * float3( d1.x, d3.x, d5.x );
		float3 c1min = bmin + extent * float3( d0.y, d2.y, d4.y ), c1max = bmin + extent * float3( d1.y, d3.y, d5.y );
		float3 c2min = bmin + extent * float3( d0.z, d2.z, d4.z ), c2max = bmin + extent * float3( d1.z, d3.z, d5.z );
		float3 c3min = bmin + extent * float3( d0.w, d2.w, d4.w ), c3max = bmin + extent * float3( d1.w, d3.w, d5.w );
		float3 t1a = (c0min - ro) * one_over_rd, t2a = (c0max - ro) * one_over_rd;
		float3 t1b = (c1min - ro) * one_over_rd, t2b = (c1max - ro) * one_over_rd;
		float3 t1c = (c2min - ro) * one_over_rd, t2c = (c2max - ro) * one_over_rd;
		float3 t1d = (c3min - ro) * one_over_rd, t2d = (c3max - ro) * one_over_rd;
		float3 minta = min( t1a, t2a ), maxta = max( t1a, t2a );
		float3 mintb = min( t1b, t2b ), maxtb = max( t1b, t2b );
		float3 mintc = min( t1c, t2c ), maxtc = max( t1c, t2c );
		float3 mintd = min( t1d, t2d ), maxtd = max( t1d, t2d );
		float tmina = max( max( max( minta.x, minta.y ), minta.z ), 0.0f );
		float tminb = max( max( max( mintb.x, mintb.y ), mintb.z ), 0.0f );
		float tminc = max( max( max( mintc.x, mintc.y ), mintc.z ), 0.0f );
		float tmind = max( max( max( mintd.x, mintd.y ), mintd.z ), 0.0f );
		float tmaxa = min( min( min( maxta.x, maxta.y ), maxta.z ), dist );
		float tmaxb = min( min( min( maxtb.x, maxtb.y ), maxtb.z ), dist );
		float tmaxc = min( min( min( maxtc.x, maxtc.y ), maxtc.z ), dist );
		float tmaxd = min( min( min( maxtd.x, maxtd.y ), maxtd.z ), dist );
		float dist0 = tmina > tmaxa ? 1e30f : tmina, dist1 = tminb > tmaxb ? 1e30f : tminb;
		float dist2 = tminc > tmaxc ? 1e30f : tminc, dist3 = tmind > tmaxd ? 1e30f : tmind, tmp;
		uint32_t c0info = asuint( data3.x ), c1info = asuint( data3.y );
		uint32_t c2info = asuint( data3.z ), c3info = asuint( data3.w );
	    if (dist0 < dist2) SWAP( dist0, dist2, c0info, c2info );
		if (dist1 < dist3) SWAP( dist1, dist3, c1info, c3info );
		if (dist0 < dist1) SWAP( dist0, dist1, c0info, c1info );
		if (dist2 < dist3) SWAP( dist2, dist3, c2info, c3info );
		if (dist1 < dist2) SWAP( dist1, dist2, c1info, c2info );
		uint32_t leaf[4] = { 0, 0, 0, 0 }, leafs = 0;
		if (dist0 < 1e30f) {
			if (c0info & 0x80000000) leaf[leafs++] = c0info; else if (c0info) stack[stack_ptr++] = c0info;
		}
		if (dist1 < 1e30f) {
			if (c1info & 0x80000000) leaf[leafs++] = c1info; else if (c1info) stack[stack_ptr++] = c1info;
		}
		if (dist2 < 1e30f) {
			if (c2info & 0x80000000) leaf[leafs++] = c2info; else if (c2info) stack[stack_ptr++] = c2info;
		}
		if (dist3 < 1e30f) {
			if (c3info & 0x80000000) leaf[leafs++] = c3info; else if (c3info) stack[stack_ptr++] = c3info;
		}
		for (uint i = 0; i < leafs; i++) {
			const uint N = (leaf[i] >> 16) & 0x7fff;
			uint tri_start = offset + (leaf[i] & 0xffff);
			for (uint j = 0; j < N; j++, tri_start += 3) {
                float4 triangle_hit = intersect_triangle(tri_start, ro, rd, blas_idx);
                if (triangle_hit.x > 0 && triangle_hit.x < dist) {
                    if (any_hit(instance_idx, triangle_hit)) {
                        return true;
                    }
                }
			}
		}

    	if (!stack_ptr) break;
		offset = stack[--stack_ptr];
    }

    return false;
}

Hit traverse_tlas(float3 ro, float3 rd, float maxd) {
    Hit hit;
    hit.distance = maxd;

    if (isnan(ro.x + ro.y + ro.z + rd.x + rd.y + rd.z)) {
        return hit;
    }

    float3 one_over_rd = 1.0 / rd;    

    uint stack[STACK_SIZE];
    uint stack_ptr = 0;
    uint node_idx = 0;

    for (;;) {
        TlasNode node = tlas_nodes[node_idx];
        if (node.tri_count != 0) { // leaf node
            for (uint i = 0; i < node.tri_count; i += 1) {
                uint     instance_idx = tlas_indices[node.first_tri + i];
                uint     blas_idx = instances[instance_idx].bbuffer;
                float4x4 to_local = instances[instance_idx].inv_transform;
                float3   ro_blas = mul(float4(ro, 1.0), to_local).xyz;
                float3   rd_blas = mul(float4(rd, 0.0), to_local).xyz;

                float4 blas_hit = traverse_blas(instance_idx, blas_idx, ro_blas, rd_blas, hit.distance);
                if (blas_hit.x < hit.distance) {
                    hit.distance = blas_hit.x;
                    hit.u = blas_hit.y;
                    hit.v = blas_hit.z;
                    hit.prim_idx = asuint(blas_hit.w);
                    hit.instance = instance_idx;
                }
            }

            if (stack_ptr == 0) {
                break;
            }

            node_idx = stack[--stack_ptr];
        } else { // internal node
            float3 t1a = (node.lmin - ro) * one_over_rd;
            float3 t2a = (node.lmax - ro) * one_over_rd;
            float3 t1b = (node.rmin - ro) * one_over_rd;
            float3 t2b = (node.rmax - ro) * one_over_rd;
            float3 minta = min(t1a, t2a);
            float3 maxta = max(t1a, t2a);
            float3 mintb = min(t1b, t2b);
            float3 maxtb = max(t1b, t2b);
            float  tmina = max(max(max(minta.x, minta.y), minta.z), 0);
            float  tminb = max(max(max(mintb.x, mintb.y), mintb.z), 0);
            float  tmaxa = min(min(min(maxta.x, maxta.y), maxta.z), hit.distance);
            float  tmaxb = min(min(min(maxtb.x, maxtb.y), maxtb.z), hit.distance);
            float  dist1 = tmina > tmaxa ? 1e30f : tmina;
            float  dist2 = tminb > tmaxb ? 1e30f : tminb;
            if (dist1 > dist2) {
                float dd = dist1;
                dist1 = dist2;
                dist2 = dd;
                uint t = node.left;
                node.left = node.right;
                node.right = t;
            }
            if (dist1 == 1e30f) {
                if (stack_ptr == 0) {
                    break;
                }

                node_idx = stack[--stack_ptr];
            } else {
                node_idx = node.left;
                if (dist2 != 1e30f) {
                    stack[stack_ptr++] = node.right;
                }
            }
        }
    }

    return hit;
}

bool is_occluded_tlas(float3 ro, float3 rd, float dist) {
    if (isnan(ro.x + ro.y + ro.z + rd.x + rd.y + rd.z)) {
        return false;
    }

    float3 one_over_rd = 1.0 / rd;    

    uint stack[STACK_SIZE];
    uint stack_ptr = 0;
    uint node_idx = 0;

    for (;;) {
        TlasNode node = tlas_nodes[node_idx];
        if (node.tri_count != 0) { // leaf node
            for (uint i = 0; i < node.tri_count; i += 1) {
                uint     instance_idx = tlas_indices[node.first_tri + i];
                uint     blas_idx = instances[instance_idx].bbuffer;
                float4x4 to_local = instances[instance_idx].inv_transform;
                float3   ro_blas = mul(float4(ro, 1.0), to_local).xyz;
                float3   rd_blas = mul(float4(rd, 0.0), to_local).xyz;

                if (is_occluded_blas(instance_idx, blas_idx, ro_blas, rd_blas, dist)) {
                    return true;
                }
            }

            if (stack_ptr == 0) {
                break;
            }

            node_idx = stack[--stack_ptr];
        } else { // internal node
            float3 t1a = (node.lmin - ro) * one_over_rd;
            float3 t2a = (node.lmax - ro) * one_over_rd;
            float3 t1b = (node.rmin - ro) * one_over_rd;
            float3 t2b = (node.rmax - ro) * one_over_rd;
            float3 minta = min(t1a, t2a);
            float3 maxta = max(t1a, t2a);
            float3 mintb = min(t1b, t2b);
            float3 maxtb = max(t1b, t2b);
            float  tmina = max(max(max(minta.x, minta.y), minta.z), 0);
            float  tminb = max(max(max(mintb.x, mintb.y), mintb.z), 0);
            float  tmaxa = min(min(min(maxta.x, maxta.y), maxta.z), dist);
            float  tmaxb = min(min(min(maxtb.x, maxtb.y), maxtb.z), dist);
            float  dist1 = tmina > tmaxa ? 1e30f : tmina;
            float  dist2 = tminb > tmaxb ? 1e30f : tminb;
            if (dist1 > dist2) {
                float dd = dist1;
                dist1 = dist2;
                dist2 = dd;
                uint t = node.left;
                node.left = node.right;
                node.right = t;
            }
            if (dist1 == 1e30f) {
                if (stack_ptr == 0) {
                    break;
                }

                node_idx = stack[--stack_ptr];
            } else {
                node_idx = node.left;
                if (dist2 != 1e30f) {
                    stack[stack_ptr++] = node.right;
                }
            }
        }
    }

    return false;
}

float3 transform_point(float3 p, float4x4 m) {
    return mul(float4(p, 1), m).xyz;
}

float3 transform_vector(float3 v, float4x4 m) {
    return mul(float4(v, 0), m).xyz;
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

void orthonormal_basis(float3 v, out float3 b0, out float3 b1) {
	float3 u = float3(0, 0, 1);
	if (abs(dot(v,u)) > 0.999) u = float3(0, 1, 0);
	b0 = normalize(cross(u, v));
	b1 = normalize(cross(b0, v));
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

    const Instance           instance = instances[hit.instance];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.vbuffer)];
    StructuredBuffer<uint>   indices = ResourceDescriptorHeap[NonUniformResourceIndex(instance.ibuffer)];
    const float4x4           transform = instance.transform;
    const float4x4           inv_transpose = instance.inv_transpose;
    const Vertex             v0 = vertices[indices[hit.prim_idx * 3]];
    const Vertex             v1 = vertices[indices[hit.prim_idx * 3 + 1]];
    const Vertex             v2 = vertices[indices[hit.prim_idx * 3 + 2]];
    float                    w = 1.0 - (hit.u + hit.v);

    data.position = transform_point(v0.position * w + v1.position * hit.u + v2.position * hit.v, transform);
    data.uv = v0.uv * w + v1.uv * hit.u + v2.uv * hit.v;
	data.geometric_normal = normalize(transform_vector(normalize(cross(v1.position - v0.position, v2.position - v0.position)), inv_transpose));
    data.normal = normalize(transform_vector(normalize(v0.normal * w + v1.normal * hit.u + v2.normal * hit.v), inv_transpose));
    data.tangeant = normalize(transform_vector(normalize(v0.tangeant * w + v1.tangeant * hit.u + v2.tangeant * hit.v), inv_transpose));
    data.bitangeant = normalize(transform_vector(normalize(v0.bitangeant * w + v1.bitangeant * hit.u + v2.bitangeant * hit.v), inv_transpose));
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
	data.h_dot_l = saturate(dot(l, data.h));
	data.h_dot_n = saturate(dot(n, data.h));
	data.h_dot_v = saturate(dot(v, data.h));
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

float smith_g1_ggx(float alpha_squared, float n_dot_s_squared) {
	return 2.0 / (sqrt(((alpha_squared * (1.0 - n_dot_s_squared)) + n_dot_s_squared) / n_dot_s_squared) + 1.0);
}

float sample_weight_ggx_vndf(float alpha_squared, float n_dot_l, float n_dot_v){
	float g1v = smith_g1_ggx(alpha_squared, n_dot_v * n_dot_v);
	float g1l = smith_g1_ggx(alpha_squared, n_dot_l * n_dot_l);
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
    float G2 = v_smith_ggx_correlated(data.n_dot_l, data.n_dot_v, data.alpha_squared);

    return min(10.0, D * data.F * G2 * one_over_pi32); // is big tweak tweak magical
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

    // we need dot(geo_n, wi) to be > 0, so project wi towards geo_n if necessary  
    float3 wi = normalize(mul(ray_direction_local, to_world));
    float  gn_dot_wi = dot(geo.normal, wi);
    if (gn_dot_wi <= 1e-4) {
        float t = 1e-4 - gn_dot_wi;
        wi = wi + t * geo.normal;
        wi = normalize(wi);
    }

    out_ray_direction = wi;
    out_sample_weight = sample_weight;
    return true;
}


[numthreads(8, 8, 1)]
void megakernel(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    uint2 pixel = dispatch_thread_id.xy;
    if (pixel.x >= uniforms.resolution.x || pixel.y >= uniforms.resolution.y) {
        return;
    }
    
    rng_state = uniforms.rng_seed + pixel.y * uniforms.resolution.x + pixel.x;

    float2 fpixel = pixel + 0.5 + random_float2() * 0.5;
    float  ndc_x = (2.0f * fpixel.x / float(uniforms.resolution.x)) - 1.0f;
    float  ndc_y = 1.0f - (2.0f * fpixel.y / float(uniforms.resolution.y));
    float4 near_clip = float4(ndc_x, ndc_y, 0.0f, 1.0f);
    float4 far_clip = float4(ndc_x, ndc_y, 1.0f, 1.0f);
    float4 near_world = mul(near_clip, uniforms.inv_view_proj);
    float4 far_world = mul(far_clip, uniforms.inv_view_proj);
    near_world /= near_world.w;
    far_world /= far_world.w;

    float3 ro = near_world.xyz;
    float3 rd = normalize(far_world.xyz - ro);

     
    float3    sky_color = float3(0.729, 0.807, 0.921) * 2.0f;
    float3    throughput = float3(1.0, 1.0, 1.0);
    float3    radiance = float3(0.0, 0.0, 0.0);
    float     maxd = 1000.0;
    const int max_bounce = 2;
    for (int bounce = 0; bounce <= max_bounce; bounce += 1) {
        Hit hit = traverse_tlas(ro, rd, maxd);
        if (hit.distance >= maxd) {
            radiance += sky_color * throughput;
            break;
        }
        
		GeometryData geo = load_geometry_data(hit, rd);
		MaterialData mat = load_material_data(hit, geo);
		radiance += mat.emissive_color * throughput * 0.01f;

		ro = geo.position + geo.geometric_normal * 0.00001f;
        if (is_occluded_tlas(ro, uniforms.sun_dir, 1000) == false) {
            //radiance += 1.0 * float3(1, 0.8, 0.5) * max(0, dot(sun, geo.shading_normal)) * throughput;
            float3 light_intensity = 15.0 * float3(1.0, 0.8, 0.5);
            radiance += eval_brdf(geo, mat, -rd, uniforms.sun_dir) * light_intensity * throughput;
        }

		float3 v = -rd;
		bool   eval_specular;
		float  spec_p = specular_probability(mat, geo.shading_normal, v);
		if (random_float() < spec_p) {
			eval_specular = true;
			throughput = throughput / spec_p;
		} else {
			eval_specular = false;
			throughput = throughput / (1.f - spec_p);
		}

		float2 rng = float2(random_float(), random_float());
        float3 brdf_weight;
		if (eval_indirect_brdf(rng, geo, mat, -rd, eval_specular, rd, brdf_weight) == false) {
			break;
		}

		throughput = brdf_weight * throughput;
    }

    out_texture[pixel] = lerp(out_texture[pixel], float4(radiance, 1.0), 1.0 / uniforms.sample_count);
}