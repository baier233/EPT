#include "base.h"

VOID readUserMemorySafe(
	IN HANDLE pid,
	IN PVOID userVirtualAddressSrc,
	IN PVOID krnlReceivedBufferDst,
	SIZE_T readSize
)
{
	PEPROCESS pe = NULL;
	PsLookupProcessByProcessId(pid, &pe);
	KAPC_STATE apc = { 0 };
	KeStackAttachProcess(pe, &apc);

	PMDL mdl = IoAllocateMdl(userVirtualAddressSrc, (ULONG)readSize, FALSE, FALSE, NULL);
	__try
	{
		MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
		PVOID krnlMapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
		RtlCopyMemory(krnlReceivedBufferDst, krnlMapped, readSize);
		MmUnlockPages(mdl);
	}
	__except (1)
	{
		KeBugCheckEx(0xC7E7A5C9, 0, 0, 0, 0);
	}
	IoFreeMdl(mdl);

	KeUnstackDetachProcess(&apc);
	ObDereferenceObject(pe);

	return;
}

VOID writeUserMemorySafe(
	IN HANDLE pid,
	IN PVOID userVirtualAddressDst,
	IN PVOID krnlWriteBufferSrc,
	SIZE_T writeSize
)
{
	PEPROCESS pe = NULL;
	PsLookupProcessByProcessId(pid, &pe);
	KAPC_STATE apc = { 0 };
	KeStackAttachProcess(pe, &apc);

	PMDL mdl = IoAllocateMdl(userVirtualAddressDst, (ULONG)writeSize, FALSE, FALSE, NULL);
	__try
	{
		MmProbeAndLockPages(mdl, UserMode, IoWriteAccess);
		PVOID krnlMapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
		RtlCopyMemory(krnlMapped, krnlWriteBufferSrc, writeSize);
		MmUnlockPages(mdl);
	}
	__except (1)
	{
		KeBugCheckEx(0xD8E7A5C9, 0, 0, 0, 0);
	}
	IoFreeMdl(mdl);

	KeUnstackDetachProcess(&apc);
	ObDereferenceObject(pe);

	return;
}

NTSTATUS readPhysicalAddress(
	IN PVOID physicalAddress,
	IN PVOID receivedBuffer,
	IN SIZE_T readSize,
	SIZE_T* bytesTransferred
)
{
	MM_COPY_ADDRESS Read = { 0 };
	Read.PhysicalAddress.QuadPart = (LONG64)physicalAddress;
	if (bytesTransferred == NULL)
	{
		SIZE_T ret = 0;
		return MmCopyMemory(receivedBuffer, Read, readSize, MM_COPY_MEMORY_PHYSICAL, &ret);
	}
	else
	{
		return MmCopyMemory(receivedBuffer, Read, readSize, MM_COPY_MEMORY_PHYSICAL, bytesTransferred);
	}
}

VOID writePhysicalMemory(
	IN ULONG_PTR physicalAddress,
	IN PUCHAR writeBuffer,
	IN SIZE_T writeLenLessThan0x1000
)
{
	PHYSICAL_ADDRESS p_address = { 0 };
	p_address.QuadPart = (LONG64)physicalAddress;
	PVOID kernelAddressMappedByPhysical = MmMapIoSpace(p_address, 0x1000, MmNonCachedUnordered);
	RtlCopyMemory(kernelAddressMappedByPhysical, writeBuffer, writeLenLessThan0x1000);
	MmUnmapIoSpace(kernelAddressMappedByPhysical, 0x1000);
	return;
}

ULONG_PTR getPhysicalAddressByCR3AndVirtualAddress(
	IN ULONG64 cr3,
	IN ULONG_PTR VirtualAddress
)
{
	cr3 = (cr3 >> 12) << 12;
	ULONG_PTR ultimatePhysicalAddress = 0;
	ULONG_PTR ultimatePhysicalAddressPageHeader = 0;
	ULONG_PTR VPO = (VirtualAddress << 52) >> 52;
	ULONG_PTR PFN4 = ((VirtualAddress << 43) >> 43) >> 12;
	ULONG_PTR PFN3 = ((VirtualAddress << 34) >> 34) >> 21;
	ULONG_PTR PFN2 = ((VirtualAddress << 25) >> 25) >> 30;
	ULONG_PTR PFN1 = ((VirtualAddress << 16) >> 16) >> 39;
	SIZE_T ret = 0;
	ULONG_PTR a = 0, b = 0, c = 0;
	readPhysicalAddress((PVOID)(cr3 + 8 * PFN1), &a, sizeof(ULONG_PTR), &ret);
	if (ret == 0) return 0;
	a = (((a << 24) >> 24) >> 12) << 12;
	readPhysicalAddress((PVOID)(a + 8 * PFN2), &b, sizeof(ULONG_PTR), &ret);
	if (ret == 0) return 0;
	b = (((b << 24) >> 24) >> 12) << 12;
	readPhysicalAddress((PVOID)(b + 8 * PFN3), &c, sizeof(ULONG_PTR), &ret);
	if (ret == 0) return 0;
	c = (((c << 24) >> 24) >> 12) << 12;
	readPhysicalAddress((PVOID)(c + 8 * PFN4), &ultimatePhysicalAddressPageHeader, sizeof(ULONG_PTR), &ret);
	if (ret == 0) return 0;
	ultimatePhysicalAddressPageHeader = (((ultimatePhysicalAddressPageHeader << 24) >> 24) >> 12) << 12;
	ultimatePhysicalAddress = ultimatePhysicalAddressPageHeader + VPO;
	return ultimatePhysicalAddress;
}

ULONG_PTR allocatePages(
	SIZE_T pagesNum
)
{
	ULONG_PTR ret = (ULONG_PTR)ExAllocatePool2(POOL_FLAG_NON_PAGED, pagesNum * PAGE_SIZE, 'zzaa');
	if (ret == 0)
	{
		KeBugCheckEx(0x343F3B34, 0, 0, 0, 0);
	}
	RtlZeroMemory((PVOID)ret, pagesNum * PAGE_SIZE);
	return ret;
}