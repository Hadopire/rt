#pragma once

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------ BASE ---------------------------------

// Arch

#if defined(__clang__)

# define COMPILER_CLANG 1

# if defined(_WIN32)
#  define OS_WINDOWS 1
# elif defined(__gnu_linux__) || defined(__linux__)
#  define OS_LINUX 1
# elif defined(__APPLE__) && defined(__MACH__)
#  define OS_MAC 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# elif defined(i386) || defined(__i386) || defined(__i386__)
#  define ARCH_X86 1
# elif defined(__aarch64__)
#  define ARCH_ARM64 1
# elif defined(__arm__)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#elif defined(_MSC_VER)

# define COMPILER_MSVC 1

# if _MSC_VER >= 1920
#  define COMPILER_MSVC_YEAR 2019
# elif _MSC_VER >= 1910
#  define COMPILER_MSVC_YEAR 2017
# elif _MSC_VER >= 1900
#  define COMPILER_MSVC_YEAR 2015
# elif _MSC_VER >= 1800
#  define COMPILER_MSVC_YEAR 2013
# elif _MSC_VER >= 1700
#  define COMPILER_MSVC_YEAR 2012
# elif _MSC_VER >= 1600
#  define COMPILER_MSVC_YEAR 2010
# elif _MSC_VER >= 1500
#  define COMPILER_MSVC_YEAR 2008
# elif _MSC_VER >= 1400
#  define COMPILER_MSVC_YEAR 2005
# else
#  define COMPILER_MSVC_YEAR 0
# endif

# if defined(_WIN32)
#  define OS_WINDOWS 1
#  if defined(_DEBUG)
#    define BUILD_DEBUG 1
#  endif
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(_M_AMD64)
#  define ARCH_X64 1
# elif defined(_M_IX86)
#  define ARCH_X86 1
# elif defined(_M_ARM64)
#  define ARCH_ARM64 1
# elif defined(_M_ARM)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#elif defined(__GNUC__) || defined(__GNUG__)

# define COMPILER_GCC 1

# if defined(__gnu_linux__) || defined(__linux__)
#  define OS_LINUX 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# elif defined(i386) || defined(__i386) || defined(__i386__)
#  define ARCH_X86 1
# elif defined(__aarch64__)
#  define ARCH_ARM64 1
# elif defined(__arm__)
#  define ARCH_ARM32 1
# else
#  error Architecture not supported.
# endif

#else
# error Compiler not supported.
#endif

#if defined(ARCH_X64)
# define ARCH_64BIT 1
#elif defined(ARCH_X86)
# define ARCH_32BIT 1
#endif

#if ARCH_ARM32 || ARCH_ARM64 || ARCH_X64 || ARCH_X86
# define ARCH_LITTLE_ENDIAN 1
#else
# error Endianness of this architecture not understood by context cracker.
#endif

#if defined(__cplusplus)
# define LANG_CPP 1
#else
# define LANG_C 1
#endif

#if !defined(ARCH_32BIT)
# define ARCH_32BIT 0
#endif
#if !defined(ARCH_64BIT)
# define ARCH_64BIT 0
#endif
#if !defined(ARCH_X64)
# define ARCH_X64 0
#endif
#if !defined(ARCH_X86)
# define ARCH_X86 0
#endif
#if !defined(ARCH_ARM64)
# define ARCH_ARM64 0
#endif
#if !defined(ARCH_ARM32)
# define ARCH_ARM32 0
#endif
#if !defined(COMPILER_MSVC)
# define COMPILER_MSVC 0
#endif
#if !defined(COMPILER_GCC)
# define COMPILER_GCC 0
#endif
#if !defined(COMPILER_CLANG)
# define COMPILER_CLANG 0
#endif
#if !defined(OS_WINDOWS)
# define OS_WINDOWS 0
#endif
#if !defined(OS_LINUX)
# define OS_LINUX 0
#endif
#if !defined(OS_MAC)
# define OS_MAC 0
#endif
#if !defined(LANG_CPP)
# define LANG_CPP 0
#endif
#if !defined(LANG_C)
# define LANG_C 0
#endif

// Qualifiers

#define vg_internal      static
#define vg_global        static
#define vg_local_persist static

// Class decorators

#define NoCopy(CLASS)   \
    CLASS(const CLASS &) = delete; \
    CLASS &operator=(const CLASS &) = delete;

#define NoMove(CLASS) \
    CLASS(CLASS &&) = delete;   \
    CLASS &operator=(CLASS &&) = delete;

#define NoCopyNoMove(CLASS) \
    NoCopy(CLASS)      \
    NoMove(CLASS)

// Helpers

#define array_count(x) (sizeof(x) / sizeof(x[0]))
#define stringify_(s) #s
#define stringify(s) stringify_(s)
#define glue_(a,b) a##b
#define glue(a,b) glue_(a,b)
#define ceil_integer_div(a,b) (((a) + (b) - 1)/(b))
#define round_up_pow2(x,b) ((x+b-1) & ~(b-1))

#define KiB(x) (x * (1ull << 10))
#define MiB(x) (x * (1ull << 20))
#define GiB(x) (x * (1ull << 30))

// Asserts

#if COMPILER_MSVC
# define trap() __debugbreak()
#elif COMPILER_CLANG || COMPILER_GCC
# define trap() __builtin_trap()
#else
# error Unknown trap intrinsic for this compiler.
#endif

