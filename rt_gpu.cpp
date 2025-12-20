#include "mini_vg.h"

#include <span>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <random>

#include "external/D3D12MemAlloc.h"
#include "external/dxcapi.h"
#define TINYBVH_IMPLEMENTATION
#define PARANOID
#include "external/tiny_bvh.h"
#include <d3dx12.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <wrl.h>
#include <intrin.h>

#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "dxcompiler.lib")

using namespace tinybvh;

#define check(expr, expected, r)                      \
	if ((expr) != (expected)) {                       \
		vg_assert(false);                             \
		printf("(%d) %s failed\n", __LINE__, #expr);  \
		return r;                                     \
	}

#define check_or_null(expr, expected) check(expr, expected, nullptr)
#define check_or_false(expr, expected) check(expr, expected, false)

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

extern "C" {
	__declspec(dllexport) extern const UINT  D3D12SDKVersion = 618;
	__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

struct Image {
    u32 *data;
    u32  width;
    u32  height;
};

struct HDRImage {
    Vec3 *data;
    u32   width;
    u32   height;
};

struct DescriptorRange {
    DescriptorRange *next;
    u64              gpu_ptr;
    u64              cpu_ptr;
    u32              heap_idx;
    u32              stride;

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(u32 n = 0) {
        return {gpu_ptr + n * stride};
    }

	D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(u32 n = 0) {
		return { cpu_ptr + n * stride };
	}
};

struct DescriptorAllocator {
    Arena                      *arena;
	ID3D12DescriptorHeap       *heap;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_heap_start;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_heap_start;
    DescriptorRange            *free_descriptor;
    u32                         pos;
    u32                         size;
	u32                         ring_pos;
	u32                         ring_size;
    u32                         stride;
    bool                        shader_visible;

    DescriptorRange *alloc() {
        DescriptorRange *descriptor = free_descriptor;
        if (descriptor == nullptr) {
			if (pos == size) return nullptr;

			descriptor = arena->push_array<DescriptorRange>(1);
			descriptor->cpu_ptr = cpu_heap_start.ptr + pos * stride;
			descriptor->gpu_ptr = shader_visible ? gpu_heap_start.ptr + pos * stride : 0;
			descriptor->heap_idx = pos;
			descriptor->stride = stride;
			pos += 1;
		} else {
			sll_stack_pop(free_descriptor);
		}

		descriptor->next = nullptr;
		return descriptor;
	}

	void release(DescriptorRange **descriptor) {
		sll_stack_push(free_descriptor, *descriptor);
		*descriptor = nullptr;
	}

	DescriptorRange ring_alloc(u32 n = 1) {
		vg_assert(n < ring_size);
		
		u32 bot = ring_pos;
		u32 top = bot + n;
		if (top >= ring_size) {
			bot = 0;
			top = n;
		}
		ring_pos = top;
	
		DescriptorRange descriptor;
		descriptor.next = nullptr;
		descriptor.cpu_ptr = cpu_heap_start.ptr + bot * stride;
		descriptor.gpu_ptr = shader_visible ? gpu_heap_start.ptr + bot * stride : 0;
		descriptor.heap_idx = bot;
		descriptor.stride = stride;
		return descriptor;
	}
};

struct Texture {
    D3D12MA::Allocation *alloc;
    DescriptorRange     *srv;
    DescriptorRange     *rtv;
    DescriptorRange     *dsv;
};

struct GpuMesh {
    D3D12MA::Allocation *vertex_buffer;
    D3D12MA::Allocation *index_buffer;
    D3D12MA::Allocation *blas;
    DescriptorRange     *vbuffer_view;
    DescriptorRange     *ibuffer_view;
    DescriptorRange     *blas_view;
    u32                  index_count;
};

struct GpuInstance {
    s32    diffuse_tex;
    s32    normal_tex;
    s32    specular_tex;
    s32    emissive_tex;
    Vec3   base_color;
    f32    roughness;
    Vec3   emissive_color;
    f32    metalness;
    s32    vbuffer;
    s32    ibuffer;
    s32    blas;
    s32    padding;
    Mat4x4 transform;
	Mat4x4 inv_transform;
    Mat4x4 inv_transpose;
};

struct UniformData {
	Mat4x4 inv_view_proj;
	Int2   resolution;
	u32    rng;
	u32    sample_count;
	Vec3   sun_dir;
};

struct ReleasedAlloc {
    ReleasedAlloc       *next;
    D3D12MA::Allocation *alloc;
};

struct ReleasedTexture {
	ReleasedTexture *next;
	Texture          texture;
};

u64                        frame = 0;
f32                        t;
f32                        dt;
OsHandle                   window;
Arena                     *arena;
Arena                     *scratch;
const u32                  framebuffer_count = 2;
ID3D12Device              *device = nullptr;
D3D12MA::Allocator        *allocator = nullptr;
IDxcUtils                 *dxc_utils;
IDxcCompiler3             *dxc_compiler;
IDxcIncludeHandler        *dxc_include_handler;
DescriptorAllocator       *srv_allocator;
DescriptorAllocator       *srv_cpu_allocator;
DescriptorAllocator       *sampler_allocator;
DescriptorAllocator       *rtv_allocator;
DescriptorAllocator       *dsv_allocator;
ID3D12CommandQueue        *gfx_queue = nullptr;
ID3D12CommandQueue        *copy_queue = nullptr;
ID3D12CommandQueue        *comp_queue = nullptr;
ID3D12CommandAllocator    *gfx_cmd_allocator[framebuffer_count] = {nullptr};
ID3D12CommandAllocator    *copy_cmd_allocator[framebuffer_count] = {nullptr};
ID3D12CommandAllocator    *comp_cmd_allocator[framebuffer_count] = {nullptr};
ID3D12GraphicsCommandList *gfx_cmd_list = nullptr;
ID3D12GraphicsCommandList *copy_cmd_list = nullptr;
ID3D12GraphicsCommandList *comp_cmd_list = nullptr;
DXGI_FORMAT                swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
DXGI_SWAP_CHAIN_FLAG       swapchain_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
IDXGISwapChain3           *swapchain = nullptr;
DescriptorRange           *default_sampler = nullptr;
ID3D12Fence               *present_fence = nullptr;
ID3D12Fence               *fence = nullptr;
u64                        fence_value = 0;
ID3D12RootSignature       *passthrough_rs;
ID3D12PipelineState       *passthrough_pso;
ID3D12PipelineState       *compute_pso;
ReleasedAlloc             *released_allocs[framebuffer_count] = {};
ReleasedAlloc             *free_released_alloc = nullptr;
ReleasedTexture           *released_textures[framebuffer_count] = {};
ReleasedTexture           *free_released_texture = nullptr;
ID3D12RootSignature       *megakernel_rs;
ID3D12PipelineState       *megakernel_pso;

struct DDSHeader {
    u32 magic;
    u32 size;
    u32 flags;
    u32 height;
    u32 width;
    u32 pitch_or_linear_size;
    u32 depth;
    u32 mip_map_count;
    u32 reserved1[11];

    struct {
        u32 size;
        u32 flags;
        u32 four_cc;
        u32 rgb_bit_count;
        u32 r_mask;
        u32 g_mask;
        u32 b_mask;
        u32 a_mask;
    } ddspf;

    u32 caps[4];
    u32 reserved2;
};

struct DDSImage {
    std::string_view name;
    DDSHeader        header;
    u8              *data;
};

struct vg_align(16) Vertex {
    Vec3 position;
    Vec3 tangeant;
    Vec3 bitangeant;
    Vec3 normal;
    Vec2 uv;
	Vec2 pad;
};

int a = sizeof(Vertex);

struct Material {
    s32  diffuse;
    s32  normal;
    s32  specular;
    s32  emissive;
    Vec3 base_color;
    Vec3 emissive_color;
    f32  roughness;
    f32  metalness;
};

struct Mesh {
    std::span<Vertex> vertices;
    std::span<u32>    indices;
	BVH4_GPU          bvh;
	u32               material;
};

struct SceneNode {
    SceneNode     *parent;
    Mat4x4         transform;
    Mat4x4         global_transform;
	Mat4x4         global_inv_trans;
    bool           dirty_global_transform;
    std::span<u32> meshes;
};

struct Ray3 {
	Vec3 origin;
	Vec3 direction;
};

struct GeometryData {
	Vec3 position;
	Vec2 uv;
	Vec3 geometric_normal;
	Vec3 normal;
	Vec3 shading_normal;
	Vec3 tangeant;
	Vec3 bitangeant;
};

struct MaterialData {
    Vec4 base_color;
    Vec3 emissive_color;
    f32  roughness;
    f32  metalness;
};

struct BrdfData {
	Vec3 f0;
	Vec3 diffuse;
	Vec3 F;

	Vec3 v;
	Vec3 n;
	Vec3 h;
	Vec3 l;

	f32 n_dot_l;
	f32 n_dot_v;
	f32 h_dot_l;
	f32 h_dot_n;
	f32 h_dot_v;
	
	f32 roughness;
	f32 alpha;
	f32 alpha_squared;
};

struct Camera {
    Vec3   position;
    Int2   resolution;
    f32    pitch;
    f32    yaw;
    f32    fov;
    f32    znear;
    f32    zfar;
	Mat4x4 transform;
    Mat4x4 inv_view_proj;

    Ray3 generate_ray(f32 pixel_x, f32 pixel_y) const {
    	f32 ndc_x = (2.0f * (pixel_x) / float(resolution.x)) - 1.0f;
    	f32 ndc_y = 1.0f - (2.0f * (pixel_y) / float(resolution.y));

    	Vec4 near_clip = Vec4(ndc_x, ndc_y, 0.0f, 1.0f);
    	Vec4 far_clip  = Vec4(ndc_x, ndc_y, 1.0f, 1.0f);

    	Vec4 near_world = near_clip * inv_view_proj;
    	Vec4 far_world  = far_clip * inv_view_proj;

    	near_world /= near_world.w;
    	far_world  /= far_world.w;

    	Vec3 o = Vec3(near_world.x, near_world.y, near_world.z);
    	Vec3 d = normalize(Vec3(far_world.x, far_world.y, far_world.z) - o);

    	return Ray3{o, d};
	}

	Vec3 forward() {
		return transform.z;
	}

	Vec3 right() {
		return transform.x;
	}

    void update() {
        f32  cos_pitch = cos(pitch);
        f32  sin_pitch = sin(pitch);
        f32  cos_yaw = cos(yaw);
        f32  sin_yaw = sin(yaw);
        Vec3 xaxis = {cos_yaw, 0.0f, -sin_yaw};
        Vec3 yaxis = {sin_pitch * sin_yaw, cos_pitch, sin_pitch * cos_yaw};
        Vec3 zaxis = {cos_pitch * sin_yaw, -sin_pitch, cos_pitch * cos_yaw};
        f32  tx = xaxis.x * position.x + xaxis.y * position.y + xaxis.z * position.z;
        f32  ty = yaxis.x * position.x + yaxis.y * position.y + yaxis.z * position.z;
        f32  tz = zaxis.x * position.x + zaxis.y * position.y + zaxis.z * position.z;
        f32  fov_y = 2.0f * atanf(tanf(fov * 0.5f) * resolution.y / resolution.x);

        Mat4x4 view = {xaxis.x, yaxis.x, zaxis.x, 0.0f, xaxis.y, yaxis.y, zaxis.y, 0.0f, xaxis.z, yaxis.z, zaxis.z, 0.0f, -tx, -ty, -tz, 1.0f};
        Mat4x4 persp = Mat4x4::perspective(fov_y, (f32)resolution.x / resolution.y, znear, zfar);
        Mat4x4 view_proj = view * persp;

		transform = view.inverse();
        inv_view_proj = view_proj.inverse();
    }
};

thread_local std::mt19937 rd;
Camera                    camera;
std::span<Mesh>           meshes;
std::span<Material>       materials;
std::span<DDSImage>       textures;
std::span<SceneNode>      scene;
std::span<BLASInstance>   blas_instances;
BVH_GPU                   tlas;
u32                       sample_count = 0;
bool                      inspect_pixel = false;
Int2                      inspect_pos;

std::span<Texture>     gpu_textures;
std::span<GpuMesh>     gpu_meshes;
D3D12MA::Allocation   *gpu_instances;
DescriptorRange       *gpu_instances_view;
D3D12MA::Allocation   *tlas_nodes;
DescriptorRange       *tlas_nodes_view;
D3D12MA::Allocation   *tlas_indices;
DescriptorRange       *tlas_indices_view;

const char *passthrough_shader = R"__HLSL(
	struct Texture {
		uint tex_idx;
		uint sampler_idx;
	};

	struct PsInput {
		float4 pos : SV_POSITION;
		float2 uv :  TEXCOORD0;
	};

	ConstantBuffer<Texture> texture : register(b0, space0);

	static const float2 vertices[] = {
		float2(-1, -1), // bottom-left
		float2(-1,  1), // top-left
		float2( 1,  1), // top-right
		float2( 1, -1)  // bottom-right
	};

	static const float2 uvs[] = {
		float2(0, 1), // bottom-left
		float2(0, 0), // top-left
		float2(1, 0), // top-right
		float2(1, 1), // bottom-right
	};

	static const uint indices[] = { 0, 1, 2, 0, 2, 3 };

	PsInput vs_main(uint vertex_id : SV_VertexID) {
		uint index = indices[vertex_id];
		PsInput output;
		output.pos = float4(vertices[index], 0, 1);
		output.uv = uvs[index];
		return output;
	}

	float3 ACES_film(float3 x) {
    	float a = 2.51f;
    	float b = 0.03f;
    	float c = 2.43f;
    	float d = 0.59f;
    	float e = 0.14f;
    	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
	}


	float4 ps_main(PsInput input) : SV_TARGET {
		SamplerState sampler = SamplerDescriptorHeap[texture.sampler_idx];
		Texture2D tex = ResourceDescriptorHeap[texture.tex_idx];
		float3 rgb = ACES_film(tex.Sample(sampler, input.uv).rgb);
		return float4(pow(rgb, 1.0 / 2.2), 1);
	}
)__HLSL";

DescriptorAllocator *alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE type, u32 size, bool shader_visible) {
    ID3D12DescriptorHeap      *heap;
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};

    desc.Type           = type;
    desc.NumDescriptors = size;
    desc.Flags          = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask       = 0;
    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (SUCCEEDED(hr) == false) {
        return nullptr;
    }

	DescriptorAllocator *allocator = arena->push_array<DescriptorAllocator>(1);
	std::memset(allocator, 0, sizeof(DescriptorAllocator));
	allocator->arena = arena;
	allocator->heap = heap;
	if (shader_visible) allocator->gpu_heap_start = heap->GetGPUDescriptorHandleForHeapStart();	
	allocator->cpu_heap_start = heap->GetCPUDescriptorHandleForHeapStart();
	allocator->pos = size / 2;
	allocator->size = size;
	allocator->ring_pos = 0;
	allocator->ring_size = allocator->pos;
	allocator->stride = device->GetDescriptorHandleIncrementSize(type);
	allocator->shader_visible = shader_visible;
	return allocator;
}

