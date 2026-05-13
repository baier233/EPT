#ifndef __BASE_HEADER__
#define __BASE_HEADER__

#include <ntifs.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE (0x1000)
#endif

#define LOCAL_MODULE_EXTERN		extern
#define EXTERNAL_MODULE_EXTERN	extern

#define VMLAUNCH_SPACE_NEEDED (PAGE_SIZE)
#define PER_CPU_MAX_PRIVATE_SPACE (3 * PAGE_SIZE)

#define print(s) \
do\
{\
	ULONG64 ______________x = (s);\
	DbgPrint("%s -> [0x%p]\n", #s, (PVOID)______________x);\
}while(0)

typedef struct _VIRTUAL_CPU_STRUCT
{
	ULONG64 currentCr4;
	LONG_PTR VMX_ON_REGION_PHYSICAL_ADDRESS;
	PVOID VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS;
	LONG_PTR VMX_VMCS_REGION_PHYSICAL_ADDRESS;
	PVOID VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS;
	PVOID virtualGuestStack;
	SIZE_T virtualGuestStackSize;
	PVOID virtualGuestStackBottom;
	PVOID virtualHostStack;
	SIZE_T virtualHostStackSize;
	PVOID virtualHostStackBottom;
	UCHAR regs[VMLAUNCH_SPACE_NEEDED];
	UCHAR thisCpuPrivateRegion[PER_CPU_MAX_PRIVATE_SPACE];
}VCPU, * PVCPU;

VOID readUserMemorySafe(
	IN HANDLE pid,
	IN PVOID userVirtualAddressSrc,
	IN PVOID krnlReceivedBufferDst,
	SIZE_T readSize
);

VOID writeUserMemorySafe(
	IN HANDLE pid,
	IN PVOID userVirtualAddressDst,
	IN PVOID krnlWriteBufferSrc,
	SIZE_T writeSize
);

NTSTATUS readPhysicalAddress(
	IN PVOID physicalAddress,
	IN PVOID receivedBuffer,
	IN SIZE_T readSize,
	SIZE_T* bytesTransferred
);

VOID writePhysicalMemory(
	IN ULONG_PTR physicalAddress,
	IN PUCHAR writeBuffer,
	IN SIZE_T writeLenLessThan0x1000
);

ULONG_PTR getPhysicalAddressByCR3AndVirtualAddress(
	IN ULONG64 cr3,
	IN ULONG_PTR VirtualAddress
);

ULONG_PTR allocatePages(
	SIZE_T pagesNum
);

#endif