#define assert_always(x) do{if(!(x)) {trap();}}while(0)
#if BUILD_DEBUG
# define vg_assert(x) assert_always(x)
#else
# define vg_assert(x) (void)(x)
#endif
#define invalid_path            vg_assert(!"Invalid Path!")
#define not_implemented         vg_assert(!"Not Implemented!")
#define no_op                   ((void)0)
#define vg_static_assert(C, ID) vg_global U8 glue(ID, __LINE__)[(C)?1:-1]

// Alignment

#if COMPILER_MSVC
# define vg_align(x) __declspec(align(x))
#elif COMPILER_CLANG || COMPILER_GCC
# define vg_align(x) __attribute__((aligned(x)))
#else
# error Unknown alignment directive for this compiler.
#endif

// Linked List Macros From Ryan Fleury

#define check_nil(nil,p) ((p) == 0 || (p) == nil)
#define set_nil(nil,p) ((p) = nil)

#define dll_insert_npz(nil,f,l,p,n,next,prev) (check_nil(nil,f) ? \
((f) = (l) = (n), set_nil(nil,(n)->next), set_nil(nil,(n)->prev)) :\
check_nil(nil,p) ? \
((n)->next = (f), (f)->prev = (n), (f) = (n), set_nil(nil,(n)->prev)) :\
((p)==(l)) ? \
((l)->next = (n), (n)->prev = (l), (l) = (n), set_nil(nil, (n)->next)) :\
(((!check_nil(nil,p) && check_nil(nil,(p)->next)) ? (0) : ((p)->next->prev = (n))), ((n)->next = (p)->next), ((p)->next = (n)), ((n)->prev = (p))))
#define dll_push_back_npz(nil,f,l,n,next,prev) dll_insert_npz(nil,f,l,l,n,next,prev)
#define dll_push_front_npz(nil,f,l,n,next,prev) dll_insert_npz(nil,l,f,f,n,prev,next)
#define dll_remove_npz(nil,f,l,n,next,prev) (((n) == (f) ? (f) = (n)->next : (0)),\
((n) == (l) ? (l) = (l)->prev : (0)),\
(check_nil(nil,(n)->prev) ? (0) :\
((n)->prev->next = (n)->next)),\
(check_nil(nil,(n)->next) ? (0) :\
((n)->next->prev = (n)->prev)))

#define sll_queue_push_nz(nil,f,l,n,next) check_nil(nil,f)?\
((f)=(l)=(n),set_nil(nil,(n)->next)):\
((l)->next=(n),(l)=(n),set_nil(nil,(n)->next)))
#define sll_queue_push_front_nz(nil,f,l,n,next) (check_nil(nil,f)?\
((f)=(l)=(n),set_nil(nil,(n)->next)):\
((n)->next=(f),(f)=(n)))
#define sll_queue_pop_nz(nil,f,l,next) ((f)==(l)?\
(set_nil(nil,f),set_nil(nil,l)):\
((f)=(f)->next))

#define sll_stack_push_n(f,n,next) ((n)->next=(f), (f)=(n))
#define sll_stack_pop_n(f,next) ((f)=(f)->next)

#define dll_insert_np(f,l,p,n,next,prev) dll_insert_npz(0,f,l,p,n,next,prev)
#define dll_push_back_np(f,l,n,next,prev) dll_push_back_npz(0,f,l,n,next,prev)
#define dll_push_front_np(f,l,n,next,prev) dll_push_front_npz(0,f,l,n,next,prev)
#define dll_remove_np(f,l,n,next,prev) dll_remove_npz(0,f,l,n,next,prev)
#define dll_insert(f,l,p,n) dll_insert_npz(0,f,l,p,n,next,prev)
#define dll_push_back(f,l,n) dll_push_back_npz(0,f,l,n,next,prev)
#define dll_push_front(f,l,n) dll_push_front_npz(0,f,l,n,next,prev)
#define dll_remove(f,l,n) dll_remove_npz(0,f,l,n,next,prev)

#define sll_queue_push_n(f,l,n,next) sll_queue_push_nz(0,f,l,n,next)
#define sll_queue_push_front_n(f,l,n,next) sll_queue_push_front_nz(0,f,l,n,next)
#define sll_queue_pop_n(f,l,next) sll_queue_pop_nz(0,f,l,next)
#define sll_queue_push(f,l,n) sll_queue_push_nz(0,f,l,n,next)
#define sll_queue_push_front(f,l,n) sll_queue_push_front_nz(0,f,l,n,next)
#define sll_queue_pop(f,l) sll_queue_pop_nz(0,f,l,next)

#define sll_stack_push(f,n) sll_stack_push_n(f,n,next)
#define sll_stack_pop(f) sll_stack_pop_n(f,next)

// Basic Types

typedef char               s8;
typedef unsigned char      u8;
typedef char               s16;
typedef unsigned short     u16;
typedef int                s32;
typedef unsigned int       u32;
typedef long long          s64;
typedef unsigned long long u64;
typedef float              f32;
typedef double             f64;

template <typename T>
union Range1 {
    struct {
        T min;
        T max;
    };
    T v[2];
};

typedef Range1<s32> Range1s32;
typedef Range1<u32> Range1u32;
typedef Range1<s64> Range1s64;
typedef Range1<u64> Range1u64;
typedef Range1<f32> Range1f32;
typedef Range1<f64> Range1f64;

// Constants

vg_global f32 pi32 = 3.1415926535897f;
vg_global f32 pi32_over_2 = pi32 / 2.f;

vg_global u32 sign32     = 0x80000000;
vg_global u32 exponent32 = 0x7F800000;
vg_global u32 mantissa32 = 0x007FFFFF;

