#ifndef __VTBASE_H__
#define __VTBASE_H__

#include "IA32.h"
#include "symbols.h"

typedef UCHAR MEMORY_TYPE;

LOCAL_MODULE_EXTERN ULONG GUEST_CS_SELECTOR_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_SS_SELECTOR_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_GS_SELECTOR_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_FS_SELECTOR_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_FS_BASE_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_CR0_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_CR3_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_CR4_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_RFLAGS_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_RSP_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_RIP_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_GDTR_LIMIT_INDEX;
LOCAL_MODULE_EXTERN ULONG GUEST_IDTR_LIMIT_INDEX;
LOCAL_MODULE_EXTERN ULONG HOST_RIP_INDEX;
LOCAL_MODULE_EXTERN ULONG HOST_RSP_INDEX;
LOCAL_MODULE_EXTERN ULONG VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS_INDEX;
LOCAL_MODULE_EXTERN ULONG VECF_SECONDARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS_INDEX;
LOCAL_MODULE_EXTERN ULONG VM_EXIT_REASON_INDEX;
LOCAL_MODULE_EXTERN ULONG VM_EXIT_QUALIFICATION_INDEX;
LOCAL_MODULE_EXTERN ULONG VM_INSTRUCTION_LENGTH_INDEX;

LOCAL_MODULE_EXTERN PVCPU g_vCpu;
LOCAL_MODULE_EXTERN PULONG_PTR g_PER_CPU_REGS;
LOCAL_MODULE_EXTERN ULONG_PTR g_bugCheck;
LOCAL_MODULE_EXTERN ULONG g_totalCpuCount;
LOCAL_MODULE_EXTERN ULONG_PTR g_ept;
LOCAL_MODULE_EXTERN ULONG_PTR g_tpPML4T, g_tpPDPT, g_tpPDT, g_tpPT;

LOCAL_MODULE_EXTERN ULONG_PTR g_NtCreateThreadEx;

#define PDT_PAGES_32GB (512ui64)
#define PDT_PAGES_8GB (32ui64)
#define PDT_PAGES(totalCpuCount) (totalCpuCount == 2) ? PDT_PAGES_8GB: PDT_PAGES_32GB

#define RWX (0x7ull)
#define RW (0x3ull)
#define X (0x4ull)

typedef enum vmControlType
{
	VMX_CONTROL_PIN_BASED,
	VMX_CONTROL_PRIMARY_PROCESSOR_BASED,
	VMX_CONTROL_SECONDARY_PROCESSOR_BASED,
	VMX_CONTROL_VMENTRY,
	VMX_CONTROL_VMEXIT
}VMX_CONTROL_TYPE;

typedef union _PIN_BASED_VMCS_CONTROL
{
	struct
	{
		ULONG externalInterruptExiting : 1;
		ULONG _ : 2;
		ULONG nmiExiting : 1;
		ULONG _1 : 1;
		ULONG virtualNMIs : 1;
		ULONG activateVMXPreemptionTimer : 1;
		ULONG processPostedInterrupts : 1;
		ULONG _2 : 24;
	}Fields;
	ULONG64 AllFields;
}PIN_BASED_VMCS_CONTROL;

typedef union _PRIMARY_PROCESSOR_BASED_VMCS_CONTROL
{
	struct
	{
		ULONG _ : 2;
		ULONG interruptWindowExiting : 1;
		ULONG useTscOffsetting : 1;
		ULONG _1 : 3;
		ULONG hltExiting : 1;
		ULONG _2 : 1;
		ULONG invlpgExiting : 1;
		ULONG mwaitExiting : 1;
		ULONG rdpmcExiting : 1;
		ULONG rdtscExiting : 1;
		ULONG _3 : 2;
		ULONG cr3LoadExiting : 1;
		ULONG cr3StoreExiting : 1;
		ULONG activateTertiaryControls : 1;
		ULONG _4 : 1;
		ULONG cr8LoadExiting : 1;
		ULONG cr8StoreExiting : 1;
		ULONG useTPRShadow : 1;
		ULONG nmiWindowExiting : 1;
		ULONG movdrExiting : 1;
		ULONG uncondIptExiting : 1;
		ULONG useIOBitmaps : 1;
		ULONG _5 : 1;
		ULONG monitorTrapFlag : 1;
		ULONG useMsrBitmaps : 1;
		ULONG monitorExiting : 1;
		ULONG pauseLoopExiting : 1;
		ULONG activateSecondaryControls : 1;
	}Fields;
	ULONG AllFields;
}PRIMARY_PROCESSOR_BASED_VMCS_CONTROL;

