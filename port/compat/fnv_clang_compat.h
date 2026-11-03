// Forced include for every decomp unit compiled by clang-cl (tools/compile_census.py).
// MSVC treats __assume(0) as "no value is produced here" and still emits the epilogue; clang makes it unreachable and
// emits ud2 (e.g. PathingLocation::HasValidTriangle 0x004ff7e0, really a destructor body). Keep the epilogue.
#ifdef __assume
#undef __assume
#endif
#define __assume(x) ((void)0)