vg_global u64 max_u64 = 0xffffffffffffffffull;
vg_global u32 max_u32 = 0xffffffff;
vg_global u16 max_u16 = 0xffff;
vg_global u8  max_u8  = 0xff;

vg_global s64 max_s64 = (s64)0x7fffffffffffffffll;
vg_global s32 max_s32 = (s32)0x7fffffff;
vg_global s16 max_s16 = (s16)0x7fff;
vg_global s8  max_s8  =  (s8)0x7f;

vg_global s64 min_s64 = (s64)0x8000000000000000ll;
vg_global s32 min_s32 = (s32)0x80000000;
vg_global s16 min_s16 = (s16)0x8000;
vg_global s8  min_s8  =  (s8)0x80;

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------ ARENA --------------------------------

struct Arena;

struct Checkpoint {
    u64 pos;
};

struct RAIICheckpoint {
    Arena     *arena = nullptr;
    Checkpoint checkpoint;

    RAIICheckpoint(Arena *arena);
    ~RAIICheckpoint();
};

struct Arena {
    Arena *prev;
    Arena *current;

    u64 base_pos;
    u64 pos;
    u64 commit;
    u64 reserved_size;
    u64 commit_size;

    template <typename T>
    T *push_array(u64 count = 1, u64 align = alignof(T)) {
        return (T*)push(count * sizeof(T), align);
    }

    void *push(u64 size, u64 align);
    void  pop_to(u64 pos);
    void  reset();

    Checkpoint     checkpoint();
    void           restore(Checkpoint checkpoint);
};

Arena *alloc_arena(u64 commit, u64 reserve);
void   release_arena(Arena **arena);


// ---------------------------------------------------------------
//  *************************************************************
// ------------------------- STRINGS -----------------------------

#include <string_view>

std::string_view  to_utf8(std::wstring_view wstr, Arena *arena);
std::wstring_view to_utf16(std::string_view str, Arena *arena);


// ---------------------------------------------------------------
//  *************************************************************
// ------------------------ MATH ---------------------------------

struct Mat4x4;
struct Quat;
struct Int2;
struct Vec2;
struct Int3;
struct Vec2;
struct Vec3;
struct Vec4;

struct Int2 {
    union {
        struct {
            s32 x, y;
        };
        s32 i[2];
    };

    Int2() = default;
    explicit Int2(s32 xy);
    Int2(s32 x, s32 y);
    Int2(const s32 *p_array);

    s32  &operator[](u64 idx);
    s32   operator[](u64 idx) const;
    Int2 &operator+=(const Int2 &rhs);
    Int2 &operator-=(const Int2 &rhs);
    Int2 &operator+=(s32 rhs);
    Int2 &operator-=(s32 rhs);
    Int2 &operator*=(s32 rhs);
    Int2 &operator/=(s32 rhs);
};
bool operator==(const Int2 &lhs, const Int2 &rhs);
bool operator!=(const Int2 &lhs, const Int2 &rhs);
Int2 operator+(const Int2 &lhs, const Int2 &rhs);
Int2 operator-(const Int2 &lhs, const Int2 &rhs);
Int2 operator-(const Int2 &rhs);
Int2 operator+(const s32 &lhs, const Int2 &rhs);
Int2 operator+(const Int2 &lhs, const s32 &rhs);
Int2 operator-(const s32 &lhs, const Int2 &rhs);
Int2 operator-(const Int2 &lhs, const s32 &rhs);
Int2 operator*(const s32 &lhs, const Int2 &rhs);
Int2 operator*(const Int2 &lhs, const s32 &rhs);
Int2 operator/(const Int2 &lhs, const s32 &rhs);
Int2 abs(Int2 a);

struct Vec2 {
    union {
        struct {
            f32 x, y;
        };
        f32 f[2];
    };

    Vec2() = default;
    explicit Vec2(s32 xy);
    explicit Vec2(f32 xy);
    Vec2(f32 x, f32 y);
    Vec2(const f32 *p_array);
    Vec2(const Int2 &vec);

    f32  &operator[](u64 idx);
    f32   operator[](u64 idx) const;
    Vec2 &operator+=(const Vec2 &rhs);
    Vec2 &operator-=(const Vec2 &rhs);
    Vec2 &operator+=(f32 rhs);
    Vec2 &operator-=(f32 rhs);
    Vec2 &operator*=(f32 rhs);
    Vec2 &operator/=(f32 rhs);

    f32  length() const;
    f32  sq_length() const;
};
bool operator==(const Vec2 &lhs, const Vec2 &rhs);
bool operator!=(const Vec2 &lhs, const Vec2 &rhs);
Vec2 operator+(const Vec2 &lhs, const Vec2 &rhs);
Vec2 operator-(const Vec2 &lhs, const Vec2 &rhs);
Vec2 operator-(const Vec2 &rhs);
Vec2 operator+(const f32 &lhs, const Vec2 &rhs);
Vec2 operator+(const Vec2 &lhs, const f32 &rhs);
Vec2 operator-(const f32 &lhs, const Vec2 &rhs);
Vec2 operator-(const Vec2 &lhs, const f32 &rhs);
Vec2 operator*(const f32 &lhs, const Vec2 &rhs);
Vec2 operator*(const Vec2 &lhs, const f32 &rhs);
// component-wise multiplication
Vec2 operator*(const Vec2 &lhs, const Vec2 &rhs);
Vec2 operator/(const Vec2 &lhs, const f32 &rhs);
f32  dot(const Vec2 &v1, const Vec2 &v2);
Vec2 normalize(const Vec2 &v);
Vec2 abs(Vec2 a);

