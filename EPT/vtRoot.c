#include "vtRoot.h"

#pragma warning(disable: 28182)
#pragma warning(disable: 6387)
#pragma warning(disable: 6011)
#pragma warning(disable: 4996)
#pragma warning(disable: 6386)

#define NO_NEED_CARE_WHEN_SETTING_HOST (0)
#define UN_USED (1)
#define USED (0)

EXTERNAL_MODULE_EXTERN PDRIVER_OBJECT g_driverObject; //from entry.c

ULONG GUEST_FS_BASE_ADDRESS_INDEX = GUEST_FS_BASE_ADDRESS;
ULONG HOST_FS_BASE_ADDRESS_INDEX = HOST_FS_BASE_ADDRESS;
ULONG GUEST_FS_BASE_INDEX = GUEST_FS_BASE_ADDRESS;
ULONG GUEST_CR0_INDEX = GUEST_CR0;
ULONG GUEST_CR3_INDEX = GUEST_CR3;
ULONG GUEST_CR4_INDEX = GUEST_CR4;
ULONG GUEST_RFLAGS_INDEX = GUEST_RFLAGS;
ULONG GUEST_RSP_INDEX = GUEST_RSP;
ULONG GUEST_RIP_INDEX = GUEST_RIP;
ULONG GUEST_GDTR_LIMIT_INDEX = GUEST_GDTR_LIMIT;
ULONG GUEST_IDTR_LIMIT_INDEX = GUEST_IDTR_LIMIT;
ULONG HOST_RIP_INDEX = HOST_RIP;
ULONG HOST_RSP_INDEX = HOST_RSP;
ULONG VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS_INDEX = VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS;
ULONG VECF_SECONDARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS_INDEX = VECF_SECONDARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS;
ULONG VEnCF_VM_ENTRY_INTERRUPTION_INFORMATION_FIELD_INDEX = VEnCF_VM_ENTRY_INTERRUPTION_INFORMATION_FIELD;
ULONG VEnCF_VM_ENTRY_EXCEPTION_ERROR_CODE_INDEX = VEnCF_VM_ENTRY_EXCEPTION_ERROR_CODE;
ULONG VM_EXIT_REASON_INDEX = VEIF_EXIT_REASON;
ULONG VM_EXIT_QUALIFICATION_INDEX = VEIF_EXIT_QUALIFICATION;
ULONG VM_INSTRUCTION_LENGTH_INDEX = VEIF_VM_EXIT_INSTRUCTION_LENGTH;

PVCPU g_vCpu;
PULONG_PTR g_PER_CPU_REGS;
ULONG_PTR g_bugCheck;
ULONG g_totalCpuCount;
ULONG_PTR g_ept;
ULONG_PTR g_tpPML4T, g_tpPDPT, g_tpPDT, g_tpPT;

SIZE_T g_MAX_PHYSICAL_ADDRESS_LENGTH;
UCHAR g_bit55OfIa32VmxBasic;
ULONG64 g_pinBasedMark;
ULONG64 g_primaryProcessorBasedMask;
ULONG64 g_vmentryControlsMask;
ULONG64	g_vmexitControlsMask;
ULONG64 g_secondaryProcessorBasedMask;

VARIABLE_RANGE_MTRR_BASE_ADDRESS g_variableRangeMtrrBaseAddresses[20];

ULONG_PTR g_NtCreateThreadEx;
ULONG_PTR g_MiProcessLoaderEntry;

UCHAR g_msrBitMap[PAGE_SIZE * 3] = { 0 };

INT32 dbgAsm(ULONG param)
{
	UNREFERENCED_PARAMETER(param);
	return 0;
}

VOID detectBit55OfIa32VmxBasic()
{
	ULONG64 base = __readmsr(IA32_VMX_BASIC);
	g_bit55OfIa32VmxBasic = (base >> 55) & 0x1;
	return;
}

VOID detectVmxControlsMark()
{
	g_secondaryProcessorBasedMask = __readmsr(IA32_VMX_PROCBASED_CTLS2);
	if (g_bit55OfIa32VmxBasic == 1)
	{
		g_pinBasedMark = __readmsr(IA32_VMX_TRUE_PINBASED_CTLS);
		g_primaryProcessorBasedMask = __readmsr(IA32_VMX_TRUE_PROCBASED_CTLS);
		g_vmentryControlsMask = __readmsr(IA32_VMX_TRUE_ENTRY_CTLS);
		g_vmexitControlsMask = __readmsr(IA32_VMX_TRUE_EXIT_CTLS);
		return;

	}
	else
	{
		g_pinBasedMark = __readmsr(IA32_VMX_PINBASED_CTLS);
		g_primaryProcessorBasedMask = __readmsr(IA32_VMX_PROCBASED_CTLS);
		g_vmentryControlsMask = __readmsr(IA32_VMX_ENTRY_CTLS);
		g_vmexitControlsMask = __readmsr(IA32_VMX_EXIT_CTLS);
		return;
	}
}

