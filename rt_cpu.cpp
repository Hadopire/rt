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
		ring_pos = n;
	
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
HDRImage                   canvas;
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
u32                        fence_value = 0;
ID3D12RootSignature       *passthrough_rs;
ID3D12PipelineState       *passthrough_pso;
ID3D12PipelineState       *compute_pso;
ReleasedAlloc             *released_allocs[framebuffer_count] = {};
ReleasedAlloc             *free_released_alloc = nullptr;
ReleasedTexture           *released_textures[framebuffer_count] = {};
ReleasedTexture           *free_released_texture = nullptr;

struct WorkItem {
	Range1u32 range_y;
};

u32                     worker_count = 0;
std::thread            *worker_threads;
WorkItem               *work_items;
bool                   *has_work;
std::mutex              cv_mutex;
std::condition_variable cv;
std::mutex              work_done_mutex;
std::condition_variable work_done_cv;
std::atomic<u32>        work_done_count;

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
};

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
	BVH4_CPU          bvh;
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
BLASInstance             *blas_instances;
BVH                       tlas;
u32                       sample_count = 0;
bool                      inspect_pixel = false;
Int2                      inspect_pos;

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
		return float4(pow(rgb, 1.0 / 2.0), 1);
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
			"-Zi", "Od",
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

D3D12_ROOT_SIGNATURE_FLAGS gfx_root_signature_flags(bool vs, bool ps, bool hs, bool ds, bool gs) {
	D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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

Texture create_texture(u32 width, u32 height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
	Texture texture = {};

    D3D12MA::ALLOCATION_DESC alloc_desc = {};
    alloc_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
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

void set_texture_data(const Texture &texture, std::span<u8> pixels) {
    D3D12MA::Allocation               *staging_buffer;
    D3D12_RESOURCE_DESC                texture_desc = texture.alloc->GetResource()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    u64                                row_size, texture_size_in_byte;
    u32                                row_count;
    
	device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &layout, &row_count, &row_size, &texture_size_in_byte);
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

    u8         *p_staging = nullptr;
    D3D12_RANGE range{0, texture_size_in_byte};
    staging_buffer->GetResource()->Map(0, &range, (void **)&p_staging);

    for (u32 i = 0; i < row_count; ++i) {
        std::memcpy(p_staging + layout.Footprint.RowPitch * i, pixels.data() + row_size * i, row_size);
    }
    staging_buffer->GetResource()->Unmap(0, &range);

    D3D12_TEXTURE_COPY_LOCATION dst;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    dst.pResource = texture.alloc->GetResource();

    D3D12_TEXTURE_COPY_LOCATION src;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    src.pResource = staging_buffer->GetResource();

    copy_cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	queue_alloc_for_release(staging_buffer);
};

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


bool init_dx12() {
#if BUILD_DEBUG
	ID3D12Debug *debug_ctrl;
	check_or_false(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl)), S_OK);
	debug_ctrl->EnableDebugLayer();
	debug_ctrl->Release();
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
	sampler_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, true);	
	rtv_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false);	
	dsv_allocator = alloc_descriptor_allocator(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false);
	check_or_false(srv_allocator!= nullptr, true);	
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
		check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_cmd_allocator[i])), S_OK);check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gfx_cmd_allocator[i])), S_OK);
		check_or_false(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&comp_cmd_allocator[i])), S_OK);
	}

	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gfx_cmd_allocator[0], nullptr, IID_PPV_ARGS(&gfx_cmd_list)), S_OK);
	gfx_cmd_list->Close();
	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_cmd_allocator[0], nullptr, IID_PPV_ARGS(&copy_cmd_list)), S_OK);
	copy_cmd_list->Close();
	check_or_false(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, comp_cmd_allocator[0], nullptr, IID_PPV_ARGS(&comp_cmd_list)), S_OK);
	comp_cmd_list->Close();

	ID3DBlob *passthrough_vs = compile_shader(passthrough_shader, "vs_main", "vs_6_6");
	ID3DBlob *passthrough_ps = compile_shader(passthrough_shader, "ps_main", "ps_6_6");
	if (passthrough_vs == nullptr || passthrough_ps == nullptr) return false;

    CD3DX12_ROOT_PARAMETER1 root_param;
    root_param.InitAsConstants(2, 0, 0);
    passthrough_rs = create_root_signature({&root_param, 1}, gfx_root_signature_flags(true, true, false, false, false));
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