void release_descriptor_allocator(DescriptorAllocator **allocator) {
	(*allocator)->heap->Release();
	*allocator = nullptr;
}

void release_queued_allocs() {
	ReleasedAlloc **released = &released_allocs[frame % framebuffer_count];
	while (*released != nullptr) {
		ReleasedAlloc *r = *released;
		sll_stack_pop(*released);		
		r->alloc->Release();
		sll_stack_push(free_released_alloc, r);
	}
}

void queue_alloc_for_release(D3D12MA::Allocation *alloc) {
	u64 bin_idx = frame % framebuffer_count;

	ReleasedAlloc *released = free_released_alloc;
	if (released == nullptr) {
		released = arena->push_array<ReleasedAlloc>(1);
	} else {
		sll_stack_pop(free_released_alloc);
	}

	released->alloc = alloc;
	sll_stack_push(released_allocs[bin_idx], released);
}

ID3DBlob *compile_shader(std::string_view src, std::string_view entry_point, std::string_view target) {
	auto checkpoint = RAIICheckpoint(arena);	
	
	ComPtr<IDxcCompilerArgs> arg_builder;
	check_or_null(dxc_utils->BuildArguments(0, 0, 0, 0, 0, 0, 0, &arg_builder), S_OK);

	char *e = arena->push_array<char>(entry_point.length() + 1);
	char *t = arena->push_array<char>(target.length() + 1);
	std::memcpy(e, entry_point.data(), entry_point.length());
	std::memcpy(t, target.data(), target.length());
	e[entry_point.length()] = 0;
	t[entry_point.length()] = 0;
	const char *cmd[] = {
		"-E", e,
		"-T", t,
		#if BUILD_DEBUG
			"Od",
			"-Zi",
			"-Qembed_debug",
		#endif
	};
	arg_builder->AddArgumentsUTF8(cmd, array_count(cmd));

    DxcBuffer            src_buffer;
    ComPtr<IDxcResult>   dxc_result = nullptr;
    ComPtr<IDxcBlobUtf8> errors = nullptr;
    src_buffer.Ptr = src.data();
	src_buffer.Size = src.length();
	src_buffer.Encoding = DXC_CP_UTF8;
	HRESULT hr = dxc_compiler->Compile(&src_buffer, arg_builder->GetArguments(), arg_builder->GetCount(), dxc_include_handler, IID_PPV_ARGS(&dxc_result));
	dxc_result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (errors != nullptr && errors->GetStringLength() > 0) {
		printf("compile_shader() failed:\n%s\n", errors->GetStringPointer());
		return nullptr;
	}

	if (hr != S_OK) {
		printf("dxc_compiler->Compile() failed\n");
		return nullptr;
	}

	ID3DBlob *shader;
	dxc_result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);
	return shader;
}

