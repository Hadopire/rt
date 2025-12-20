#include "mini_vg.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <emmintrin.h>

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------ ARENA --------------------------------

vg_internal u64 page_size = 4096;

RAIICheckpoint::RAIICheckpoint(Arena *_arena) {
    arena = _arena;
    checkpoint = arena->checkpoint();
}

RAIICheckpoint::~RAIICheckpoint() {
    if (arena != nullptr) {
        arena->restore(checkpoint);
    }
}

void *Arena::push(u64 size, u64 align) {
    vg_assert(size != 0);

    u64 bot = round_up_pow2(current->pos, align);
    u64 top = bot + size;

    if (top >= reserved_size) {
        Arena *new_arena;

        u64 commit_size = current->commit_size;
        u64 reserved_size = current->reserved_size;
        if (size > reserved_size) {
            commit_size = size;
            reserved_size = size;
        }
        
        new_arena = alloc_arena(commit_size, reserved_size);
        new_arena->base_pos = current->base_pos + current->reserved_size;
        sll_stack_push_n(current, new_arena, prev);
        bot = round_up_pow2(current->pos, align);
        top = bot + size;
    }

    if (top >= current->commit) {
        current->commit = std::min(current->reserved_size, top + current->commit_size - 1 - (top + current->commit_size - 1) % current->commit_size);
        current->commit = round_up_pow2(current->commit, page_size); 
        vg_assert(os_commit(current, current->commit));
    }

    current->pos = top;
    return (u8*)current + bot;
}

void Arena::pop_to(u64 pos) {
    pos = std::max(pos, sizeof(Arena));
    for (Arena *prev = nullptr; current->base_pos >= pos; current = prev) {
        prev = current->prev;
        os_free(current);
    }

    u64 new_pos = pos - current->base_pos;
    vg_assert(new_pos <= current->pos);
    current->pos = new_pos;
}

void Arena::reset() {
    pop_to(0);
}

Checkpoint Arena::checkpoint() {
    return {current->pos + current->base_pos};
}

void Arena::restore(Checkpoint checkpoint) {
    pop_to(checkpoint.pos);
}

Arena *alloc_arena(u64 commit, u64 reserve) {
    vg_assert(reserve >= commit);

    u64 commit_size = round_up_pow2(commit + sizeof(Arena), page_size);
    u64 reserve_size = round_up_pow2(reserve + sizeof(Arena), page_size);

    Arena *arena = (Arena*)os_reserve(reserve_size);
    if (arena == nullptr) return nullptr;

    arena = (Arena*)os_commit(arena, commit_size);
    if (arena == nullptr) {
        os_free(arena);
        return nullptr;
    }

    arena->base_pos = 0;
    arena->prev = nullptr;
    arena->current = arena;
    arena->pos = sizeof(Arena);
    arena->commit = arena->commit_size = commit_size;
    arena->reserved_size = reserve_size;

    return arena;
}

void release_arena(Arena **arena) {
    for (Arena *curr = (*arena)->current, *prev = nullptr; curr != nullptr; curr = prev) {
        prev = curr->prev;
        os_free(curr);
    }

    *arena = nullptr;
}

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------- STRINGS -----------------------------

vg_internal u32 utf8_next_codepoint(std::string_view &str) {
    u32 len, codepoint;
    if ((str[0] & 0x80) == 0) {
        len = 1;
        codepoint = str[0];
    } else if ((str[0] & 0xE0) == 0xC0) {
        len = 2;
        codepoint = str[0] & 0x1F;
    } else if ((str[0] & 0xF0) == 0xE0) {
        len = 3;
        codepoint = str[0] & 0x0F;
    } else if ((str[0] & 0xF8) == 0xF0) {
        len = 4;
        codepoint = str[0] & 0x07;
    }

    for (u32 i = 1; i < len; ++i) {
        codepoint = (codepoint << 6) | (str[i] & 0x3F);
    }

    str = std::string_view(str.data() + len, str.size() - len);
    return codepoint;
}

vg_internal u32 utf16_next_codepoint(std::wstring_view &wstr) {
    u32 len, codepoint;
    if (wstr[0] <= 0xFFFF) {
        len = 1;
        codepoint = wstr[0];
    } else {
        len = 2;
        codepoint = ((wstr[0] << 10) | (wstr[1] & 0x3FF)) + 0x10000;
    }

    wstr = std::wstring_view(wstr.data() + len, wstr.size() - len);
    return codepoint;
}

std::string_view to_utf8(std::wstring_view wstr, Arena *arena) {
    char *str = arena->push_array<char>(wstr.length() * 2 + 1);
    u32   length = 0;

    while (wstr.size()) {
        u32 codepoint = utf16_next_codepoint(wstr);
        if (codepoint < 0x80) {
            str[length++] = (char)codepoint;
        } else if (codepoint < 0x800) {
            str[length++] = (char)(((codepoint >> 6) & 0x1F) | 0xC0);
            str[length++] = (char)((codepoint & 0x3F) | 0x80);
        } else if (codepoint < 0x100000) {
            str[length++] = (char)(((codepoint >> 12) & 0x0F) | 0xE0);
            str[length++] = (char)(((codepoint >> 6) & 0x3F) | 0x80);
            str[length++] = (char)((codepoint & 0x3F) | 0x80);
        } else {
            str[length++] = (char)(((codepoint >> 18) & 0x07) | 0xF0);
            str[length++] = (char)(((codepoint >> 12) & 0x3F) | 0x80);
            str[length++] = (char)(((codepoint >> 6) & 0x3F) | 0x80);
            str[length++] = (char)((codepoint & 0x3F) | 0x80);
        }
    }

    str[length] = 0;
    return std::string_view(str, length);
}

std::wstring_view to_utf16(std::string_view str, Arena *arena) {
    wchar_t *wstr = arena->push_array<wchar_t>(str.length() + 1);
    u32      length = 0;

    while (str.size()) {
        u32 codepoint = utf8_next_codepoint(str);
        if (codepoint <= 0xffff) {
            wstr[length++] = (wchar_t)codepoint;
        } else {
            codepoint = codepoint - 0x10000;
            wstr[length++] = 0xD800 + (codepoint >> 10);
            wstr[length++] = 0xDC00 + (codepoint & 0x3FF);
        }
    }

    wstr[length] = 0;
    return std::wstring_view(wstr, length);
}

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------ MATH ---------------------------------

// ------------------- 2 component int vectors ------------------

Int2::Int2(s32 xy) {
    x = y = xy;
}
    
Int2::Int2(s32 _x, s32 _y) {
    x = _x, y = _y;
}

Int2::Int2(const s32 *p_array) {
    x = p_array[0], y = p_array[1];
}

s32 &Int2::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 1);
    return ((s32 *)this)[idx];
}

s32 Int2::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 1);
    return ((s32 *)this)[idx];
}

Int2 &Int2::operator+=(const Int2 &rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
}

Int2 &Int2::operator-=(const Int2 &rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
}

Int2 &Int2::operator+=(s32 rhs) {
    x += rhs;
    y += rhs;
    return *this;
}

Int2 &Int2::operator-=(s32 rhs) {
    x -= rhs;
    y -= rhs;
    return *this;
}

Int2 &Int2::operator*=(s32 rhs) {
    x *= rhs;
    y *= rhs;
    return *this;
}

Int2 &Int2::operator/=(s32 rhs) {
    vg_assert(rhs != 0.0f);
    x /= rhs;
    y /= rhs;
    return *this;
}

bool operator==(const Int2 &lhs, const Int2 &rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

bool operator!=(const Int2 &lhs, const Int2 &rhs) {
    return !(lhs == rhs);
}

Int2 operator+(const Int2 &lhs, const Int2 &rhs) {
    Int2 result = lhs;
    return result += rhs;
}

Int2 operator-(const Int2 &lhs, const Int2 &rhs) {
    Int2 result = lhs;
    return result -= rhs;
}

Int2 operator-(const Int2 &rhs) {
    return Int2(-rhs.x, -rhs.y);
}

Int2 operator+(const s32 &lhs, const Int2 &rhs) {
    Int2 result = rhs;
    return result += lhs;
}

Int2 operator+(const Int2 &lhs, const s32 &rhs) {
    Int2 result = lhs;
    return result += rhs;
}

Int2 operator-(const s32 &lhs, const Int2 &rhs) {
    Int2 result = rhs;
    return result += lhs;
}

Int2 operator-(const Int2 &lhs, const s32 &rhs) {
    Int2 result = lhs;
    return result -= rhs;
}

Int2 operator*(const s32 &lhs, const Int2 &rhs) {
    Int2 result = rhs;
    return result *= lhs;
}

Int2 operator*(const Int2 &lhs, const s32 &rhs) {
    Int2 result = lhs;
    return result *= rhs;
}

Int2 operator/(const Int2 &lhs, const s32 &rhs) {
    Int2 result = lhs;
    return result /= rhs;
}

Int2 abs(Int2 a) {
    return Int2(abs(a.x), abs(a.y));
}

// ------------------- 2 component vectors ------------------

Vec2::Vec2(s32 xy) {
    x = y = (f32)xy;
}

Vec2::Vec2(f32 xy) {
    x = y = xy;
}

Vec2::Vec2(f32 _x, f32 _y) {
    x = _x, y = _y;
}

Vec2::Vec2(const f32 *p_array) {
    x = p_array[0], y = p_array[1];
}

Vec2::Vec2(const Int2 &vec) {
    x = (f32)vec.x, y = (f32)vec.y;
}

f32 &Vec2::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 1);
    return ((f32 *)this)[idx];
}
f32 Vec2::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 1);
    return ((f32 *)this)[idx];
}
Vec2 &Vec2::operator+=(const Vec2 &rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
}

Vec2 &Vec2::operator-=(const Vec2 &rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
}

Vec2 &Vec2::operator+=(f32 rhs) {
    x += rhs;
    y += rhs;
    return *this;
}

Vec2 &Vec2::operator-=(f32 rhs) {
    x -= rhs;
    y -= rhs;
    return *this;
}

Vec2 &Vec2::operator*=(f32 rhs) {
    x *= rhs;
    y *= rhs;
    return *this;
}

Vec2 &Vec2::operator/=(f32 rhs) {
    vg_assert(rhs != 0.0f);
    x /= rhs;
    y /= rhs;
    return *this;
}

f32 Vec2::length() const {
    return sqrtf(x * x + y * y);
}

f32 Vec2::sq_length() const {
    return x * x + y * y;
}

bool operator==(const Vec2 &lhs, const Vec2 &rhs) {
    return f32_cmp(lhs.x, rhs.x) && f32_cmp(lhs.y, rhs.y);
}

bool operator!=(const Vec2 &lhs, const Vec2 &rhs) {
    return !(lhs == rhs);
}

Vec2 operator+(const Vec2 &lhs, const Vec2 &rhs) {
    Vec2 result = lhs;
    return result += rhs;
}

Vec2 operator-(const Vec2 &lhs, const Vec2 &rhs) {
    Vec2 result = lhs;
    return result -= rhs;
}