struct Int3 {
    union {
        struct {
            s32 x, y, z;
        };
        s32 f[3];
    };

    Int3() = default;
    explicit Int3(s32 xyz);
    Int3(s32 x, s32 y, s32 z);
    Int3(const s32 *p_array);

    s32  &operator[](u64 idx);
    s32   operator[](u64 idx) const;
    Int3 &operator+=(const Int3 &rhs);
    Int3 &operator-=(const Int3 &rhs);
    Int3 &operator+=(s32 rhs);
    Int3 &operator-=(s32 rhs);
    Int3 &operator*=(s32 rhs);
    Int3 &operator/=(s32 rhs);
};
bool operator==(const Int3 &lhs, const Int3 &rhs);
bool operator!=(const Int3 &lhs, const Int3 &rhs);
Int3 operator+(const Int3 &lhs, const Int3 &rhs);
Int3 operator-(const Int3 &lhs, const Int3 &rhs);
Int3 operator-(const Int3 &rhs);
Int3 operator+(const s32 &lhs, const Int3 &rhs);
Int3 operator+(const Int3 &lhs, const s32 &rhs);
Int3 operator-(const s32 &lhs, const Int3 &rhs);
Int3 operator-(const Int3 &lhs, const s32 &rhs);
Int3 operator*(const s32 &lhs, const Int3 &rhs);
Int3 operator*(const Int3 &lhs, const s32 &rhs);
Int3 operator/(const Int3 &lhs, const s32 &rhs);
Int3 abs(Int3 a);

struct Vec3 {
    union {
        struct {
            f32 x, y, z;
        };
        f32 f[3];
    };

    Vec3() = default;
    explicit Vec3(s32 xyz);
    explicit Vec3(f32 xyz);
    Vec3(f32 x, f32 y, f32 z);
    Vec3(const f32 *p_array);
    Vec3(const Int3 &vec);
    Vec3(const Vec4 &vec);

    f32  &operator[](u64 idx);
    f32   operator[](u64 idx) const;
    Vec3 &operator+=(const Vec3 &rhs);
    Vec3 &operator-=(const Vec3 &rhs);
    Vec3 &operator+=(f32 rhs);
    Vec3 &operator-=(f32 rhs);
    Vec3 &operator*=(f32 rhs);
    Vec3 &operator/=(f32 rhs);

    f32  length() const;
    f32  sq_length() const;
};
bool operator==(const Vec3 &lhs, const Vec3 &rhs);
bool operator!=(const Vec3 &lhs, const Vec3 &rhs);
Vec3 operator+(const Vec3 &lhs, const Vec3 &rhs);
Vec3 operator-(const Vec3 &lhs, const Vec3 &rhs);
Vec3 operator-(const Vec3 &rhs);
Vec3 operator+(const f32 &lhs, const Vec3 &rhs);
Vec3 operator+(const Vec3 &lhs, const f32 &rhs);
Vec3 operator-(const f32 &lhs, const Vec3 &rhs);
Vec3 operator-(const Vec3 &lhs, const f32 &rhs);
Vec3 operator*(const f32 &lhs, const Vec3 &rhs);
Vec3 operator*(const Vec3 &lhs, const f32 &rhs);
// component-wise multiplication
Vec3 operator*(const Vec3 &lhs, const Vec3 &rhs);
Vec3 operator/(const Vec3 &lhs, const f32 &rhs);
f32  dot(const Vec3 &v1, const Vec3 &v2);
Vec3 cross(const Vec3 &v1, const Vec3 &v2);
Vec3 normalize(const Vec3 &v);
Vec3 abs(Vec3 a);
void orthonormal_basis(const Vec3 &v, Vec3 *b0, Vec3 *b1);

struct Vec4 {
    union {
        struct {
            f32 x, y, z, w;
        };
        f32 f[4];
    };

    Vec4() = default;
    explicit Vec4(s32 xyzw);
    explicit Vec4(f32 xyzw);
    Vec4(f32 x, f32 y, f32 z, f32 w);
    Vec4(const Vec3 &vec, f32 w);
    Vec4(const f32 *p_array);

    f32  &operator[](u64 idx);
    f32   operator[](u64 idx) const;
    Vec4 &operator+=(const Vec4 &rhs);
    Vec4 &operator-=(const Vec4 &rhs);
    Vec4 &operator+=(f32 rhs);
    Vec4 &operator-=(f32 rhs);
    Vec4 &operator*=(f32 rhs);
    Vec4 &operator/=(f32 rhs);

    f32  length() const;
    f32  sq_length() const;
};
bool operator==(const Vec4 &lhs, const Vec4 &rhs);
bool operator!=(const Vec4 &lhs, const Vec4 &rhs);
Vec4 operator+(const Vec4 &lhs, const Vec4 &rhs);
Vec4 operator-(const Vec4 &lhs, const Vec4 &rhs);
Vec4 operator-(const Vec4 &rhs);
Vec4 operator+(const f32 &lhs, const Vec4 &rhs);
Vec4 operator+(const Vec4 &lhs, const f32 &rhs);
Vec4 operator-(const f32 &lhs, const Vec4 &rhs);
Vec4 operator-(const Vec4 &lhs, const f32 &rhs);
Vec4 operator*(const f32 &lhs, const Vec4 &rhs);
Vec4 operator*(const Vec4 &lhs, const f32 &rhs);
// component-wise multiplication
Vec4 operator*(const Vec4 &lhs, const Vec4 &rhs);
Vec4 operator/(const Vec4 &lhs, const f32 &rhs);
f32  dot(const Vec4 &v1, const Vec4 &v2);
Vec4 normalize(const Vec4 &v);
Vec4 abs(Vec4 a);