u32 pack_rgba(Vec4 col) {
	col = saturate(col);
	u32 r = (u32)(col.x * 255.0f + 0.5f) & 0xFF;
	u32 g = (u32)(col.y * 255.0f + 0.5f) & 0xFF;
	u32 b = (u32)(col.z * 255.0f + 0.5f) & 0xFF;
	u32 a = (u32)(col.w * 255.0f + 0.5f) & 0xFF;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

Vec4 unpack_rgba(u32 rgba) {
	u32 r = rgba & 0xFF;
	u32 g = (rgba >> 8) & 0xFF;
	u32 b = (rgba >> 16) & 0xFF;
	u32 a = (rgba >> 24) & 0xFF;
	return Vec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

Vec4 sample(Image &image, u32 x, u32 y) {
	return unpack_rgba(image.data[y*image.width+x]);
}

Vec4 sample(HDRImage &image, u32 x, u32 y) {
	return Vec4(image.data[y*image.width+x], 1.0f);
}

Vec4 decode_dxt1_block(const u8* block, u32 px, u32 py) {
    u16 c0 = *(u16*)(block + 0);
    u16 c1 = *(u16*)(block + 2);

    auto decode_565 = [](u16 c)->Vec3 {
        f32 r = (f32)((c >> 11) & 31) / 31.0f;
        f32 g = (f32)((c >> 5)  & 63) / 63.0f;
        f32 b = (f32)( c        & 31) / 31.0f;
        return Vec3(r,g,b);
    };

    Vec3 col0 = decode_565(c0);
    Vec3 col1 = decode_565(c1);

    Vec3 c[4];
    c[0] = col0;
    c[1] = col1;

    if (c0 > c1) {
        c[2] = (2.0f/3.0f)*col0 + (1.0f/3.0f)*col1;
        c[3] = (1.0f/3.0f)*col0 + (2.0f/3.0f)*col1;
    } else {
        c[2] = 0.5f*(col0 + col1);
        c[3] = Vec3(0,0,0);
    }

    u32 indices = *(u32*)(block + 4);
    u32 shift = 2 * (py*4 + px);
    u32 idx = (indices >> shift) & 3;

    Vec3 rgb = c[idx];
    f32 a = (idx == 3 && c0 <= c1) ? 0.0f : 1.0f;

    return Vec4(rgb.x, rgb.y, rgb.z, a);
}

Vec4 decode_dxt5_block(const u8* block, u32 px, u32 py) {
    u8 a0 = block[0];
    u8 a1 = block[1];

    const u8* alpha_idx = block + 2;
    u64 ai = (u64)alpha_idx[0]
           | ((u64)alpha_idx[1] << 8)
           | ((u64)alpha_idx[2] << 16)
           | ((u64)alpha_idx[3] << 24)
           | ((u64)alpha_idx[4] << 32)
           | ((u64)alpha_idx[5] << 40);

    u32 id = (px + py*4) * 3;
    u8 alpha_code = (ai >> id) & 7;

    f32 alpha_vals[8];
    alpha_vals[0] = a0 / 255.0f;
    alpha_vals[1] = a1 / 255.0f;

    if (a0 > a1) {
        alpha_vals[2] = (6*a0 + 1*a1)/7.0f / 255.0f;
        alpha_vals[3] = (5*a0 + 2*a1)/7.0f / 255.0f;
        alpha_vals[4] = (4*a0 + 3*a1)/7.0f / 255.0f;
        alpha_vals[5] = (3*a0 + 4*a1)/7.0f / 255.0f;
        alpha_vals[6] = (2*a0 + 5*a1)/7.0f / 255.0f;
        alpha_vals[7] = (1*a0 + 6*a1)/7.0f / 255.0f;
    } else {
        alpha_vals[2] = (4*a0 + 1*a1)/5.0f / 255.0f;
        alpha_vals[3] = (3*a0 + 2*a1)/5.0f / 255.0f;
        alpha_vals[4] = (2*a0 + 3*a1)/5.0f / 255.0f;
        alpha_vals[5] = (1*a0 + 4*a1)/5.0f / 255.0f;
        alpha_vals[6] = 0.0f;
        alpha_vals[7] = 1.0f;
    }

    f32 a = alpha_vals[alpha_code];
    Vec4 col = decode_dxt1_block(block + 8, px, py);
	col.w = alpha_vals[alpha_code];
    return col;
}

f32 interpolate_bc5_channel(const u8* block, u32 px, u32 py) {
    u8 c0 = block[0];
    u8 c1 = block[1];
    const u8* idx = block + 2;

    u64 table =
        ((u64)idx[0]) |
        ((u64)idx[1] << 8) |
        ((u64)idx[2] << 16) |
        ((u64)idx[3] << 24) |
        ((u64)idx[4] << 32) |
        ((u64)idx[5] << 40);

    u32 shift = (px + py*4) * 3;
    u32 code = (table >> shift) & 7;

    f32 v[8];
    v[0] = c0 / 255.0f;
    v[1] = c1 / 255.0f;

    if (c0 > c1) {
        v[2] = (6*c0 + 1*c1)/7.0f / 255.0f;
        v[3] = (5*c0 + 2*c1)/7.0f / 255.0f;
        v[4] = (4*c0 + 3*c1)/7.0f / 255.0f;
        v[5] = (3*c0 + 4*c1)/7.0f / 255.0f;
        v[6] = (2*c0 + 5*c1)/7.0f / 255.0f;
        v[7] = (1*c0 + 6*c1)/7.0f / 255.0f;
    } else {
        v[2] = (4*c0 + 1*c1)/5.0f / 255.0f;
        v[3] = (3*c0 + 2*c1)/5.0f / 255.0f;
        v[4] = (2*c0 + 3*c1)/5.0f / 255.0f;
        v[5] = (1*c0 + 4*c1)/5.0f / 255.0f;
        v[6] = 0.0f;
        v[7] = 1.0f;
    }

    return v[code];
}

Vec4 sample(const DDSImage &image, Vec2 uv) {
    const DDSHeader &header = image.header;
    const u32 w = header.width;
    const u32 h = header.height;

    uv.x = uv.x - floorf(uv.x);
    uv.y = uv.y - floorf(uv.y);

    u32 px = (u32)(uv.x * (f32)w);
    u32 py = (u32)(uv.y * (f32)h);

    const u8 *data = image.data;

    if (header.ddspf.four_cc == 0)
    {
        u32 bpp = header.ddspf.rgb_bit_count / 8;
        if (bpp == 3) {
            const u8 *p = data + (py * w + px) * 3;
            return Vec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f);
        } else if (bpp == 4) {
            const u8 *p = data + (py * w + px) * 4;
            return Vec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
        } else {
            return Vec4(0,0,0,0);
        }
    } else if (header.ddspf.four_cc == '1TXD') {
        u32 bx = px / 4;
        u32 by = py / 4;

        const u8 *block = data + (by * (w/4) + bx) * 8;
        return decode_dxt1_block(block, px % 4, py % 4);
    } else if (header.ddspf.four_cc == '5TXD') {
        u32 bx = px / 4;
        u32 by = py / 4;

        const u8 *block = data + (by * (w/4) + bx) * 16;
        return decode_dxt5_block(block, px % 4, py % 4);
    } else if (header.ddspf.four_cc == '2ITA') {
        u32 bx = px / 4;
        u32 by = py / 4;
        const u8 *block = data + (by * (w/4) + bx) * 16;
		f32 r = interpolate_bc5_channel(block + 0, px % 4, py % 4);
        f32 g = interpolate_bc5_channel(block + 8, px % 4, py % 4);
        return Vec4(r, g, 0.0f, 1.0f);
    }

    return Vec4(0,0,0,0);
}

Vec3 unpack_normal_map(const Vec4 &sample) {
	Vec2 n = Vec2(sample.f) * 2.0f - 1.0f;
	f32 z = sqrt(saturate(1.0f - dot(n,n)));
	return Vec3(n.x, n.y, z);
}

void put_pixel(Image &image, u32 x, u32 y, Vec3 col) {
	image.data[y*image.width+x] = pack_rgba(Vec4(saturate(col), 1.0f));
}

void put_pixel(HDRImage &image, u32 x, u32 y, Vec3 col) {
	image.data[y*image.width+x] = col;
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

Vec2 sample_uniform_disk(Vec2 u) {
	u = 2 * u - Vec2(1, 1);
	if (u.x == 0.0f && u.y == 0.0f) return Vec2(0.0f, 0.0f);

	f32 theta, r;
	if (std::abs(u.x) > std::abs(u.y)) {
		r = u.x;
		theta = pi32_over_2 * 0.5f * (u.y / u.x);
	} else {
		r = u.y;
		theta = pi32_over_2 - pi32_over_2 * 0.5f * (u.x / u.y);
	}
	return r * Vec2(cos(theta), sin(theta));
}

Vec3 sample_cosine_hemisphere(Vec2 u) {
	Vec2 d = sample_uniform_disk(u);
	f32 z = 1 - d.x * d.x - d.y * d.y;
	z = sqrtf(std::max(0.0f, z));
	return Vec3(d.x, d.y, z);
}

f32 luminance(Vec3 rgb) {
	return dot(rgb, Vec3(0.2126f, 0.7152f, 0.0722f));
}

Vec3 reflect(const Vec3 &v, const Vec3 &n) {
	return v - 2.0f * dot(v, n) * n;
}

GeometryData load_geometry_data(const Intersection &hit, const Ray3 ray) {
	GeometryData data;

    const BLASInstance &instance = blas_instances[hit.inst];
    const SceneNode    &node = *(SceneNode *)instance.pnode;
    const Mesh         &mesh = meshes[instance.blasIdx];
	const Material     &material = materials[mesh.material];
    const Mat4x4       &transform = node.global_transform;
    const Mat4x4       &inv_transpose = node.global_inv_trans;
    const Vertex       &v0 = mesh.vertices[mesh.indices[hit.prim * 3]];
    const Vertex       &v1 = mesh.vertices[mesh.indices[hit.prim * 3 + 1]];
    const Vertex       &v2 = mesh.vertices[mesh.indices[hit.prim * 3 + 2]];
    f32                 w = 1.0f - (hit.u + hit.v);

    data.position = (v0.position * w + v1.position * hit.u + v2.position * hit.v) * transform;
    data.uv = v0.uv * w + v1.uv * hit.u + v2.uv * hit.v;
	data.geometric_normal = normalize(normalize(cross(v1.position - v0.position, v2.position - v0.position)) * inv_transpose);
    data.normal = normalize(normalize(v0.normal * w + v1.normal * hit.u + v2.normal * hit.v) * inv_transpose);
    data.tangeant = normalize(normalize(v0.tangeant * w + v1.tangeant * hit.u + v2.tangeant * hit.v) * inv_transpose);
    data.bitangeant = normalize(normalize(v0.bitangeant * w + v1.bitangeant * hit.u + v2.bitangeant * hit.v) * inv_transpose);
    if (dot(ray.direction, data.geometric_normal) > 0.0f) {
        data.geometric_normal *= -1;
		data.normal *= -1;
    }

	data.shading_normal = data.normal;
	if (material.normal != -1) {
		Vec3 norm = unpack_normal_map(sample(textures[material.normal], data.uv));
		data.shading_normal = normalize(data.tangeant * norm.x + data.bitangeant * norm.y + data.normal * norm.z);	
	}

	return data;
}

MaterialData load_material_data(const Intersection &hit, const GeometryData &geo) {
	MaterialData data;

    const BLASInstance &instance = blas_instances[hit.inst];
    const Mesh         &mesh = meshes[instance.blasIdx];
	const Material     &material = materials[mesh.material];
	
	data.base_color = Vec4(material.base_color, 1.0f);
	if (material.diffuse != -1) {
		data.base_color = data.base_color * sample(textures[material.diffuse], geo.uv);
	}

	data.emissive_color = Vec3(material.emissive_color);
	if (material.emissive != -1) {
		data.emissive_color = data.emissive_color * sample(textures[material.emissive], geo.uv);
	}

	data.roughness = material.roughness;
	data.metalness = material.metalness;
	if (material.specular != -1) {
		Vec3 orm = sample(textures[material.specular], geo.uv);
		data.roughness *= orm.y;
		data.metalness *= orm.z;
	}

	return data;
}

Vec3 fresnel_schlick(Vec3 f0, f32 h_dot_v) {
	const Vec3 f90 = Vec3(1);
	return f0 + (f90 - f0) * powf(saturate(1 - h_dot_v), 5);	
}

BrdfData load_brdf_data(const MaterialData &mat, const Vec3 &n, const Vec3 &l, const Vec3 &v) {
	BrdfData data;

	data.f0 = lerp(Vec3(0.04f), Vec3(mat.base_color), mat.metalness);
	data.diffuse = Vec3(mat.base_color) * (1.0f - mat.metalness);
	data.v = v;
	data.n = n;
	data.h = normalize(l + v);
	data.l = l;
	data.n_dot_l = std::min(std::max(0.0001f, dot(n, l)), 1.0f);
	data.n_dot_v = std::min(std::max(0.0001f, dot(n, v)), 1.0f);
	data.h_dot_l = saturate(dot(l, data.h));
	data.h_dot_n = saturate(dot(n, data.h));
	data.h_dot_v = saturate(dot(v, data.h));
	data.roughness = mat.roughness;
	data.alpha = mat.roughness * mat.roughness;
	data.alpha_squared = data.alpha * data.alpha;
	data.F = fresnel_schlick(data.f0, data.h_dot_v);

	return data;
}

f32 specular_probability(const MaterialData &mat, const Vec3 &n, const Vec3 &v) {
	f32 f0 = luminance(lerp(Vec3(0.04f), Vec3(mat.base_color), mat.metalness));
	f32 diffuse = luminance(Vec3(mat.base_color) * (1.0f - mat.metalness));
	f32 f = saturate(luminance(fresnel_schlick(Vec3(f0), std::max(0.0f, dot(v, n)))));

	f32 s = f;
	f32 d = diffuse * (1.0f - f);
	return std::max(0.1f, std::min(0.9f, s / std::max(0.0001f, s + d)));
}

// Samples a microfacet normal for the GGX distribution using VNDF method.
// Source: "Sampling the GGX Distribution of Visible Normals" by Heitz
// See also https://hal.inria.0fr/hal-00996995v1/document and http://jcgt.org/published/0007/04/01/
// Random variables 'u' must be in <0;1) interval
// PDF is 'G1(NdotV) * D'
Vec3 sample_ggx_vndf(const Vec3 &Ve, Vec2 alpha2, Vec2 rng) {
	// Section 3.2: transforming the view direction to the hemisphere configuration
	Vec3 Vh = normalize(Vec3(alpha2.x * Ve.x, alpha2.y * Ve.y, Ve.z));

	// Section 4.1: orthonormal basis (with special case if cross product is zero)
	f32  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	Vec3 T1 = lensq > 0.0f ? Vec3(-Vh.y, Vh.x, 0.0f) * (1.0f / sqrt(lensq)) : Vec3(1.0f, 0.0f, 0.0f);
	Vec3 T2 = cross(Vh, T1);

	// Section 4.2: parameterization of the projected area
	f32 r = sqrt(rng.x);
	f32 phi = 2 * pi32 * rng.y;
	f32 t1 = r * cos(phi);
	f32 t2 = r * sin(phi);
	f32 s = 0.5f * (1.0f + Vh.z);
	t2 = lerp(sqrt(1.0f - t1 * t1), t2, s);

	// Section 4.3: reprojection onto hemisphere
	Vec3 Nh = t1 * T1 + t2 * T2 + sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

	// Section 3.4: transforming the normal back to the ellipsoid configuration
	return normalize(Vec3(alpha2.x * Nh.x, alpha2.y * Nh.y, std::max(0.0f, Nh.z)));
}

f32 smith_g1_ggx(f32 alpha_squared, f32 n_dot_s_squared) {
	return 2.0f / (sqrt(((alpha_squared * (1.0f - n_dot_s_squared)) + n_dot_s_squared) / n_dot_s_squared) + 1.0f);
}

f32 sample_weight_ggx_vndf(f32 alpha_squared, f32 n_dot_l, f32 n_dot_v){
	f32 g1v = smith_g1_ggx(alpha_squared, n_dot_v * n_dot_v);
	f32 g1l = smith_g1_ggx(alpha_squared, n_dot_l * n_dot_l);
	return g1l / (g1v + g1l - g1v * g1l);
}

bool eval_indirect_brdf(Vec2 rng, GeometryData &geo, MaterialData &mat, const Vec3 &v, bool specular, Vec3 *out_ray_direction, Vec3 *out_sample_weight) {
	Mat3x3 to_local, to_world;
	to_world.z = geo.shading_normal;
	orthonormal_basis(to_world.z, &to_world.x, &to_world.y);
	to_local = to_world.transpose();
	Vec3 v_local = normalize(v * to_local);

	Vec3 n_local = Vec3(0, 0, 1);
	Vec3 ray_direction_local = Vec3(0);
	Vec3 sample_weight = Vec3(0);

	if (specular) {
		BrdfData data = load_brdf_data(mat, n_local, Vec3(0,0,1), v_local);
		Vec3     h_local = Vec3(0, 0, 1);
		
		if (data.alpha != 0.0f) {
			h_local = sample_ggx_vndf(v_local, Vec2(data.alpha), rng);
		}
		ray_direction_local = normalize(reflect(-v_local, h_local));

		f32  h_dot_l = std::max(0.00001f, std::min(1.0f, dot(h_local, ray_direction_local)));
		f32  n_dot_l = std::max(0.00001f, std::min(1.0f, dot(n_local, ray_direction_local)));
		f32  n_dot_v = std::max(0.00001f, std::min(1.0f, dot(n_local, v_local)));
		Vec3 f = fresnel_schlick(data.f0, h_dot_l);
		sample_weight = f * sample_weight_ggx_vndf(data.alpha_squared, n_dot_l, n_dot_v);
    } else {
        ray_direction_local = sample_cosine_hemisphere(rng);
        BrdfData data = load_brdf_data(mat, n_local, ray_direction_local, v_local);
        Vec3     h_specular = Vec3(0, 0, 1);

        if (data.alpha != 0.0f) {
            h_specular = sample_ggx_vndf(v_local, Vec2(data.alpha), rng);
        }

        f32  h_dot_v = std::max(0.00001f, std::min(1.0f, dot(v_local, h_specular)));
        Vec3 kd = Vec3(1.0f) - fresnel_schlick(data.f0, h_dot_v);
        sample_weight = data.diffuse * kd;
    }

    if (luminance(sample_weight) == 0.0f) {
		return false;
	}

	//*out_ray_direction = normalize(ray_direction_local * to_world);
	//*out_sample_weight = sample_weight;
	//if (dot(geo.geometric_normal, *out_ray_direction) <= 0.0f) {
	//	return false;
	//}


    Vec3 wi = normalize(ray_direction_local * to_world); // world-space outgoing

    // --- hemisphere fix vs geometric normal ---
    // If wi is a bit behind the geometric normal, gently project it back.
    f32 n_geom_dot = dot(geo.geometric_normal, wi);
    const f32 eps = 1e-4f;
    if (n_geom_dot <= eps) {
        // Project onto plane and push slightly above
        // wi' = normalize(wi - n_geom_dot * ng + eps * ng)
        wi = wi - n_geom_dot * geo.geometric_normal + eps * geo.geometric_normal;
        wi = normalize(wi);
        n_geom_dot = dot(geo.geometric_normal, wi);
        if (n_geom_dot <= 0.0f) {
            // Still completely invalid, give up this sample
            return false;
        }
    }

    *out_ray_direction = wi;
    *out_sample_weight = sample_weight;
    return true;
}

Intersection intersect(const BVH &tlas, const Ray3 &ray) {
	tinybvh::Ray r(bvhvec3(ray.origin.x, ray.origin.y, ray.origin.z), bvhvec3(ray.direction.x, ray.direction.y, ray.direction.z));
	tlas.Intersect(r);
	return r.hit;
}

bool is_occluded(const BVH &tlas, const Ray3 &ray) {
	tinybvh::Ray r(bvhvec3(ray.origin.x, ray.origin.y, ray.origin.z), bvhvec3(ray.direction.x, ray.direction.y, ray.direction.z));
	return tlas.IsOccluded(r);
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
        	check_or_false(fread(&mesh.vertices[vi], sizeof(Vec3) * 4 + sizeof(Vec2), 1, file), 1);
		}
        u32 index_count;
        check_or_false(fread(&index_count, sizeof(index_count), 1, file), 1);
        mesh.indices = {arena->push_array<u32>(index_count), index_count};
        check_or_false(fread(mesh.indices.data(), sizeof(u32), index_count, file), index_count);

		bvhvec4slice layout((bvhvec4*)mesh.vertices.data(), (u32)mesh.vertices.size(), sizeof(Vertex));
		mesh.bvh.BuildHQ(layout, mesh.indices.data(), (u32)mesh.indices.size() / 3);
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

    blas_instances = arena->push_array<BLASInstance>(instance_count);
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
	tlas.Build(blas_instances, instance_count, blasses, (u32)meshes.size());

    fclose(file);

	camera = {};
	camera.position = Vec3(-13.3f, 6.76f, -0.79f);
	camera.pitch = 0.371f;
	camera.yaw = -5.003f;
	camera.fov = pi32 / 2.0f;
 	camera.resolution = Int2((s32)canvas.width, (s32)canvas.height);
	camera.znear = 0.1f;
	camera.zfar = 1000.0f;
	camera.update();
	return true;
}

void draw(HDRImage &canvas, Range1u32 range_y) {
    float     sample_weight = 1.0f / sample_count;
    Vec3      sky_color = Vec3(0.529f, 0.807f, 0.921f) * 2.0f;
    const u32 max_bounce = 2;

    for (u32 py = range_y.min; py < range_y.max; py += 1) {
        for (u32 px = 0; px < canvas.width; px += 1) {
			if (inspect_pixel && inspect_pos.x == px && inspect_pos.y == py) {
				inspect_pixel = false;
				trap();
			}

			f32 ox = random_f32_in_range(-0.5f, 0.5f);
			f32 oy = random_f32_in_range(-0.5f, 0.5f);
			Vec3 throughput = Vec3(1.0f);
			Vec3 radiance = Vec3(0.0f);
            Ray3 ray = camera.generate_ray(px + 0.5f + ox, py + 0.5f + oy);
			for (s32 bounce = 0; bounce <= max_bounce; bounce += 1) {
				Intersection hit = intersect(tlas, ray);
				if (hit.t >= camera.zfar) { // ray missed, output sky color
					radiance += sky_color * throughput;
					break;
				}

				GeometryData geo = load_geometry_data(hit, ray);
				MaterialData mat = load_material_data(hit, geo);
				if (mat.base_color.w < 0.5f) { // alpha cutout
					ray.origin = geo.position + ray.direction * 0.001f;
					bounce -= 1;
					continue;
				}


				radiance += mat.emissive_color * throughput;

				Vec3 v = -ray.direction;
				bool eval_specular;
				f32  spec_p = specular_probability(mat, geo.shading_normal, v);
				if (random_f32() < spec_p) {
					eval_specular = true;
					throughput = throughput / spec_p;
				} else {
					eval_specular = false;
					throughput = throughput / (1.f - spec_p);
				}

				Vec2 rng = Vec2(random_f32(), random_f32());
				Vec3 brdf_weight;
				if (eval_indirect_brdf(rng, geo, mat, -ray.direction, eval_specular, &ray.direction, &brdf_weight) == false) {
					break;
				}

				ray.origin = geo.position + geo.geometric_normal * 0.00001f;
				throughput = brdf_weight * throughput;
            }

			Vec3 canvas_col = sample(canvas, px, py);
			put_pixel(canvas, px, py, lerp(canvas_col, radiance, sample_weight));
		}
    }
}

void worker_thread(u32 thread_idx) {
	init_rd();

	for (;;) {
		if (has_work[thread_idx]) {
			draw(canvas, work_items[thread_idx].range_y);
			has_work[thread_idx] = false;
			work_done_count++;
			std::unique_lock lock(work_done_mutex);
			work_done_cv.notify_all();
		}

		std::unique_lock lock(cv_mutex);
		cv.wait(lock, [thread_idx] { return has_work[thread_idx]; });
	}
}

void tick() {
	sample_count += 1;
	work_done_count = 0;

	u32 work_size = canvas.height;
	u32 work_item_size = work_size / (worker_count + 1);
	for (u32 i = 0; i < worker_count; i += 1) {
		work_items[i].range_y.min = work_item_size * i;
		work_items[i].range_y.max = work_item_size * i + work_item_size;
		has_work[i] = true;
	}

	{
		std::unique_lock lock(cv_mutex);
		cv.notify_all();
	}

	Range1u32 my_range;
	my_range.min = work_item_size * worker_count;
	my_range.max = canvas.height;
	draw(canvas, my_range);

	std::unique_lock lock(work_done_mutex);
	work_done_cv.wait(lock, [] { return work_done_count == worker_count; });
}

int entry_point() {
    arena = alloc_arena(MiB(8), MiB(256));
	scratch = alloc_arena(MiB(8), MiB(64));

	canvas.data = arena->push_array<Vec3>(3840 * 2160);
	std::memset(canvas.data, 0, 3840*2160*sizeof(canvas.data[0]));
	canvas.width = 300;
	canvas.height = 300;
    window = os_create_window("raytracing", canvas.width, canvas.height);

	init_rd();
	worker_count = std::thread::hardware_concurrency() * 3;
	worker_threads = arena->push_array<std::thread>(worker_count);
	work_items = arena->push_array<WorkItem>(worker_count);
	has_work = arena->push_array<bool>(worker_count);
	for (u32 i = 0; i < worker_count; i += 1) {
		new (&worker_threads[i]) std::thread(worker_thread, i);
	}

    if (init_dx12() == false) {
        return -1;
    }
	Texture canvas_texture = create_texture(canvas.width, canvas.height, DXGI_FORMAT_R32G32B32_FLOAT);

	if (init_scene() == false) {
		return -1;
	}

	bool down[Key_Count] = {};
	bool pressed[Key_Count] = {};
	Int2 mouse = {};
    bool quit = false;
    bool resize = false;
	auto start = std::chrono::steady_clock::now();
	auto before = std::chrono::steady_clock::now();
    for (; quit == false; frame += 1) {
        wait_for_fence(present_fence, frame);

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
				canvas.width = (u32)evt->size.x;
				canvas.height = (u32)evt->size.y;
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
			canvas_texture = create_texture(canvas.width, canvas.height, DXGI_FORMAT_R32G32B32_FLOAT);
			camera.resolution = Int2((s32)canvas.width, (s32)canvas.height);
			camera.update();
			std::memset(canvas.data, 0, canvas.width * canvas.height * sizeof(canvas.data[0]));
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

        if (down[Key_MouseButtonRight]) {
			os_show_cursor(false);
			os_freeze_cursor(true);
            const f32 spd = 5.0f;
			bool moved = false;
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
				std::memset(canvas.data, 0, canvas.width * canvas.height);
			}
        } else {
			os_show_cursor(true);
			os_freeze_cursor(false);
		}
		
		tick();
		
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

        u32 backbuffer_idx = swapchain->GetCurrentBackBufferIndex();
		copy_cmd_allocator[backbuffer_idx]->Reset();
		copy_cmd_list->Reset(copy_cmd_allocator[backbuffer_idx], nullptr);
		set_texture_data(canvas_texture, {(u8*)canvas.data, canvas.width * canvas.height * sizeof(canvas.data[0])});
		copy_cmd_list->Close();
		{
 			ID3D12CommandList *cmd_lists[]{copy_cmd_list};
			copy_queue->ExecuteCommandLists(1, cmd_lists);
		}
		copy_queue->Signal(fence, ++fence_value);
		gfx_queue->Wait(fence, fence_value);

        auto            rtv_cpu_handle = rtv_allocator->ring_alloc().cpu_handle();
        ID3D12Resource *backbuffer;
        swapchain->GetBuffer(backbuffer_idx, IID_PPV_ARGS(&backbuffer));
        device->CreateRenderTargetView(backbuffer, nullptr, rtv_cpu_handle);

        CD3DX12_RESOURCE_BARRIER barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        CD3DX12_RESOURCE_BARRIER barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        f32                      clear_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        gfx_cmd_allocator[backbuffer_idx]->Reset();
        gfx_cmd_list->Reset(gfx_cmd_allocator[backbuffer_idx], nullptr);
		ID3D12DescriptorHeap *heaps[2] = { srv_allocator->heap, sampler_allocator->heap };
		gfx_cmd_list->SetDescriptorHeaps(2, heaps);
        gfx_cmd_list->ResourceBarrier(1, &barrier1);
		gfx_cmd_list->SetPipelineState(passthrough_pso);
		gfx_cmd_list->SetGraphicsRootSignature(passthrough_rs);
        gfx_cmd_list->OMSetRenderTargets(1, &rtv_cpu_handle, FALSE, nullptr);
        gfx_cmd_list->ClearRenderTargetView(rtv_cpu_handle, clear_color, 0, nullptr);
		gfx_cmd_list->SetGraphicsRoot32BitConstant(0, canvas_texture.srv->heap_idx, 0);
		gfx_cmd_list->SetGraphicsRoot32BitConstant(0, default_sampler->heap_idx, 1);
		gfx_cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		D3D12_VIEWPORT viewport = {};
		viewport.Width = (f32)canvas.width;
		viewport.Height = (f32)canvas.height;
		gfx_cmd_list->RSSetViewports(1, &viewport);
		D3D12_RECT scissor = {};
		scissor.right = canvas.width;
		scissor.bottom = canvas.height;
		gfx_cmd_list->RSSetScissorRects(1, &scissor);
		gfx_cmd_list->DrawInstanced(6, 1, 0, 0);
        gfx_cmd_list->ResourceBarrier(1, &barrier2);
        gfx_cmd_list->Close();

		{
        	ID3D12CommandList *cmd_lists[]{gfx_cmd_list};
        	gfx_queue->ExecuteCommandLists(1, cmd_lists);
		}
		backbuffer->Release();

        swapchain->Present(1, 0);
        gfx_queue->Signal(present_fence, frame + framebuffer_count);
    }

	release_arena(&arena);
	release_arena(&scratch);
    return 0;
}