typedef union _SECONDARY_PROCESSOR_BASED_VMCS_CONTROL
{
	struct
	{
		ULONG virtualizeApicAccesses : 1;
		ULONG enableEpt : 1;
		ULONG descriptorTableExiting : 1;
		ULONG enableRdtscp : 1;
		ULONG virtualizeX2ApicMode : 1;
		ULONG enableVpid : 1;
		ULONG wbinvdExiting : 1;
		ULONG unrestrictedGuest : 1;
		ULONG apicRegisterVirtualization : 1;
		ULONG virtualInterruptDelivery : 1;
		ULONG pauseLoopExiting : 1;
		ULONG rdrandExiting : 1;
		ULONG enableInvpcid : 1;
		ULONG enableVmfunc : 1;
		ULONG vmcsShadowing : 1;
		ULONG enableEnclsExiting : 1;
		ULONG rdseedExiting : 1;
		ULONG enablePml : 1;
		ULONG eptViolationExiting : 1;
		ULONG concealVmxFromSmx : 1;
		ULONG enableXsaveXstores : 1;
		ULONG pasidTranslation : 1;
		ULONG modeBasedExecutionControlForEpt : 1;
		ULONG subPageWritePermissionsForEpt : 1;
		ULONG intelPtUseGuestPhysicalAddresses : 1;
		ULONG useTscScaling : 1;
		ULONG enableUserWaitAndPause : 1;
		ULONG enablePconfig : 1;
		ULONG enableEnclvExiting : 1;
		ULONG _ : 1;
		ULONG vmmBusLockDetection : 1;
		ULONG instructionTimeout : 1;
	}Fields;
	ULONG AllFields;
}SECONDARY_PROCESSOR_BASED_VMCS_CONTROL;

typedef union _VM_EXIT_CONTROL
{
	struct
	{
		ULONG _ : 2;
		ULONG saveDebugControls : 1;
		ULONG _1 : 6;
		ULONG hostAddressSpaceSize : 1;
		ULONG _2 : 2;
		ULONG loadIa32PerfGlobalControl : 1;
		ULONG _3 : 2;
		ULONG acknowledgeInterruptOnExit : 1;
		ULONG _4 : 2;
		ULONG saveIa32Pat : 1;
		ULONG loadIa32Pat : 1;
		ULONG saveIa32Efer : 1;
		ULONG loadIa32Efer : 1;
		ULONG saveVmxPreemptionTimerValue : 1;
		ULONG clearIa32Bndcfgs : 1;
		ULONG concealVmxFromPt : 1;
		ULONG clearIa32RtitCtl : 1;
		ULONG clearIa32LbrCtl : 1;
		ULONG clearUinv : 1;
		ULONG loadCetState : 1;
		ULONG loadPkrs : 1;
		ULONG saveIa32PerfGlobalControl : 1;
		ULONG activateSecondaryControls : 1;
	}Fields;
	ULONG AllFields;
}VM_EXIT_CONTROL;

typedef union _VM_ENTRY_CONTROL
{
	struct
	{
		ULONG _ : 2;
		ULONG loadDebugControls : 1;
		ULONG _1 : 6;
		ULONG ia32eModeGuest : 1;
		ULONG entryToSmm : 1;
		ULONG deactivateDualMonitorTreatment : 1;
		ULONG _2 : 1;
		ULONG loadIa32PerfGlobalControl : 1;
		ULONG loadIa32Pat : 1;
		ULONG loadIa32Efer : 1;
		ULONG loadIa32Bndcfgs : 1;
		ULONG concealVmxFromPt : 1;
		ULONG loadIa32RtitCtl : 1;
		ULONG loadUinv : 1;
		ULONG loadCetState : 1;
		ULONG loadGuestIa32LbrCtl : 1;
		ULONG loadPkrs : 1;
		ULONG _3 : 9;
	}Fields;
	ULONG AllFields;
}VM_ENTRY_CONTROL;

typedef struct _VARIABLE_RANGE_MTRR_BASE_ADDRESS
{
	ULONG_PTR physicalBaseForRegion;
	ULONG_PTR sizeForRegion;
	UCHAR memoryTypeForRegion;
}VARIABLE_RANGE_MTRR_BASE_ADDRESS, * PVARIABLE_RANGE_MTRR_BASE_ADDRESS;

VOID getSegementRegisterAttributes(
	IN ULONG type,
	IN ULONG64 selector,
	IN UCHAR unusable,
	IN OUT SRA* sra
);

MEMORY_TYPE getPhysicalMemoryCacheType(
	ULONG_PTR physicalAddress
);

ULONG_PTR virtualOffVtAndEpt(
	ULONG_PTR arg
);

ULONG_PTR virtualizationVtAndEpt(
	ULONG_PTR arg
);

VOID enterVtStateWithEpt();

VOID exitVtStateWithEpt();

#endif