D3D12_ROOT_SIGNATURE_FLAGS root_signature_flags(bool gfx, bool vs, bool ps, bool hs, bool ds, bool gs) {
	D3D12_ROOT_SIGNATURE_FLAGS flags = {};
	if (gfx) flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!vs) flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;
    if (!ps) flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    if (!hs) flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
    if (!ds) flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
    if (!gs) flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    flags |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
	return flags;
}

ID3D12RootSignature *create_root_signature(std::span<CD3DX12_ROOT_PARAMETER1> params, D3D12_ROOT_SIGNATURE_FLAGS flags) {
    ID3D12RootSignature                  *root_signature;
    ComPtr<ID3DBlob>                      serialized_signature, error_msg;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC signature_desc;
	signature_desc.Init_1_1((UINT)params.size(), params.data(), 0, nullptr, flags);
	if (D3D12SerializeVersionedRootSignature(&signature_desc, &serialized_signature, &error_msg) != S_OK) {
		printf("D3D12SerializeVersionedRootSignature() failed: %s\n", (char*)error_msg->GetBufferPointer());
		return nullptr;
	}
	check_or_null(device->CreateRootSignature(0, serialized_signature->GetBufferPointer(), serialized_signature->GetBufferSize(), IID_PPV_ARGS(&root_signature)), S_OK);
	return root_signature;
}

Texture create_texture(u32 width, u32 height, DXGI_FORMAT format, u32 mip_count = 1, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
	Texture texture = {};

    D3D12MA::ALLOCATION_DESC alloc_desc = {};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = mip_count;
    texture_desc.Format = (DXGI_FORMAT)format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = flags;
	allocator->CreateResource(&alloc_desc, &texture_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &texture.alloc, __uuidof(ID3D12Resource), nullptr);
	
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : format;
	srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = -1;
	texture.srv = srv_allocator->alloc();
	device->CreateShaderResourceView(texture.alloc->GetResource(), &srv_desc, texture.srv->cpu_handle());

	if (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
		texture.rtv = rtv_allocator->alloc();
		device->CreateRenderTargetView(texture.alloc->GetResource(), nullptr, texture.rtv->cpu_handle());
	}

	if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
		texture.dsv = dsv_allocator->alloc();
		device->CreateDepthStencilView(texture.alloc->GetResource(), nullptr, texture.dsv->cpu_handle());
	}

	return texture;
}

void set_texture_data(const Texture &texture, std::span<u8> pixels, u64 pitch = 0, u32 mip_level = 0) {
    D3D12MA::Allocation               *staging_buffer;
    D3D12_RESOURCE_DESC                texture_desc = texture.alloc->GetResource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    u64                                row_size, texture_size_in_byte;
    u32                                row_count;
    
	device->GetCopyableFootprints(&texture_desc, mip_level, 1, 0, &layout, &row_count, &row_size, &texture_size_in_byte);
    D3D12MA::ALLOCATION_DESC staging_alloc_desc = {};
    staging_alloc_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC staging_desc;
    staging_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    staging_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    staging_desc.Width = texture_size_in_byte;
    staging_desc.Height = 1;
    staging_desc.DepthOrArraySize = 1;
    staging_desc.MipLevels = 1;
    staging_desc.Format = DXGI_FORMAT_UNKNOWN;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    staging_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
	allocator->CreateResource(&staging_alloc_desc, &staging_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &staging_buffer, __uuidof(ID3D12Resource), nullptr);

	if (pitch == 0) {
		pitch = row_size;
	}

    u8         *p_staging = nullptr;
    D3D12_RANGE range{0, texture_size_in_byte};
    staging_buffer->GetResource()->Map(0, &range, (void **)&p_staging);

    for (u32 i = 0; i < row_count; ++i) {
        std::memcpy(p_staging + layout.Footprint.RowPitch * i, pixels.data() + pitch * i, row_size);
    }
    staging_buffer->GetResource()->Unmap(0, &range);

    D3D12_TEXTURE_COPY_LOCATION dst;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = mip_level;
    dst.pResource = texture.alloc->GetResource();

    D3D12_TEXTURE_COPY_LOCATION src;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    src.pResource = staging_buffer->GetResource();

    copy_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	queue_alloc_for_release(staging_buffer);
};

D3D12MA::Allocation *create_buffer(u64 size_in_byte, D3D12_HEAP_TYPE heap_type) {
	D3D12MA::Allocation     *alloc;
	D3D12MA::ALLOCATION_DESC alloc_desc = {};
	alloc_desc.HeapType = heap_type;
	D3D12_RESOURCE_DESC buffer_desc;
	buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    buffer_desc.Width = size_in_byte;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.SampleDesc.Quality = 0;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	allocator->CreateResource(&alloc_desc, &buffer_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &alloc, __uuidof(ID3D12Resource), nullptr);
	return alloc;	
}