struct Mat3x3 {
    union {
        struct {
            f32 _11, _12, _13;
            f32 _21, _22, _23;
            f32 _31, _32, _33;
        };
        struct {
            Vec3 x, y, z;
        };
        Vec3 v[3];
        f32  f[9];
    };

    Mat3x3() = default;
    Mat3x3(f32 _11, f32 _12, f32 _13,
           f32 _21, f32 _22, f32 _23,
           f32 _31, f32 _32, f32 _33);
    Mat3x3(const Vec3 &x, const Vec3 &y, const Vec3 &z);
    Mat3x3(const f32 *p_array);
    Mat3x3(const Mat4x4 &rhs);
    static Mat3x3 identity();

    Vec3       &operator[](u64 idx);
    const Vec3 &operator[](u64 idx) const;
    Mat3x3     &operator*=(const Mat3x3 &rhs);
    Mat3x3     &operator*=(f32 rhs);

    f32 det() const;
    Mat3x3 inverse() const;
    Mat3x3 transpose() const;

    static Mat3x3 rotation(const Vec3 &eulers);
    static Mat3x3 rotation(const Vec3 &axis, f32 angle);
    static Mat3x3 rotation_x(f32 angle);
    static Mat3x3 rotation_y(f32 angle);
    static Mat3x3 rotation_z(f32 angle);
    static Mat3x3 scale(const Vec3 &scale);
};
bool   operator==(const Mat3x3 &lhs, const Mat3x3 &rhs);
bool   operator!=(const Mat3x3 &lhs, const Mat3x3 &rhs);
Mat3x3 operator*(const Mat3x3 &lhs, const Mat3x3 &rhs);
Mat3x3 operator*(const f32 lhs, const Mat3x3 &rhs);
Mat3x3 operator*(const Mat3x3 &lhs, const f32 rhs);

struct Mat4x4 {
    union {
        struct {
            f32 _11, _12, _13, _14;
            f32 _21, _22, _23, _24;
            f32 _31, _32, _33, _34;
            f32 _41, _42, _43, _44;
        };
        struct {
            Vec4 x, y, z, w;
        };
        Vec4 v[4];
        f32  f[16];
    };

    Mat4x4() = default;
    Mat4x4(f32 _11, f32 _12, f32 _13, f32 _14,
           f32 _21, f32 _22, f32 _23, f32 _24,
           f32 _31, f32 _32, f32 _33, f32 _34,
           f32 _41, f32 _42, f32 _43, f32 _44);
    Mat4x4(const Vec4 &x, const Vec4 &y, const Vec4 &z, const Vec4 &w);
    Mat4x4(const f32 *p_array);
    static Mat4x4 identity();

    Vec4       &operator[](u64 idx);
    const Vec4 &operator[](u64 idx) const;
    Mat4x4     &operator*=(const Mat4x4 &rhs);
    Mat4x4     &operator*=(f32 rhs);

    f32    det() const;
    void   transform_decomposition(Vec3 *scale, Mat4x4 *rotation, Vec3 *translation) const;
    void   transform_decomposition(Vec3 *scale, Quat *rotation, Vec3 *translation) const;
    Mat4x4 inverse() const;
    Mat4x4 transpose() const;

    static Mat4x4 rotation(const Vec3 &eulers);
    static Mat4x4 rotation(const Vec3 &axis, f32 angle);
    static Mat4x4 rotation_x(f32 angle);
    static Mat4x4 rotation_y(f32 angle);
    static Mat4x4 rotation_z(f32 angle);
    static Mat4x4 translation(const Vec3 &translation);
    static Mat4x4 scale(const Vec3 &scale);

    static Mat4x4 look_at(const Vec3 &eye, const Vec3 &at, const Vec3 &up);
    static Mat4x4 look_to(const Vec3 &eye, const Vec3 &dir, const Vec3 &up);
    static Mat4x4 perspective(f32 fov_y, f32 aspect, f32 znear, f32 zfar);
    static Mat4x4 orthographic(f32 width, f32 height, f32 znear, f32 zfar);
};
bool   operator==(const Mat4x4 &lhs, const Mat4x4 &rhs);
bool   operator!=(const Mat4x4 &lhs, const Mat4x4 &rhs);
Mat4x4 operator*(const Mat4x4 &lhs, const Mat4x4 &rhs);
Mat4x4 operator*(const f32 lhs, const Mat4x4 &rhs);
Mat4x4 operator*(const Mat4x4 &lhs, const f32 rhs);

Vec3 operator*(const Vec3 &lhs, const Mat3x3 &rhs);
Vec3 operator*(const Vec3 &lhs, const Mat4x4 &rhs);
Vec4 operator*(const Vec4 &lhs, const Mat4x4 &rhs);

Vec2 min(const Vec2 &a, const Vec2 &b);
Vec3 min(const Vec3 &a, const Vec3 &b);
Vec4 min(const Vec4 &a, const Vec4 &b);
Vec2 max(const Vec2 &a, const Vec2 &b);
Vec3 max(const Vec3 &a, const Vec3 &b);
Vec4 max(const Vec4 &a, const Vec4 &b);