VOID detectVariableRangeMtrrBaseAddresses()
{
	SIZE_T phyBaseMsrIndex = 0x200;
	SIZE_T phyMaskMsrIndex = 0x201;

	SIZE_T idx = 0;

	for (size_t j = 0; j < 10; j += 2)
	{
		ULONG64 currentPhyBaseEntry = __readmsr(phyBaseMsrIndex + j);
		ULONG64 currentPhyMaskEntry = __readmsr(phyMaskMsrIndex + j);

		UCHAR currentPhyRegionMemoryType = currentPhyBaseEntry & 0xFFui8;
		BOOLEAN bIsCurrentPhyRegionValid = (currentPhyMaskEntry & 0x800) == 0 ? FALSE : TRUE;

		if (bIsCurrentPhyRegionValid == TRUE)
		{
			ULONG_PTR currentPhyRegionBase = currentPhyBaseEntry & ~0xFFFull;
			ULONG64 currentPhyRegionSize = (((~(currentPhyMaskEntry & ~0xFFFull)) & (~(1ull << g_MAX_PHYSICAL_ADDRESS_LENGTH))) & ~(MAXULONG64 << g_MAX_PHYSICAL_ADDRESS_LENGTH));
			g_variableRangeMtrrBaseAddresses[idx].physicalBaseForRegion = currentPhyRegionBase;
			g_variableRangeMtrrBaseAddresses[idx].sizeForRegion = currentPhyRegionSize;
			g_variableRangeMtrrBaseAddresses[idx].memoryTypeForRegion = currentPhyRegionMemoryType;
			DbgPrint("[0x%llX] [0x%llX] 0x%llX -> 0x%llX: 0x%hhX\n", currentPhyBaseEntry, currentPhyMaskEntry, g_variableRangeMtrrBaseAddresses[idx].physicalBaseForRegion, g_variableRangeMtrrBaseAddresses[idx].physicalBaseForRegion + g_variableRangeMtrrBaseAddresses[idx].sizeForRegion, currentPhyRegionMemoryType);
		}
		else
		{
			g_variableRangeMtrrBaseAddresses[idx].physicalBaseForRegion = 0xFFFFFFFFFFFFFFFF;
			g_variableRangeMtrrBaseAddresses[idx].sizeForRegion = 0xFFFFFFFFFFFFFFFF;
			continue;
		}

		idx++;
	}
}

ULONG64 fixControlValue(
	ULONG64 controlValue,
	VMX_CONTROL_TYPE controlMark
)
{
	ULONG bit0_31 = 0;
	ULONG bit32_63 = 0;
	switch (controlMark)
	{
	case VMX_CONTROL_PIN_BASED:
		bit0_31 = g_pinBasedMark & 0xFFFFFFFF;
		bit32_63 = (g_pinBasedMark >> 32) & 0xFFFFFFFF;
		controlValue |= bit0_31;
		controlValue &= bit32_63;
		break;
	case VMX_CONTROL_PRIMARY_PROCESSOR_BASED:
		bit0_31 = g_primaryProcessorBasedMask & 0xFFFFFFFF;
		bit32_63 = (g_primaryProcessorBasedMask >> 32) & 0xFFFFFFFF;
		controlValue |= bit0_31;
		controlValue &= bit32_63;
		break;
	case VMX_CONTROL_SECONDARY_PROCESSOR_BASED:
		bit0_31 = g_secondaryProcessorBasedMask & 0xFFFFFFFF;
		bit32_63 = (g_secondaryProcessorBasedMask >> 32) & 0xFFFFFFFF;
		controlValue |= bit0_31;
		controlValue &= bit32_63;
		break;
	case VMX_CONTROL_VMENTRY:
		bit0_31 = g_vmentryControlsMask & 0xFFFFFFFF;
		bit32_63 = (g_vmentryControlsMask >> 32) & 0xFFFFFFFF;
		controlValue |= bit0_31;
		controlValue &= bit32_63;
		break;
	case VMX_CONTROL_VMEXIT:
		bit0_31 = g_vmexitControlsMask & 0xFFFFFFFF;
		bit32_63 = (g_vmexitControlsMask >> 32) & 0xFFFFFFFF;
		controlValue |= bit0_31;
		controlValue &= bit32_63;
		break;
	default:
		break;
	}
	return controlValue;
}