void set_buffer_data(D3D12MA::Allocation *buffer, std::span<u8> data) {
    D3D12MA::Allocation     *staging_buffer;
    CD3DX12_RESOURCE_DESC    staging_buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(data.size_bytes(), D3D12_RESOURCE_FLAG_NONE, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    D3D12MA::ALLOCATION_DESC alloc_desc = {};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    allocator->CreateResource(&alloc_desc, &staging_buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &staging_buffer, __uuidof(ID3D12Resource), nullptr);
    u8         *p_staging;
    D3D12_RANGE range{0, data.size_bytes()};
    staging_buffer->GetResource()->Map(0, &range, (void **)&p_staging);
    std::memcpy(p_staging, data.data(), data.size_bytes());
    staging_buffer->GetResource()->Unmap(0, &range);

    copy_cmd_list->CopyBufferRegion(buffer->GetResource(), 0, staging_buffer->GetResource(), 0, data.size_bytes());
    queue_alloc_for_release(staging_buffer);
}

void release_texture(Texture *texture) {
	texture->alloc->Release();
	texture->alloc = nullptr;
	srv_allocator->release(&texture->srv);
	if (texture->rtv) rtv_allocator->release(&texture->rtv);
	if (texture->dsv) dsv_allocator->release(&texture->dsv);
}

void release_queued_textures() {
	ReleasedTexture **released = &released_textures[frame % framebuffer_count];
	while (*released != nullptr) {
		ReleasedTexture *r = *released;
		sll_stack_pop(*released);
		//printf("releasing texture queued on frame %llu and we are on frame %llu\n", r->frame, frame);
		release_texture(&(r->texture));
		sll_stack_push(free_released_texture, r);
	}
}

void queue_texture_for_release(Texture *texture) {
	u64 bin_idx = frame % framebuffer_count;

	ReleasedTexture *released = free_released_texture;
	if (released == nullptr) {
		released = arena->push_array<ReleasedTexture>(1);
	} else {
		sll_stack_pop(free_released_texture);
	}

	released->texture = *texture;
	sll_stack_push(released_textures[bin_idx], released);

	std::memset(texture, 0, sizeof(Texture));
}

std::string_view read_entire_file(std::string_view path, Arena *arena) {
	FILE *file = nullptr;
	fopen_s(&file, path.data(), "rb");
	if (file == nullptr) {
		return {};
	}

	fseek(file, 0, SEEK_END);
	u64 size = ftell(file);
	fseek(file, 0, SEEK_SET);

	s8 *buf = arena->push_array<s8>(size);
	u64 read = fread(buf, size, 1, file);
	fclose(file);

	if (read != 1) return {};
	return {buf, size};
}

bool init_dx12() {
#if BUILD_DEBUG
	ID3D12Debug *debug_ctrl;
	check_or_false(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl)), S_OK);
	debug_ctrl->EnableDebugLayer();
	ID3D12Debug1 *debug_ctrl1;
	debug_ctrl->QueryInterface(IID_PPV_ARGS(&debug_ctrl1));
	debug_ctrl1->SetEnableGPUBasedValidation(true);
	debug_ctrl1->SetEnableSynchronizedCommandQueueValidation(true);
#endif

    u64            best_vram = 0; // lol
    IDXGIAdapter1 *tmp = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIFactory4 *dxgi;
    check_or_false(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)), S_OK);
    for (s32 i = 0; dxgi->EnumAdapters1(i, &tmp) != DXGI_ERROR_NOT_FOUND; i += 1) {
		DXGI_ADAPTER_DESC1 desc;
		tmp->GetDesc1(&desc);

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
		if (desc.DedicatedVideoMemory > best_vram) {
			best_vram = desc.DedicatedVideoMemory;
			adapter = tmp;
		}
	}
	if (adapter == nullptr) return false;

	DXGI_ADAPTER_DESC1 adapter_desc;
	adapter->GetDesc1(&adapter_desc);
	printf("%s\n", to_utf8(adapter_desc.Description, arena).data());
	check_or_false(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&device)), S_OK);

	D3D12MA::ALLOCATOR_DESC allocator_desc = {};
	allocator_desc.pDevice = device;
	allocator_desc.pAdapter = adapter;
	check_or_false(FAILED(D3D12MA::CreateAllocator(&allocator_desc, &allocator)), false);

	check_or_false(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils)), S_OK);
	check_or_false(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler)), S_OK);
	check_or_false(dxc_utils->CreateDefaultIncludeHandler(&dxc_include_handler), S_OK);

	srv_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);
	srv_cpu_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, false);	
	sampler_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, true);	
	rtv_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false);	
	dsv_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false);
	check_or_false(srv_allocator!= nullptr, true);	
	check_or_false(srv_cpu_allocator!= nullptr, true);	
	check_or_false(sampler_allocator != nullptr, true);	
	check_or_false(rtv_allocator != nullptr, true);	
	check_or_false(dsv_allocator != nullptr, true);
	
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	check_or_false(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&gfx_queue)), S_OK);
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	check_or_false(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&copy_queue)), S_OK);
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	check_or_false(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&comp_queue)), S_OK);

	check_or_false(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence)), S_OK);
	check_or_false(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), S_OK);
	for (u32 i = 0; i < framebuffer_count; i += 1) {
		check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gfx_cmd_allocator[i])), S_OK);
		check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_cmd_allocator[i])), S_OK);
		check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&comp_cmd_allocator[i])), S_OK);
		gfx_cmd_allocator[i]->SetName(L"gfx_cmd_allocator");
		copy_cmd_allocator[i]->SetName(L"copy_cmd_allocator");
		comp_cmd_allocator[i]->SetName(L"comp_cmd_allocator");
	}

	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gfx_cmd_allocator[0], nullptr, IID_PPV_ARGS(&gfx_cmd_list)), S_OK);
	gfx_cmd_list->SetName(L"gfx_cmd_list");
	gfx_cmd_list->Close();
	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_cmd_allocator[0], nullptr, IID_PPV_ARGS(&copy_cmd_list)), S_OK);
	copy_cmd_list->SetName(L"copy_cmd_list");
	copy_cmd_list->Close();
	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, comp_cmd_allocator[0], nullptr, IID_PPV_ARGS(&comp_cmd_list)), S_OK);
	comp_cmd_list->SetName(L"comp_cmd_list");
	comp_cmd_list->Close();

	ID3DBlob *passthrough_vs = compile_shader(passthrough_shader, "vs_main", "vs_6_6");
	ID3DBlob *passthrough_ps = compile_shader(passthrough_shader, "ps_main", "ps_6_6");
	if (passthrough_vs == nullptr || passthrough_ps == nullptr) return false;

    CD3DX12_ROOT_PARAMETER1 root_param;
    root_param.InitAsConstants(2, 0, 0);
    passthrough_rs = create_root_signature({&root_param, 1}, root_signature_flags(true, true, true, false, false, false));
	if (passthrough_rs == nullptr) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.VS = {passthrough_vs->GetBufferPointer(), passthrough_vs->GetBufferSize()};
    pso_desc.PS = {passthrough_ps->GetBufferPointer(), passthrough_ps->GetBufferSize()};
    pso_desc.pRootSignature = passthrough_rs;
    pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso_desc.SampleMask = 0xFFFFFFFF;
    pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.InputLayout = D3D12_INPUT_LAYOUT_DESC{nullptr, 0};
    pso_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc = {1, 0};
    pso_desc.NodeMask = 0;
	check_or_false(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&passthrough_pso)), S_OK);

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
	IDXGISwapChain1      *tmp_swapchain;
	swapchain_desc.Format = swapchain_format;
	swapchain_desc.SampleDesc.Count = 1;
	swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapchain_desc.BufferCount = framebuffer_count;
	swapchain_desc.Scaling = DXGI_SCALING_NONE;
	swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapchain_desc.Flags = swapchain_flags;
	check_or_false(dxgi->CreateSwapChainForHwnd(gfx_queue, os_hwnd_from_handle(window), &swapchain_desc, nullptr, nullptr, &tmp_swapchain), S_OK);
	dxgi->MakeWindowAssociation(os_hwnd_from_handle(window), DXGI_MWA_NO_ALT_ENTER);
	tmp_swapchain->QueryInterface(IID_PPV_ARGS(&swapchain));

	DXGI_RGBA transparent = {1, 1, 1, 0};
	swapchain->SetBackgroundColor(&transparent);

	default_sampler = sampler_allocator->alloc();
	D3D12_SAMPLER_DESC sampler_desc = {};
	sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler_desc.MaxLOD = 1000.0f;
	device->CreateSampler(&sampler_desc, default_sampler->cpu_handle());

    std::string_view megakernel_src = read_entire_file("megakernel.hlsl", scratch);
    ID3DBlob        *megakernel_cs = compile_shader(megakernel_src, "megakernel", "cs_6_6");
	if (megakernel_cs == nullptr) return false;

    CD3DX12_ROOT_PARAMETER1 root_params[6];
	D3D12_DESCRIPTOR_RANGE1 ranges[5] {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	root_params[0].InitAsDescriptorTable(1, &ranges[0]);
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 1;
	root_params[1].InitAsDescriptorTable(1, &ranges[1]);
	ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[2].NumDescriptors = 1;
	ranges[2].BaseShaderRegister = 2;
	root_params[2].InitAsDescriptorTable(1, &ranges[2]);
	ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[3].NumDescriptors = 1;
	ranges[3].BaseShaderRegister = 0;
	root_params[3].InitAsDescriptorTable(1, &ranges[3]);
	ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[4].NumDescriptors = 1;
	ranges[4].BaseShaderRegister = 0;
	root_params[4].InitAsDescriptorTable(1, &ranges[4]);
	root_params[5].InitAsConstantBufferView(0);
    megakernel_rs = create_root_signature({&root_params[0], array_count(root_params)}, root_signature_flags(false, false, false, false, false, false));
	if (megakernel_rs == nullptr) return false;

	D3D12_COMPUTE_PIPELINE_STATE_DESC cpso_desc = {};
	cpso_desc.pRootSignature = megakernel_rs;
	cpso_desc.CS = { megakernel_cs->GetBufferPointer(), megakernel_cs->GetBufferSize() };
	check_or_false(device->CreateComputePipelineState(&cpso_desc, IID_PPV_ARGS(&megakernel_pso)), S_OK);

    return true;
}