Vec2 operator-(const Vec2 &rhs) {
    return Vec2(-rhs.x, -rhs.y);
}

Vec2 operator+(const f32 &lhs, const Vec2 &rhs) {
    Vec2 result = rhs;
    return result += lhs;
}

Vec2 operator+(const Vec2 &lhs, const f32 &rhs) {
    Vec2 result = lhs;
    return result += rhs;
}

Vec2 operator-(const f32 &lhs, const Vec2 &rhs) {
    return Vec2(lhs - rhs.x, lhs - rhs.y);
}

Vec2 operator-(const Vec2 &lhs, const f32 &rhs) {
    Vec2 result = lhs;
    return result -= rhs;
}

Vec2 operator*(const f32 &lhs, const Vec2 &rhs) {
    Vec2 result = rhs;
    return result *= lhs;
}

Vec2 operator*(const Vec2 &lhs, const f32 &rhs) {
    Vec2 result = lhs;
    return result *= rhs;
}

Vec2 operator*(const Vec2 &lhs, const Vec2 &rhs) {
    return Vec2(lhs.x * rhs.x, lhs.y * rhs.y);
}

Vec2 operator/(const Vec2 &lhs, const f32 &rhs) {
    Vec2 result = lhs;
    return result /= rhs;
}

f32 dot(const Vec2 &v1, const Vec2 &v2) {
    return v1.x * v2.x + v1.y * v2.y;
}

Vec2 normalize(const Vec2 &v) {
    f32 len = v.length();
    vg_assert(len > 0.0f);
    f32 inv_len = 1.0f / len;
    return Vec2(v.x * inv_len, v.y * inv_len);
}

Vec2 abs(Vec2 a) {
    return Vec2(fabsf(a.x), fabsf(a.y));
}

// ------------------- 3 component int vectors ------------------

Int3::Int3(s32 xyz) {
    x = y = z = xyz;
}

Int3::Int3(s32 _x, s32 _y, s32 _z) {
    x = _x, y = _y, z = _z;
}

Int3::Int3(const s32 *p_array) {
    x = p_array[0];
    y = p_array[1];
    z = p_array[2];
}

s32  &Int3::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 2);
    return ((s32 *)this)[idx];
}

s32   Int3::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 2);
    return ((s32 *)this)[idx];
}

Int3 &Int3::operator+=(const Int3 &rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
}

Int3 &Int3::operator-=(const Int3 &rhs) {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    return *this;
}

Int3 &Int3::operator+=(s32 rhs) {
    x += rhs;
    y += rhs;
    z += rhs;
    return *this;
}

Int3 &Int3::operator-=(s32 rhs) {
    x -= rhs;
    y -= rhs;
    z -= rhs;
    return *this;
}

Int3 &Int3::operator*=(s32 rhs) {
    x *= rhs;
    y *= rhs;
    z *= rhs;
    return *this;
}

Int3 &Int3::operator/=(s32 rhs) {
    vg_assert(rhs != 0.0f);
    x /= rhs;
    y /= rhs;
    z /= rhs;
    return *this;
}

bool operator==(const Int3 &lhs, const Int3 &rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool operator!=(const Int3 &lhs, const Int3 &rhs) {
    return !(lhs == rhs);
}

Int3 operator+(const Int3 &lhs, const Int3 &rhs) {
    Int3 result = lhs;
    return result += rhs;
}

Int3 operator-(const Int3 &lhs, const Int3 &rhs) {
    Int3 result = lhs;
    return result -= rhs;
}

Int3 operator-(const Int3 &rhs) {
    return Int3(-rhs.x, -rhs.y, -rhs.z);
}

Int3 operator+(const s32 &lhs, const Int3 &rhs) {
    Int3 result = rhs;
    return result += lhs;
}

Int3 operator+(const Int3 &lhs, const s32 &rhs) {
    Int3 result = lhs;
    return result += rhs;
}

Int3 operator-(const s32 &lhs, const Int3 &rhs) {
    return Int3(lhs - rhs.x, lhs - rhs.y, lhs - rhs.z);
}

Int3 operator-(const Int3 &lhs, const s32 &rhs) {
    Int3 result = lhs;
    return result -= rhs;
}

Int3 operator*(const s32 &lhs, const Int3 &rhs) {
    Int3 result = rhs;
    return result *= lhs;
}

Int3 operator*(const Int3 &lhs, const s32 &rhs) {
    Int3 result = lhs;
    return result *= rhs;
}

Int3 operator*(const Int3 &lhs, const Int3 &rhs) {
    return Int3(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z);
}

Int3 operator/(const Int3 &lhs, const s32 &rhs) {
    Int3 result = lhs;
    return result /= rhs;
}

Int3 abs(Int3 a) {
    return Int3(abs(a.x), abs(a.y), abs(a.z));
}

// ------------------- 3 component vectors ------------------

Vec3::Vec3(s32 xyz) {
    x = y = z = (f32)xyz;
}

Vec3::Vec3(f32 xyz) {
    x = y = z = xyz;
}

Vec3::Vec3(f32 _x, f32 _y, f32 _z) {
    x = _x, y = _y, z = _z;
}

Vec3::Vec3(const f32 *p_array) {
    x = p_array[0], y = p_array[1], z = p_array[2];
}

Vec3::Vec3(const Int3 &vec) {
    x = (f32)vec.x, y = (f32)vec.y, z = (f32)vec.z;
}

Vec3::Vec3(const Vec4 &vec) {
    x = vec.x, y = vec.y, z = vec.z;
}

f32 &Vec3::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 2);
    return ((f32 *)this)[idx];
}

f32 Vec3::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 2);
    return ((f32 *)this)[idx];
}

Vec3 &Vec3::operator+=(const Vec3 &rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
}

Vec3 &Vec3::operator-=(const Vec3 &rhs) {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    return *this;
}

Vec3 &Vec3::operator+=(f32 rhs) {
    x += rhs;
    y += rhs;
    z += rhs;
    return *this;
}

Vec3 &Vec3::operator-=(f32 rhs) {
    x -= rhs;
    y -= rhs;
    z -= rhs;
    return *this;
}

Vec3 &Vec3::operator*=(f32 rhs) {
    x *= rhs;
    y *= rhs;
    z *= rhs;
    return *this;
}

Vec3 &Vec3::operator/=(f32 rhs) {
    vg_assert(rhs != 0.0f);
    x /= rhs;
    y /= rhs;
    z /= rhs;
    return *this;
}

f32 Vec3::length() const {
    return std::sqrt(x * x + y * y + z * z);
}

f32 Vec3::sq_length() const {
    return x * x + y * y + z * z;
}

bool operator==(const Vec3 &lhs, const Vec3 &rhs) {
    return f32_cmp(lhs.x, rhs.x) && f32_cmp(lhs.y, rhs.y) && f32_cmp(lhs.z, rhs.z);
}

bool operator!=(const Vec3 &lhs, const Vec3 &rhs) {
    return !(lhs == rhs);
}

Vec3 operator+(const Vec3 &lhs, const Vec3 &rhs) {
    Vec3 result = lhs;
    return result += rhs;
}

Vec3 operator-(const Vec3 &lhs, const Vec3 &rhs) {
    Vec3 result = lhs;
    return result -= rhs;
}

Vec3 operator-(const Vec3 &rhs) {
    return Vec3(-rhs.x, -rhs.y, -rhs.z);
}

Vec3 operator+(const f32 &lhs, const Vec3 &rhs) {
    Vec3 result = rhs;
    return result += lhs;
}

Vec3 operator+(const Vec3 &lhs, const f32 &rhs) {
    Vec3 result = lhs;
    return result += rhs;
}

Vec3 operator-(const f32 &lhs, const Vec3 &rhs) {
    return Vec3(lhs - rhs.x, lhs - rhs.y, lhs - rhs.z);
}

Vec3 operator-(const Vec3 &lhs, const f32 &rhs) {
    Vec3 result = lhs;
    return result -= rhs;
}

Vec3 operator*(const f32 &lhs, const Vec3 &rhs) {
    Vec3 result = rhs;
    return result *= lhs;
}

Vec3 operator*(const Vec3 &lhs, const f32 &rhs) {
    Vec3 result = lhs;
    return result *= rhs;
}

Vec3 operator*(const Vec3 &lhs, const Vec3 &rhs) {
    return Vec3(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z);
}

Vec3 operator/(const Vec3 &lhs, const f32 &rhs) {
    Vec3 result = lhs;
    return result /= rhs;
}

f32 dot(const Vec3 &v1, const Vec3 &v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}

Vec3 cross(const Vec3 &v1, const Vec3 &v2) {
    return Vec3(
        v1.y * v2.z - v1.z * v2.y,
        v1.z * v2.x - v1.x * v2.z,
        v1.x * v2.y - v1.y * v2.x
    );
}

Vec3 normalize(const Vec3 &v) {
    f32 len = v.length();
    vg_assert(len > 0.0f);
    f32 inv_len = 1.0f / len;
    return Vec3(v.x * inv_len, v.y * inv_len, v.z * inv_len);
}

Vec3 abs(Vec3 a) {
    return Vec3(fabsf(a.x), fabsf(a.y), fabsf(a.z));
}

void orthonormal_basis(const Vec3 &v, Vec3 *b0, Vec3 *b1) {
	Vec3 u = Vec3(0, 0, 1);
	if (std::abs(dot(v,u)) > 0.999f) u = Vec3(0, 1, 0);
	*b0 = normalize(cross(u, v));
	*b1 = normalize(cross(*b0, v));
}


// ------------------- 4 component vectors ------------------

Vec4::Vec4(s32 xyzw) {
    x = y = z = w = (f32)xyzw;
}

Vec4::Vec4(f32 xyzw) {
    x = y = z = w = xyzw;
}

Vec4::Vec4(f32 _x, f32 _y, f32 _z, f32 _w) { 
    x = _x; 
    y = _y; 
    z = _z; 
    w = _w; 
}

Vec4::Vec4(const Vec3 &vec, f32 _w) {
    x = vec.x;
    y = vec.y;
    z = vec.z;
    w = _w;
}

Vec4::Vec4(const f32 *p_array) {
    x = p_array[0];
    y = p_array[1];
    z = p_array[2];
    w = p_array[3];
}

f32 &Vec4::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 3);
    return ((f32 *)this)[idx];
}

f32 Vec4::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 3);
    return ((f32 *)this)[idx];
}

Vec4 &Vec4::operator+=(const Vec4 &rhs) {
    x += rhs.x; y += rhs.y; z += rhs.z; w += rhs.w;
    return *this;
}

Vec4 &Vec4::operator-=(const Vec4 &rhs) {
    x -= rhs.x; y -= rhs.y; z -= rhs.z; w -= rhs.w;
    return *this;
}

Vec4 &Vec4::operator+=(f32 rhs) {
    x += rhs;
    y += rhs;
    z += rhs;
    w += rhs;
    return *this;
}