Vec4 create_plane(const Vec3 &point, const Vec3 &normal);
f32  plane_point_sdf(const Vec4 &plane, const Vec3 point);

struct Quat {
    union {
        struct {
            f32 b, c, d, a;
        };
        f32 f[4];
    };

    Quat() = default;
    Quat(const Vec3 &axis, f32 angle);
    Quat(f32 b, f32 c, f32 d, f32 a);
    Quat(const f32 *p_array);
    Quat(const Mat3x3 &mat);
    static Quat identity();

    f32  &operator[](u64 idx);
    f32   operator[](u64 idx) const;
    Quat &operator*=(const Quat &rhs);

    Mat4x4 to_matrix() const;
};
bool operator==(const Quat &lhs, const Quat &rhs);
bool operator!=(const Quat &lhs, const Quat &rhs);
Quat operator*(const Quat &rhs, const Quat &lhs);

Vec3 line_point_nearest(const Vec3 &origin, const Vec3 &direction, const Vec3 &point);

struct RectF32 {
    Vec2 min;
    Vec2 max;
};

struct RectS32 {
    Int2 min;
    Int2 max;
};

struct BoxS32 {
    s32 left;
    s32 top;
    s32 front;
    s32 right;
    s32 bottom;
    s32 back;
};

bool f32_cmp(f32 a, f32 b, f32 threshold = 0.0001f);
f32  saturate(f32 value);
Vec2 saturate(const Vec2 &value);
Vec3 saturate(const Vec3 &value);
Vec4 saturate(const Vec4 &value);
template <typename T>
T lerp(T a, T b, f32 t) {
    return a + (b - a) * saturate(t);
}

void nearest_euler(Vec3 *euler, const Vec3 &hint);
// extract two possible triplets of ZXY euler angles from a rotation matrix
void matrix_to_euler2(const Mat3x3 &rotation, Vec3 eulers[2]);
Vec3 matrix_to_euler(const Mat3x3 &rotation, const Vec3 &hint);
Vec3 round_near_zero(const Vec3 &value, u32 precision);

f32 to_radian(f32 deg);
f32 to_degree(f32 rad);

// ---------------------------------------------------------------
//  *************************************************************
// ------------------------- OS ----------------------------------

typedef u64 OsHandle;

enum Key {
    Key_Unknown = 0x00,

    Key_A = 0x04, // keyboard a and A
    Key_B = 0x05, // keyboard b and B
    Key_C = 0x06, // keyboard c and C
    Key_D = 0x07, // keyboard d and D
    Key_E = 0x08, // keyboard e and E
    Key_F = 0x09, // keyboard f and F
    Key_G = 0x0a, // keyboard g and G
    Key_H = 0x0b, // keyboard h and H
    Key_I = 0x0c, // keyboard i and I
    Key_J = 0x0d, // keyboard j and J
    Key_K = 0x0e, // keyboard k and K
    Key_L = 0x0f, // keyboard l and L
    Key_M = 0x10, // keyboard m and M
    Key_N = 0x11, // keyboard n and N
    Key_O = 0x12, // keyboard o and O
    Key_P = 0x13, // keyboard p and P
    Key_Q = 0x14, // keyboard q and Q
    Key_R = 0x15, // keyboard r and R
    Key_S = 0x16, // keyboard s and S
    Key_T = 0x17, // keyboard t and T
    Key_U = 0x18, // keyboard u and U
    Key_V = 0x19, // keyboard v and V
    Key_W = 0x1a, // keyboard w and W
    Key_X = 0x1b, // keyboard x and X
    Key_Y = 0x1c, // keyboard y and Y
    Key_Z = 0x1d, // keyboard z and Z

    Key_K1 = 0x1e, // keyboard 1 and !
    Key_K2 = 0x1f, // keyboard 2 and @
    Key_K3 = 0x20, // keyboard 3 and #
    Key_K4 = 0x21, // keyboard 4 and $
    Key_K5 = 0x22, // keyboard 5 and %
    Key_K6 = 0x23, // keyboard 6 and ^
    Key_K7 = 0x24, // keyboard 7 and &
    Key_K8 = 0x25, // keyboard 8 and *
    Key_K9 = 0x26, // keyboard 9 and (
    Key_K0 = 0x27, // keyboard 0 and )

    Key_Return     = 0x28, // keyboard return (enter)
    Key_Esc        = 0x29, // keyboard escape
    Key_Backspace  = 0x2a, // keyboard delete (backspace)
    Key_Tab        = 0x2b, // keyboard tab
    Key_Space      = 0x2c, // keyboard spacebar
    Key_Minus      = 0x2d, // keyboard - and _
    Key_Equal      = 0x2e, // keyboard = and +
    Key_Leftbrace  = 0x2f, // keyboard [ and {
    Key_Rightbrace = 0x30, // keyboard ] and }
    Key_Backslash  = 0x31, // keyboard \ and |
    Key_Hashtilde  = 0x32, // keyboard non-us # and ~
    Key_Semicolon  = 0x33, // keyboard ; and :
    Key_Apostrophe = 0x34, // keyboard ' and "
    Key_Grave      = 0x35, // keyboard ` and ~
    Key_Comma      = 0x36, // keyboard , and <
    Key_Period     = 0x37, // keyboard . and >
    Key_Slash      = 0x38, // keyboard / and ?
    Key_Capslock   = 0x39, // keyboard caps lock