void wait_for_fence(ID3D12Fence *fence, u64 value) {
	if (fence->GetCompletedValue() >= value) {
		return;
	}

	HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(value, event);
	WaitForSingleObject(event, INFINITE);
	CloseHandle(event);
}

u32 dispatch_size(u32 work_size, u32 group_size) {
	return (work_size + group_size - 1) / group_size;
}

Mat4x4 get_global_transform(SceneNode *node) {
	if (node->dirty_global_transform == false) {
		return node->global_transform;
	}

	node->dirty_global_transform = false;
	Mat4x4 parent_transform = node->parent ? get_global_transform(node->parent) : Mat4x4::identity();
	node->global_transform = node->transform * parent_transform;
	node->global_inv_trans = node->global_transform.inverse().transpose();
	return node->global_transform;
}

void init_rd() {
	rd = std::mt19937(std::random_device()());
}

f32 random_f32_in_range(f32 min, f32 max) {
	std::uniform_real_distribution<f32> dist(min, max);
	return dist(rd);
}

f32 random_f32() {
	return random_f32_in_range(0.0f, 1.0f);
}

u32 random_u32() {
	std::uniform_int_distribution<u32> dist(0, max_u32);
	return dist(rd);
}

bool init_scene() {
	FILE *file = nullptr;
    fopen_s(&file, "bistro.bin", "rb");
	check_or_false(file != nullptr, true);

    u32 mesh_count;
    check_or_false(fread(&mesh_count, sizeof(mesh_count), 1, file), 1);
    meshes = { arena->push_array<Mesh>(mesh_count), mesh_count };
	BVHBase **blasses = arena->push_array<BVHBase*>(meshes.size());

	for (u32 i = 0; i < meshes.size(); i += 1) {
		Mesh &mesh = meshes[i];
		new (&mesh) Mesh();

        u32 vertex_count;
        check_or_false(fread(&vertex_count, sizeof(vertex_count), 1, file), 1);
        mesh.vertices = {arena->push_array<Vertex>(vertex_count), vertex_count};
		for (u32 vi = 0; vi < vertex_count; vi += 1) {
        	check_or_false(fread(&mesh.vertices[vi].position, sizeof(Vec3), 1, file), 1);
        	check_or_false(fread(&mesh.vertices[vi].tangeant, sizeof(Vec3), 1, file), 1);
        	check_or_false(fread(&mesh.vertices[vi].bitangeant, sizeof(Vec3), 1, file), 1);
        	check_or_false(fread(&mesh.vertices[vi].normal, sizeof(Vec3), 1, file), 1);
        	check_or_false(fread(&mesh.vertices[vi].uv, sizeof(Vec2), 1, file), 1);
		}
        u32 index_count;
        check_or_false(fread(&index_count, sizeof(index_count), 1, file), 1);
        mesh.indices = {arena->push_array<u32>(index_count), index_count};
        check_or_false(fread(mesh.indices.data(), sizeof(u32), index_count, file), index_count);

		bvhvec4slice layout((bvhvec4*)mesh.vertices.data(), (u32)mesh.vertices.size(), sizeof(Vertex));
		mesh.bvh.Build(layout, mesh.indices.data(), (u32)mesh.indices.size() / 3);
		blasses[i] = (BVHBase*)&mesh.bvh;

        check_or_false(fread(&mesh.material, sizeof(mesh.material), 1, file), 1);
    }

    u32 texture_count;
    check_or_false(fread(&texture_count, sizeof(texture_count), 1, file), 1);
    textures = { arena->push_array<DDSImage>(texture_count), texture_count };

    for (DDSImage &texture : textures) {
        u32   name_size;
        char *name;
        u64   size_in_byte;
        u64   data_size_in_byte;
        check_or_false(fread(&name_size, sizeof(name_size), 1, file), 1);
		name = arena->push_array<char>(name_size);
		check_or_false(fread(name, 1, name_size, file), name_size);
        texture.name = { name, name_size - 1};
        check_or_false(fread(&size_in_byte, sizeof(size_in_byte), 1, file), 1);
        data_size_in_byte = size_in_byte - sizeof(texture.header);
        check_or_false(fread(&texture.header, sizeof(texture.header), 1, file), 1);
        texture.data = arena->push_array<u8>(data_size_in_byte);
        check_or_false(fread(texture.data, data_size_in_byte, 1, file), 1);
    }

	u32 material_count;
	check_or_false(fread(&material_count, sizeof(material_count), 1, file), 1);
	materials = { arena->push_array<Material>(material_count), material_count };

	for (Material &material : materials) {
		check_or_false(fread(&material.diffuse, sizeof(material.diffuse), 1, file), 1);
		check_or_false(fread(&material.normal, sizeof(material.normal), 1, file), 1);
		check_or_false(fread(&material.specular, sizeof(material.normal), 1, file), 1);
		check_or_false(fread(&material.emissive, sizeof(material.normal), 1, file), 1);
		check_or_false(fread(&material.base_color[0], sizeof(float), 3, file), 3);
		check_or_false(fread(&material.emissive_color[0], sizeof(float), 3, file), 3);
		check_or_false(fread(&material.roughness, sizeof(float), 1, file), 1);
		check_or_false(fread(&material.metalness, sizeof(float), 1, file), 1);

		if (material.emissive != -1) {
			auto &tex = textures[material.emissive];
			if (tex.name.find("String") == tex.name.npos) {
				//material.emissive_color *= 150.0;
			} else {
				//material.emissive_color *= 10.0;
			}
		}
	}

	u32 node_count;
	check_or_false(fread(&node_count, sizeof(node_count), 1, file), 1);
	scene = { arena->push_array<SceneNode>(node_count), node_count };

	u32 instance_count = 0;
	u32 triangle_count = 0;
	for (SceneNode &node : scene) {
		s32 parent_idx;
		check_or_false(fread(&parent_idx, sizeof(parent_idx), 1, file), 1);
		node.parent = parent_idx >= 0 ? &scene[parent_idx] : nullptr;
		check_or_false(fread(&node.transform, sizeof(node.transform), 1, file), 1);
		node.dirty_global_transform = true;
		node.meshes = {};

		u32 mesh_count;
		check_or_false(fread(&mesh_count, sizeof(mesh_count), 1, file), 1);
		if (mesh_count > 0) {
			node.meshes = { arena->push_array<u32>(mesh_count), mesh_count };
			check_or_false(fread(node.meshes.data(), sizeof(u32), mesh_count, file), mesh_count);
		}
	
		instance_count += mesh_count;
	}

    blas_instances = { arena->push_array<BLASInstance>(instance_count), instance_count };
    u32 inst_idx = 0;
    for (SceneNode &node : scene) {
		Mat4x4 transform = get_global_transform(&node).transpose();
		for (u32 mesh_idx : node.meshes) {
			BLASInstance &inst = blas_instances[inst_idx++] = BLASInstance(mesh_idx);
			inst.transform = *(bvhmat4*)&transform;
			inst.pnode = (void*)&node;
			triangle_count += (u32)meshes[mesh_idx].indices.size() / 3;
		}
	}

	printf("instance_count: %d\n", instance_count);
	printf("triangle_count: %d\n", triangle_count);
	tlas.Build(blas_instances.data(), instance_count, blasses, (u32)meshes.size());

    fclose(file);

	return true;
}