Vec4 &Vec4::operator-=(f32 rhs) {
    x -= rhs;
    y -= rhs;
    z -= rhs;
    w -= rhs;
    return *this;
}

Vec4 &Vec4::operator*=(f32 rhs) {
    x *= rhs;
    y *= rhs;
    z *= rhs;
    w *= rhs;
    return *this;
}

Vec4 &Vec4::operator/=(f32 rhs) {
    vg_assert(rhs != 0.0f);
    x /= rhs;
    y /= rhs;
    z /= rhs;
    w /= rhs;
    return *this;
}

f32 Vec4::length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

f32 Vec4::sq_length() const {
    return x * x + y * y + z * z + w * w;
}

bool operator==(const Vec4 &lhs, const Vec4 &rhs) {
    return f32_cmp(lhs.x, rhs.x) && f32_cmp(lhs.y, rhs.y) && f32_cmp(lhs.z, rhs.z) && f32_cmp(lhs.w, rhs.w);
}

bool operator!=(const Vec4 &lhs, const Vec4 &rhs) {
    return !(lhs == rhs);
}

Vec4 operator+(const Vec4 &lhs, const Vec4 &rhs) {
    Vec4 result = lhs;
    return result += rhs;
}

Vec4 operator-(const Vec4 &lhs, const Vec4 &rhs) {
    Vec4 result = lhs;
    return result -= rhs;
}

Vec4 operator-(const Vec4 &rhs) {
    return Vec4(-rhs.x, -rhs.y, -rhs.z, -rhs.w);
}

Vec4 operator+(const f32 &lhs, const Vec4 &rhs) {
    Vec4 result = rhs;
    return result += lhs;
}

Vec4 operator+(const Vec4 &lhs, const f32 &rhs) {
    Vec4 result = lhs;
    return result += rhs;
}

Vec4 operator-(const f32 &lhs, const Vec4 &rhs) {
    return Vec4(lhs - rhs.x, lhs - rhs.y, lhs - rhs.z, lhs - rhs.w);
}

Vec4 operator-(const Vec4 &lhs, const f32 &rhs) {
    Vec4 result = lhs;
    return result -= rhs;
}

Vec4 operator*(const f32 &lhs, const Vec4 &rhs) {
    Vec4 result = rhs;
    return result *= lhs;
}

Vec4 operator*(const Vec4 &lhs, const f32 &rhs) {
    Vec4 result = lhs;
    return result *= rhs;
}

Vec4 operator*(const Vec4 &lhs, const Vec4 &rhs) {
    return Vec4(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z, lhs.w * rhs.w);
}

Vec4 operator/(const Vec4 &lhs, const f32 &rhs) {
    Vec4 result = lhs;
    return result /= rhs;
}

f32 dot(const Vec4 &v1, const Vec4 &v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z + v1.w * v2.w;
}

Vec4 normalize(const Vec4 &v) {
    f32 len = v.length();
    vg_assert(len > 0.0f);
    f32 inv_len = 1.0f / len;
    return Vec4(v.x * inv_len, v.y * inv_len, v.z * inv_len, v.w * inv_len);
}

Vec4 abs(Vec4 a) {
    return Vec4(std::fabs(a.x), std::fabs(a.y), std::fabs(a.z), std::fabs(a.w));
}

// ---------------- 3 by 3 row major matrix ---------------------

Mat3x3::Mat3x3(f32 m11, f32 m12, f32 m13,
               f32 m21, f32 m22, f32 m23,
               f32 m31, f32 m32, f32 m33) {    
    _11 = m11, _12 = m12, _13 = m13;
    _21 = m21, _22 = m22, _23 = m23;
    _31 = m31, _32 = m32, _33 = m33;
}


Mat3x3::Mat3x3(const Vec3 &vx, const Vec3 &vy, const Vec3 &vz) {
    x = vx;
    y = vy;
    z = vz;
}

Mat3x3::Mat3x3(const f32 *p_array) { 
    std::memcpy(f, p_array, sizeof(*this));
}

Mat3x3::Mat3x3(const Mat4x4 &rhs) {
    _11 = rhs._11, _12 = rhs._12, _13 = rhs._13;
    _21 = rhs._21, _22 = rhs._22, _23 = rhs._23;
    _31 = rhs._31, _32 = rhs._32, _33 = rhs._33;
}

Mat3x3 Mat3x3::identity() { 
    return {1, 0, 0, 0, 1, 0, 0, 0, 1};

}

Vec3 &Mat3x3::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 2);
    return v[idx];
}

const Vec3 &Mat3x3::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 2);
    return v[idx];

}

Mat3x3 &Mat3x3::operator*=(const Mat3x3 &rhs) {
    Mat3x3 r;

    r._11 = _11 * rhs._11 + _12 * rhs._21 + _13 * rhs._31;
    r._12 = _11 * rhs._12 + _12 * rhs._22 + _13 * rhs._32;
    r._13 = _11 * rhs._13 + _12 * rhs._23 + _13 * rhs._33;

    r._21 = _21 * rhs._11 + _22 * rhs._21 + _23 * rhs._31;
    r._22 = _21 * rhs._12 + _22 * rhs._22 + _23 * rhs._32;
    r._23 = _21 * rhs._13 + _22 * rhs._23 + _23 * rhs._33;

    r._31 = _31 * rhs._11 + _32 * rhs._21 + _33 * rhs._31;
    r._32 = _31 * rhs._12 + _32 * rhs._22 + _33 * rhs._32;
    r._33 = _31 * rhs._13 + _32 * rhs._23 + _33 * rhs._33;

    *this = r;
    return *this;
}

Mat3x3 &Mat3x3::operator*=(f32 rhs) {
    for (u32 i = 0; i < array_count(v); i += 1) {
        v[i] *= rhs;
    }
    return *this;
}

f32 Mat3x3::det() const {
    return _11 * (_22 * _33 - _23 * _32) -
           _12 * (_21 * _33 - _23 * _31) +
           _13 * (_21 * _32 - _22 * _31);
}

Mat3x3 Mat3x3::inverse() const {
    Mat3x3 result;
    f32 d = det();
    vg_assert(fabsf(d) > 0.0001f);
    f32 inv_det = 1.0f / d;
    
    result._11 = (_22 * _33 - _23 * _32) * inv_det;
    result._12 = (_13 * _32 - _12 * _33) * inv_det;
    result._13 = (_12 * _23 - _13 * _22) * inv_det;
    
    result._21 = (_23 * _31 - _21 * _33) * inv_det;
    result._22 = (_11 * _33 - _13 * _31) * inv_det;
    result._23 = (_13 * _21 - _11 * _23) * inv_det;
    
    result._31 = (_21 * _32 - _22 * _31) * inv_det;
    result._32 = (_12 * _31 - _11 * _32) * inv_det;
    result._33 = (_11 * _22 - _12 * _21) * inv_det;
    
    return result;
}

Mat3x3 Mat3x3::transpose() const {
    return Mat3x3(
        _11, _21, _31,
        _12, _22, _32,
        _13, _23, _33
    );
}

bool operator==(const Mat3x3 &lhs, const Mat3x3 &rhs) {
    return f32_cmp(lhs._11, rhs._11) && f32_cmp(lhs._12, rhs._12) && f32_cmp(lhs._13, rhs._13) &&
           f32_cmp(lhs._21, rhs._21) && f32_cmp(lhs._22, rhs._22) && f32_cmp(lhs._23, rhs._23) &&
           f32_cmp(lhs._31, rhs._31) && f32_cmp(lhs._32, rhs._32) && f32_cmp(lhs._33, rhs._33);
}

bool operator!=(const Mat3x3 &lhs, const Mat3x3 &rhs) {
    return !(lhs == rhs);
}

Mat3x3 operator*(const Mat3x3 &lhs, const Mat3x3 &rhs) {
    Mat3x3 result = lhs;
    return result *= rhs;
}

Mat3x3 operator*(const f32 lhs, const Mat3x3 &rhs) {
    Mat3x3 result = rhs;
    return result *= lhs;
}

Mat3x3 operator*(const Mat3x3 &lhs, const f32 rhs) {
    Mat3x3 result = lhs;
    return result *= rhs;
}

Mat3x3 Mat3x3::rotation(const Vec3 &eulers) {
    // ZXY rotation order (yaw, pitch, roll)
    f32 cx = cosf(eulers.x);
    f32 sx = sinf(eulers.x);
    f32 cy = cosf(eulers.y);
    f32 sy = sinf(eulers.y);
    f32 cz = cosf(eulers.z);
    f32 sz = sinf(eulers.z);
    
    Mat3x3 result;
    result._11 = cy * cz + sx * sy * sz;
    result._12 = cx * sz;
    result._13 = -sy * cz + sx * cy * sz;
    
    result._21 = -cy * sz + sx * sy * cz;
    result._22 = cx * cz;
    result._23 = sy * sz + sx * cy * cz;
    
    result._31 = cx * sy;
    result._32 = -sx;
    result._33 = cx * cy;
    
    return result;
}

Mat3x3 Mat3x3::rotation(const Vec3 &axis, f32 angle) {
    Vec3 n = normalize(axis);
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    f32 t = 1.0f - c;
    
    Mat3x3 result;
    result._11 = t * n.x * n.x + c;
    result._12 = t * n.x * n.y + s * n.z;
    result._13 = t * n.x * n.z - s * n.y;
    
    result._21 = t * n.x * n.y - s * n.z;
    result._22 = t * n.y * n.y + c;
    result._23 = t * n.y * n.z + s * n.x;
    
    result._31 = t * n.x * n.z + s * n.y;
    result._32 = t * n.y * n.z - s * n.x;
    result._33 = t * n.z * n.z + c;
    
    return result;
}

Mat3x3 Mat3x3::rotation_x(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat3x3(
        1, 0,  0,
        0, c,  s,
        0, -s, c
    );
}

Mat3x3 Mat3x3::rotation_y(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat3x3(
        c, 0, -s,
        0, 1,  0,
        s, 0,  c
    );
}

Mat3x3 Mat3x3::rotation_z(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat3x3(
        c,  s, 0,
        -s, c, 0,
        0,  0, 1
    );
}

Mat3x3 Mat3x3::scale(const Vec3 &scale) {
    return Mat3x3(
        scale.x, 0, 0,
        0, scale.y, 0,
        0, 0, scale.z
    );
}

// ---------------- 4 by 4 row major matrix ---------------------

Mat4x4::Mat4x4(f32 m11, f32 m12, f32 m13, f32 m14,
               f32 m21, f32 m22, f32 m23, f32 m24,
               f32 m31, f32 m32, f32 m33, f32 m34,
               f32 m41, f32 m42, f32 m43, f32 m44) {
    _11 = m11; _12 = m12; _13 = m13; _14 = m14;
    _21 = m21; _22 = m22; _23 = m23; _24 = m24;
    _31 = m31; _32 = m32; _33 = m33; _34 = m34;
    _41 = m41; _42 = m42; _43 = m43; _44 = m44;
}

