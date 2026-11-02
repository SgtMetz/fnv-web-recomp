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

// dynamic_cast<T*>(p) where T is only forward-declared at that point (port/compat/fnv_rtti.cpp)
extern "C" void* __cdecl fnv_dynamic_cast_by_name(void* object, const char* decorated);
template <class T, class U> inline T* fnv_dyncast(const char* decorated, U* p)
{
    return (T*)fnv_dynamic_cast_by_name((void*)p, decorated);
}

#endif