void prepare_gpu_data() {
    gpu_textures = {arena->push_array<Texture>(textures.size()), textures.size()};
    for (u32 i = 0; i < textures.size(); i += 1) {
        DDSImage   &img = textures[i];
        DXGI_FORMAT format;
        u32         mip_count = 1;
		u32         block_size = 1;

        if (img.header.flags & 0x20000) {
            mip_count = img.header.mip_map_count;
        }

        bool is_base_color = img.name.find("BaseColor") != img.name.npos;
        if (img.header.ddspf.four_cc == 0) {
            u32 bpp = img.header.ddspf.rgb_bit_count / 8;
            vg_assert(bpp == 4);
            format = is_base_color ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            //format = DXGI_FORMAT_R8G8B8A8_UNORM;
        } else if (img.header.ddspf.four_cc == '1TXD') {
            format = is_base_color ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
            //format = DXGI_FORMAT_BC1_UNORM;
			block_size = 8;
        } else if (img.header.ddspf.four_cc == '5TXD') {
            format = is_base_color ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
            //format = DXGI_FORMAT_BC3_UNORM;
			block_size = 16;
        } else if (img.header.ddspf.four_cc == '2ITA') {
            format = DXGI_FORMAT_BC5_UNORM;
			block_size = 16;
        } else {
            vg_assert(false);
        }

		u32 width = img.header.width;
		u32 height = img.header.height;
		u64 offset = 0;
		gpu_textures[i] = create_texture(width, height, format, mip_count);
		for (u32 mip = 0; mip < mip_count; mip += 1) {
			u32 pitch;
			u32 size_in_byte;
			if (block_size == 1) {
				pitch = width * 4;
				size_in_byte = pitch * height;
			} else {
				pitch = std::max(1u, ((width+3)/4)) * block_size;
				size_in_byte = pitch * std::max(1u, ((height+3)/4));
			}

			std::span<u8> data = { img.data + offset, size_in_byte };
			set_texture_data(gpu_textures[i], data, pitch, mip);

			offset += size_in_byte;
			width = std::max(1u, width/2);
			height = std::max(1u, height/2);
		}
    }

    gpu_meshes = {arena->push_array<GpuMesh>(meshes.size()), meshes.size()};
    for (u32 i = 0; i < meshes.size(); i += 1) {
        Mesh    &mesh = meshes[i];
        GpuMesh &gpu_mesh = gpu_meshes[i];
        u64      blas_size_in_byte = mesh.bvh.allocatedBlocks * sizeof(mesh.bvh.bvh4Data[0]);

        gpu_mesh.vertex_buffer = create_buffer(mesh.vertices.size_bytes(), D3D12_HEAP_TYPE_DEFAULT);
        gpu_mesh.index_buffer = create_buffer(mesh.indices.size_bytes(), D3D12_HEAP_TYPE_DEFAULT);
        gpu_mesh.blas = create_buffer(blas_size_in_byte, D3D12_HEAP_TYPE_DEFAULT);
        set_buffer_data(gpu_mesh.vertex_buffer, {(u8 *)mesh.vertices.data(), mesh.vertices.size_bytes()});
        set_buffer_data(gpu_mesh.index_buffer, {(u8 *)mesh.indices.data(), mesh.indices.size_bytes()});
        set_buffer_data(gpu_mesh.blas, {(u8 *)mesh.bvh.bvh4Data, blas_size_in_byte});

		gpu_mesh.vbuffer_view = srv_allocator->alloc();
		gpu_mesh.ibuffer_view = srv_allocator->alloc();
		gpu_mesh.blas_view = srv_allocator->alloc();
		D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
		view_desc.Format = DXGI_FORMAT_UNKNOWN;
		view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view_desc.Buffer.FirstElement = 0;
		view_desc.Buffer.NumElements = (u32)mesh.vertices.size();
		view_desc.Buffer.StructureByteStride = sizeof(mesh.vertices[0]);
		device->CreateShaderResourceView(gpu_mesh.vertex_buffer->GetResource(), &view_desc, gpu_mesh.vbuffer_view->cpu_handle());
		view_desc.Buffer.NumElements = (u32)mesh.indices.size();
		view_desc.Buffer.StructureByteStride = sizeof(mesh.indices[0]);
		device->CreateShaderResourceView(gpu_mesh.index_buffer->GetResource(), &view_desc, gpu_mesh.ibuffer_view->cpu_handle());
		view_desc.Buffer.NumElements = mesh.bvh.allocatedBlocks;
		view_desc.Buffer.StructureByteStride = sizeof(mesh.bvh.bvh4Data[0]);
		device->CreateShaderResourceView(gpu_mesh.blas->GetResource(), &view_desc, gpu_mesh.blas_view->cpu_handle());
		
		gpu_mesh.index_count = (u32)mesh.indices.size();
    }

	std::span<GpuInstance> gpu_instances_data = { scratch->push_array<GpuInstance>(blas_instances.size()), blas_instances.size() };
    for (u32 i = 0; i < blas_instances.size(); i += 1) {
        BLASInstance &instance = blas_instances[i];
        SceneNode    &node = *(SceneNode *)instance.pnode;
        Mesh         &mesh = meshes[instance.blasIdx];
		GpuMesh      &gpu_mesh = gpu_meshes[instance.blasIdx];
        Material     &material = materials[mesh.material];
        GpuInstance  &gpu_instance = gpu_instances_data[i];

		gpu_instance.diffuse_tex = material.diffuse != -1 ? gpu_textures[material.diffuse].srv->heap_idx : -1;
		gpu_instance.normal_tex = material.normal != -1 ? gpu_textures[material.normal].srv->heap_idx : -1;
		gpu_instance.specular_tex = material.specular != -1 ? gpu_textures[material.specular].srv->heap_idx : -1;
		gpu_instance.emissive_tex = material.emissive != -1 ? gpu_textures[material.emissive].srv->heap_idx : -1;
		gpu_instance.base_color = material.base_color;
		gpu_instance.roughness = material.roughness;
		gpu_instance.emissive_color = material.emissive_color;
		gpu_instance.metalness = material.metalness;

		gpu_instance.vbuffer = gpu_mesh.vbuffer_view->heap_idx;
		gpu_instance.ibuffer = gpu_mesh.ibuffer_view->heap_idx;
		gpu_instance.blas = gpu_mesh.blas_view->heap_idx;
		
		gpu_instance.transform = node.global_transform;
		gpu_instance.inv_transform = node.global_transform.inverse();
		gpu_instance.inv_transpose = node.global_inv_trans;
	}
	gpu_instances = create_buffer(gpu_instances_data.size_bytes(), D3D12_HEAP_TYPE_DEFAULT);
	set_buffer_data(gpu_instances, {(u8*)gpu_instances_data.data(), gpu_instances_data.size_bytes()});
	D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
	view_desc.Format = DXGI_FORMAT_UNKNOWN;
	view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view_desc.Buffer.FirstElement = 0;
	view_desc.Buffer.NumElements = (u32)gpu_instances_data.size();
	view_desc.Buffer.StructureByteStride = sizeof(gpu_instances_data[0]);
	gpu_instances_view = srv_allocator->alloc();
	device->CreateShaderResourceView(gpu_instances->GetResource(), &view_desc, gpu_instances_view->cpu_handle());

	tlas_nodes = create_buffer(tlas.allocatedNodes * sizeof(tlas.bvhNode[0]), D3D12_HEAP_TYPE_DEFAULT);
	tlas_indices = create_buffer(tlas.idxCount * sizeof(tlas.bvh.primIdx[0]), D3D12_HEAP_TYPE_DEFAULT);
	set_buffer_data(tlas_nodes, {(u8*)tlas.bvhNode, tlas.allocatedNodes * sizeof(tlas.bvhNode[0])});
	set_buffer_data(tlas_indices, {(u8*)tlas.bvh.primIdx, tlas.idxCount * sizeof(tlas.bvh.primIdx[0])});
	tlas_nodes_view = srv_allocator->alloc();
	tlas_indices_view = srv_allocator->alloc();
	view_desc.Buffer.NumElements = tlas.allocatedNodes;
	view_desc.Buffer.StructureByteStride = sizeof(tlas.bvhNode[0]);
	device->CreateShaderResourceView(tlas_nodes->GetResource(), &view_desc, tlas_nodes_view->cpu_handle());
	view_desc.Buffer.NumElements = tlas.idxCount;
	view_desc.Buffer.StructureByteStride = sizeof(tlas.bvh.primIdx[0]);
	device->CreateShaderResourceView(tlas_indices->GetResource(), &view_desc, tlas_indices_view->cpu_handle());
}