Mat4x4::Mat4x4(const Vec4 &vx, const Vec4 &vy, const Vec4 &vz, const Vec4 &vw) {
    x = vx;
    y = vy;
    z = vz;
    w = vw;
}

Mat4x4::Mat4x4(const f32 *p_array) {
    std::memcpy(f, p_array, sizeof(*this));
}

Mat4x4      Mat4x4::identity() {
    return {1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1};
}

Vec4       &Mat4x4::operator[](u64 idx) {
    vg_assert(idx >= 0 && idx <= 3);
    return v[idx];
}
const Vec4 &Mat4x4::operator[](u64 idx) const {
    vg_assert(idx >= 0 && idx <= 3);
    return v[idx];
}

Mat4x4 &Mat4x4::operator*=(const Mat4x4 &rhs) {
    Mat4x4 r;

    r._11 = _11 * rhs._11 + _12 * rhs._21 + _13 * rhs._31 + _14 * rhs._41;
    r._12 = _11 * rhs._12 + _12 * rhs._22 + _13 * rhs._32 + _14 * rhs._42;
    r._13 = _11 * rhs._13 + _12 * rhs._23 + _13 * rhs._33 + _14 * rhs._43;
    r._14 = _11 * rhs._14 + _12 * rhs._24 + _13 * rhs._34 + _14 * rhs._44;

    r._21 = _21 * rhs._11 + _22 * rhs._21 + _23 * rhs._31 + _24 * rhs._41;
    r._22 = _21 * rhs._12 + _22 * rhs._22 + _23 * rhs._32 + _24 * rhs._42;
    r._23 = _21 * rhs._13 + _22 * rhs._23 + _23 * rhs._33 + _24 * rhs._43;
    r._24 = _21 * rhs._14 + _22 * rhs._24 + _23 * rhs._34 + _24 * rhs._44;

    r._31 = _31 * rhs._11 + _32 * rhs._21 + _33 * rhs._31 + _34 * rhs._41;
    r._32 = _31 * rhs._12 + _32 * rhs._22 + _33 * rhs._32 + _34 * rhs._42;
    r._33 = _31 * rhs._13 + _32 * rhs._23 + _33 * rhs._33 + _34 * rhs._43;
    r._34 = _31 * rhs._14 + _32 * rhs._24 + _33 * rhs._34 + _34 * rhs._44;

    r._41 = _41 * rhs._11 + _42 * rhs._21 + _43 * rhs._31 + _44 * rhs._41;
    r._42 = _41 * rhs._12 + _42 * rhs._22 + _43 * rhs._32 + _44 * rhs._42;
    r._43 = _41 * rhs._13 + _42 * rhs._23 + _43 * rhs._33 + _44 * rhs._43;
    r._44 = _41 * rhs._14 + _42 * rhs._24 + _43 * rhs._34 + _44 * rhs._44;

    *this = r;
    return *this;
}

Mat4x4 &Mat4x4::operator*=(f32 rhs) {
    for (u32 i = 0; i < array_count(v); i += 1)
        v[i] *= rhs;
    return *this;
}

f32 Mat4x4::det() const {
    f32 det2_01_01 = _11 * _22 - _12 * _21;
    f32 det2_01_02 = _11 * _23 - _13 * _21;
    f32 det2_01_03 = _11 * _24 - _14 * _21;
    f32 det2_01_12 = _12 * _23 - _13 * _22;
    f32 det2_01_13 = _12 * _24 - _14 * _22;
    f32 det2_01_23 = _13 * _24 - _14 * _23;
    
    f32 det3_201_012 = _31 * det2_01_12 - _32 * det2_01_02 + _33 * det2_01_01;
    f32 det3_201_013 = _31 * det2_01_13 - _32 * det2_01_03 + _34 * det2_01_01;
    f32 det3_201_023 = _31 * det2_01_23 - _33 * det2_01_03 + _34 * det2_01_02;
    f32 det3_201_123 = _32 * det2_01_23 - _33 * det2_01_13 + _34 * det2_01_12;
    
    return -_41 * det3_201_123 + _42 * det3_201_023 - _43 * det3_201_013 + _44 * det3_201_012;
}

void Mat4x4::transform_decomposition(Vec3 *scale, Mat4x4 *rotation, Vec3 *translation) const {
    if (translation != nullptr) {
        translation->x = _41;
        translation->y = _42;
        translation->z = _43;
    }

    Vec3 row0(_11, _12, _13);
    Vec3 row1(_21, _22, _23);
    Vec3 row2(_31, _32, _33);

    Vec3 scale_vec;
    scale_vec.x = row0.length();
    scale_vec.y = row1.length();
    scale_vec.z = row2.length();

    if (scale != nullptr) {
        *scale = scale_vec;
    }

    if (rotation != nullptr) {
        f32 inv_sx = (scale_vec.x != 0.0f) ? 1.0f / scale_vec.x : 0.0f;
        f32 inv_sy = (scale_vec.y != 0.0f) ? 1.0f / scale_vec.y : 0.0f;
        f32 inv_sz = (scale_vec.z != 0.0f) ? 1.0f / scale_vec.z : 0.0f;

        *rotation = Mat4x4(
            _11 * inv_sx, _12 * inv_sx, _13 * inv_sx, 0,
            _21 * inv_sy, _22 * inv_sy, _23 * inv_sy, 0,
            _31 * inv_sz, _32 * inv_sz, _33 * inv_sz, 0,
            0,0,0,1
        );
    }
}

void Mat4x4::transform_decomposition(Vec3 *scale, Quat *rotation, Vec3 *translation) const {
    Mat4x4 rot_mat;
    transform_decomposition(scale, &rot_mat, translation);
    
    if (rotation != nullptr) {
        *rotation = Quat(Mat3x3(rot_mat));
    }
}

Mat4x4 Mat4x4::inverse() const {
    Mat4x4 mt = transpose();
    
    __m128 r0 = _mm_loadu_ps(&mt.v[0].x);
    __m128 r1 = _mm_loadu_ps(&mt.v[1].x);
    __m128 r2 = _mm_loadu_ps(&mt.v[2].x);
    __m128 r3 = _mm_loadu_ps(&mt.v[3].x);
    
    __m128 v00 = _mm_shuffle_ps(r2, r2, _MM_SHUFFLE(1,1,0,0));
    __m128 v10 = _mm_shuffle_ps(r3, r3, _MM_SHUFFLE(3,2,3,2));
    __m128 v01 = _mm_shuffle_ps(r0, r0, _MM_SHUFFLE(1,1,0,0));
    __m128 v11 = _mm_shuffle_ps(r1, r1, _MM_SHUFFLE(3,2,3,2));
    __m128 v02 = _mm_shuffle_ps(r2, r0, _MM_SHUFFLE(2,0,2,0));
    __m128 v12 = _mm_shuffle_ps(r3, r1, _MM_SHUFFLE(3,1,3,1));

    __m128 d0 = _mm_mul_ps(v00, v10);
    __m128 d1 = _mm_mul_ps(v01, v11);
    __m128 d2 = _mm_mul_ps(v02, v12);

    v00 = _mm_shuffle_ps(r2, r2, _MM_SHUFFLE(3,2,3,2));
    v10 = _mm_shuffle_ps(r3, r3, _MM_SHUFFLE(1,1,0,0));
    v01 = _mm_shuffle_ps(r0, r0, _MM_SHUFFLE(3,2,3,2));
    v11 = _mm_shuffle_ps(r1, r1, _MM_SHUFFLE(1,1,0,0));
    v02 = _mm_shuffle_ps(r2, r0, _MM_SHUFFLE(3,1,3,1));
    v12 = _mm_shuffle_ps(r3, r1, _MM_SHUFFLE(2,0,2,0));

    v00 = _mm_mul_ps(v00, v10);
    v01 = _mm_mul_ps(v01, v11);
    v02 = _mm_mul_ps(v02, v12);
    d0 = _mm_sub_ps(d0, v00);
    d1 = _mm_sub_ps(d1, v01);
    d2 = _mm_sub_ps(d2, v02);

    v11 = _mm_shuffle_ps(d0, d2, _MM_SHUFFLE(1,1,3,1));
    v00 = _mm_shuffle_ps(r1, r1, _MM_SHUFFLE(1,0,2,1));
    v10 = _mm_shuffle_ps(v11, d0, _MM_SHUFFLE(0,3,0,2));
    v01 = _mm_shuffle_ps(r0, r0, _MM_SHUFFLE(0,1,0,2));
    v11 = _mm_shuffle_ps(v11, d0, _MM_SHUFFLE(2,1,2,1));

    __m128 v13 = _mm_shuffle_ps(d1, d2, _MM_SHUFFLE(3,3,3,1));
    v02 = _mm_shuffle_ps(r3, r3, _MM_SHUFFLE(1,0,2,1));
    v12 = _mm_shuffle_ps(v13, d1, _MM_SHUFFLE(0,3,0,2));
    __m128 v03 = _mm_shuffle_ps(r2, r2, _MM_SHUFFLE(0,1,0,2));
    v13 = _mm_shuffle_ps(v13, d1, _MM_SHUFFLE(2,1,2,1));

    __m128 c0 = _mm_mul_ps(v00, v10);
    __m128 c2 = _mm_mul_ps(v01, v11);
    __m128 c4 = _mm_mul_ps(v02, v12);
    __m128 c6 = _mm_mul_ps(v03, v13);

    v11 = _mm_shuffle_ps(d0, d2, _MM_SHUFFLE(0,0,1,0));
    v00 = _mm_shuffle_ps(r1, r1, _MM_SHUFFLE(2,1,3,2));
    v10 = _mm_shuffle_ps(d0, v11, _MM_SHUFFLE(2,1,0,3));
    v01 = _mm_shuffle_ps(r0, r0, _MM_SHUFFLE(1,3,2,3));
    v11 = _mm_shuffle_ps(d0, v11, _MM_SHUFFLE(0,2,1,2));

    v13 = _mm_shuffle_ps(d1, d2, _MM_SHUFFLE(2,2,1,0));
    v02 = _mm_shuffle_ps(r3, r3, _MM_SHUFFLE(2,1,3,2));
    v12 = _mm_shuffle_ps(d1, v13, _MM_SHUFFLE(2,1,0,3));
    v03 = _mm_shuffle_ps(r2, r2, _MM_SHUFFLE(1,3,2,3));
    v13 = _mm_shuffle_ps(d1, v13, _MM_SHUFFLE(0,2,1,2));

    v00 = _mm_mul_ps(v00, v10);
    v01 = _mm_mul_ps(v01, v11);
    v02 = _mm_mul_ps(v02, v12);
    v03 = _mm_mul_ps(v03, v13);
    c0 = _mm_sub_ps(c0, v00);
    c2 = _mm_sub_ps(c2, v01);
    c4 = _mm_sub_ps(c4, v02);
    c6 = _mm_sub_ps(c6, v03);

    v00 = _mm_shuffle_ps(r1, r1, _MM_SHUFFLE(0,3,0,3));
    v10 = _mm_shuffle_ps(d0, d2, _MM_SHUFFLE(1,0,2,2));
    v10 = _mm_shuffle_ps(v10, v10, _MM_SHUFFLE(0,2,3,0));
    v01 = _mm_shuffle_ps(r0, r0, _MM_SHUFFLE(2,0,3,1));
    v11 = _mm_shuffle_ps(d0, d2, _MM_SHUFFLE(1,0,3,0));
    v11 = _mm_shuffle_ps(v11, v11, _MM_SHUFFLE(2,1,0,3));

    v02 = _mm_shuffle_ps(r3, r3, _MM_SHUFFLE(0,3,0,3));
    v12 = _mm_shuffle_ps(d1, d2, _MM_SHUFFLE(3,2,2,2));
    v12 = _mm_shuffle_ps(v12, v12, _MM_SHUFFLE(0,2,3,0));
    v03 = _mm_shuffle_ps(r2, r2, _MM_SHUFFLE(2,0,3,1));
    v13 = _mm_shuffle_ps(d1, d2, _MM_SHUFFLE(3,2,3,0));
    v13 = _mm_shuffle_ps(v13, v13, _MM_SHUFFLE(2,1,0,3));

    v00 = _mm_mul_ps(v00, v10);
    v01 = _mm_mul_ps(v01, v11);
    v02 = _mm_mul_ps(v02, v12);
    v03 = _mm_mul_ps(v03, v13);
    __m128 c1 = _mm_sub_ps(c0, v00);
    c0 = _mm_add_ps(c0, v00);
    __m128 c3 = _mm_add_ps(c2, v01);
    c2 = _mm_sub_ps(c2, v01);
    __m128 c5 = _mm_sub_ps(c4, v02);
    c4 = _mm_add_ps(c4, v02);
    __m128 c7 = _mm_add_ps(c6, v03);
    c6 = _mm_sub_ps(c6, v03);

    c0 = _mm_shuffle_ps(c0, c1, _MM_SHUFFLE(3,1,2,0));
    c2 = _mm_shuffle_ps(c2, c3, _MM_SHUFFLE(3,1,2,0));
    c4 = _mm_shuffle_ps(c4, c5, _MM_SHUFFLE(3,1,2,0));
    c6 = _mm_shuffle_ps(c6, c7, _MM_SHUFFLE(3,1,2,0));
    c0 = _mm_shuffle_ps(c0, c0, _MM_SHUFFLE(3,1,2,0));
    c2 = _mm_shuffle_ps(c2, c2, _MM_SHUFFLE(3,1,2,0));
    c4 = _mm_shuffle_ps(c4, c4, _MM_SHUFFLE(3,1,2,0));
    c6 = _mm_shuffle_ps(c6, c6, _MM_SHUFFLE(3,1,2,0));

    // Compute determinant
    __m128 vtmp0 = _mm_mul_ps(r0, c0);
    __m128 vtmp1 = _mm_shuffle_ps(vtmp0, vtmp0, _MM_SHUFFLE(2,3,0,1));
    vtmp0 = _mm_add_ps(vtmp0, vtmp1);
    vtmp1 = _mm_shuffle_ps(vtmp0, vtmp0, _MM_SHUFFLE(1,0,3,2));
    vtmp0 = _mm_add_ps(vtmp0, vtmp1);
    
    f32 det;
    _mm_store_ss(&det, vtmp0);
    vtmp0 = _mm_set1_ps(1.0f / det);
    
    Mat4x4 result;
    _mm_storeu_ps(&result.v[0].x, _mm_mul_ps(c0, vtmp0));
    _mm_storeu_ps(&result.v[1].x, _mm_mul_ps(c2, vtmp0));
    _mm_storeu_ps(&result.v[2].x, _mm_mul_ps(c4, vtmp0));
    _mm_storeu_ps(&result.v[3].x, _mm_mul_ps(c6, vtmp0));
    
    return result;
}