    Key_F1  = 0x3a, // keyboard f1
    Key_F2  = 0x3b, // keyboard f2
    Key_F3  = 0x3c, // keyboard f3
    Key_F4  = 0x3d, // keyboard f4
    Key_F5  = 0x3e, // keyboard f5
    Key_F6  = 0x3f, // keyboard f6
    Key_F7  = 0x40, // keyboard f7
    Key_F8  = 0x41, // keyboard f8
    Key_F9  = 0x42, // keyboard f9
    Key_F10 = 0x43, // keyboard f10
    Key_F11 = 0x44, // keyboard f11
    Key_F12 = 0x45, // keyboard f12

    Key_Printscreen = 0x46, // keyboard print screen
    Key_ScrollLock  = 0x47, // keyboard scroll lock
    Key_Pause       = 0x48, // keyboard pause
    Key_Insert      = 0x49, // keyboard insert
    Key_Home        = 0x4a, // keyboard home
    Key_PageUp      = 0x4b, // keyboard page up
    Key_Del         = 0x4c, // keyboard delete forward
    Key_End         = 0x4d, // keyboard end
    Key_PageDown    = 0x4e, // keyboard page down
    Key_Right       = 0x4f, // keyboard right arrow
    Key_Left        = 0x50, // keyboard left arrow
    Key_Down        = 0x51, // keyboard down arrow
    Key_Up          = 0x52, // keyboard up arrow

    Key_NumLock     = 0x53, // keyboard num lock and clear
    Key_NumSlash    = 0x54, // keypad /
    Key_NumAsterisk = 0x55, // keypad *
    Key_NumMinus    = 0x56, // keypad -
    Key_NumPlus     = 0x57, // keypad +
    Key_NumEnter    = 0x58, // keypad enter
    Key_Num1        = 0x59, // keypad 1 and end
    Key_Num2        = 0x5a, // keypad 2 and down arrow
    Key_Num3        = 0x5b, // keypad 3 and pagedn
    Key_Num4        = 0x5c, // keypad 4 and left arrow
    Key_Num5        = 0x5d, // keypad 5
    Key_Num6        = 0x5e, // keypad 6 and right arrow
    Key_Num7        = 0x5f, // keypad 7 and home
    Key_Num8        = 0x60, // keypad 8 and up arrow
    Key_Num9        = 0x61, // keypad 9 and page up
    Key_Num0        = 0x62, // keypad 0 and insert
    Key_NumPeriod   = 0x63, // keypad . and delete

    Key_Nonusbackslash = 0x64, // keyboard \ | (iso keyboards only)
    Key_Compose        = 0x65, // keyboard application
    Key_Power          = 0x66, // keyboard power
    Key_NumEqual       = 0x67, // keypad =

    Key_F13 = 0x68, // keyboard f13
    Key_F14 = 0x69, // keyboard f14
    Key_F15 = 0x6a, // keyboard f15
    Key_F16 = 0x6b, // keyboard f16
    Key_F17 = 0x6c, // keyboard f17
    Key_F18 = 0x6d, // keyboard f18
    Key_F19 = 0x6e, // keyboard f19
    Key_F20 = 0x6f, // keyboard f20
    Key_F21 = 0x70, // keyboard f21
    Key_F22 = 0x71, // keyboard f22
    Key_F23 = 0x72, // keyboard f23
    Key_F24 = 0x73, // keyboard f24

    Key_Execute    = 0x74, // keyboard execute
    Key_Help       = 0x75, // keyboard help
    Key_Props      = 0x76, // keyboard menu
    Key_Select     = 0x77, // keyboard select
    Key_Stop       = 0x78, // keyboard stop
    Key_Again      = 0x79, // keyboard again
    Key_Undo       = 0x7a, // keyboard undo
    Key_Cut        = 0x7b, // keyboard cut
    Key_Copy       = 0x7c, // keyboard copy
    Key_Paste      = 0x7d, // keyboard paste
    Key_Find       = 0x7e, // keyboard find
    Key_Mute       = 0x7f, // keyboard mute
    Key_Volumeup   = 0x80, // keyboard volume up
    Key_Volumedown = 0x81, // keyboard volume down

    Key_NumComma       = 0x85, // keypad comma
    Key_NumEqualsas400 = 0x86, // keypad equal sign (used on as/400 keyboards)

    Key_International1 = 0x87, // keyboard international1
    Key_International2 = 0x88, // keyboard international2
    Key_International3 = 0x89, // keyboard international3
    Key_International4 = 0x8a, // keyboard international4
    Key_International5 = 0x8b, // keyboard international5
    Key_International6 = 0x8c, // keyboard international6
    Key_International7 = 0x8d, // keyboard international7
    Key_International8 = 0x8e, // keyboard international8
    Key_International9 = 0x8f, // keyboard international9

    Key_Lang1 = 0x90, // keyboard lang1
    Key_Lang2 = 0x91, // keyboard lang2
    Key_Lang3 = 0x92, // keyboard lang3
    Key_Lang4 = 0x93, // keyboard lang4
    Key_Lang5 = 0x94, // keyboard lang5
    Key_Lang6 = 0x95, // keyboard lang6
    Key_Lang7 = 0x96, // keyboard lang7
    Key_Lang8 = 0x97, // keyboard lang8
    Key_Lang9 = 0x98, // keyboard lang9