int entry_point() {
    arena = alloc_arena(MiB(8), MiB(256));
	scratch = alloc_arena(MiB(8), MiB(64));

	u32 width = 200;
	u32 height = 200;
	camera = {};
	camera.resolution = {(s32)width, (s32)height};
	camera.position = Vec3(-13.3f, 6.76f, -0.79f);
	camera.pitch = 0.371f;
	camera.yaw = -5.003f;
	camera.fov = pi32 / 2.0f;
	camera.znear = 0.1f;
	camera.zfar = 1000.0f;
	camera.update();

    window = os_create_window("raytracing", width, height);

	init_rd();

    if (init_dx12() == false) {
        return -1;
    }
	Texture canvas_texture = create_texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	if (init_scene() == false) {
		return -1;
	}

    Vec3 sun = normalize(Vec3(0.32, 0.62, 0.72));

	bool down[Key_Count] = {};
	bool pressed[Key_Count] = {};
	Int2 mouse = {};
    bool quit = false;
    bool resize = false;
	auto start = std::chrono::steady_clock::now();
	auto before = std::chrono::steady_clock::now();
    for (; quit == false; frame += 1) {
        wait_for_fence(present_fence, frame);
		//printf("frame: %d, fence_value: %d\n", frame, fence_value);

		u32 cmd_allocator_idx = frame % framebuffer_count;
		copy_cmd_allocator[cmd_allocator_idx]->Reset();
		copy_cmd_list->Reset(copy_cmd_allocator[cmd_allocator_idx], nullptr);
		comp_cmd_allocator[cmd_allocator_idx]->Reset();
		comp_cmd_list->Reset(comp_cmd_allocator[cmd_allocator_idx], nullptr);
        gfx_cmd_allocator[cmd_allocator_idx]->Reset();
        gfx_cmd_list->Reset(gfx_cmd_allocator[cmd_allocator_idx], nullptr);
		
		release_queued_allocs();
		release_queued_textures();
		std::memset(pressed, 0, sizeof(pressed));
        scratch->reset();
		Int2 mouse_delta = {};

		OsEventList *evt_list = os_poll_events(scratch);
        for (OsEvent *evt = evt_list->first; evt != nullptr; evt = evt->next) {
            if (evt->type == OsEventType_Quit || evt->type == OsEventType_WindowClosed) {
                quit = true;
            } else if (evt->type == OsEventType_WindowResize) {
				width = (u32)evt->size.x;
				height = (u32)evt->size.y;
                resize = true;
			} else if (evt->type == OsEventType_KeyPress) {
				pressed[evt->key] = true;
				down[evt->key] = true;
			} else if (evt->type == OsEventType_KeyRelease) {
				down[evt->key] = false;
			} else if (evt->type == OsEventType_WindowLoseFocus) {
				std::memset(down, 0, sizeof(down));
			} else if (evt->type == OsEventType_MouseMoveRel) {
				mouse_delta += evt->delta;
			} else if (evt->type == OsEventType_MouseMoveAbs) {
				mouse = evt->pos;
			}
        }
		fflush(stdout);

        if (resize) {
            wait_for_fence(present_fence, frame + framebuffer_count - 1);
            swapchain->ResizeBuffers(framebuffer_count, 0, 0, swapchain_format, swapchain_flags);
			queue_texture_for_release(&canvas_texture);
			canvas_texture = create_texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			camera.resolution = Int2(width, height);
			camera.update();
			sample_count = 0;
            resize = false;
        }

        auto now = std::chrono::steady_clock::now();
		t = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count() / 1000000.0f;
		dt = std::chrono::duration_cast<std::chrono::microseconds>(now - before).count() / 1000000.0f;
		before = now;
	
		if (pressed[Key_G]) {
			inspect_pixel = true;
			inspect_pos = mouse;
		}

		bool moved = false;
        if (down[Key_MouseButtonRight]) {
			os_show_cursor(false);
			os_freeze_cursor(true);
            f32 spd = 5.0f;
			if (down[Key_LeftShift]) spd *= 3.f;
            if (down[Key_W]) {
                camera.position += camera.forward() * spd * dt;
				moved = true;
            }
            if (down[Key_S]) {
                camera.position -= camera.forward() * spd * dt;
				moved = true;
            }
            if (down[Key_D]) {
                camera.position += camera.right() * spd * dt;
				moved = true;
            }
            if (down[Key_A]) {
                camera.position -= camera.right() * spd * dt;
				moved = true;
            }
			if (down[Key_Space]) {
				camera.position.y += spd * dt;
				moved = true;
			}
			if (down[Key_LeftCtrl]) {
				camera.position.y -= spd * dt;
				moved = true;
			}

			const f32 sensitivity = 0.001f;
			if (mouse_delta != Int2(0, 0)) {
				camera.yaw += mouse_delta.x * sensitivity;
				camera.pitch += mouse_delta.y * sensitivity;
				camera.pitch = std::max(-pi32_over_2, camera.pitch);
				camera.pitch = std::min(pi32_over_2, camera.pitch);	
				moved = true;
			}

			if (moved) {
				camera.update();
				sample_count = 0;
			}
        } else {
			os_show_cursor(true);
			os_freeze_cursor(false);
		}

		if (pressed[Key_Return]) {
			sun = -normalize(camera.forward());
			sample_count = 0;
		}
		
		vg_local_persist f32 update_title_t = 0.0f;
		vg_local_persist u32 update_title_frames = 0;
		update_title_t += dt;
		update_title_frames += 1;
		if (update_title_t >= 0.25f) {
			char window_title[255] = {};
			f32 fps = update_title_frames * (1.0f / update_title_t);
			snprintf(window_title, array_count(window_title), "%d fps (%.2f ms)", (u32)fps, 1.0f / fps * 1000.0f);
			os_set_window_title(window, window_title);
			update_title_t = 0.0f;
			update_title_frames = 0;
		}

		CD3DX12_RESOURCE_BARRIER barrier;
		ID3D12DescriptorHeap *heaps[2] = { srv_allocator->heap, sampler_allocator->heap };

		if (frame == 0) {
			prepare_gpu_data();
		}


		DescriptorRange uav_view = srv_allocator->ring_alloc();
		DescriptorRange uav_cpu_view = srv_cpu_allocator->ring_alloc();
		device->CreateUnorderedAccessView(canvas_texture.alloc->GetResource(), nullptr, nullptr, uav_view.cpu_handle());
		device->CreateUnorderedAccessView(canvas_texture.alloc->GetResource(), nullptr, nullptr, uav_cpu_view.cpu_handle());
		comp_cmd_list->SetDescriptorHeaps(2, heaps);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(canvas_texture.alloc->GetResource(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		comp_cmd_list->ResourceBarrier(1, &barrier);
		barrier = CD3DX12_RESOURCE_BARRIER::UAV(canvas_texture.alloc->GetResource());
		if (sample_count == 0) {
			Vec4 zero = Vec4(0);
			comp_cmd_list->ClearUnorderedAccessViewFloat(uav_view.gpu_handle(), uav_cpu_view.cpu_handle(), canvas_texture.alloc->GetResource(), zero.f, 0, nullptr);
			comp_cmd_list->ResourceBarrier(1, &barrier);
		}
		UniformData megakernel_data;
		megakernel_data.inv_view_proj = camera.inv_view_proj;
		megakernel_data.resolution = camera.resolution;
		megakernel_data.rng = random_u32();
		megakernel_data.sample_count = ++sample_count;
		megakernel_data.sun_dir = sun;
		D3D12_RANGE map_range;
		map_range.Begin = 0;
		map_range.End = sizeof(megakernel_data);
		void *cbuffer_mapped;
		auto uniform_buffer = create_buffer(sizeof(UniformData), D3D12_HEAP_TYPE_UPLOAD);
		queue_alloc_for_release(uniform_buffer);
		uniform_buffer->GetResource()->Map(0, &map_range, &cbuffer_mapped);
		memcpy(cbuffer_mapped, &megakernel_data, sizeof(megakernel_data));
		comp_cmd_list->SetPipelineState(megakernel_pso);
		comp_cmd_list->SetComputeRootSignature(megakernel_rs);
		comp_cmd_list->SetComputeRootDescriptorTable(0, tlas_nodes_view->gpu_handle());
		comp_cmd_list->SetComputeRootDescriptorTable(1, tlas_indices_view->gpu_handle());
		comp_cmd_list->SetComputeRootDescriptorTable(2, gpu_instances_view->gpu_handle());
		comp_cmd_list->SetComputeRootDescriptorTable(3, default_sampler->gpu_handle());
		comp_cmd_list->SetComputeRootDescriptorTable(4, uav_view.gpu_handle());
		comp_cmd_list->SetComputeRootConstantBufferView(5, uniform_buffer->GetResource()->GetGPUVirtualAddress());
		comp_cmd_list->Dispatch(dispatch_size(width, 8), dispatch_size(height, 8), 1);
		comp_cmd_list->ResourceBarrier(1, &barrier);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(canvas_texture.alloc->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		comp_cmd_list->ResourceBarrier(1, &barrier);
		comp_cmd_list->Close();

        auto            rtv_cpu_handle = rtv_allocator->ring_alloc().cpu_handle();
        ID3D12Resource *backbuffer;
        u32 			backbuffer_idx = swapchain->GetCurrentBackBufferIndex();
        swapchain->GetBuffer(backbuffer_idx, IID_PPV_ARGS(&backbuffer));
        device->CreateRenderTargetView(backbuffer, nullptr, rtv_cpu_handle);

        f32 clear_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
		gfx_cmd_list->SetDescriptorHeaps(2, heaps);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gfx_cmd_list->ResourceBarrier(1, &barrier);
		gfx_cmd_list->SetPipelineState(passthrough_pso);
		gfx_cmd_list->SetGraphicsRootSignature(passthrough_rs);
        gfx_cmd_list->OMSetRenderTargets(1, &rtv_cpu_handle, FALSE, nullptr);
        gfx_cmd_list->ClearRenderTargetView(rtv_cpu_handle, clear_color, 0, nullptr);
		gfx_cmd_list->SetGraphicsRoot32BitConstant(0, canvas_texture.srv->heap_idx, 0);
		//u64 tex_idx = (u64)(t * 2) % gpu_textures.size();
		//gfx_cmd_list->SetGraphicsRoot32BitConstant(0, gpu_textures[tex_idx].srv->heap_idx, 0);
		gfx_cmd_list->SetGraphicsRoot32BitConstant(0, default_sampler->heap_idx, 1);
		gfx_cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		D3D12_VIEWPORT viewport = {};
		viewport.Width =  (f32)width;
		viewport.Height = (f32)height;
		gfx_cmd_list->RSSetViewports(1, &viewport);
		D3D12_RECT scissor = {};
		scissor.right = width;
		scissor.bottom = height;
		gfx_cmd_list->RSSetScissorRects(1, &scissor);
		gfx_cmd_list->DrawInstanced(6, 1, 0, 0);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        gfx_cmd_list->ResourceBarrier(1, &barrier);
        gfx_cmd_list->Close();

		copy_cmd_list->Close();
		copy_queue->Wait(fence, fence_value);
		{
 			ID3D12CommandList *cmd_lists[]{copy_cmd_list};
			copy_queue->ExecuteCommandLists(1, cmd_lists);
		}
		copy_queue->Signal(fence, ++fence_value);
		comp_queue->Wait(fence, fence_value);
		{
			ID3D12CommandList *cmd_lists[]{comp_cmd_list};
			comp_queue->ExecuteCommandLists(1, cmd_lists);
		}
		comp_queue->Signal(fence, ++fence_value);
		gfx_queue->Wait(fence, fence_value);
		{
        	ID3D12CommandList *cmd_lists[]{gfx_cmd_list};
        	gfx_queue->ExecuteCommandLists(1, cmd_lists);
		}
		gfx_queue->Signal(fence, ++fence_value);
		backbuffer->Release();

        swapchain->Present(1, 0);
        gfx_queue->Signal(present_fence, frame + framebuffer_count);
    }

	release_arena(&arena);
	release_arena(&scratch);
    return 0;
}