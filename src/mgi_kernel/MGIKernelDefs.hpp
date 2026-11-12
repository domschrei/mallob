#ifndef MGI_KERNEL_DEFS
#define MGI_KERNEL_DEFS

#ifdef MGI_API_OCL
#define MGI_KERNEL(name) __kernel void name

#ifndef MGI_GLOBAL
    #define MGI_GLOBAL global 
    #define MGI_SHARED local 
    #define MGI_CONST const constant
    #define MGI_LOCAL private 
#endif

#define MGI_GID_X get_global_id(0)
#define MGI_GID MGI_GID_X
#define MGI_GID_Y get_global_id(1)
#define MGI_GID_Z get_global_id(2)
#define MGI_GID_ND(x) get_global_id(x)

#define MGI_GSIZE_X get_global_size(0)
#define MGI_GSIZE MGI_GSIZE_X
#define MGI_GSIZE_Y get_global_size(1)
#define MGI_GSIZE_Z get_global_size(2)
#define MGI_GSIZE_ND(x) get_global_size(x)

#elif defined(MGI_API_VULKAN) // Find Kernel targets in cmake

#define MGI_KERNEL(name) void name

#ifndef MGI_GLOBAL
    #define MGI_GLOBAL global 
    #define MGI_SHARED local 
    #define MGI_CONST const constant
    #define MGI_LOCAL private 
#endif
#define MGI_GID_X gl_GlobalInvocationID[0]
#define MGI_GID MGI_GID_X
#define MGI_GID_Y gl_GlobalInvocationID[1]
#define MGI_GID_Z gl_GlobalInvocationID[2]
#define MGI_GID_ND(x) gl_GlobalInvocationID[x]

#define MGI_GSIZE_X (gl_NumWorkGroups[0] * gl_WorkGroupSize[0])
#define MGI_GSIZE MGI_GSIZE_X
#define MGI_GSIZE_Y (gl_NumWorkGroups[1] * gl_WorkGroupSize[1])
#define MGI_GSIZE_Z (gl_NumWorkGroups[2] * gl_WorkGroupSize[2])
#define MGI_GSIZE_ND(x) (gl_NumWorkGroups[x] * gl_WorkGroupSize[x])

#else

static uint32_t __mgi__xID = 0;
static uint32_t __mgi__yID = 0;
static uint32_t __mgi__xSize = 1;
static uint32_t __mgi__ySize = 1;

#ifndef MGI_GLOBAL
    #define MGI_GLOBAL  
    #define MGI_LOCAL  
    #define MGI_CONST 
    #define MGI_PRIVATE  
#endif

#define MGI_KERNEL(name) void name

#define MGI_GID_X __mgi__xID
#define MGI_GID_Y __mgi__yID
#define MGI_GID MGI_GID_X
#define MGI_GID_Z 1
#define MGI_GID_ND(x) 1

#define MGI_GSIZE_X __mgi__xSize
#define MGI_GSIZE_Y __mgi__ySize
#define MGI_GSIZE __mgi__xSize
#define MGI_GSIZE_Z 1
#define MGI_GSIZE_ND(x) 1

#endif

#endif //MGI_KERNEL_DEFS