    Key_Alterase   = 0x99, // keyboard alternate erase
    Key_Sysreq     = 0x9a, // keyboard sysreq/attention
    Key_Cancel     = 0x9b, // keyboard cancel
    Key_Clear      = 0x9c, // keyboard clear
    Key_Prior      = 0x9d, // keyboard prior
    Key_Return2    = 0x9e, // keyboard return
    Key_Separator  = 0x9f, // keyboard separator
    Key_Kout       = 0xa0, // keyboard out
    Key_Oper       = 0xa1, // keyboard oper
    Key_Clearagain = 0xa2, // keyboard clear/again
    Key_Crsel      = 0xa3, // keyboard crsel/props
    Key_Exsel      = 0xa4, // keyboard exsel

    Key_Num00                 = 0xb0, // keypad 00
    Key_Num000                = 0xb1, // keypad 000
    Key_NumThousandsSeparator = 0xb2, // thousands separator
    Key_NumDecimalSeparator   = 0xb3, // decimal separator
    Key_NumCurrencyUnit       = 0xb4, // currency unit
    Key_NumCurrencySubunit    = 0xb5, // currency sub-unit
    Key_NumLeftParen          = 0xb6, // keypad (
    Key_NumRightParen         = 0xb7, // keypad )
    Key_NumLeftBrace          = 0xb8, // keypad {
    Key_NumRightBrace         = 0xb9, // keypad }
    Key_NumTab                = 0xba, // keypad tab
    Key_NumBackspace          = 0xbb, // keypad backspace
    Key_NumA                  = 0xbc, // keypad a
    Key_NumB                  = 0xbd, // keypad b
    Key_NumC                  = 0xbe, // keypad c
    Key_NumD                  = 0xbf, // keypad d
    Key_NumE                  = 0xc0, // keypad e
    Key_NumF                  = 0xc1, // keypad f
    Key_NumXor                = 0xc2, // keypad xor
    Key_NumPower              = 0xc3, // keypad ^
    Key_NumPercent            = 0xc4, // keypad %
    Key_NumLess               = 0xc5, // keypad <
    Key_NumGreater            = 0xc6, // keypad >
    Key_NumAmpersand          = 0xc7, // keypad &
    Key_NumDblampersand       = 0xc8, // keypad &&
    Key_NumVerticalbar        = 0xc9, // keypad |
    Key_NumDblverticalbar     = 0xca, // keypad ||
    Key_NumColon              = 0xcb, // keypad :
    Key_NumHash               = 0xcc, // keypad #
    Key_NumSpace              = 0xcd, // keypad space
    Key_NumAt                 = 0xce, // keypad @
    Key_NumExclam             = 0xcf, // keypad !
    Key_NumMemstore           = 0xd0, // keypad memory store
    Key_NumMemrecall          = 0xd1, // keypad memory recall
    Key_NumMemclear           = 0xd2, // keypad memory clear
    Key_NumMemadd             = 0xd3, // keypad memory add
    Key_NumMemsubtract        = 0xd4, // keypad memory subtract
    Key_NumMemmultiply        = 0xd5, // keypad memory multiply
    Key_NumMemdivide          = 0xd6, // keypad memory divide
    Key_NumPlusminus          = 0xd7, // keypad +/-
    Key_NumClear              = 0xd8, // keypad clear
    Key_NumClearentry         = 0xd9, // keypad clear entry
    Key_NumBinary             = 0xda, // keypad binary
    Key_NumOctal              = 0xdb, // keypad octal
    Key_NumDecimal            = 0xdc, // keypad decimal
    Key_NumHexadecimal        = 0xdd, // keypad hexadecimal

    Key_LeftCtrl   = 0xe0, // keyboard left control
    Key_LeftShift  = 0xe1, // keyboard left shift
    Key_LeftAlt    = 0xe2, // keyboard left alt
    Key_LeftMeta   = 0xe3, // keyboard left gui
    Key_RightCtrl  = 0xe4, // keyboard right control
    Key_RightShift = 0xe5, // keyboard right shift
    Key_RightAlt   = 0xe6, // keyboard right alt
    Key_RightMeta  = 0xe7, // keyboard right gui
    
    Key_MouseButtonLeft,
    Key_MouseButtonRight,
    Key_MouseButtonMiddle,
    Key_MouseButtonX1,
    Key_MouseButtonX2,

    Key_Count,
};

enum OsEventType {
    OsEventType_KeyPress,
    OsEventType_KeyRelease,
    OsEventType_MouseMoveAbs,
    OsEventType_MouseMoveRel,
    OsEventType_Scroll,
    OsEventType_WindowResize,
    OsEventType_WindowLoseFocus,
    OsEventType_WindowGainFocus,
    OsEventType_WindowClosed,
    OsEventType_Quit,

    OsEventType_Count,
};

struct OsEvent {
    OsEvent    *next;
    OsEvent    *prev;
    OsHandle    window;
    OsEventType type;
    Key         key;
    Int2        pos;
    Int2        delta;
    Int2        size;
};

struct OsEventList {
    OsEvent *first;
    OsEvent *last;
    u64      count;
};

void        *os_reserve(u64 size_in_byte);
void        *os_commit(void *ptr, u64 size_in_byte);
void         os_free(void *ptr);
OsHandle     os_create_window(std::string_view title, u32 width, u32 height);
void         os_set_window_title(OsHandle window, std::string_view title);
void         os_destroy_window(OsHandle window);
OsEventList *os_poll_events(Arena *arena);
void         os_pcore_ecore_count(u32 *pcore, u32 *ecore);
u32          os_pcore_count();
u32          os_ecore_count();
void         os_freeze_cursor(bool freeze);
void         os_show_cursor(bool show);

#if OS_WINDOWS
#   define NOMINMAX
#   include <windows.h>
    HWND os_hwnd_from_handle(OsHandle handle);
#endif