Mat4x4 Mat4x4::transpose() const {
    return Mat4x4(
        _11, _21, _31, _41,
        _12, _22, _32, _42,
        _13, _23, _33, _43,
        _14, _24, _34, _44
    );
}

Mat4x4 Mat4x4::rotation(const Vec3 &eulers) {
    // ZXY rotation order (yaw, pitch, roll)
    f32 cx = cosf(eulers.x);
    f32 sx = sinf(eulers.x);
    f32 cy = cosf(eulers.y);
    f32 sy = sinf(eulers.y);
    f32 cz = cosf(eulers.z);
    f32 sz = sinf(eulers.z);
    
    Mat4x4 result;
    result._11 = cy * cz + sx * sy * sz;
    result._12 = cx * sz;
    result._13 = -sy * cz + sx * cy * sz;
    result._14 = 0;
    
    result._21 = -cy * sz + sx * sy * cz;
    result._22 = cx * cz;
    result._23 = sy * sz + sx * cy * cz;
    result._24 = 0;
    
    result._31 = cx * sy;
    result._32 = -sx;
    result._33 = cx * cy;
    result._34 = 0;
    
    result._41 = 0;
    result._42 = 0;
    result._43 = 0;
    result._44 = 1;
    
    return result;
}

Mat4x4 Mat4x4::rotation(const Vec3 &axis, f32 angle) {
    Vec3 n = normalize(axis);
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    f32 t = 1.0f - c;
    
    Mat4x4 result;
    result._11 = t * n.x * n.x + c;
    result._12 = t * n.x * n.y + s * n.z;
    result._13 = t * n.x * n.z - s * n.y;
    result._14 = 0;
    
    result._21 = t * n.x * n.y - s * n.z;
    result._22 = t * n.y * n.y + c;
    result._23 = t * n.y * n.z + s * n.x;
    result._24 = 0;
    
    result._31 = t * n.x * n.z + s * n.y;
    result._32 = t * n.y * n.z - s * n.x;
    result._33 = t * n.z * n.z + c;
    result._34 = 0;
    
    result._41 = 0;
    result._42 = 0;
    result._43 = 0;
    result._44 = 1;
    
    return result;
}

Mat4x4 Mat4x4::rotation_x(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat4x4(
        1, 0,  0, 0,
        0, c,  s, 0,
        0, -s, c, 0,
        0, 0,  0, 1
    );
}

Mat4x4 Mat4x4::rotation_y(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat4x4(
        c, 0, -s, 0,
        0, 1,  0, 0,
        s, 0,  c, 0,
        0, 0,  0, 1
    );
}

Mat4x4 Mat4x4::rotation_z(f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    return Mat4x4(
        c,  s, 0, 0,
        -s, c, 0, 0,
        0,  0, 1, 0,
        0,  0, 0, 1
    );
}

Mat4x4 Mat4x4::translation(const Vec3 &translation) {
    return Mat4x4(
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        translation.x, translation.y, translation.z, 1
    );
}

Mat4x4 Mat4x4::scale(const Vec3 &scale) {
    return Mat4x4(
        scale.x, 0, 0, 0,
        0, scale.y, 0, 0,
        0, 0, scale.z, 0,
        0, 0, 0, 1
    );
}

Mat4x4 Mat4x4::look_at(const Vec3 &eye, const Vec3 &at, const Vec3 &up) {
    return look_to(eye, at - eye, up);
}

Mat4x4 Mat4x4::look_to(const Vec3 &eye, const Vec3 &dir, const Vec3 &up) {
    Vec3 z_axis = normalize(dir);
    Vec3 x_axis = normalize(Vec3(
        up.y * z_axis.z - up.z * z_axis.y,
        up.z * z_axis.x - up.x * z_axis.z,
        up.x * z_axis.y - up.y * z_axis.x
    ));
    Vec3 y_axis = Vec3(
        z_axis.y * x_axis.z - z_axis.z * x_axis.y,
        z_axis.z * x_axis.x - z_axis.x * x_axis.z,
        z_axis.x * x_axis.y - z_axis.y * x_axis.x
    );
    
    return Mat4x4(
        x_axis.x, y_axis.x, z_axis.x, 0,
        x_axis.y, y_axis.y, z_axis.y, 0,
        x_axis.z, y_axis.z, z_axis.z, 0,
        -dot(x_axis, eye), -dot(y_axis, eye), -dot(z_axis, eye), 1
    );
}

Mat4x4 Mat4x4::perspective(f32 fov_y, f32 aspect, f32 znear, f32 zfar) {
    f32 tan_half_fov = tanf(fov_y * 0.5f);
    f32 range = zfar / (zfar - znear);
    
    return Mat4x4(
        1.0f / (aspect * tan_half_fov), 0, 0, 0,
        0, 1.0f / tan_half_fov, 0, 0,
        0, 0, range, 1,
        0, 0, -range * znear, 0
    );
}

Mat4x4 Mat4x4::orthographic(f32 width, f32 height, f32 znear, f32 zfar) {
    f32 range = 1.0f / (zfar - znear);
    
    return Mat4x4(
        2.0f / width, 0, 0, 0,
        0, 2.0f / height, 0, 0,
        0, 0, range, 0,
        0, 0, -range * znear, 1
    );
}

bool operator==(const Mat4x4 &lhs, const Mat4x4 &rhs) {
    return f32_cmp(lhs._11, rhs._11) && f32_cmp(lhs._12, rhs._12) && f32_cmp(lhs._13, rhs._13) && f32_cmp(lhs._14, rhs._14) &&
           f32_cmp(lhs._21, rhs._21) && f32_cmp(lhs._22, rhs._22) && f32_cmp(lhs._23, rhs._23) && f32_cmp(lhs._24, rhs._24) &&
           f32_cmp(lhs._31, rhs._31) && f32_cmp(lhs._32, rhs._32) && f32_cmp(lhs._33, rhs._33) && f32_cmp(lhs._34, rhs._34) &&
           f32_cmp(lhs._41, rhs._41) && f32_cmp(lhs._42, rhs._42) && f32_cmp(lhs._43, rhs._43) && f32_cmp(lhs._44, rhs._44);
}
bool operator!=(const Mat4x4 &lhs, const Mat4x4 &rhs) {
    return !(lhs == rhs);
}
Mat4x4 operator*(const Mat4x4 &lhs, const Mat4x4 &rhs) {
    Mat4x4 result = lhs;
    return result *= rhs;
}
Mat4x4 operator*(const f32 lhs, const Mat4x4 &rhs) {
    Mat4x4 result = rhs;
    return result *= lhs;
}
Mat4x4 operator*(const Mat4x4 &lhs, const f32 rhs) {
    Mat4x4 result = lhs;
    return result *= rhs;
}

