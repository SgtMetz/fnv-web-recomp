// dynamic_cast by decorated type name, on MSVC x86 RTTI (the data image's and clang-cl's use the same layout).
// The overlay uses it where the decomp casts to a class that is only forward-declared at that point (VC2008
// accepted it; the target's RTTI is all a dynamic_cast needs). Non-virtual bases only (mdisp), which is all the
// game's form hierarchy uses.
#include <string.h>

namespace
{
struct TypeDescriptor
{
    const void* vftable;
    void* spare;
    char name[1];
};
struct PMD
{
    int mdisp, pdisp, vdisp;
};
struct BaseClassDescriptor
{
    TypeDescriptor* type;
    unsigned int numContainedBases;
    PMD where;
    unsigned int attributes;
};
struct ClassHierarchyDescriptor
{
    unsigned int signature, attributes, numBaseClasses;
    BaseClassDescriptor** baseClasses;
};
struct CompleteObjectLocator
{
    unsigned int signature, offset, cdOffset;
    TypeDescriptor* type;
    ClassHierarchyDescriptor* hierarchy;
};

// ".?AVName@@" and ".?AUName@@" name the same type for this purpose (class vs struct key)
bool same_type(const char* a, const char* b)
{
    return a[0] == '.' && b[0] == '.' && a[1] == '?' && b[1] == '?' && !strcmp(a + 3, b + 3);
}
}

extern "C" void* __cdecl fnv_dynamic_cast_by_name(void* object, const char* decorated)
{
    if (!object)
        return nullptr;
    void** vftable = *(void***)object;
    auto col = (const CompleteObjectLocator*)vftable[-1];
    char* complete = (char*)object - col->offset;
    const ClassHierarchyDescriptor* h = col->hierarchy;
    for (unsigned int i = 0; i < h->numBaseClasses; ++i)
    {
        const BaseClassDescriptor* b = h->baseClasses[i];
        if (b->where.pdisp == -1 && same_type(b->type->name, decorated))
            return complete + b->where.mdisp;
    }
    return nullptr;
}
