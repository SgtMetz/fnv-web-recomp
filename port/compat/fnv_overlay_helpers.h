// Force-included into units that went through tools/overlay.py. Helpers the overlay's rewrites refer to.
#pragma once
#ifdef __cplusplus

// VC2008 let a temporary bind to a non-const reference (and allowed `(T&)rvalue`); the temporary lives until the
// end of the full expression. fnv_lvalue(expr) gives the same: an lvalue naming the temporary for that long.
template <class T> inline T& fnv_lvalue(T&& value)
{
    return value;
}
template <class T> inline T& fnv_lvalue(T& value)
{
    return value;
}

// `X::X();` inside a member of X (VC2008: run X's constructor on this). Placement construction does the same.
inline void* operator new(unsigned int, void* where, int fnv_tag) { (void)fnv_tag; return where; }
inline void operator delete(void*, void*, int) {}
template <class T> inline void fnv_construct_in_place(T* self)
{
    ::new ((void*)self, 0) T();
}

// `&InterlockedDecrement` as a __stdcall function pointer (VC2008: the kernel32 import; clang: an intrinsic with
// no such address). Same operations, as real __stdcall functions.
inline long __stdcall fnv_InterlockedIncrement_stdcall(volatile long* p) { return __sync_add_and_fetch(p, 1); }
inline long __stdcall fnv_InterlockedDecrement_stdcall(volatile long* p) { return __sync_sub_and_fetch(p, 1); }
inline long __stdcall fnv_InterlockedExchange_stdcall(volatile long* p, long v) { return __sync_lock_test_and_set(p, v); }
inline long __stdcall fnv_InterlockedExchangeAdd_stdcall(volatile long* p, long v) { return __sync_fetch_and_add(p, v); }
inline long __stdcall fnv_InterlockedCompareExchange_stdcall(volatile long* p, long x, long c)
{
    return __sync_val_compare_and_swap(p, c, x);
}

// dynamic_cast<T*>(p) where T is only forward-declared at that point (port/compat/fnv_rtti.cpp)
extern "C" void* __cdecl fnv_dynamic_cast_by_name(void* object, const char* decorated);
template <class T, class U> inline T* fnv_dyncast(const char* decorated, U* p)
{
    return (T*)fnv_dynamic_cast_by_name((void*)p, decorated);
}

#endif