Vec3 operator*(const Vec3 &lhs, const Mat3x3 &rhs) {
    Vec3 out;
    out.x = lhs.x * rhs._11 + lhs.y * rhs._21 + lhs.z * rhs._31;
    out.y = lhs.x * rhs._12 + lhs.y * rhs._22 + lhs.z * rhs._32;
    out.z = lhs.x * rhs._13 + lhs.y * rhs._23 + lhs.z * rhs._33;
    return out;
}
Vec3 operator*(const Vec3 &lhs, const Mat4x4 &rhs) {
    Vec4 v = Vec4(lhs, 1.f);
    return Vec3(v * rhs);
}
Vec4 operator*(const Vec4 &lhs, const Mat4x4 &rhs) {
    Vec4 r;
    r.x = lhs.x * rhs._11 + lhs.y * rhs._21 + lhs.z * rhs._31 + lhs.w * rhs._41;
    r.y = lhs.x * rhs._12 + lhs.y * rhs._22 + lhs.z * rhs._32 + lhs.w * rhs._42;
    r.z = lhs.x * rhs._13 + lhs.y * rhs._23 + lhs.z * rhs._33 + lhs.w * rhs._43;
    r.w = lhs.x * rhs._14 + lhs.y * rhs._24 + lhs.z * rhs._34 + lhs.w * rhs._44;
    return r;
}

// --------------------- vector min max -------------------------