MEMORY_TYPE getPhysicalMemoryCacheType(
	ULONG_PTR physicalAddress //may not aligned!!!!!!!
)
{
	ULONG_PTR pa = physicalAddress & ~0xFFFull;
	if (pa <= 0xFFFFFull)
	{
		//Fixed-Range MTRR Checking....
		if (pa <= 0x7FFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x250);
			if (pa <= 0xFFFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0x10000ull && pa <= 0x1FFFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0x20000ull && pa <= 0x2FFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0x30000ull && pa <= 0x3FFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0x40000ull && pa <= 0x4FFFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0x50000ull && pa <= 0x5FFFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0x60000ull && pa <= 0x6FFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else if (pa >= 0x80000ull && pa <= 0x9FFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x258);
			if (pa >= 0x80000ull && pa <= 0x83FFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0x84000ull && pa <= 0x87FFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0x88000ull && pa <= 0x8BFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0x8C000ull && pa <= 0x8FFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0x90000ull && pa <= 0x93FFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0x94000ull && pa <= 0x97FFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0x98000ull && pa <= 0x9BFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else if (pa >= 0xA0000ull && pa <= 0xBFFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x259);
			if (pa >= 0xA0000ull && pa <= 0xA3FFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0xA4000ull && pa <= 0xA7FFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0xA8000ull && pa <= 0xABFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0xAC000ull && pa <= 0xAFFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0xB0000ull && pa <= 0xB3FFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0xB4000ull && pa <= 0xB7FFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0xB8000ull && pa <= 0xBBFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else
		{
			if (pa >= 0xC0000ull && pa <= 0xC7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x268);
				if (pa >= 0xC0000ull && pa <= 0xC0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xC1000ull && pa <= 0xC1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xC2000ull && pa <= 0xC2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xC3000ull && pa <= 0xC3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xC4000ull && pa <= 0xC4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xC5000ull && pa <= 0xC5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xC6000ull && pa <= 0xC6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xC8000ull && pa <= 0xCFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x269);
				if (pa >= 0xC8000ull && pa <= 0xC8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xC9000ull && pa <= 0xC9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xCA000ull && pa <= 0xCAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xCB000ull && pa <= 0xCBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xCC000ull && pa <= 0xCCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xCD000ull && pa <= 0xCDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xCE000ull && pa <= 0xCEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xD0000ull && pa <= 0xD7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26A);
				if (pa >= 0xD0000ull && pa <= 0xD0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xD1000ull && pa <= 0xD1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xD2000ull && pa <= 0xD2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xD3000ull && pa <= 0xD3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xD4000ull && pa <= 0xD4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xD5000ull && pa <= 0xD5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xD6000ull && pa <= 0xD6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xD8000ull && pa <= 0xDFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26B);
				if (pa >= 0xD8000ull && pa <= 0xD8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xD9000ull && pa <= 0xD9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xDA000ull && pa <= 0xDAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xDB000ull && pa <= 0xDBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xDC000ull && pa <= 0xDCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xDD000ull && pa <= 0xDDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xDE000ull && pa <= 0xDEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xE0000ull && pa <= 0xE7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26C);
				if (pa >= 0xE0000ull && pa <= 0xE0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xE1000ull && pa <= 0xE1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xE2000ull && pa <= 0xE2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xE3000ull && pa <= 0xE3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xE4000ull && pa <= 0xE4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xE5000ull && pa <= 0xE5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xE6000ull && pa <= 0xE6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xE8000ull && pa <= 0xEFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26D);
				if (pa >= 0xE8000ull && pa <= 0xE8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xE9000ull && pa <= 0xE9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xEA000ull && pa <= 0xEAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xEB000ull && pa <= 0xEBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xEC000ull && pa <= 0xECFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xED000ull && pa <= 0xEDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xEE000ull && pa <= 0xEEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xF0000ull && pa <= 0xF7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26E);
				if (pa >= 0xF0000ull && pa <= 0xF0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xF1000ull && pa <= 0xF1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xF2000ull && pa <= 0xF2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xF3000ull && pa <= 0xF3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xF4000ull && pa <= 0xF4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xF5000ull && pa <= 0xF5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xF6000ull && pa <= 0xF6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26F);
				if (pa >= 0xF8000ull && pa <= 0xF8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xF9000ull && pa <= 0xF9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xFA000ull && pa <= 0xFAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xFB000ull && pa <= 0xFBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xFC000ull && pa <= 0xFCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xFD000ull && pa <= 0xFDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xFE000ull && pa <= 0xFEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
		}
	}
	else
	{
		for (size_t j = 0; j < 5; j++)
		{
			if (pa >= g_variableRangeMtrrBaseAddresses[j].physicalBaseForRegion && pa <= g_variableRangeMtrrBaseAddresses[j].physicalBaseForRegion + g_variableRangeMtrrBaseAddresses[j].sizeForRegion)
			{
				return g_variableRangeMtrrBaseAddresses[j].memoryTypeForRegion;
			}
			else
			{
				continue;
			}
		}

		return  0x6; // __readmsr(0x2FF) & 0xFFui8;
	}
}

/*
* 
* 
* 
MEMORY_TYPE oGetPhysicalMemoryCacheType(
	ULONG_PTR physicalAddress //may not aligned!!!!!!!
)
{
	if (physicalAddress <= 0x9FFFFull) //FIXED_MTRR!64K & FIXED!16K_80000
	{
		return 0x6; //WB
	}
	else if (physicalAddress >= 0xA0000ull && physicalAddress <= 0xBFFFFull) //FIXED_MTRR!16K_A0000
	{
		return 0x0; //UC
	}
	else if (physicalAddress >= 0xC0000ull && physicalAddress <= 0xFFFFFull) //FIXED_MTRR!others
	{
		return 0x5; //WP
	}
	else if (physicalAddress >= 0x7B000000ull && physicalAddress <= 0x7FFFFFFFull) //[7B000000, 7BFFFFFF] + [7C000000, 7FFFFFFF]
	{
		return 0x0; //WB
	}
	else if (physicalAddress >= 0x80000000ull && physicalAddress <= 0x8FFFFFFFull) //[80000000, 8FFFFFFF]
	{
		return 0x1; //UC
	}
	else if (physicalAddress >= 0x90000000ull && physicalAddress <= 0xFFFFFFFFull) //[90000000, 9FFFFFFF] + [A0000000, BFFFFFFF] + [C0000000, FFFFFFFF]
	{
		return 0x0; //WP
	}
	else
	{
		return 0x6; //WB
	}
}

MEMORY_TYPE getPhysicalMemoryCacheType(
	ULONG_PTR physicalAddress //may not aligned!!!!!!!
)
{
	ULONG_PTR pa = physicalAddress & ~0xFFFull;
	if (pa <= 0xFFFFFull)
	{
		//Fixed-Range MTRR Checking....
		if (pa <= 0x7FFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x250);
			if (pa <= 0xFFFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0x10000ull && pa <= 0x1FFFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0x20000ull && pa <= 0x2FFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0x30000ull && pa <= 0x3FFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0x40000ull && pa <= 0x4FFFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0x50000ull && pa <= 0x5FFFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0x60000ull && pa <= 0x6FFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else if (pa >= 0x80000ull && pa <= 0x9FFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x258);
			if (pa >= 0x80000ull && pa <= 0x83FFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0x84000ull && pa <= 0x87FFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0x88000ull && pa <= 0x8BFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0x8C000ull && pa <= 0x8FFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0x90000ull && pa <= 0x93FFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0x94000ull && pa <= 0x97FFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0x98000ull && pa <= 0x9BFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else if (pa >= 0xA0000ull && pa <= 0xBFFFFull)
		{
			ULONG64 thisRegionMtrrValue = __readmsr(0x259);
			if (pa >= 0xA0000ull && pa <= 0xA3FFFull)
			{
				return thisRegionMtrrValue & 0xFFui8;
			}
			else if (pa >= 0xA4000ull && pa <= 0xA7FFFull)
			{
				return (thisRegionMtrrValue >> 8) & 0xFFui8;
			}
			else if (pa >= 0xA8000ull && pa <= 0xABFFFull)
			{
				return (thisRegionMtrrValue >> 16) & 0xFFui8;
			}
			else if (pa >= 0xAC000ull && pa <= 0xAFFFFull)
			{
				return (thisRegionMtrrValue >> 24) & 0xFFui8;
			}
			else if (pa >= 0xB0000ull && pa <= 0xB3FFFull)
			{
				return (thisRegionMtrrValue >> 32) & 0xFFui8;
			}
			else if (pa >= 0xB4000ull && pa <= 0xB7FFFull)
			{
				return (thisRegionMtrrValue >> 40) & 0xFFui8;
			}
			else if (pa >= 0xB8000ull && pa <= 0xBBFFFull)
			{
				return (thisRegionMtrrValue >> 48) & 0xFFui8;
			}
			else
			{
				return (thisRegionMtrrValue >> 56) & 0xFFui8;
			}
		}
		else
		{
			if (pa >= 0xC0000ull && pa <= 0xC7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x268);
				if (pa >= 0xC0000ull && pa <= 0xC0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xC1000ull && pa <= 0xC1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xC2000ull && pa <= 0xC2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xC3000ull && pa <= 0xC3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xC4000ull && pa <= 0xC4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xC5000ull && pa <= 0xC5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xC6000ull && pa <= 0xC6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xC8000ull && pa <= 0xCFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x269);
				if (pa >= 0xC8000ull && pa <= 0xC8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xC9000ull && pa <= 0xC9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xCA000ull && pa <= 0xCAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xCB000ull && pa <= 0xCBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xCC000ull && pa <= 0xCCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xCD000ull && pa <= 0xCDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xCE000ull && pa <= 0xCEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xD0000ull && pa <= 0xD7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26A);
				if (pa >= 0xD0000ull && pa <= 0xD0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xD1000ull && pa <= 0xD1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xD2000ull && pa <= 0xD2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xD3000ull && pa <= 0xD3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xD4000ull && pa <= 0xD4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xD5000ull && pa <= 0xD5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xD6000ull && pa <= 0xD6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xD8000ull && pa <= 0xDFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26B);
				if (pa >= 0xD8000ull && pa <= 0xD8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xD9000ull && pa <= 0xD9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xDA000ull && pa <= 0xDAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xDB000ull && pa <= 0xDBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xDC000ull && pa <= 0xDCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xDD000ull && pa <= 0xDDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xDE000ull && pa <= 0xDEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xE0000ull && pa <= 0xE7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26C);
				if (pa >= 0xE0000ull && pa <= 0xE0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xE1000ull && pa <= 0xE1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xE2000ull && pa <= 0xE2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xE3000ull && pa <= 0xE3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xE4000ull && pa <= 0xE4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xE5000ull && pa <= 0xE5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xE6000ull && pa <= 0xE6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xE8000ull && pa <= 0xEFFFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26D);
				if (pa >= 0xE8000ull && pa <= 0xE8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xE9000ull && pa <= 0xE9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xEA000ull && pa <= 0xEAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xEB000ull && pa <= 0xEBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xEC000ull && pa <= 0xECFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xED000ull && pa <= 0xEDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xEE000ull && pa <= 0xEEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else if (pa >= 0xF0000ull && pa <= 0xF7FFFull)
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26E);
				if (pa >= 0xF0000ull && pa <= 0xF0FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xF1000ull && pa <= 0xF1FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xF2000ull && pa <= 0xF2FFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xF3000ull && pa <= 0xF3FFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xF4000ull && pa <= 0xF4FFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xF5000ull && pa <= 0xF5FFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xF6000ull && pa <= 0xF6FFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
			else
			{
				ULONG64 thisRegionMtrrValue = __readmsr(0x26F);
				if (pa >= 0xF8000ull && pa <= 0xF8FFFull)
				{
					return thisRegionMtrrValue & 0xFFui8;
				}
				else if (pa >= 0xF9000ull && pa <= 0xF9FFFull)
				{
					return (thisRegionMtrrValue >> 8) & 0xFFui8;
				}
				else if (pa >= 0xFA000ull && pa <= 0xFAFFFull)
				{
					return (thisRegionMtrrValue >> 16) & 0xFFui8;
				}
				else if (pa >= 0xFB000ull && pa <= 0xFBFFFull)
				{
					return (thisRegionMtrrValue >> 24) & 0xFFui8;
				}
				else if (pa >= 0xFC000ull && pa <= 0xFCFFFull)
				{
					return (thisRegionMtrrValue >> 32) & 0xFFui8;
				}
				else if (pa >= 0xFD000ull && pa <= 0xFDFFFull)
				{
					return (thisRegionMtrrValue >> 40) & 0xFFui8;
				}
				else if (pa >= 0xFE000ull && pa <= 0xFEFFFull)
				{
					return (thisRegionMtrrValue >> 48) & 0xFFui8;
				}
				else
				{
					return (thisRegionMtrrValue >> 56) & 0xFFui8;
				}
			}
		}
	}
	else
	{
		for (size_t j = 0; j < 5; j++)
		{
			if (pa >= g_variableRangeMtrrBaseAddresses[j].physicalBaseForRegion && pa <= g_variableRangeMtrrBaseAddresses[j].physicalBaseForRegion + g_variableRangeMtrrBaseAddresses[j].sizeForRegion)
			{
				return g_variableRangeMtrrBaseAddresses[j].memoryTypeForRegion;
			}
			else
			{
				continue;
			}
		}

		return  0x6; // __readmsr(0x2FF) & 0xFFui8;
	}
}


*/

VOID getSegementRegisterAttributes(
	IN ULONG type,
	IN ULONG64 selector,
	IN UCHAR unusable,
	IN OUT SRA* sra
)
{
	RtlZeroBytes(sra, sizeof(SRA));

	UCHAR gdt[10] = { 0 };
	_sgdt(&gdt);
	ULONG_PTR GDTbase = *(ULONG_PTR*)&gdt[2];
	ULONG64 index = selector >> 3;
	ULONG_PTR currentSelectorGdtEntryPointer = GDTbase + index * 8; //8: magic number defined in INTEL® SDL
	ULONG64 currentSelectorGdtEntry = *(ULONG64*)currentSelectorGdtEntryPointer;

	GDT_DESCRIPTOR_ENTRY thisSegGdtEntry = { 0 };
	thisSegGdtEntry.all = currentSelectorGdtEntry;
	VMM_SEGEMENT_ACCESS_RIGHTS thisSegAccessRights = { 0 };
	thisSegAccessRights.all = ((currentSelectorGdtEntry >> 40) & 0xFFFFull);
	thisSegAccessRights.detail.reserved1 = 0;
	if (unusable)
	{
		thisSegAccessRights.detail.unusable = 1;
	}
	ULONG rawLimit = (ULONG)(thisSegGdtEntry.detail.limit_0_15 | (thisSegGdtEntry.detail.limit_16_19 << 16));
	sra->selector = (USHORT)selector;
	if (type == 'tr')
	{
		sra->baseAddress = (*(ULONG_PTR*)(currentSelectorGdtEntryPointer + 8)) << 32;
		sra->baseAddress += (ULONG_PTR)(((currentSelectorGdtEntry >> 16) & 0xFFFFFFull) + ((currentSelectorGdtEntry & 0xFF00000000000000ull) >> 32));
	}
	else
	{
		sra->baseAddress = thisSegGdtEntry.detail.base_0_15 | (((ULONG_PTR)(thisSegGdtEntry.detail.base_16_23)) << 16) | (((ULONG_PTR)(thisSegGdtEntry.detail.base_24_31)) << 24);
	}
	if (thisSegAccessRights.detail.G)
	{
		sra->segmentLimit = (rawLimit << 12) | 0xFFF; //4K
	}
	else
	{
		sra->segmentLimit = rawLimit;
	}
	sra->rawAccessRight = thisSegAccessRights.all;
	return;
}

ULONG_PTR virtualOffVtAndEpt(
	ULONG_PTR arg
)
{
	UNREFERENCED_PARAMETER(arg);
	__vsm__vmxoffSaveRegisters();
	return 0;
}

ULONG_PTR virtualizationVtAndEpt(
	ULONG_PTR arg
)
{
	UNREFERENCED_PARAMETER(arg);

	SRA h_cs = { 0 }, h_ss = { 0 }, h_ds = { 0 }, h_es = { 0 }, h_fs = { 0 }, h_gs = { 0 }, h_ldtr = { 0 }, h_tr = { 0 };
	SRA g_cs = { 0 }, g_ss = { 0 }, g_ds = { 0 }, g_es = { 0 }, g_fs = { 0 }, g_gs = { 0 }, g_ldtr = { 0 }, g_tr = { 0 };

	ULONG j = KeGetCurrentProcessorNumber();
	//开启CR4.VMXE位
	__vasm__setCR4VMXEBit();
	g_vCpu[j].currentCr4 = __readcr4();
	//Vmxon
	__vmx_on(&g_vCpu[j].VMX_ON_REGION_PHYSICAL_ADDRESS);
	//选择current VMCS
	__vmx_vmclear(&g_vCpu[j].VMX_VMCS_REGION_PHYSICAL_ADDRESS);
	__vmx_vmptrld(&g_vCpu[j].VMX_VMCS_REGION_PHYSICAL_ADDRESS);
	//写入VMCS各个字段
	/*1.VM-Exit Control字段*/
	VM_EXIT_CONTROL vmExitCtrl = { 0 };
	vmExitCtrl.AllFields = 0;
	vmExitCtrl.Fields.hostAddressSpaceSize = 1;
	__vmx_vmwrite(VExCF_PRIMARY_VM_EXIT_CONTROLS, fixControlValue(vmExitCtrl.AllFields, VMX_CONTROL_VMEXIT)); //物理机 0x36FFB
	/*2.VM-Entry Control字段*/
	VM_ENTRY_CONTROL vmEntryCtrl = { 0 };
	vmEntryCtrl.AllFields = 0;
	vmEntryCtrl.Fields.ia32eModeGuest = 1;
	__vmx_vmwrite(VEnCF_VM_ENTRY_CONTROLS, fixControlValue(vmEntryCtrl.AllFields, VMX_CONTROL_VMENTRY)); //物理机 0x13FBul
	/*3.VM-Execution Control字段*/
	PIN_BASED_VMCS_CONTROL pinCtrl = { 0 };
	pinCtrl.AllFields = 0;
	__vmx_vmwrite(VECF_PIN_BASED_VM_EXECUTION_CONTROL, fixControlValue(pinCtrl.AllFields, VMX_CONTROL_PIN_BASED)); //物理机 0x16ul
	PRIMARY_PROCESSOR_BASED_VMCS_CONTROL priProb = { 0 };
	priProb.AllFields = 0;
	priProb.Fields.activateSecondaryControls = 1;
	priProb.Fields.useMsrBitmaps = 1;
	__vmx_vmwrite(VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, fixControlValue(priProb.AllFields, VMX_CONTROL_PRIMARY_PROCESSOR_BASED)); //minimal 0x84006172ul
	__vmx_vmwrite(VECF_MSR_BITMAPS, (ULONG_PTR)(((ULONG64)MmGetPhysicalAddress((PVOID)g_msrBitMap).QuadPart + PAGE_SIZE) & ~0xFFFull));
	SECONDARY_PROCESSOR_BASED_VMCS_CONTROL secProb = { 0 };
	secProb.AllFields = 0;
	secProb.Fields.enableEpt = 1;
	secProb.Fields.enableRdtscp = 1;
	secProb.Fields.enableInvpcid = 1;
	secProb.Fields.enableXsaveXstores = 1;
	__vmx_vmwrite(VECF_SECONDARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, fixControlValue(secProb.AllFields, VMX_CONTROL_SECONDARY_PROCESSOR_BASED)); //物理机 0x10100Aul
	__vmx_vmwrite(VECF_EXTENDED_PAGE_TABLE_POINTER, (ULONG_PTR)g_ept | 0x1E);
	/*4.Host-State Area字段*/
	getSegementRegisterAttributes('cs', __vsm__getCsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_cs);
	getSegementRegisterAttributes('ss', __vsm__getSsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_ss);
	getSegementRegisterAttributes('ds', __vsm__getDsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_ds);
	getSegementRegisterAttributes('es', __vsm__getEsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_es);
	getSegementRegisterAttributes('fs', __vsm__getFsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_fs);
	getSegementRegisterAttributes('gs', __vsm__getGsSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_gs);
	getSegementRegisterAttributes('ldtr', __vsm__getLDTR(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_ldtr);
	getSegementRegisterAttributes('tr', __vsm__getTrSelector(), NO_NEED_CARE_WHEN_SETTING_HOST, &h_tr);
	__vmx_vmwrite(HOST_CR0, __readcr0());
	__vmx_vmwrite(HOST_CR3, __readcr3());
	__vmx_vmwrite(HOST_CR4, __readcr4());
	__vmx_vmwrite(HOST_RSP, (PVOID)g_vCpu[j].virtualHostStackBottom);
	__vmx_vmwrite(HOST_RIP, (PVOID)__vsm__hostEntry);
	__vmx_vmwrite(HOST_CS_SELECTOR, h_cs.selector & ~0x7ull);
	__vmx_vmwrite(HOST_SS_SELECTOR, h_ss.selector & ~0x7ull);
	__vmx_vmwrite(HOST_DS_SELECTOR, h_ds.selector & ~0x7ull);
	__vmx_vmwrite(HOST_ES_SELECTOR, h_es.selector & ~0x7ull);
	__vmx_vmwrite(HOST_FS_SELECTOR, h_fs.selector & ~0x7ull);
	__vmx_vmwrite(HOST_GS_SELECTOR, h_gs.selector & ~0x7ull);
	__vmx_vmwrite(HOST_TR_SELECTOR, h_tr.selector & ~0x7ull);
	__vmx_vmwrite(HOST_FS_BASE_ADDRESS, 0xCCCCCCCC); //IA-32e模式，FS.base干脆用不上，这个数值为了debug compatibility mode模式的访问冲突特征码
	__vmx_vmwrite(HOST_GS_BASE_ADDRESS, __readmsr(IA32_GS_BASE));
	__vmx_vmwrite(HOST_TR_BASE_ADDRESS, (PVOID)h_tr.baseAddress);
	__vmx_vmwrite(HOST_GDTR_BASE_ADDRESS, (PVOID)__vsm__getGDTbase());
	__vmx_vmwrite(HOST_IDTR_BASE_ADDRESS, (PVOID)__vsm__getIDTbase());
	/*5.Guest-State Area字段*/
	getSegementRegisterAttributes('cs', __vsm__getCsSelector(), USED, &g_cs);
	getSegementRegisterAttributes('ss', __vsm__getSsSelector(), UN_USED, &g_ss);
	getSegementRegisterAttributes('ds', __vsm__getDsSelector(), UN_USED, &g_ds);
	getSegementRegisterAttributes('es', __vsm__getEsSelector(), UN_USED, &g_es);
	getSegementRegisterAttributes('fs', __vsm__getFsSelector(), USED, &g_fs);
	getSegementRegisterAttributes('gs', __vsm__getGsSelector(), USED, &g_gs);
	getSegementRegisterAttributes('ldtr', __vsm__getLDTR(), UN_USED, &g_ldtr);
	getSegementRegisterAttributes('tr', __vsm__getTrSelector(), USED, &g_tr);
	//选择子
	__vmx_vmwrite(GUEST_CS_SELECTOR, g_cs.selector);
	__vmx_vmwrite(GUEST_FS_SELECTOR, g_fs.selector);
	__vmx_vmwrite(GUEST_GS_SELECTOR, g_gs.selector);
	__vmx_vmwrite(GUEST_TR_SELECTOR, g_tr.selector);
	//基地址
	__vmx_vmwrite(GUEST_FS_BASE_ADDRESS, __readmsr(IA32_FS_BASE));
	__vmx_vmwrite(GUEST_GS_BASE_ADDRESS, __readmsr(IA32_GS_BASE));
	__vmx_vmwrite(GUEST_TR_BASE_ADDRESS, g_tr.baseAddress);
	//限制
	__vmx_vmwrite(GUEST_FS_SEGEMENT_LIMIT, g_fs.segmentLimit);
	__vmx_vmwrite(GUEST_GS_SEGEMENT_LIMIT, g_gs.segmentLimit);
	__vmx_vmwrite(GUEST_TR_SEGEMENT_LIMIT, g_tr.segmentLimit);
	//权限
	__vmx_vmwrite(GUEST_CS_ACCESS_RIGHTS, g_cs.rawAccessRight);
	__vmx_vmwrite(GUEST_SS_ACCESS_RIGHTS, g_ss.rawAccessRight);
	__vmx_vmwrite(GUEST_DS_ACCESS_RIGHTS, g_ds.rawAccessRight);
	__vmx_vmwrite(GUEST_ES_ACCESS_RIGHTS, g_es.rawAccessRight);
	__vmx_vmwrite(GUEST_FS_ACCESS_RIGHTS, g_fs.rawAccessRight);
	__vmx_vmwrite(GUEST_GS_ACCESS_RIGHTS, g_gs.rawAccessRight);
	__vmx_vmwrite(GUEST_LDTR_ACCESS_RIGHTS, g_ldtr.rawAccessRight);
	__vmx_vmwrite(GUEST_TR_ACCESS_RIGHTS, g_tr.rawAccessRight);
	//G/IDT表
	__vmx_vmwrite(GUEST_GDTR_BASE, __vsm__getGDTbase());
	__vmx_vmwrite(GUEST_IDTR_BASE, __vsm__getIDTbase());
	__vmx_vmwrite(GUEST_GDTR_LIMIT, __vsm__getGDTlimit()); //28.3.1.3
	__vmx_vmwrite(GUEST_IDTR_LIMIT, __vsm__getIDTlimit()); //28.3.1.3
	//其他
	__vmx_vmwrite(GUEST_VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFull);
	//寄存器
	__vmx_vmwrite(GUEST_CR0, __readcr0());
	__vmx_vmwrite(GUEST_CR3, __readcr3());
	__vmx_vmwrite(GUEST_CR4, __readcr4());
	__vmx_vmwrite(GUEST_DR7, __readdr(7));
	__vmx_vmwrite(GUEST_RFLAGS, __readeflags());
	__vmx_vmwrite(GUEST_RSP, (PVOID)g_vCpu[j].virtualGuestStackBottom);
	__vmx_vmwrite(GUEST_RIP, (PVOID)__vsm__guestEntry);

	__vsm__vmlaunchSaveRegisters();

	return 0;
}

VOID enterVtStateWithEpt()
{
	g_bugCheck = (ULONG_PTR)KeBugCheckEx;

	g_totalCpuCount = KeQueryActiveProcessorCount(NULL);

	g_MAX_PHYSICAL_ADDRESS_LENGTH = __vasm__detectMaxPhysicalAddressLength();

	detectBit55OfIa32VmxBasic();

	detectVmxControlsMark();

	detectVariableRangeMtrrBaseAddresses();

	getPsLoadedModuleListHeadAddress(g_driverObject);

	fillInLoadKrnlModulesArray();

	UNICODE_STRING ntoskrnl = RTL_CONSTANT_STRING(L"ntoskrnl.exe");
	SIZE_T ntoskrnlIndex = getKrnlModuleEntryIndexByUnicodeModuleName(&ntoskrnl);

	CONST CHAR* ntCreateThreadExSpecOpcode = (CONST CHAR*)
		"40 55 53 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D ?? ?? ?? 48 8B ?? ?? ?? ?? ?? 48 33 C5 48 89 ?? ?? ?? ?? ?? 4D 8B F9 4C 89 ?? ?? 89 ?? ?? 48 8B F1";
	g_NtCreateThreadEx = searchOpcodeInKrnlModule(
		g_krnlModules[ntoskrnlIndex].moduleBase,
		g_krnlModules[ntoskrnlIndex].moduleImageSize,
		ntCreateThreadExSpecOpcode,
		55
	);

	if (g_NtCreateThreadEx == (ULONG_PTR)NULL)
	{
		//KeBugCheckEx(0x1239998C, 0, 0, 0, 0);
		DbgPrint("[!] 无法执行模块注入操作！\n");
	}

	CONST CHAR* miProcessLoaderEntrySpecOpcode = (CONST CHAR*)
		"48 8B C4 48 89 58 ?? 48 89 68 ?? 48 89 70 ?? 57 48 83 EC ?? 65 48 8B 2C 25 ?? ?? ?? ?? 48 83 ?? ?? 8B DA ?? ?? ?? ?? 48 8B F9";
	g_MiProcessLoaderEntry = searchOpcodeInKrnlModule(
		g_krnlModules[ntoskrnlIndex].moduleBase,
		g_krnlModules[ntoskrnlIndex].moduleImageSize,
		miProcessLoaderEntrySpecOpcode,
		42
	);

	if (g_MiProcessLoaderEntry != (ULONG_PTR)NULL)
	{
		//Hide this driver! Oops! U are the king of this machine now!
		((__int64 (*)(void*, unsigned __int32))g_MiProcessLoaderEntry)(g_driverObject->DriverSection, FALSE);
	}
	else
	{
		DbgPrint("[!] 隐藏驱动模块失败！\n");
	}

	g_PER_CPU_REGS = (ULONG_PTR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_totalCpuCount * sizeof(ULONG_PTR), 'zzaa');
	RtlZeroMemory(g_PER_CPU_REGS, g_totalCpuCount * sizeof(ULONG_PTR));

	g_vCpu = (PVCPU)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_totalCpuCount * sizeof(VCPU), 'zzaa');
	RtlZeroMemory(g_vCpu, g_totalCpuCount * sizeof(VCPU));

	SIZE_T regionSizeNeeded = 0x1000;
	ULONG vmcsIdentifier = (ULONG)(__readmsr(IA32_VMX_BASIC) & ~(1ull << 31));
	PHYSICAL_ADDRESS tempVmxon = { 0 };
	PHYSICAL_ADDRESS tempVmcsRegion = { 0 };

	for (size_t j = 0; j < g_totalCpuCount; j++)
	{
		g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS = ExAllocatePool2(POOL_FLAG_NON_PAGED, regionSizeNeeded, 'zzaa');
		RtlZeroMemory(g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS, regionSizeNeeded);
		tempVmxon = MmGetPhysicalAddress(g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS);
		g_vCpu[j].VMX_ON_REGION_PHYSICAL_ADDRESS = tempVmxon.QuadPart;

		g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS = ExAllocatePool2(POOL_FLAG_NON_PAGED, regionSizeNeeded, 'zzaa');
		RtlZeroMemory(g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS, regionSizeNeeded);
		tempVmcsRegion = MmGetPhysicalAddress(g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS);
		g_vCpu[j].VMX_VMCS_REGION_PHYSICAL_ADDRESS = tempVmcsRegion.QuadPart;

		RtlCopyMemory((PVOID)(g_vCpu[j].regs + 0x500), (PVOID)&g_vCpu[j].VMX_VMCS_REGION_PHYSICAL_ADDRESS, sizeof(ULONG_PTR));

		g_vCpu[j].virtualGuestStackSize = 0x5000;
		g_vCpu[j].virtualGuestStack = ExAllocatePool2(POOL_FLAG_NON_PAGED, g_vCpu[j].virtualGuestStackSize, 'zzaa');
		RtlZeroMemory(g_vCpu[j].virtualGuestStack, g_vCpu[j].virtualGuestStackSize);
		g_vCpu[j].virtualGuestStackBottom = (PVOID)((ULONG_PTR)g_vCpu[j].virtualGuestStack + g_vCpu[j].virtualGuestStackSize - 0x2000);

		g_vCpu[j].virtualHostStackSize = 0x5000;
		g_vCpu[j].virtualHostStack = ExAllocatePool2(POOL_FLAG_NON_PAGED, g_vCpu[j].virtualHostStackSize, 'zzaa');
		RtlZeroMemory(g_vCpu[j].virtualHostStack, g_vCpu[j].virtualHostStackSize);
		g_vCpu[j].virtualHostStackBottom = (PVOID)((ULONG_PTR)g_vCpu[j].virtualHostStack + g_vCpu[j].virtualHostStackSize - 0x2000);

		g_PER_CPU_REGS[j] = (ULONG_PTR)g_vCpu[j].regs;

		*(ULONG*)g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS = vmcsIdentifier;
		*(ULONG*)g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS = vmcsIdentifier;
	}

	SIZE_T PDTpages = PDT_PAGES(g_totalCpuCount);

	ULONG_PTR pPML4T = allocatePages(1);
	ULONG_PTR pPDPT = allocatePages(1);
	ULONG_PTR pPDT = allocatePages(PDTpages);
	ULONG_PTR pPT = allocatePages(PDTpages * 512);

	g_tpPML4T = pPML4T;
	g_tpPDPT = pPDPT;
	g_tpPDT = pPDT;
	g_tpPT = pPT;

	*(ULONG_PTR*)pPML4T = pPDPT;
	for (size_t i = 0; i < PDTpages; i++)
	{
		((ULONG_PTR*)pPDPT)[i] = pPDT + i * PAGE_SIZE;
	}
	for (size_t i = 0; i < PDTpages * 512; i++)
	{
		((ULONG_PTR*)pPDT)[i] = pPT + i * PAGE_SIZE;
	}
	for (size_t pteIndex = 0; pteIndex < PDTpages * 512 * 512; pteIndex++)
	{
		size_t BIT_20_12 = pteIndex % 512;
		size_t BIT_29_21 = (pteIndex / 512) % 512;
		size_t BIT_38_30 = (pteIndex / 0x40000) % 512;
		ULONG_PTR phyPageAddress = (ULONG_PTR)(((BIT_38_30 << 30) + (BIT_29_21 << 21) + (BIT_20_12 << 12)) & ~0xFFFull);
		UCHAR cacheType = getPhysicalMemoryCacheType((ULONG_PTR)phyPageAddress);
		((ULONG_PTR*)pPT)[pteIndex] = phyPageAddress | RWX | ((ULONG64)cacheType << 3);
	}
	for (size_t i = 0; i < PDTpages * 512; i++)
	{
		((ULONG_PTR*)pPDT)[i] = (ULONG_PTR)MmGetPhysicalAddress((PVOID)((ULONG_PTR*)pPDT)[i]).QuadPart | 0x7;
	}
	for (size_t i = 0; i < PDTpages; i++)
	{
		((ULONG_PTR*)pPDPT)[i] = (ULONG_PTR)MmGetPhysicalAddress((PVOID)((ULONG_PTR*)pPDPT)[i]).QuadPart | 0x7;
	}

	*(ULONG_PTR*)pPML4T = (ULONG_PTR)MmGetPhysicalAddress((PVOID) * (ULONG_PTR*)pPML4T).QuadPart | 0x7;
	pPML4T = (ULONG_PTR)MmGetPhysicalAddress((PVOID)pPML4T).QuadPart;
	g_ept = pPML4T;

	for (size_t j = 0; j < g_totalCpuCount; j++)
	{
		KAFFINITY oAffinity = KeSetSystemAffinityThreadEx((KAFFINITY)(1ull << j));
		virtualizationVtAndEpt(0);
		KeRevertToUserAffinityThreadEx(oAffinity);
	}

	return;
}

VOID exitVtStateWithEpt()
{
	for (size_t j = 0; j < g_totalCpuCount; j++)
	{
		KAFFINITY oAffinity = KeSetSystemAffinityThreadEx((KAFFINITY)(1ull << j));

		virtualOffVtAndEpt(0);

		UCHAR gdt[10] = { 0 };
		_sgdt(&gdt);
		*(USHORT*)gdt = 0x57i32;
		_lgdt(&gdt);

		UCHAR idt[10] = { 0 };
		__sidt(&idt);
		*(USHORT*)idt = 0xFFFui32;
		__lidt(&idt);

		KeRevertToUserAffinityThreadEx(oAffinity);
	}
	for (size_t j = 0; j < g_totalCpuCount; j++)
	{
		if (g_vCpu[j].virtualGuestStack)
		{
			ExFreePool(g_vCpu[j].virtualGuestStack);
			g_vCpu[j].virtualGuestStack = NULL;
		}
		if (g_vCpu[j].virtualHostStack)
		{
			ExFreePool(g_vCpu[j].virtualHostStack);
			g_vCpu[j].virtualHostStack = NULL;
		}
		if (g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS)
		{
			ExFreePool(g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS);
			g_vCpu[j].VMX_VMCS_REGION_VIRTUAL_KERNEL_ADDRESS = NULL;
		}
		if (g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS)
		{
			ExFreePool(g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS);
			g_vCpu[j].VMX_ON_REGION_VIRTUAL_KERNEL_ADDRESS = NULL;
		}
	}
	if (g_vCpu)
	{
		ExFreePool(g_vCpu);
		g_vCpu = NULL;
	}
	if (g_PER_CPU_REGS)
	{
		ExFreePool(g_PER_CPU_REGS);
		g_PER_CPU_REGS = NULL;
	}
	if (g_tpPT)
	{
		ExFreePool((PVOID)g_tpPT);
		g_tpPT = 0;
	}
	if (g_tpPDT)
	{
		ExFreePool((PVOID)g_tpPDT);
		g_tpPDT = 0;
	}
	if (g_tpPDPT)
	{
		ExFreePool((PVOID)g_tpPDPT);
		g_tpPDPT = 0;
	}
	if (g_tpPML4T)
	{
		ExFreePool((PVOID)g_tpPML4T);
		g_tpPML4T = 0;
	}
	if (g_MiProcessLoaderEntry != (ULONG_PTR)NULL)
	{
		((__int64 (*)(void*, unsigned __int32))g_MiProcessLoaderEntry)(g_driverObject->DriverSection, TRUE);
	}
	return;
}