Vec2 min(const Vec2 &a, const Vec2 &b) {
    return Vec2(std::min(a.x, b.x), std::min(a.y, b.y));
}
Vec3 min(const Vec3 &a, const Vec3 &b) {
    return Vec3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
Vec4 min(const Vec4 &a, const Vec4 &b) {
    return Vec4(std::min(a.x, b.x),std::min(a.y, b.y),std::min(a.z, b.z),std::min(a.w, b.w));
}
Vec2 max(const Vec2 &a, const Vec2 &b) {
    return Vec2(std::max(a.x, b.x), std::max(a.y, b.y));
}
Vec3 max(const Vec3 &a, const Vec3 &b) {
    return Vec3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
}
Vec4 max(const Vec4 &a, const Vec4 &b) {
    return Vec4(std::max(a.x, b.x),std::max(a.y, b.y),std::max(a.z, b.z),std::max(a.w, b.w));
}

// ------------------------- planes -----------------------------

Vec4 create_plane(const Vec3 &point, const Vec3 &normal) {
    Vec3 n = normalize(normal);
    f32 d = -dot(point, n);
    return Vec4(n.x, n.y, n.z, d);
}

f32 plane_point_sdf(const Vec4 &plane, const Vec3 point) {
    return dot(point, Vec3(plane.f)) + plane.w;
}

// ---------------------- quaternions ---------------------------

Quat::Quat(const Vec3 &axis, f32 angle) {
    Vec3 n = normalize(axis);
    f32 half_angle = angle * 0.5f;
    f32 s = sinf(half_angle);
    f32 cos_half = cosf(half_angle);
    b = n.x * s;
    c = n.y * s;
    d = n.z * s;
    a = cos_half;
}
Quat::Quat(f32 _b, f32 _c, f32 _d, f32 _a) { b = _b, c = _c, d = _d, a = _a; }
Quat::Quat(const f32 *p_array) { b = p_array[0], c = p_array[1], d = p_array[2], a = p_array[3]; }
Quat::Quat(const Mat3x3 &mat) {
    f32 trace = mat._11 + mat._22 + mat._33;
    
    if (trace > 0.0f) {
        f32 s = sqrtf(trace + 1.0f);
        a = s * 0.5f;
        s = 0.5f / s;
        b = (mat._23 - mat._32) * s;
        c = (mat._31 - mat._13) * s;
        d = (mat._12 - mat._21) * s;
    } else if (mat._11 > mat._22 && mat._11 > mat._33) {
        f32 s = sqrtf(1.0f + mat._11 - mat._22 - mat._33);
        b = s * 0.5f;
        s = 0.5f / s;
        c = (mat._12 + mat._21) * s;
        d = (mat._31 + mat._13) * s;
        a = (mat._23 - mat._32) * s;
    } else if (mat._22 > mat._33) {
        f32 s = sqrtf(1.0f + mat._22 - mat._11 - mat._33);
        c = s * 0.5f;
        s = 0.5f / s;
        b = (mat._12 + mat._21) * s;
        d = (mat._23 + mat._32) * s;
        a = (mat._31 - mat._13) * s;
    } else {
        f32 s = sqrtf(1.0f + mat._33 - mat._11 - mat._22);
        d = s * 0.5f;
        s = 0.5f / s;
        b = (mat._31 + mat._13) * s;
        c = (mat._23 + mat._32) * s;
        a = (mat._12 - mat._21) * s;
    }
}
Quat  Quat::identity() { return Quat(0.f, 0.f, 0.f, 1.f); }
f32  &Quat::operator[](u64 idx) { vg_assert(idx >= 0 && idx <= 3); return ((f32 *)this)[idx]; }
f32   Quat::operator[](u64 idx) const { vg_assert(idx >= 0 && idx <= 3); return ((f32 *)this)[idx]; }
Quat &Quat::operator*=(const Quat &rhs) {
    f32 tb = b, tc = c, td = d, ta = a;
    b = ta * rhs.b + tb * rhs.a + tc * rhs.d - td * rhs.c;
    c = ta * rhs.c - tb * rhs.d + tc * rhs.a + td * rhs.b;
    d = ta * rhs.d + tb * rhs.c - tc * rhs.b + td * rhs.a;
    a = ta * rhs.a - tb * rhs.b - tc * rhs.c - td * rhs.d;
    return *this;
}
Mat4x4 Quat::to_matrix() const {
    f32 xx = b * b;
    f32 yy = c * c;
    f32 zz = d * d;
    f32 xy = b * c;
    f32 xz = b * d;
    f32 yz = c * d;
    f32 wx = a * b;
    f32 wy = a * c;
    f32 wz = a * d;
    
    return Mat4x4(
        1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0,
        2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0,
        2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0,
        0, 0, 0, 1
    );
}

bool operator==(const Quat &lhs, const Quat &rhs) { return f32_cmp(lhs[0], rhs[0]) && f32_cmp(lhs[1], rhs[1]) && f32_cmp(lhs[2], rhs[2]) && f32_cmp(lhs[3], rhs[3]); }
bool operator!=(const Quat &lhs, const Quat &rhs) { return !(lhs == rhs); }
Quat operator*(const Quat &rhs, const Quat &lhs) {
    Quat result = rhs;
    return result *= lhs;
}

Vec3 line_point_nearest(const Vec3 &origin, const Vec3 &direction, const Vec3 &point) {
    Vec3 dir = normalize(direction);
    Vec3 v = point - origin;

    f32 dot_p = dot(v, dir);
    return origin + dir * dot_p;
}

bool f32_cmp(f32 a, f32 b, f32 threshold) {
    return abs(a - b) < threshold;
}

f32 saturate(f32 value) {
    if (value < 0.f) return 0.f;
    if (value > 1.f) return 1.f;
    return value;
}

Vec2 saturate(const Vec2 &value) {
    return Vec2(saturate(value.x), saturate(value.y));
}

Vec3 saturate(const Vec3 &value) {
    return Vec3(saturate(value.x), saturate(value.y), saturate(value.z));
}

Vec4 saturate(const Vec4 &value) {
    return Vec4(saturate(value.x), saturate(value.y), saturate(value.z), value.w);
}

// https://github.com/dfelinto/blender/blob/master/source/blender/blenlib/intern/math_rotation.c#L1388
void nearest_euler(Vec3 *p_euler, const Vec3 &hint) {
    Vec3     &euler = *p_euler;
    const f32 pi_thresh = (5.1f);
    f32       delta[3];

    for (u32 i = 0; i < 3; i++) {
        delta[i] = euler[i] - hint[i];
        if (delta[i] > pi_thresh) {
            euler[i] -= floorf((delta[i] / pi32 * 2) + 0.5f) * pi32 * 2;
            delta[i] = euler[i] - hint[i];
        } else if (delta[i] < -pi_thresh) {
            euler[i] += floorf((-delta[i] / pi32 * 2) + 0.5f) * pi32 * 2;
            delta[i] = euler[i] - hint[i];
        }
    }

    if (abs(delta[0]) > 3.2f && abs(delta[1]) < 1.6f && abs(delta[2]) < 1.6f) {
        if (delta[0] > 0.0f) {
            euler[0] -= pi32 * 2;
        } else {
            euler[0] += pi32 * 2;
        }
    }

    if (abs(delta[1]) > 3.2f && abs(delta[2]) < 1.6f && abs(delta[0]) < 1.6f) {
        if (delta[1] > 0.0f) {
            euler[1] -= pi32 * 2;
        } else {
            euler[1] += pi32 * 2;
        }
    }

    if (abs(delta[2]) > 3.2f && abs(delta[0]) < 1.6f && abs(delta[1]) < 1.6f) {
        if (delta[2] > 0.0f) {
            euler[2] -= pi32 * 2;
        } else {
            euler[2] += pi32 * 2;
        }
    }
}

void matrix_to_euler2(const Mat3x3 &rotation, Vec3 eulers[2]) {
    // _31 = cos(x)sin(y)
    // _33 = cos(x)cos(y)
    // 1 = sqrt(cos(y)^2 + sin(y)^2)
    // cos(x) * 1 = cos(x) * sqrt(cos(y)^2 + sin(y)^2)
    // cos(x) = +-sqrt(cos(x)^2 * cos(y)^2 + cos(x)^2 + sin(y)^2)
    // cos(x) = +-sqrt(_13^2 + _33^2)
    f32 abs_cx = sqrtf(rotation._31 * rotation._31 + rotation._33 * rotation._33);
    if (abs_cx > 0.0001f) {
        // _32 = -sin(x), arcsin(-_32) has solutions x and pi-x, we have 2 solutions
        // cos(pi-x) = -cos(x)
        // x    = atan2(sin(x),  cos(x))
        // pi-x = atan2(sin(x), -cos(x))

        // -_32/cos(x) = sin(x)/cos(x) = tan(x)
        // if cos(x) > 0 -> abs_cx =  cos(x), we get solution x
        // if cos(x) < 0 -> abs_cx = -cos(x), we get solution pi-x
        eulers[0].x = atan2f(-rotation._32, abs_cx);
        // _31/_33 = cos(x)sin(y)/cos(x)cos(y) = sin(y)/cos(y) = tan(y)
        // _12/_22 = cos(x)sin(z)/cos(x)cos(z) = sin(z)/cos(z) = tan(z)
        // if cos(x) > 0 -> atan2( sin([yz]),  cos([yz])), solution for abs_cx =  cos(x)
        // if cos(x) < 0 -> get atan2(-sin([yz]), -cos([yz])), solution for abs_cx = -cos(x) = cos(pi-x)
        eulers[0].y = atan2f(rotation._31, rotation._33);
        eulers[0].z = atan2f(rotation._12, rotation._22);

        // negate abs_cx and all cos(x) to get the other solution
        eulers[1].x = atan2f(-rotation._32, -abs_cx);
        eulers[1].y = atan2f(-rotation._31, -rotation._33);
        eulers[1].z = atan2f(-rotation._12, -rotation._22);
    } else {
        // cos(x) = 0
        // if sin(x) = 1
        //   -_32 = -(sin(y)cos(z) - cos(y)sin(z)) = sin(z)cos(y) - cos(z)sin(y) = sin(z - y)
        //    _21 = sin(z)sin(y) + cos(z)cos(y) = cos(z - y)
        //    z - y = atan2(-_32, _21)
        // if sin(x) = -1
        //   -_32 = -(-sin(y)cos(z) - cos(y)sin(z)) = sin(y)cos(z) + cos(y)sin(z) = sin(z + y)
        //    _21 = -sin(z)sin(y) + cos(z)cos(y) = cos(z + y)
        //    z + y = atan2(-_32, _21)
        // set y = 0, in both cases we have z = atan2(-_32, _21)
        eulers[0].x = atan2f(-rotation._32, abs_cx);
        eulers[0].y = 0;
        eulers[0].z = atan2f(-rotation._21, rotation._11);
        eulers[1].x = atan2f(-rotation._32, abs_cx);
        eulers[1].y = 0;
        eulers[1].z = atan2f(-rotation._21, rotation._11);
    }
}

Vec3 matrix_to_euler(const Mat3x3 &rotation, const Vec3 &hint) {
    Vec3 eulers[2];

    matrix_to_euler2(rotation, eulers);
    nearest_euler(&eulers[0], hint);
    nearest_euler(&eulers[1], hint);
    f32 d0 = abs(eulers[0].x - hint.x) + abs(eulers[0].y - hint.y) + abs(eulers[0].z - hint.z);
    f32 d1 = abs(eulers[1].x - hint.x) + abs(eulers[1].y - hint.y) + abs(eulers[1].z - hint.z);

    if (d0 < d1) {
        return eulers[0];
    } else {
        return eulers[1];
    }
}

Vec3 round_near_zero(const Vec3 &value, u32 precision) {
    Vec3 ret = value;
    f32  thresh = std::powf(10.f, -(f32)precision);
    ret.x = std::abs(ret.x) < thresh ? 0.f : ret.x;
    ret.y = std::abs(ret.y) < thresh ? 0.f : ret.y;
    ret.z = std::abs(ret.z) < thresh ? 0.f : ret.z;
    return ret;
}

f32 to_radian(f32 deg) {
    return deg * pi32 / 180.f;
}

f32 to_degree(f32 rad) {
    return rad * 180.f / pi32;
}

// ---------------------------------------------------------------
//  *************************************************************
// --------------------- OS_WIN32 --------------------------------

#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#pragma comment(lib, "user32")
#pragma comment(lib, "winmm")
#pragma comment(lib, "shell32")
#pragma comment(lib, "kernel32")

struct Win32Window {
    Win32Window *next;
    Win32Window *prev;

    HWND     hwnd;
    Int2     size;
    RectS32  before_rect;
    Int2     before_mouse;
    bool     fullscreen;
    bool     minimized;
    bool     has_focus;
    bool     menu_op;
};

vg_internal LPVOID       win32_main_fiber = nullptr;
vg_internal LPVOID       win32_msg_fiber = nullptr;
vg_internal HANDLE       win32_timer;
vg_internal Arena       *win32_evt_arena = nullptr;
vg_internal OsEventList *win32_evt_list;
vg_internal Arena       *win32_arena;
vg_internal Win32Window *win32_free_window = nullptr;

vg_internal const Key win32_to_scancode[] = {
    Key_Unknown,
    Key_Esc,
    Key_K1,
    Key_K2,
    Key_K3,
    Key_K4,
    Key_K5,
    Key_K6,
    Key_K7,
    Key_K8,
    Key_K9,
    Key_K0,
    Key_Minus,
    Key_Equal,
    Key_Backspace,
    Key_Tab,
    Key_Q,
    Key_W,
    Key_E,
    Key_R,
    Key_T,
    Key_Y,
    Key_U,
    Key_I,
    Key_O,
    Key_P,
    Key_Leftbrace,
    Key_Rightbrace,
    Key_Return,
    Key_LeftCtrl,
    Key_A,
    Key_S,
    Key_D,
    Key_F,
    Key_G,
    Key_H,
    Key_J,
    Key_K,
    Key_L,
    Key_Semicolon,
    Key_Apostrophe,
    Key_Grave,
    Key_LeftShift,
    Key_Backslash,
    Key_Z,
    Key_X,
    Key_C,
    Key_V,
    Key_B,
    Key_N,
    Key_M,
    Key_Comma,
    Key_Period,
    Key_Slash,
    Key_RightShift,
    Key_Printscreen,
    Key_LeftAlt,
    Key_Space,
    Key_Capslock,
    Key_F1,
    Key_F2,
    Key_F3,
    Key_F4,
    Key_F5,
    Key_F6,
    Key_F7,
    Key_F8,
    Key_F9,
    Key_F10,
    Key_NumLock,
    Key_ScrollLock,
    Key_Home,
    Key_Up,
    Key_PageUp,
    Key_NumMinus,
    Key_Left,
    Key_Num5,
    Key_Right,
    Key_NumPlus,
    Key_End,
    Key_Down,
    Key_PageDown,
    Key_Insert,
    Key_Del,
    Key_Unknown,
    Key_Unknown,
    Key_Nonusbackslash,
    Key_F11,
    Key_F12,
    Key_Pause,
    Key_Unknown,
    Key_LeftMeta,
    Key_RightMeta,
    Key_Compose,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_F13,
    Key_F14,
    Key_F15,
    Key_F16,
    Key_F17,
    Key_F18,
    Key_F19,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
    Key_Unknown,
};

vg_internal Key translate_key(u32 virtual_key, u32 win_scancode, u32 flags) {
    static bool pause_scancode_read = false;
    Key         scancode = Key_Unknown;

    if (virtual_key == VK_SHIFT) {
        // correct left-hand / right-hand SHIFT
        virtual_key = MapVirtualKey(win_scancode, MAPVK_VSC_TO_VK_EX);
    }
    // e0 and e1 are escape sequences used for certain special keys, such as PRINT and PAUSE/BREAK.
    // http://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
    bool is_e0 = ((flags & RI_KEY_E0) != 0);
    bool is_e1 = ((flags & RI_KEY_E1) != 0);

    if (is_e1) {
        if (win_scancode == 0x1D) {
            // scancode for PAUSE is 0xE11D followed by 0x45
            // scancode for NUMLOCK is 0x45
            // we need to remember 0xE11D to differentiate between PAUSE and NUMLOCK
            pause_scancode_read = true;
            return Key_Unknown;
        } else {
            // for escaped sequences, turn the virtual key into the correct scan code using MapVirtualKey.
            win_scancode = MapVirtualKey(virtual_key, MAPVK_VK_TO_VSC);
        }
    }

    if (win_scancode < array_count(win32_to_scancode)) {
        scancode = win32_to_scancode[win_scancode];
    }

    if (pause_scancode_read) {
        pause_scancode_read = false;
        if (scancode == Key_NumLock) {
            return Key_Pause;
        }
    }

    switch (scancode) {
    // right-hand CONTROL and ALT have their e0 bit set
    case Key_LeftCtrl:
        if (is_e0) scancode = Key_RightCtrl;
        break;
    case Key_LeftAlt:
        if (is_e0) scancode = Key_RightAlt;
        break;
    // NUMPAD ENTER has its e0 bit set
    case Key_Return:
        if (is_e0) scancode = Key_NumEnter;
        break;
    // the standard INSERT, DELETE, HOME, END, PRIOR and NEXT keys will always have their e0 bit set, but the
    // corresponding keys on the NUMPAD will not.
    case Key_Insert:
        if (!is_e0) scancode = Key_Num0;
        break;
    case Key_Del:
        if (!is_e0) scancode = Key_NumPeriod;
        break;
    case Key_Home:
        if (!is_e0) scancode = Key_Num7;
        break;
    case Key_End:
        if (!is_e0) scancode = Key_Num1;
        break;
    case Key_PageUp:
        if (!is_e0) scancode = Key_Num9;
        break;
    case Key_PageDown:
        if (!is_e0) scancode = Key_Num3;
        break;
    // the standard arrow keys will always have their e0 bit set, but the
    // corresponding keys on the NUMPAD will not.
    case Key_Left:
        if (!is_e0) scancode = Key_Num4;
        break;
    case Key_Right:
        if (!is_e0) scancode = Key_Num6;
        break;
    case Key_Up:
        if (!is_e0) scancode = Key_Num8;
        break;
    case Key_Down:
        if (!is_e0) scancode = Key_Num2;
        break;
    // NUMPAD 5 doesn't have its e0 bit set
    case Key_Clear:
        if (!is_e0) scancode = Key_Num5;
        break;
    }
    return scancode;
}

vg_internal OsEvent *win32_push_event(OsEventType type, OsHandle window) {
    OsEvent *evt = win32_evt_arena->push_array<OsEvent>(1);
    evt->type = type;
    evt->window = window;
    dll_push_back(win32_evt_list->first, win32_evt_list->last, evt);
    win32_evt_list->count += 1;

    return evt;
}

vg_internal LRESULT CALLBACK win_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32Window *window;
    if (msg == WM_CREATE) {
        window = (Win32Window *)((LPCREATESTRUCT)lParam)->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)window);
    } else {
        window = (Win32Window *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    if (window == nullptr || win32_evt_arena == nullptr) {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    LRESULT  result = 0;
    bool     release = true;
    bool     switch_fiber = false;
    OsHandle handle = (OsHandle)window;

    switch (msg) {
    case WM_DESTROY:
        delete window;
        return 0;
    case WM_MENUCHAR:
        // Note: removes beeping sound when pressing an invalid alt + key combination
        //       https://docs.microsoft.com/en-us/windows/win32/menurc/wm-menuchar
        return MNC_CLOSE << 16;
    case WM_CLOSE:
        win32_push_event(OsEventType_WindowClosed, handle);
        break;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            s32 width = GET_X_LPARAM(lParam);
            s32 height = GET_Y_LPARAM(lParam);
            if (window->minimized) {
                win32_push_event(OsEventType_WindowGainFocus, handle);
            }

            window->minimized = false;
            window->size.x = width;
            window->size.y = height;
            OsEvent *evt = win32_push_event(OsEventType_WindowResize, handle);
            evt->size = window->size;
        } else {
            window->minimized = true;
            win32_push_event(OsEventType_WindowLoseFocus, handle);
        }

        if (GetCurrentFiber() == win32_msg_fiber) {
            SwitchToFiber(win32_main_fiber);
        }

        break;
    case WM_MOUSEMOVE: {
        OsEvent *evt = win32_push_event(OsEventType_MouseMoveAbs, handle);
        evt->pos = {
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam),
        };
        break;
    }
    case WM_CHAR: {
        return 0;
    }
    case WM_INPUTLANGCHANGE:
        return 0;
    case WM_SETFOCUS:
        window->has_focus = true;
        win32_push_event(OsEventType_WindowGainFocus, handle);
        break;
    case WM_KILLFOCUS:
        ReleaseCapture();
        window->has_focus = false;
        win32_push_event(OsEventType_WindowLoseFocus, handle);
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        release = false;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        OsEvent *evt = win32_push_event(release ? OsEventType_KeyRelease : OsEventType_KeyPress, handle);
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            evt->key = Key_MouseButtonLeft;
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            evt->key = Key_MouseButtonRight;
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            evt->key = Key_MouseButtonMiddle;
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            evt->key = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? Key_MouseButtonX1 : Key_MouseButtonX2;
            break;
        }

        if (release) {
            ReleaseCapture();
        } else {
            SetCapture(hwnd);
        }
        break;
    }
    case WM_INPUT: {
        // https://blog.molecular-matters.com/2011/09/05/properly-handling-keyboard-input/
        char buffer[sizeof(RAWINPUT)] = {};
        u32  size = sizeof(RAWINPUT);
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));

        // extract keyboard raw input data
        RAWINPUT *raw = (RAWINPUT *)(buffer);
        if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            UINT virtual_key = raw->data.keyboard.VKey;
            UINT win_scancode = raw->data.keyboard.MakeCode;
            UINT flags = raw->data.keyboard.Flags;
            Key key = translate_key(virtual_key, win_scancode, flags);
            if (key != Key_Unknown) {
                OsEvent *evt = win32_push_event((flags & RI_KEY_BREAK) ? OsEventType_KeyRelease : OsEventType_KeyPress, handle);
                evt->key = key;                
            }
        } else if (raw->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE &raw_mouse = raw->data.mouse;
            if ((raw_mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == MOUSE_MOVE_ABSOLUTE) {
                RECT rect;
                if (raw_mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) {
                    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    rect.right = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    rect.bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                } else {
                    rect.left = 0;
                    rect.top = 0;
                    rect.right = GetSystemMetrics(SM_CXSCREEN);
                    rect.bottom = GetSystemMetrics(SM_CYSCREEN);
                }

                int abs_x = MulDiv(raw_mouse.lLastX, rect.right, max_u16) + rect.left;
                int abs_y = MulDiv(raw_mouse.lLastY, rect.bottom, max_u16) + rect.top;
                OsEvent *evt = win32_push_event(OsEventType_MouseMoveRel, handle);
                evt->delta = {
                    abs_x - window->before_mouse.x,
                    abs_y - window->before_mouse.y,
                };
                window->before_mouse = {abs_x, abs_y};
            } else if (raw_mouse.lLastX != 0 || raw_mouse.lLastY != 0) {
                OsEvent *evt = win32_push_event(OsEventType_MouseMoveRel, handle);
                evt->delta = {
                    raw_mouse.lLastX,
                    raw_mouse.lLastY,
                };
            }
        }
        break;
    }
    case WM_ENTERMENULOOP:
    case WM_ENTERSIZEMOVE:
        window->menu_op = true;
        SetTimer(hwnd, 1, 1, nullptr);
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_EXITMENULOOP:
    case WM_EXITSIZEMOVE:
        window->menu_op = false;
        KillTimer(hwnd, 1);
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_TIMER:
        if (wParam == 1 && GetCurrentFiber() == win32_msg_fiber) {
            SwitchToFiber(win32_main_fiber);
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    return result;
}

void msg_loop(void *p) {
    (void)p;

    while (true) {
        MSG msg = {};

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                OsEvent *evt = win32_evt_arena->push_array<OsEvent>(1);
                evt->type = OsEventType_Quit;
                dll_push_back(win32_evt_list->first, win32_evt_list->last, evt);
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        SwitchToFiber(win32_main_fiber);
    }
}

typedef BOOL WINAPI set_process_dpi_aware(void);
typedef BOOL WINAPI set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT);
vg_internal bool win32_init() {
    HMODULE win_user = LoadLibraryW(L"user32.dll");
    set_process_dpi_awareness_context *SetProcessDPIAwarenessContext = (set_process_dpi_awareness_context *)GetProcAddress(win_user, "SetProcessDPIAwarenessContext");
    if (SetProcessDPIAwarenessContext) {
        SetProcessDPIAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    } else {
        set_process_dpi_aware *SetProcessDPIAware = (set_process_dpi_aware *)GetProcAddress(win_user, "SetProcessDPIAware");
        if (SetProcessDPIAware) {
            SetProcessDPIAware();
        }
    }

    RAWINPUTDEVICE rawinput_devices[2] = {};
    rawinput_devices[0].usUsagePage = 0x01;
    rawinput_devices[0].usUsage = 0x06; // keyboard
    rawinput_devices[1].usUsagePage = 0x01;
    rawinput_devices[1].usUsage = 0x02; // mouse
    if (!RegisterRawInputDevices(rawinput_devices, 2, sizeof(rawinput_devices[0]))) {
        return false;
    }

    HINSTANCE  instance = GetModuleHandle(NULL);
    WNDCLASSEXW winclass{
        .cbSize = sizeof(WNDCLASSEX),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = win_proc,
        .cbClsExtra = NULL,
        .cbWndExtra = NULL,
        .hInstance = instance,
        .hIcon = LoadIcon(NULL, IDI_APPLICATION),
        .hCursor = LoadCursor(NULL, IDC_ARROW),
        .hbrBackground = 0,
        .lpszMenuName = NULL,
        .lpszClassName = L"mini_vg",
        .hIconSm = LoadIcon(NULL, IDI_APPLICATION),
    };
    if (!RegisterClassExW(&winclass)) {
        return false;
    }

    win32_main_fiber = ConvertThreadToFiber(nullptr);
    win32_msg_fiber = CreateFiber(0, msg_loop, nullptr);
    if (win32_main_fiber == nullptr || win32_msg_fiber == nullptr) {
        return false;
    }

    win32_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!win32_timer) {
        return false;
    }

    const u64 arena_size = sizeof(Win32Window) * 100;
    win32_arena = alloc_arena(arena_size, arena_size);
    return true;
}

void *os_reserve(u64 size_in_byte) {
    void *preserved = VirtualAlloc(nullptr, size_in_byte, MEM_RESERVE, PAGE_READWRITE);
    return preserved;
}

void *os_commit(void *ptr, u64 size_in_byte) {
    void *pcommitted = VirtualAlloc(ptr, size_in_byte, MEM_COMMIT, PAGE_READWRITE);
    if (pcommitted == nullptr) {
        os_free(ptr);
    }

    return pcommitted;
}

void os_free(void *ptr) {
    VirtualFree(ptr, 0, MEM_RELEASE);
}

vg_internal Win32Window *alloc_w32_window() {
    Win32Window *window = win32_free_window;
    if (window != nullptr) {
        sll_stack_pop(win32_free_window);
    } else {
        window = win32_arena->push_array<Win32Window>(1);
        memset(window, 0, sizeof(window));
    }

    return window;
}

OsHandle os_create_window(std::string_view title, u32 width, u32 height) {
    Win32Window *window = alloc_w32_window();

    Checkpoint        checkpoint = win32_arena->checkpoint();
    std::wstring_view title16 = to_utf16(title, win32_arena);

    window->hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        L"mini_vg",
        title16.data(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        (s32)width,
        (s32)height,
        NULL,
        NULL,
        GetModuleHandleA(nullptr),
        window
    );
    window->fullscreen = false;
    window->menu_op = false;
    window->before_mouse = {0, 0};
    ShowWindow(window->hwnd, SW_SHOW);

    win32_arena->restore(checkpoint);

    return (OsHandle)window;
}

void os_set_window_title(OsHandle window, std::string_view title) {
    Checkpoint        checkpoint = win32_arena->checkpoint();
    std::wstring_view title16 = to_utf16(title, win32_arena);
    Win32Window      *w32_window = (Win32Window *)window;
    SetWindowTextW(w32_window->hwnd, title16.data());
    win32_arena->restore(checkpoint);
}

void os_destroy_window(OsHandle window) {
    Win32Window *w32_window = (Win32Window*)window;
    DestroyWindow(w32_window->hwnd);
    sll_stack_push(win32_free_window, w32_window);
}

OsEventList *os_poll_events(Arena *arena) {
    win32_evt_arena = arena;
    win32_evt_list = arena->push_array<OsEventList>(1);
    win32_evt_list->count = 0;
    win32_evt_list->first = nullptr;
    win32_evt_list->last = nullptr;
    SwitchToFiber(win32_msg_fiber);
    win32_evt_arena = nullptr;
    return win32_evt_list;
}

void os_pcore_ecore_count(u32 *pcore, u32 *ecore) {
    Checkpoint checkpoint = win32_arena->checkpoint();

	DWORD buffer_length = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &buffer_length);
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *cpu_infos = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)win32_arena->push(buffer_length, 8);
	GetLogicalProcessorInformationEx(RelationProcessorCore, cpu_infos, &buffer_length);

	u32 max_efficiency = 0;
	u32 min_efficiency = 1000;
	u32 max_count = 0;
	u32 min_count = 0;
	for (DWORD at = 0; at < buffer_length;) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *core_infos = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)&(((u8*)cpu_infos)[at]);

		u32 efficiency = core_infos->Processor.EfficiencyClass;
		if (efficiency > max_efficiency) {
			max_efficiency = efficiency;
			max_count = 0;
		}
		if (efficiency < min_efficiency) {
			min_efficiency = efficiency;
			min_count = 0;
		}

		if (efficiency == max_efficiency) max_count += 1;
		if (efficiency == min_efficiency) min_count += 1;

		at += core_infos->Size;
	}

    *pcore = max_count;
    *ecore = min_count;

    win32_arena->restore(checkpoint);
}

u32 os_pcore_count() {
    u32 pcore, ecore;
    os_pcore_ecore_count(&pcore, &ecore);
    return pcore;
}

u32 os_ecore_count() {
    u32 pcore, ecore;
    os_pcore_ecore_count(&pcore, &ecore);
    return ecore;
}

void os_freeze_cursor(bool freeze) {
    if (freeze) {
        RECT clip_rect;
        POINT cursor;
        GetCursorPos(&cursor);
        clip_rect.left = cursor.x;
        clip_rect.top = cursor.y;
        clip_rect.right = clip_rect.left + 1;
        clip_rect.bottom = clip_rect.top + 1;
        ClipCursor(&clip_rect);
    } else {
        ClipCursor(nullptr);
    }
}

vg_internal bool is_cursor_visible() {
    CURSORINFO info;
    info.cbSize = sizeof(CURSORINFO);
    GetCursorInfo(&info);

    return info.flags == CURSOR_SHOWING;
}

void os_show_cursor(bool show) {
    while (is_cursor_visible() != show) {
        ShowCursor(show);
    }
}

HWND os_hwnd_from_handle(OsHandle handle) {
    Win32Window *window = (Win32Window*)handle;
    return window->hwnd;
}

int entry_point();
int APIENTRY WinMain(HINSTANCE hinst, HINSTANCE hinst_prev, PSTR cmdline, int cmdshow) {
    if (win32_init() == false) {
        return -1;
    }

    return entry_point();
}

#endif
