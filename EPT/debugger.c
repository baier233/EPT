#include "debugger.h"
#include <ntstrsafe.h>

#pragma warning(disable: 4702 6387 6011 6386)

//版本：v 1.0.0
//此版本暂时不分离文件管理，集中写在一个.c源文件中，是一个最简化demo
//后期会逐步分离

//各大功能的回调专用：当驱动卸载触发时，【手动设置此数值为TRUE】并设置事件引起所有的被动线程退出
ULONG g_dbgGlobal_isDriverUnloadCalled;
//全部CPU数量
#define TOTAL_CPU_COUNT (20)

/*************************************************************************************************/
//CPUID_EXIT后是否需要真正执行CPUID的标志数组
//注意，如果不跳过CPUID，那么其执行结果可能会意外地改变寄存器数值，从而跑偏控制流
//此数值已经在COUID_C_DISPATCHER中进行过赋值，在各个子功能(YJDD, ZLGZ, FXGZ, NCDX等)的HANDLER内不用处理
ULONG g_whetherCpuidNeedExecuteReally[TOTAL_CPU_COUNT];
/*************************************************************************************************/

/*************************************************************************************************/
//调试器(debugger)相关全局变量。它们一旦被赋值，截止到DLL卸载前不会再次改动
ULONG_PTR g_dbgGlobal_debuggerPid;
ULONG_PTR g_dbgGlobal_debuggerCr3;
SIZE_T g_dbgGlobal_debuggerIoRegionSize;
ULONG_PTR g_dbgGlobal_debuggerIoRegionUser;
/*************************************************************************************************/

/*************************************************************************************************/
//被调试程序(debuggee)相关全局变量。根据调试器UI输入，他们可能会改变
#define USER_MODE_MODULE_NAME_MAX_ANSI_CHARS  (100) //被调试程序中每个模块的以ANSI字符计算的名字最大字符限制(包括后缀)
#define USER_MODE_MODULES_COUNT_LIMIT (500) //被调试程序中所有模块计数的最大数量限制
#define MAGIC_NUMBER_OF_USER_MODULE_EXISTING (0x11223344) //用于记录被调试程序中模块条目的存在性标头。存在时，此前导魔法数字会被赋值到条目头部，用于快速遍历
#define INVALID_USER_MODULE_INDEX (0xFFFFFFFFFFFFFFFF) //记录当遍历被调试程序的模块时，如果未找到模块情况下的INDEX非法返回值
typedef struct _IN_LOAD_USER_MODE_MODULES
{
	ULONG existingHeader; //存在性标头。如果此模块条目存在，此值会被定义为宏MAGIC_NUMBER_OF_USER_MODULE_EXISTING
	ULONG_PTR moduleBase; //模块在内存中加载的基地址
	UCHAR moduleNameInWideChar[USER_MODE_MODULE_NAME_MAX_ANSI_CHARS]; //注意：以WCHAR存储！例如abc.exe\0，那么moduleNameInWideChar->a 0 b 0 c 0 . 0 e 0 x 0 e 0 \0 0
	SIZE_T moduleImageSize; //模块在内存中的映像大小
}IN_LOAD_USER_MODE_MODULES, * PIN_LOAD_USER_MODE_MODULES;
//在每个TabPage功能页面点击【锁定】控件后，此五者会根据调试器输入的PID进行动态赋值！
//这五个成员被赋值时的调用号没有统一路径，各个控件都有可能对它们五个成员赋值
//在任何TabPage功能页面点击【解锁】控件后，此五者必须恢复原值！
//恢复时，只需调用一次【通用API】的restoreDebuggeeGlobalAttributesInitialState函数就可以完成
//问题：为什么g_dbgGlobal_funcId不需要per-CPU？
//	1.同一时间在【解锁】之前，调试子功能Id【不会被改变】(见checkDebuggeeCertainGlobalAttributesBeforeAttach)；
//	2.CPU触发MTF/EPT_VIOLATION后，需要根据g_dbgGlobal_funcId来找到进入哪里的VT流程，但是其他核心作用到某个EPT页面后并不会知道哪些核心会访问此页面：
//		比如核心0选择了ZLGZ功能，cpu[0].g_dbgGlobal_funcId == ZLGZ，把页面CORE_PAGE的EPT设置为RW禁X；
//		现在核心1也要访问CORE_PAGE，但是cpu[1].g_dbgGlobal_funcId并没有被显式地赋值为ZLGZ的funcId，这会把cpu[1]错误地分配到enum==0的那个【默认】EPT分支导致严重错误！
//		根据g_dbgGlobal_funcId的【不变性】，只要核心1修改了g_dbgGlobal_funcId，其他所有核心都可以根据唯一的g_dbgGlobal_funcId定位正确的EPT/MTF分支！
//简单记忆g_dbgGlobal_funcId的含义：在【同一个】调试子功能下，所有核心的MTF/EPTV都会被路由到同一funcId的处理函数
ULONG_PTR g_dbgGlobal_debuggeePid = 0xFFFFFFFFFFFFFFFF;								  //【1】
ULONG_PTR g_dbgGlobal_debuggeeCr3 = 0xFFFFFFFFFFFFFFFF;								  //【2】
ULONG g_dbgGlobal_isUserSpaceOnly = 0xFFFFFFFF;										  //【3】
ULONG g_dbgGlobal_funcId = 0xFFFFFFFF;												  //【4】
IN_LOAD_USER_MODE_MODULES g_dbgGlobal_debuggeeModules[USER_MODE_MODULES_COUNT_LIMIT]; //【5】
/*************************************************************************************************/

//内核模块和用户模块以窄字符格式记录的长度的最大值
#define MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE max(USER_MODE_MODULE_NAME_MAX_ANSI_CHARS, KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS)

/*************************************************************************************************/
//通用API
VOID enableMTF()
{
	PRIMARY_PROCESSOR_BASED_VMCS_CONTROL priCtl = { 0 };
	__vmx_vmread(VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, &priCtl.AllFields);
	priCtl.Fields.monitorTrapFlag = 1;
	__vmx_vmwrite(VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, priCtl.AllFields);
	return;
}

VOID disableMTF()
{
	PRIMARY_PROCESSOR_BASED_VMCS_CONTROL priCtl = { 0 };
	__vmx_vmread(VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, &priCtl.AllFields);
	priCtl.Fields.monitorTrapFlag = 0;
	__vmx_vmwrite(VECF_PRIMARY_PROCESSOR_BASED_VM_EXECUTION_CONTROLS, priCtl.AllFields);
	return;
}

ULONG_PTR getKernelDiscriptorTableBaseRawInKprcb(
	ULONG_PTR _kprcb
)
{
	return *(ULONG_PTR*)(_kprcb + KERNEL_DISCRIPTOR_TABLE_BASE_OFFSET_IN_KPRCB);
}

UINT32 isCurrentProcessorInDebuggeeContext(
	ULONG_PTR vmGuestCr3,
	ULONG_PTR kernelDiscriptorTableBaseRawInKprcb
)
{
	if ((vmGuestCr3 == g_dbgGlobal_debuggeeCr3) || (((kernelDiscriptorTableBaseRawInKprcb << 16) >> 16) == g_dbgGlobal_debuggeeCr3))
	{
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

SIZE_T getPteIndexByPhysicalAddressAligned(
	ULONG_PTR physicalAddressAligned
)
{
	ULONG_PTR alignedAddress = physicalAddressAligned & ~0xFFFull;

	SIZE_T pteIndex = (alignedAddress >> 12) & 0x1FFull;  // [20:12]
	SIZE_T pdtIndex = (alignedAddress >> 21) & 0x1FFull;  // [29:21]
	SIZE_T pdptIndex = (alignedAddress >> 30) & 0x1FFull; // [38:30]
	SIZE_T pml4Index = (alignedAddress >> 39) & 0x1FFull; // [47:39]

	return (pml4Index * 512 * 512 * 512) + (pdptIndex * 512 * 512) + (pdtIndex * 512) + pteIndex;
}

VOID fixSinglePte(
	ULONG_PTR pPte, 
	ULONG_PTR pageFramePhysicalAddressAligned, 
	ULONG64 rights
)
{
	if (pPte == g_tpPT)
	{
		//这里如果蓝屏意味着调用此函数时，pteIndex为0导致g_tpPT + pteIndex * sizeof(ULONG_PTR) == g_tpPT，也就是修改了g_tpPT[0].
		//而g_tpPT[0]是整个EPT的根基，如果被修改了，那么整个EPT都会崩溃，所以直接蓝屏
		KeBugCheckEx(0xC4826ACE, 0, 0, 0, 0);
	}
	*(ULONG_PTR*)pPte = pageFramePhysicalAddressAligned | rights | ((ULONG64)getPhysicalMemoryCacheType(pageFramePhysicalAddressAligned) << 3);
	__vsm__flushEptCache();
	return;
}

ULONG64 getCR3SaferByPID(
	IN ULONG64 pid
)
{
	PEPROCESS pe = NULL;
	PsLookupProcessByProcessId((HANDLE)pid, &pe);
	ULONG64 cr3 = *(ULONG64*)((ULONG64)pe + 0x28);
	ObDereferenceObject(pe);
	return cr3;
}

SIZE_T getUserModuleEntryIndexByUnicodeModuleName(
	PUNICODE_STRING userModuleNameUnicode
)
{
	if (!userModuleNameUnicode || !userModuleNameUnicode->Buffer)
		return INVALID_USER_MODULE_INDEX;

	SIZE_T userLenChars = userModuleNameUnicode->Length / sizeof(WCHAR);
	if (userLenChars == 0)
		return INVALID_USER_MODULE_INDEX;

	SIZE_T maxModuleChars =
		USER_MODE_MODULE_NAME_MAX_ANSI_CHARS / sizeof(WCHAR);

	for (SIZE_T i = 0; i < USER_MODE_MODULES_COUNT_LIMIT; ++i)
	{
		PIN_LOAD_USER_MODE_MODULES mod = &g_dbgGlobal_debuggeeModules[i];

		if (mod->existingHeader != MAGIC_NUMBER_OF_USER_MODULE_EXISTING)
			break;

		PWCHAR moduleName = (PWCHAR)mod->moduleNameInWideChar;

		SIZE_T j = 0;
		for (; j < userLenChars && j < maxModuleChars; ++j)
		{
			WCHAR c1 = moduleName[j];
			WCHAR c2 = userModuleNameUnicode->Buffer[j];

			if (c1 == L'\0')
				break;

			if (c1 >= L'A' && c1 <= L'Z') c1 += (L'a' - L'A');
			if (c2 >= L'A' && c2 <= L'Z') c2 += (L'a' - L'A');

			if (c1 != c2)
				break;
		}

		if (j == userLenChars && moduleName[j] == L'\0')
		{
			return i;
		}
	}

	return INVALID_USER_MODULE_INDEX;
}

VOID transWideModuleNameStringToAnsi(
	UCHAR* wideStr,
	UCHAR (*ansiBuffer)[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE]
)
{
	SIZE_T idx = 0;
	for (SIZE_T j = 0; j < MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE - 2 && wideStr[j] != 0x0; j += 2)
	{
		(*ansiBuffer)[idx++] = wideStr[j];
	}
	(*ansiBuffer)[idx++] = 0x0;
	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//被调试程序全局资源的初始化，验证和清理专用API
//他们会动态地修改g_dbgGlobal_debuggee前导全局变量
//注意：checkDebuggeeCertainGlobalAttributesBeforeAttach中有主动蓝屏的行为
VOID checkDebuggeeCertainGlobalAttributesBeforeAttach()
{
	//为什么不检查g_dbgGlobal_funcID？
	//	1.g_dbgGlobal_funcID在CPUID_C_DISPATCHER就被赋值了，顺序比attach更早；
	//	2.检查它没有意义：g_dbgGlobal_funcID永远都是最后一次的调试功能号，把不同功能Id作为attach的参数时，结果永远相等
	//	2的因果就是先分发DISPATCHER，此时g_dbgGlobal_funcID已经被赋值路由到CPUID_FUNCID_HANDLER，在CPUID_FUNCID_HANDLER作检查FUNCID == g_dbgGlobal_funcID没意义
	if (
		g_dbgGlobal_debuggeePid != 0xFFFFFFFFFFFFFFFF
		||
		g_dbgGlobal_debuggeeCr3 != 0xFFFFFFFFFFFFFFFF
		||
		g_dbgGlobal_isUserSpaceOnly != 0xFFFFFFFF
		||
		g_dbgGlobal_debuggeeModules[0].existingHeader == MAGIC_NUMBER_OF_USER_MODULE_EXISTING
	)
	{
		KeBugCheckEx(0x7A7B7C7D, 0, 0, 0, 0);
	}
	return;
}

VOID restoreDebuggeeFiveGlobalAttributesInitialState()
{
	g_dbgGlobal_isUserSpaceOnly = 0xFFFFFFFF;
	g_dbgGlobal_funcId = 0xFFFFFFFF;
	g_dbgGlobal_debuggeePid = 0xFFFFFFFFFFFFFFFF;
	g_dbgGlobal_debuggeeCr3 = 0xFFFFFFFFFFFFFFFF;
	RtlZeroMemory(g_dbgGlobal_debuggeeModules, sizeof(g_dbgGlobal_debuggeeModules));
	return;
}

VOID getDebuggeePidCr3AndModulesInfo(
	IN ULONG debuggeePid
)
{
	//如果任何一个TabPage试图锁定PID但是先前已经有值，那么直接蓝屏
	//原因是使用完某一个调试子功能后，没有点击【解锁】按钮的前提下在其他调试子功能界面又一次锁定了PID
	checkDebuggeeCertainGlobalAttributesBeforeAttach();

	g_dbgGlobal_debuggeePid = (ULONG_PTR)debuggeePid;

	g_dbgGlobal_debuggeeCr3 = getCR3SaferByPID(g_dbgGlobal_debuggeePid);

	PEPROCESS pe = NULL;
	PsLookupProcessByProcessId((HANDLE)g_dbgGlobal_debuggeePid, &pe);

	//注意：ObDereferenceObject(pe)在此函数最后的return之前，这里千万别重复写

	if (g_dbgGlobal_debuggeeCr3 == 0 || g_dbgGlobal_debuggeeCr3 == 0xFFFFFFFFFFFFFFFF)
	{
		KeBugCheckEx(0xACCA1818, 0, 0, 0, 0);
	}

	//这里pebAddress = (ULONG_PTR)pe + 0x550而pe是内核地址，所以pebAddress也一定是内核地址
	ULONG_PTR peb = *(ULONG_PTR*)((ULONG_PTR)pe + 0x550);
	DbgPrint("当前被调试程序的debugPort: 0x%llX\n", (ULONG_PTR)pe + 0x578);
	ULONG_PTR pldAddress = peb + 0x18;

	ULONG_PTR pldPhysical = getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, pldAddress);
	ULONG_PTR pld = 0;
	readPhysicalAddress((PVOID)pldPhysical, &pld, sizeof(ULONG_PTR), NULL); 
	ULONG_PTR initialEntryAddress = (ULONG_PTR)pld + 0x10;
	ULONG_PTR temp = initialEntryAddress;
	SIZE_T idx = 0x0;
	ULONG_PTR initialEntryAddress_Blink = 0;
	readPhysicalAddress((PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, initialEntryAddress + sizeof(PVOID)), &initialEntryAddress_Blink, sizeof(ULONG_PTR), NULL);
	while (temp != initialEntryAddress_Blink)
	{
		ULONG_PTR temp_Flink = 0;
		readPhysicalAddress(
			(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, temp),
			&temp_Flink,
			sizeof(ULONG_PTR),
			NULL
		);

		g_dbgGlobal_debuggeeModules[idx].existingHeader = MAGIC_NUMBER_OF_USER_MODULE_EXISTING;

		ULONG_PTR moduleBaseAddress = temp_Flink + 0x30;
		readPhysicalAddress(
			(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, moduleBaseAddress),
			&g_dbgGlobal_debuggeeModules[idx].moduleBase,
			sizeof(ULONG_PTR),
			NULL
		);

		ULONG_PTR moduleImageSizeAddress = temp_Flink + 0x40;
		readPhysicalAddress(
			(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, moduleImageSizeAddress),
			&g_dbgGlobal_debuggeeModules[idx].moduleImageSize,
			sizeof(SIZE_T),
			NULL
		);

		//根据DDK上说明,Unicode格式(%C, %S, %lc, %ls, %wc, %ws, and %wZ)只能在 IRQL = PASSIVE_LEVEL 时才能使用.
		UNICODE_STRING tempUnicodeStringStruct = { 0 };
		ULONG_PTR pBaseDllName = temp_Flink + 0x58;
		readPhysicalAddress(
			(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, pBaseDllName),
			&tempUnicodeStringStruct,
			sizeof(UNICODE_STRING),
			NULL
		);

		ULONG_PTR w_moduleNamePointer = (ULONG_PTR)tempUnicodeStringStruct.Buffer;
		SIZE_T realSize = min(tempUnicodeStringStruct.MaximumLength, USER_MODE_MODULE_NAME_MAX_ANSI_CHARS - 2);

		readPhysicalAddress(
			(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, w_moduleNamePointer),
			g_dbgGlobal_debuggeeModules[idx].moduleNameInWideChar,
			realSize,
			NULL
		);

		temp = temp_Flink;
		idx++;
	}
	ObDereferenceObject(pe);
	return;
}

VOID printDebuggeeModulesInfo()
{
	for (SIZE_T j = 0; j < USER_MODE_MODULES_COUNT_LIMIT; j++)
	{
		if (g_dbgGlobal_debuggeeModules[j].existingHeader == MAGIC_NUMBER_OF_USER_MODULE_EXISTING)
		{
			UCHAR tempModuleNameInAnsi[(USER_MODE_MODULE_NAME_MAX_ANSI_CHARS / 2) + 2] = { 0 };
			RtlZeroMemory(tempModuleNameInAnsi, (USER_MODE_MODULE_NAME_MAX_ANSI_CHARS / 2) + 2);
			SIZE_T idx = 0;
			for (SIZE_T i = 0; i < USER_MODE_MODULE_NAME_MAX_ANSI_CHARS; i += 2)
			{
				if (g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar[i] != 0x0)
				{
					tempModuleNameInAnsi[idx++] = g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar[i];
				}
				else
				{
					break;
				}
			}
			tempModuleNameInAnsi[idx] = 0x0;
			//禁止使用UNICODE说明符%C S wS wZ打印，否则触发由于DbgPrint导致的蓝屏：IRQL_NOT_LESS_OR_EQUAL
			DbgPrint("模块: %s \t 基地址: 0x%llX \t 模块大小: 0x%llX\n",
				(CONST CHAR*)tempModuleNameInAnsi,
				g_dbgGlobal_debuggeeModules[j].moduleBase,
				g_dbgGlobal_debuggeeModules[j].moduleImageSize
			);
		}
		else
		{
			return;
		}
	}
}
/*************************************************************************************************/

/*************************************************************************************************/
//DLL加载回调：对调试器(debugger)相关全局变量进行赋值。它们一旦被赋值，截止到DLL卸载前不会再次改动
VOID CPUID_LOAD_DLL_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	//测试通过
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_kprcb);

	g_dbgGlobal_debuggerPid = *(ULONG_PTR*)(_rbp + 0x80); //rcx
	g_dbgGlobal_debuggerIoRegionUser = *(ULONG_PTR*)(_rbp + 0x78); //rdx
	g_dbgGlobal_debuggerIoRegionSize = *(SIZE_T*)(_rbp + 0x58); //r8
	g_dbgGlobal_debuggerCr3 = getCR3SaferByPID(g_dbgGlobal_debuggerPid);

	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//硬件断点专用相关结构和变量
//测试用例：坐标.exe.测试谁读写了地址0xAAB82FFAE0
//UI中，硬件断点界面的地址输入框的数量
#define YJDD_MONITORING_ADDRESSES_COUNT (8)
//UI中，YJDD_MONITORING_ADDRESSES_COUNT(8)个文本框对应的地址数组
ULONG_PTR g_YJDD_monitorAddressArray[YJDD_MONITORING_ADDRESSES_COUNT];
//EPT相关：每个CPU核心触发EPT_VIOLATION的三个关键属性
SIZE_T g_YJDD_faultPteIndex[TOTAL_CPU_COUNT];
ULONG_PTR g_YJDD_physicalAddress[TOTAL_CPU_COUNT];
UINT32 g_YJDD_isHittedInArray[TOTAL_CPU_COUNT];
//访问者RIP的结构定义
#define YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE (52)
typedef struct _YJDD_ACCESSOR_RIP_INFO
{
	ULONG_PTR accessorRip;
	ULONG_PTR accesseeAddress;
	SIZE_T offsetOfAccessorRipInModule;
	USHORT accessTimes;
	USHORT accessType;
	UCHAR moduleNameAnsi[YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE];
}YJDD_ACCESSOR_RIP_INFO, * PYJDD_ACCESSOR_RIP_INFO;
//定义全新的访问者的最大数量
//只要访问者和访问方式有一个不同，就被视为一个全新的条目（同一个访问者的读/写访问被视为两个条目）
#define YJDD_MAX_ACCESSORS_COUNT (1000)
//全部的访问者在案条目
YJDD_ACCESSOR_RIP_INFO g_YJDD_allAccessorsInRecord[YJDD_MAX_ACCESSORS_COUNT]; //后续修改为侵入式链表
//当访问者RIP不属于任何模块时，成员offsetOfAccessorRipInModule的魔法数值
#define YJDD_ACCESSOR_RIP_NOT_FIND_IN_ALL_MODULES (0xFFFFui16)
//硬件断点API
VOID registerAccessorRipInfo(
	IN  ULONG_PTR accessorRip,
	IN  ULONG_PTR accesseeAddress,
	IN  USHORT accessType
)
{
	SIZE_T currentEnptyEntryIndex = 0;
	for (; currentEnptyEntryIndex < YJDD_MAX_ACCESSORS_COUNT; currentEnptyEntryIndex++)
	{
		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip == accessorRip && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessType == accessType && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accesseeAddress == accesseeAddress)
		{
			g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessTimes++;
			return;
		}
		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip == (ULONG_PTR)NULL)
		{
			break;
		}
	}

	g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip = accessorRip;
	g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accesseeAddress = accesseeAddress;
	g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessType = accessType;

	if ((LONG64)g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip < 0)
	{
		for (USHORT j = 0; j < KRNL_MODE_MODULES_COUNT_LIMIT; j++)
		{
			PIN_LOAD_KRNL_MODE_MODULES mod = &g_krnlModules[j];

			if (mod->existingHeader != MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING)
				break;

			ULONG_PTR base = mod->moduleBase;
			ULONG_PTR end = base + mod->moduleImageSize;

			if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip >= base && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip < end)
			{
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule = (SIZE_T)(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip - base);

				UCHAR moduleAnsiName[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE] = { 0 };
				transWideModuleNameStringToAnsi(g_krnlModules[j].moduleNameInWideChar, &moduleAnsiName);
				moduleAnsiName[YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE - 1] = 0x0;
				RtlCopyMemory(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi, moduleAnsiName, YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE);

				break;
			}
		}

		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule == 0)
		{
			g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule = 0xFFFFFFFFFFFFFFFF;
		}

		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi[0] == 0x0)
		{
			RtlCopyMemory(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi, "UnknownModule.sys", sizeof("UnknownModule.sys"));
		}
	}
	else
	{
		for (USHORT j = 0; j < USER_MODE_MODULES_COUNT_LIMIT; j++)
		{
			PIN_LOAD_USER_MODE_MODULES mod = &g_dbgGlobal_debuggeeModules[j];

			if (mod->existingHeader != MAGIC_NUMBER_OF_USER_MODULE_EXISTING)
				break;

			ULONG_PTR base = mod->moduleBase;
			ULONG_PTR end = base + mod->moduleImageSize;

			if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip >= base && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip < end)
			{
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule = (SIZE_T)(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip - base);

				UCHAR moduleAnsiName[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE] = { 0 };
				transWideModuleNameStringToAnsi(g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar, &moduleAnsiName);
				moduleAnsiName[YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE - 1] = 0x0;
				RtlCopyMemory(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi, moduleAnsiName, YJDD_MAX_ANSI_SIZE_OF_ACCESSOR_RIP_LOCATED_MODULE);
				
				break;
			}
		}

		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule == 0)
		{
			g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule = 0xFFFFFFFFFFFFFFFF;
		}

		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi[0] == 0x0)
		{
			RtlCopyMemory(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi, "UnknownModule.exe", sizeof("UnknownModule.exe"));
		}
	}

	return;
}

VOID printRegisteredAccessorRipInfo(
	ULONG_PTR accessorRip,
	ULONG_PTR accesseeAddress,
	USHORT accessType
)
{
	for (SIZE_T currentEnptyEntryIndex = 0; currentEnptyEntryIndex < YJDD_MAX_ACCESSORS_COUNT; currentEnptyEntryIndex++)
	{
		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip == accessorRip && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessType == accessType && g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accesseeAddress == accesseeAddress)
		{
			DbgPrint("位于地址0x%llX(%s + 0x%llX)的指令 %s 了内存地址0x%llX共计%hu次\n",
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip,
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].moduleNameAnsi,
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].offsetOfAccessorRipInModule,
				(g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessType == 1) ? "读" : "写",
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accesseeAddress,
				g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessTimes
			);
		}
		if (g_YJDD_allAccessorsInRecord[currentEnptyEntryIndex].accessorRip == (ULONG_PTR)NULL)
		{
			return;
		}
	}
	//理应不会打印，除非记录的个数超过了YJDD_MAX_ACCESSORS_COUNT
	//但是这个情况太少见了
	DbgPrint("此rip: 0x%llX未注册！\n", accessorRip);
	return;
}

VOID MTF_YJDD_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG currentProcessorNumber = KeGetCurrentProcessorNumber();

	fixSinglePte(
		g_tpPT + g_YJDD_faultPteIndex[currentProcessorNumber] * sizeof(ULONG_PTR),
		g_YJDD_physicalAddress[currentProcessorNumber] & ~0xFFF,
		X
	);

	disableMTF();

	return;
}

VOID EPT_VIOLATION_YJDD_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG_PTR guestLinerAddress = 0;
	__vmx_vmread(VEIF_GUEST_LINER_ADDRESS, &guestLinerAddress);

	ULONG_PTR guestPhysicalAddress = 0;
	__vmx_vmread(VEIF_GUEST_PHYSICAL_ADDRESS, &guestPhysicalAddress);

	ULONG_PTR exitQualification = 0;
	__vmx_vmread(VEIF_EXIT_QUALIFICATION, &exitQualification);

	ULONG currentProcessorNumber = KeGetCurrentProcessorNumber();

	//不要把他理解为数组赋值
	//把它理解成：当前这个CPU核心对应的faultPteIndex/physicalAddress/isHittedInArray是什么数值
	g_YJDD_faultPteIndex[currentProcessorNumber] = getPteIndexByPhysicalAddressAligned(guestPhysicalAddress & ~0xFFF);
	g_YJDD_physicalAddress[currentProcessorNumber] = guestPhysicalAddress;
	g_YJDD_isHittedInArray[currentProcessorNumber] = FALSE;

	SIZE_T addressEntryIndexOnUiTextBox = 0;

	//不要把它理解为数组循环
	//把它理解成：当前这个CPU触发的访问违例地址是否在监控列表中
	for (SIZE_T j = 0; j < YJDD_MONITORING_ADDRESSES_COUNT; j++)
	{
		if (guestLinerAddress == g_YJDD_monitorAddressArray[j])
		{
			g_YJDD_isHittedInArray[currentProcessorNumber] = TRUE;
			addressEntryIndexOnUiTextBox = j; //addressEntryIndexOnUiTextBox的0~7对应TextBox的1~8
			break;
		}
	}

	UINT32 shouldMonitor = g_YJDD_isHittedInArray[currentProcessorNumber];

	if (g_dbgGlobal_isUserSpaceOnly == TRUE && ((LONG64)_rip < 0))
	{
		shouldMonitor = FALSE;
	}

	USHORT accessType = 0;

	if (shouldMonitor)
	{
		if (exitQualification & 0x1ull)
		{
			accessType = 1; //读
		}
		if (exitQualification & 0x2ull)
		{
			accessType = 2; //写
		}
		registerAccessorRipInfo(_rip, guestLinerAddress, accessType);
		printRegisteredAccessorRipInfo(_rip, guestLinerAddress, accessType);
	}

	enableMTF();

	fixSinglePte(
		g_tpPT + g_YJDD_faultPteIndex[currentProcessorNumber] * sizeof(ULONG_PTR),
		g_YJDD_physicalAddress[currentProcessorNumber] & ~0xFFF,
		RWX
	);

	return;
}

VOID CPUID_YJDD_HANDLER(
	ULONG eaxNumber,
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG64 rcx = *(ULONG64*)(_rbp + 0x80);
	ULONG64 rdx = *(ULONG64*)(_rbp + 0x78);

	switch (eaxNumber)
	{
		case 0xACC00001: //__asm__YJDD__lockPidCallBack PROC ;rcx == debuggeePid
		{
			getDebuggeePidCr3AndModulesInfo((ULONG)rcx);
			break;
		}
		case 0xACC00010: //__asm__YJDD__unlockPidCallBack PROC ;none
		{
			//清空YJDD模块私有内存
			RtlZeroMemory(g_YJDD_monitorAddressArray, YJDD_MONITORING_ADDRESSES_COUNT * sizeof(ULONG_PTR));
			RtlZeroMemory(g_YJDD_faultPteIndex, TOTAL_CPU_COUNT * sizeof(SIZE_T));
			RtlZeroMemory(g_YJDD_physicalAddress, TOTAL_CPU_COUNT * sizeof(ULONG_PTR));
			RtlZeroMemory(g_YJDD_isHittedInArray, TOTAL_CPU_COUNT * sizeof(UINT32));
			RtlZeroMemory(g_YJDD_allAccessorsInRecord, YJDD_MAX_ACCESSORS_COUNT * sizeof(YJDD_ACCESSOR_RIP_INFO));
			//恢复被调试程序(debuggee)相关的五大全局变量
			restoreDebuggeeFiveGlobalAttributesInitialState();
			break;
		}
		case 0xACC00100: //__asm__YJDD__deliverUiIndexAndAddressesToDriver PROC; rcx == addressTextBoxIndex, rdx == addressAdded
		{
			//Dll会重复调用此过程一共YJDD_MONITORING_ADDRESSES_COUNT(8)次
			g_YJDD_monitorAddressArray[rcx] = rdx;
			break;
		}
		case 0xACC01000: //__asm__YJDD__startMonitoringCallBack PROC ;rcx == uIsUserSpaceOnly
		{
			//进行仅用户空间判定赋值
			g_dbgGlobal_isUserSpaceOnly = (ULONG)rcx;
			//清空YJDD模块私有内存
			RtlZeroMemory(g_YJDD_faultPteIndex, TOTAL_CPU_COUNT * sizeof(SIZE_T));
			RtlZeroMemory(g_YJDD_physicalAddress, TOTAL_CPU_COUNT * sizeof(ULONG_PTR));
			RtlZeroMemory(g_YJDD_isHittedInArray, TOTAL_CPU_COUNT * sizeof(UINT32));
			RtlZeroMemory(g_YJDD_allAccessorsInRecord, YJDD_MAX_ACCESSORS_COUNT * sizeof(YJDD_ACCESSOR_RIP_INFO));
			//循环地把UI中的那YJDD_MONITORING_ADDRESSES_COUNT(8)个地址所对应的物理地址的EPT页面权限设置为【不可读不可写可执行】X
			for (SIZE_T j = 0; j < YJDD_MONITORING_ADDRESSES_COUNT && g_YJDD_monitorAddressArray[j] != 0x0; j++)
			{
				ULONG_PTR currentEntryAddressMonitoringPhysicalAligned = getPhysicalAddressByCR3AndVirtualAddress(
					g_dbgGlobal_debuggeeCr3, 
					g_YJDD_monitorAddressArray[j]
				) & ~0xFFFull;
				if (currentEntryAddressMonitoringPhysicalAligned != 0)
				{
					SIZE_T currentPhysicalAddressPte = getPteIndexByPhysicalAddressAligned(currentEntryAddressMonitoringPhysicalAligned);
					fixSinglePte(
						g_tpPT + currentPhysicalAddressPte * sizeof(ULONG_PTR),
						currentEntryAddressMonitoringPhysicalAligned,
						X
					);
				}
			}
			//返回
			break;
		}
		case 0xACC10000: //__asm__YJDD__stopMonitoringCallBack PROC ;none
		{
			for (SIZE_T j = 0; j < YJDD_MONITORING_ADDRESSES_COUNT && g_YJDD_monitorAddressArray[j] != 0x0; j++) //物理机第一次蓝屏：0x8F8FCCD6. 原因：没有添加 g_YJDD_monitorAddressArray[j] != 0x0 条件
			{
				//循环地把UI中的那YJDD_MONITORING_ADDRESSES_COUNT(8)个地址所对应的物理地址的EPT页面权限设置为【可读可写可执行】RWX
				ULONG_PTR currentEntryAddressMonitoringPhysicalAligned = getPhysicalAddressByCR3AndVirtualAddress(
					g_dbgGlobal_debuggeeCr3,
					g_YJDD_monitorAddressArray[j]
				) & ~0xFFFull;
				if (currentEntryAddressMonitoringPhysicalAligned != 0)
				{
					SIZE_T currentPhysicalAddressPte = getPteIndexByPhysicalAddressAligned(currentEntryAddressMonitoringPhysicalAligned);
					fixSinglePte(
						g_tpPT + currentPhysicalAddressPte * sizeof(ULONG_PTR),
						currentEntryAddressMonitoringPhysicalAligned,
						RWX
					);
				}
			}
			break;
		}
		default:
		{
			break;
		}
	}
	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//指令跟踪专用相关结构和变量
//测试用例：坐标.exe.从0x7FF70C0F109A跟踪后续1000条指令
//UI中，起始地址的输入框输入内容
ULONG_PTR g_ZLGZ_addressBeginTracingVirtual;
//UI中，起始地址输入框内容对应的物理地址
ULONG_PTR g_ZLGZ_addressBeginTracingPhysical;
//UI中，起始地址输入框内容对应的4KB对齐物理地址
ULONG_PTR g_ZLGZ_addressBeginTracingPhysicalAligned;
//UI中，起始地址输入框内容对应的物理地址的PTE索引
ULONG_PTR g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned;
//UI中，跟踪次数的输入框输入内容
SIZE_T g_ZLGZ_tracingTimes;
//UI中，保存的跟踪次数的输入框输入内容，不参与计算，只负责调试输出
SIZE_T g_ZLGZ_tracingTimesStatic;
//指示某个核心是否触发了和用户UI输入相同的EPT_VIOLATION
//此变量非per-CPU：一旦被赋值，全部核心共享。多核下保证只会被赋值一次
ULONG g_ZLGZ_uIsFaultRipTriggered;
//发生控制流转移前的通用寄存器结构和栈信息
#define ZLGZ_GENERAL_PURPOSE_REGISTERS_COUNT (16)
#define ZLGZ_STACK_SNAPSHOT_DEPTH (10)
typedef struct _ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED
{
	ULONG_PTR ripBeginJmp;
	ULONG_PTR ripAfterJmp;
	ULONG64 generalRegs[ZLGZ_GENERAL_PURPOSE_REGISTERS_COUNT];
	ULONG64 stackInfo[ZLGZ_STACK_SNAPSHOT_DEPTH];
}ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED, *PZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED;
//指令跟踪逻辑中每个模块名字以窄字符计数的最大长度
#define ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE (52)
//重复刷新的结构
ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED g_ZLGZ_context;
//用于提取MODRM机器字节中间3位的掩码
#define ZLGZ_EXTRACT_MID_3_BITS_OF_MODRM_SPEC_BYTE (0x38)
//寄存器编号（这里和INTEL定义的不同，是我自己的定义）
enum _GENERAL_PURPOSE_REGISTER_INDEX
{
	index_rax,
	index_rbx,
	index_rcx,
	index_rdx,
	index_rsi,
	index_rdi,
	index_rsp,
	index_rbp,
	index_r8,
	index_r9,
	index_r10,
	index_r11,
	index_r12,
	index_r13,
	index_r14,
	index_r15
};
//上一次的RIP
ULONG_PTR g_ZLGZ_prevRip;
//英特尔处理器最大单条指令长度
#define ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION (15)
UCHAR g_ZLGZ_prevRipOpcodes[ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION];
//指令跟踪API
INT getSign(
	LONG64 number
)
{
	return ((LONG64)number > 0) ? (1) : (-1);
}

VOID getContextAfterControlTransfered(
	ULONG_PTR _rbp,
	ULONG_PTR ripBeginJump,
	ULONG_PTR ripAfterJump
)
{
	g_ZLGZ_context.ripBeginJmp = ripBeginJump;
	g_ZLGZ_context.ripAfterJmp = ripAfterJump;

	ULONG_PTR rsp = 0;
	__vmx_vmread(GUEST_RSP, &rsp);

	g_ZLGZ_context.generalRegs[index_rax] = *(ULONG64*)(_rbp + 0x90);
	g_ZLGZ_context.generalRegs[index_rbx] = *(ULONG64*)(_rbp + 0x88);
	g_ZLGZ_context.generalRegs[index_rcx] = *(ULONG64*)(_rbp + 0x80);
	g_ZLGZ_context.generalRegs[index_rdx] = *(ULONG64*)(_rbp + 0x78);
	g_ZLGZ_context.generalRegs[index_rsi] = *(ULONG64*)(_rbp + 0x70);
	g_ZLGZ_context.generalRegs[index_rdi] = *(ULONG64*)(_rbp + 0x68);
	g_ZLGZ_context.generalRegs[index_rsp] = rsp;
	g_ZLGZ_context.generalRegs[index_rbp] = *(ULONG64*)(_rbp + 0x60);
	g_ZLGZ_context.generalRegs[index_r8] = *(ULONG64*)(_rbp + 0x58);
	g_ZLGZ_context.generalRegs[index_r9] = *(ULONG64*)(_rbp + 0x50);
	g_ZLGZ_context.generalRegs[index_r10] = *(ULONG64*)(_rbp + 0x48);
	g_ZLGZ_context.generalRegs[index_r11] = *(ULONG64*)(_rbp + 0x40);
	g_ZLGZ_context.generalRegs[index_r12] = *(ULONG64*)(_rbp + 0x38);
	g_ZLGZ_context.generalRegs[index_r13] = *(ULONG64*)(_rbp + 0x30);
	g_ZLGZ_context.generalRegs[index_r14] = *(ULONG64*)(_rbp + 0x28);
	g_ZLGZ_context.generalRegs[index_r15] = *(ULONG64*)(_rbp + 0x20);

	readPhysicalAddress(
		(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, rsp),
		g_ZLGZ_context.stackInfo,
		ZLGZ_STACK_SNAPSHOT_DEPTH * sizeof(ULONG_PTR),
		NULL
	);

	return;
}

VOID printContextAfterControlTransfered()
{
	UCHAR moduleNameBeginJump[ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE] = { 0 };
	SIZE_T moduleOffsetBeginJump = 0;
	UCHAR moduleNameAfterJump[ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE] = { 0 };
	SIZE_T moduleOffsetAfterJump = 0;

	UCHAR tempModuleAnsi[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE] = { 0 };

	for (USHORT j = 0; j < USER_MODE_MODULES_COUNT_LIMIT; j++)
	{
		PIN_LOAD_USER_MODE_MODULES mod = &g_dbgGlobal_debuggeeModules[j];

		if (mod->existingHeader != MAGIC_NUMBER_OF_USER_MODULE_EXISTING)
			break;

		ULONG_PTR base = mod->moduleBase;
		ULONG_PTR end = base + mod->moduleImageSize;

		if (g_ZLGZ_context.ripBeginJmp >= base && g_ZLGZ_context.ripBeginJmp < end)
		{
			moduleOffsetBeginJump = (SIZE_T)(g_ZLGZ_context.ripBeginJmp - base);

			transWideModuleNameStringToAnsi(g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar, &tempModuleAnsi);
			moduleNameBeginJump[ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE - 1] = 0x0;
			RtlCopyMemory(moduleNameBeginJump, tempModuleAnsi, ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE);
		}

		if (g_ZLGZ_context.ripAfterJmp >= base && g_ZLGZ_context.ripAfterJmp < end)
		{
			moduleOffsetAfterJump = (SIZE_T)(g_ZLGZ_context.ripAfterJmp - base);

			transWideModuleNameStringToAnsi(g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar, &tempModuleAnsi);
			moduleNameAfterJump[ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE - 1] = 0x0;
			RtlCopyMemory(moduleNameAfterJump, tempModuleAnsi, ZLGZ_MAX_ANSI_SIZE_OF_RIPS_LOCATED_MODULE);
		}

		if (moduleOffsetBeginJump != 0 && moduleOffsetAfterJump != 0)
		{
			break;
		}
	}

	if (moduleOffsetBeginJump == 0)
	{
		moduleOffsetBeginJump = 0xFFFFFFFFFFFFFFFF;
	}

	if (moduleOffsetAfterJump == 0)
	{
		moduleOffsetAfterJump = 0xFFFFFFFFFFFFFFFF;
	}

	if (moduleNameBeginJump[0] == 0x0)
	{
		RtlCopyMemory(moduleNameBeginJump, "UnknownSymbol.exe", sizeof("UnknownSymbol.exe"));
	}

	if (moduleNameAfterJump[0] == 0x0)
	{
		RtlCopyMemory(moduleNameAfterJump, "UnknownSymbol.exe", sizeof("UnknownSymbol.exe"));
	}

	DbgPrint("------------------------------\n");
	DbgPrint("<第%zu步出现跳转> 从 [%s + 0x%llX]!0x%llX -> [%s + 0x%llX]!0x%llX的跳转执行后，上下文如下:\n",
		g_ZLGZ_tracingTimesStatic - g_ZLGZ_tracingTimes,
		moduleNameBeginJump,
		moduleOffsetBeginJump,
		g_ZLGZ_context.ripBeginJmp,
		moduleNameAfterJump,
		moduleOffsetAfterJump,
		g_ZLGZ_context.ripAfterJmp
	);
	DbgPrint("rax == 0x%llX, rbx == 0x%llX, rcx == 0x%llX, rdx == 0x%llX",
		g_ZLGZ_context.generalRegs[index_rax],
		g_ZLGZ_context.generalRegs[index_rbx],
		g_ZLGZ_context.generalRegs[index_rcx],
		g_ZLGZ_context.generalRegs[index_rdx]
	);
	DbgPrint("rsi == 0x%llX, rdi == 0x%llX, rsp == 0x%llX, rbp == 0x%llX",
		g_ZLGZ_context.generalRegs[index_rsi],
		g_ZLGZ_context.generalRegs[index_rdi],
		g_ZLGZ_context.generalRegs[index_rsp],
		g_ZLGZ_context.generalRegs[index_rbp]
	);
	DbgPrint("r8 == 0x%llX, r9 == 0x%llX, r10 == 0x%llX, r11 == 0x%llX",
		g_ZLGZ_context.generalRegs[index_r8],
		g_ZLGZ_context.generalRegs[index_r9],
		g_ZLGZ_context.generalRegs[index_r10],
		g_ZLGZ_context.generalRegs[index_r11]
	);
	DbgPrint("r12 == 0x%llX, r13 == 0x%llX, r14 == 0x%llX, r15== 0x%llX",
		g_ZLGZ_context.generalRegs[index_r12],
		g_ZLGZ_context.generalRegs[index_r13],
		g_ZLGZ_context.generalRegs[index_r14],
		g_ZLGZ_context.generalRegs[index_r15]
	);
	DbgPrint("rsp[0] == 0x%llX, rsp[1] == 0x%llX, rsp[2] == 0x%llX, rsp[3] == 0x%llX, rsp[4] == 0x%llX",
		g_ZLGZ_context.stackInfo[0],
		g_ZLGZ_context.stackInfo[1],
		g_ZLGZ_context.stackInfo[2],
		g_ZLGZ_context.stackInfo[3],
		g_ZLGZ_context.stackInfo[4]
	);
	DbgPrint("rsp[5] == 0x%llX, rsp[6] == 0x%llX, rsp[7] == 0x%llX, rsp[8] == 0x%llX, rsp[9] == 0x%llX",
		g_ZLGZ_context.stackInfo[5],
		g_ZLGZ_context.stackInfo[6],
		g_ZLGZ_context.stackInfo[7],
		g_ZLGZ_context.stackInfo[8],
		g_ZLGZ_context.stackInfo[9]
	);
	DbgPrint("------------------------------\n");
	return;
}

VOID MTF_ZLGZ_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_cr3);

	ULONG_PTR rsp = 0;
	__vmx_vmread(GUEST_RSP, &rsp);

	ULONG_PTR guestCr3 = 0;
	__vmx_vmread(GUEST_CR3, &guestCr3);

	ULONG_PTR guestRsp = 0;
	__vmx_vmread(GUEST_RSP, &guestRsp);

	if (g_ZLGZ_uIsFaultRipTriggered != TRUE)
	{
		//只要g_ZLGZ_uIsFaultRipTriggered没有置位，那么允许一次通过，随后赶紧再次监控
		disableMTF();
		//EPT_VIOLATION中if分支会来到这里，会改成RW
		fixSinglePte(
			g_tpPT + g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned * sizeof(ULONG_PTR),
			g_ZLGZ_addressBeginTracingPhysicalAligned,
			RW
		);
		return;
	}
	else //EPT_VIOLATION中else分支会来到这里，【不会】改成RW
	{
		if (g_ZLGZ_prevRip == (ULONG_PTR)NULL && g_ZLGZ_prevRipOpcodes[0] == 0x0)
		{
			//第一次进入MTF
			g_ZLGZ_prevRip = _rip;
			--g_ZLGZ_tracingTimes;
			RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION * sizeof(UCHAR));

			if (g_ZLGZ_tracingTimes == 0)
			{
				disableMTF();
				g_ZLGZ_prevRip = 0;
				RtlZeroMemory(&g_ZLGZ_context, sizeof(ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED));
				RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION * sizeof(UCHAR));
			}

			return;
		}
		else
		{
			if ((LONG64)_rip > 0 && isCurrentProcessorInDebuggeeContext(_cr3, getKernelDiscriptorTableBaseRawInKprcb(_kprcb)) == TRUE)
			{
				UCHAR isControlTransfered = TRUE;

				readPhysicalAddress(
					(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, g_ZLGZ_prevRip + 0),
					g_ZLGZ_prevRipOpcodes,
					1 * sizeof(UCHAR),
					NULL
				);

				switch (g_ZLGZ_prevRipOpcodes[0])
				{
					//call series
					case 0xC3:
					{
						isControlTransfered = TRUE;
						//DbgPrint("[call type 0: ret] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						break;
					}
					case 0xE8:
					{
						isControlTransfered = TRUE;
						//DbgPrint("[call type 1: E8] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						break;
					}
					case 0x41:
					{
						readPhysicalAddress(
							(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, g_ZLGZ_prevRip + 0),
							g_ZLGZ_prevRipOpcodes,
							2 * sizeof(UCHAR),
							NULL
						);
						if (g_ZLGZ_prevRipOpcodes[1] == 0xFF)
						{
							readPhysicalAddress(
								(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, g_ZLGZ_prevRip + 0),
								g_ZLGZ_prevRipOpcodes,
								3 * sizeof(UCHAR),
								NULL
							);
							if (((g_ZLGZ_prevRipOpcodes[2] & ZLGZ_EXTRACT_MID_3_BITS_OF_MODRM_SPEC_BYTE) >> 3) == 2ui8) // FF /2
							{
								isControlTransfered = TRUE;
								//DbgPrint("[call type 3: rm64 or regs >= r8] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
							}
							else
							{
								isControlTransfered = FALSE;
								break;
							}
						}
						else
						{
							isControlTransfered = FALSE;
							break;
						}
						break;
					}
					//jcc series
					case 0x70:
					case 0x71:
					case 0x72:
					case 0x73:
					case 0x74:
					case 0x75:
					case 0x76:
					case 0x77:
					case 0x78:
					case 0x79:
					case 0x7A:
					case 0x7B:
					case 0x7C:
					case 0x7D:
					case 0x7E:
					case 0x7F:
					{
						if ((LONG64)_rip != (LONG64)g_ZLGZ_prevRip + (LONG64)2) // 2 == sizeof(<jcc rel8> instruction length)
						{
							isControlTransfered = TRUE;
							//DbgPrint("[jcc type 0: jcc rel8] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						}
						else
						{
							isControlTransfered = FALSE;
							break;
						}
						break;
					}
					case 0x0F:
					{
						readPhysicalAddress(
							(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, g_ZLGZ_prevRip + 0),
							g_ZLGZ_prevRipOpcodes,
							2 * sizeof(UCHAR),
							NULL
						);
						if (g_ZLGZ_prevRipOpcodes[1] >= 0x80 && g_ZLGZ_prevRipOpcodes[1] <= 0x8F)
						{
							if ((LONG64)_rip != (LONG64)g_ZLGZ_prevRip + (LONG64)6) // 2 == sizeof(<jcc rel32> instruction length)
							{
								isControlTransfered = TRUE;
								//DbgPrint("[jcc type 1: jcc rel32] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
							}
							else
							{
								isControlTransfered = FALSE;
								break;
							}
						}
						else
						{
							isControlTransfered = FALSE;
							break;
						}
						break;
					}
					//jmp series
					case 0xEB:
					case 0xE9:
					{
						isControlTransfered = TRUE;
						//DbgPrint("[jmp type 0: jmp rel8/32] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						break;
					}
					//FF byte special
					case 0xFF:
					{
						readPhysicalAddress(
							(PVOID)getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, g_ZLGZ_prevRip + 0),
							g_ZLGZ_prevRipOpcodes,
							2 * sizeof(UCHAR),
							NULL
						);
						if (((g_ZLGZ_prevRipOpcodes[1] & ZLGZ_EXTRACT_MID_3_BITS_OF_MODRM_SPEC_BYTE) >> 3) == 2ui8) // FF </2>
						{
							isControlTransfered = TRUE;
							//DbgPrint("[call type 2: rm64 or regs < r8] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						}
						else if (((g_ZLGZ_prevRipOpcodes[1] & ZLGZ_EXTRACT_MID_3_BITS_OF_MODRM_SPEC_BYTE) >> 3) == 4ui8) // FF /4
						{
							isControlTransfered = TRUE;
							//DbgPrint("[jmp type 1: jmp rm64] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_prevRip, _rip);
						}
						else
						{
							isControlTransfered = FALSE;
							break;
						}
						break;
					}
					default:
					{
						isControlTransfered = FALSE;
						break;
					}
				}

				if (isControlTransfered == TRUE)
				{
					//DbgPrint("[第%zu步] 位于0x%llX处的指令执行时出现了一次跳转，跳转后控制流被转移至0x%llX.\n", g_ZLGZ_tracingTimesStatic - g_ZLGZ_tracingTimes, g_ZLGZ_prevRip, _rip);
					getContextAfterControlTransfered(_rbp, g_ZLGZ_prevRip, _rip);
					printContextAfterControlTransfered();
				}

				g_ZLGZ_prevRip = _rip;
				--g_ZLGZ_tracingTimes;
				RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION * sizeof(UCHAR));
			}
			else
			{
				--g_ZLGZ_tracingTimes;
				//DbgPrint("当前处理器不在被调试进程上下文.\n");
			}

			if (g_ZLGZ_tracingTimes == 0)
			{
				g_ZLGZ_prevRip = 0;
				g_ZLGZ_tracingTimes = 0;
				RtlZeroMemory(&g_ZLGZ_context, sizeof(ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED));
				RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION * sizeof(UCHAR));
				DbgPrint("跟踪已经停止.\n");
				disableMTF();
			}
		}
	}

	return;
}

VOID EPT_VIOLATION_ZLGZ_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	//不用加入对g_ZLGZ_uIsFaultRipTriggered == TRUE进而直接return的判断，见else分支

	if (_rip != g_ZLGZ_addressBeginTracingVirtual)
	{
		enableMTF();
		fixSinglePte(
			g_tpPT + g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned * sizeof(ULONG_PTR),
			g_ZLGZ_addressBeginTracingPhysicalAligned,
			RWX
		);
		return;
	}
	else
	{
		DbgPrint("[+] current rip: 0x%llX, debuggee begin rip: 0x%llX\n", _rip, g_ZLGZ_addressBeginTracingVirtual);
		//这里一旦g_ZLGZ_uIsFaultRipTriggered被写入TRUE后，便会永远定格，除非重新按【开始】或者【解锁】
		g_ZLGZ_uIsFaultRipTriggered = TRUE;
		enableMTF();
		//此代码执行后，g_ZLGZ_addressBeginTracingPhysical页面永不触发EPT_VIOLATION(因为MTF中else分支不会再次fixPte到RW)
		//其他核心在此代码执行后，完全可以正常访问g_ZLGZ_addressBeginTracingPhysical所在的页面
		//指令追踪的关键手段在于【【抓到第一次匹配到用户RIP的核心】不停触发MTF】！MTF和EPT_VIOLATION功能上是分开的，互不影响
		//EPT_VIOLATION只是一个前导手段：匹配到用户输入RIP!
		//总结：同样是恢复RWX，在if分支内进入MTF会继续改成RW，但是在else分支(也就是这里)进入MTF就【不会再改成RW】了！
		fixSinglePte(
			g_tpPT + g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned * sizeof(ULONG_PTR),
			g_ZLGZ_addressBeginTracingPhysicalAligned,
			RWX
		);
		//恢复成RWX后，由于MTF不会再改成RW，所以此页面永不触发EPT_VIOLATION了
		//也就是从此开始：
		//	1.执行了此分支的CPU会完全被MTF接管
		//	2.所有核心都可以正常访问g_ZLGZ_addressBeginTracingPhysical所在页面
		//没有EPT_VIOLATION的任何事了
		return;
	}
}

VOID CPUID_ZLGZ_HANDLER(
	ULONG eaxNumber,
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG64 rcx = *(ULONG64*)(_rbp + 0x80);
	ULONG64 rdx = *(ULONG64*)(_rbp + 0x78);
	ULONG64 r8 = *(ULONG64*)(_rbp + 0x58);

	switch (eaxNumber)
	{
		case 0xBCC00001: //__asm__ZLGZ__lockPidCallBack PROC; rcx == debuggeePid
		{
			getDebuggeePidCr3AndModulesInfo((ULONG)rcx);
			break;
		}
		case 0xBCC00010: //__asm__ZLGZ__unlockPidCallBack PROC ;none
		{
			//清空ZLGZ模块私有数据
			g_ZLGZ_addressBeginTracingVirtual = 0;
			g_ZLGZ_tracingTimes = 0;
			g_ZLGZ_addressBeginTracingPhysical = 0;
			g_ZLGZ_addressBeginTracingPhysicalAligned = 0;
			g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned = 0;
			g_ZLGZ_uIsFaultRipTriggered = FALSE;
			g_ZLGZ_prevRip = 0;
			g_ZLGZ_tracingTimesStatic = 0;
			RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION);
			RtlZeroMemory(&g_ZLGZ_context, sizeof(ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED));
			//恢复被调试程序(debuggee)相关的五大全局变量(dbgGlobal)
			restoreDebuggeeFiveGlobalAttributesInitialState();
			//返回
			break;
		}
		case 0xBCC00100: //__asm__ZLGZ__startMonitoringCallBack PROC; rcx == addressStartTracing, rdx == tracingTimes, r8 == uIsUserSpaceOnly
		{
			//进行指令跟踪专用全局变量赋值
			g_ZLGZ_addressBeginTracingVirtual = rcx;
			g_ZLGZ_tracingTimes = rdx;
			g_ZLGZ_addressBeginTracingPhysical = getPhysicalAddressByCR3AndVirtualAddress(
				g_dbgGlobal_debuggeeCr3,
				g_ZLGZ_addressBeginTracingVirtual
			);
			g_ZLGZ_addressBeginTracingPhysicalAligned = g_ZLGZ_addressBeginTracingPhysical & ~0xFFFull;
			g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned = getPteIndexByPhysicalAddressAligned(g_ZLGZ_addressBeginTracingPhysicalAligned);
			g_ZLGZ_uIsFaultRipTriggered = FALSE;
			g_ZLGZ_prevRip = 0;
			g_ZLGZ_tracingTimesStatic = g_ZLGZ_tracingTimes;
			RtlZeroMemory(g_ZLGZ_prevRipOpcodes, ZLGZ_MAX_OPCODES_LENGTH_PER_INSTRUCTION);
			RtlZeroMemory(&g_ZLGZ_context, sizeof(ZLGZ_CONTEXT_WHEN_CONTROL_TRANSFERED));
			//进行仅用户空间判定赋值
			g_dbgGlobal_isUserSpaceOnly = (ULONG)r8;
			//修改PTE为RW，禁止执行
			if (g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned != 0)
			{
				fixSinglePte(
					g_tpPT + g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned * sizeof(ULONG_PTR),
					g_ZLGZ_addressBeginTracingPhysicalAligned,
					RW
				);
			}
			break;
		}
		case 0xBCC01000: //__asm__ZLGZ__stopMonitoringCallBack PROC ;none
		{
			//UI不允许重复点击此控件，点击此控件后只能点击【开始】和【解锁】按钮
			//因此在这里不必清零全局变量
			if (g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned != 0)
			{
				fixSinglePte(
					g_tpPT + g_ZLGZ_pteIndexOfAddressBeginTracingPhysicalAligned * sizeof(ULONG_PTR),
					g_ZLGZ_addressBeginTracingPhysicalAligned,
					RWX
				);
			}
			break;
		}
		default:
		{
			break;
		}
	}
	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//反向跟踪专用相关结构和变量
//测试用例：坐标.exe，输入0x7FF6A764109A查询位于此地址的指令访问了哪些内存
//UI中，起始地址的输入框输入内容
ULONG_PTR g_FXGZ_addressBeginBackwardTracingVirtual;
//UI中，起始地址输入框内容对应的物理地址
ULONG_PTR g_FXGZ_addressBeginBackwardTracingPhysical;
//UI中，起始地址输入框内容对应的4KB对齐物理地址
ULONG_PTR g_FXGZ_addressBeginBackwardTracingPhysicalAligned;
//UI中，起始地址输入框内容对应的物理地址的PTE索引
ULONG_PTR g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned;
//被访问者地址的结构定义
#define FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE (52)
typedef struct _FXGZ_ACCESSEE_ADDRESS_INFO
{
	ULONG_PTR accesseeAddress;
	SIZE_T offsetOfAccesseeAddressInModule;
	ULONG accessedType;
	USHORT accessedTimes;
	UCHAR moduleNameAnsi[FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE];
}FXGZ_ACCESSEE_ADDRESS_INFO, *PFXGZ_ACCESSEE_ADDRESS_INFO;
//当被访问者地址不属于任何模块时，成员offsetOfAccesseeAddressInModule的魔法数值
#define FXGZ_ACCESSEE_ADDRESS_NOT_FIND_IN_ALL_MODULES (0xFFFFui16)
//定义全新的被访问地址的最大数量
//只要被访问地址和访问属性有一个不同，就被视为一个全新的条目（对同一个被访问地址进行的读/写被视为两个条目）
#define FXGZ_MAX_ACCESSEES_COUNT (1000)
//全部的被访问者在案条目
FXGZ_ACCESSEE_ADDRESS_INFO g_FXGZ_allAccesseeInRecord[FXGZ_MAX_ACCESSEES_COUNT];
//反向跟踪API
VOID registerAccesseeInfo(
	ULONG_PTR accesseeAddress,
	USHORT accessedType
)
{
	SIZE_T currentEnptyEntryIndex = 0;
	for (; currentEnptyEntryIndex < FXGZ_MAX_ACCESSEES_COUNT; currentEnptyEntryIndex++)
	{
		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress == accesseeAddress && g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedType == accessedType)
		{
			g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedTimes++;
			return;
		}
		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress == (ULONG_PTR)NULL)
		{
			break;
		}
	}

	g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress = accesseeAddress;
	g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedType = accessedType;

	if ((LONG64)g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress < 0)
	{
		for (USHORT j = 0; j < KRNL_MODE_MODULES_COUNT_LIMIT; j++)
		{
			PIN_LOAD_KRNL_MODE_MODULES mod = &g_krnlModules[j];

			if (mod->existingHeader != MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING)
				break;

			ULONG_PTR base = mod->moduleBase;
			ULONG_PTR end = base + mod->moduleImageSize;

			if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress >= base && g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress < end)
			{
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule = (SIZE_T)(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress - base);

				UCHAR moduleAnsiName[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE] = { 0 };
				transWideModuleNameStringToAnsi(g_krnlModules[j].moduleNameInWideChar, &moduleAnsiName);
				moduleAnsiName[FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE - 1] = 0x0;
				RtlCopyMemory(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi, moduleAnsiName, FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE);

				break;
			}
		}

		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule == 0)
		{
			g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule = 0xFFFFFFFFFFFFFFFF;
		}

		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi[0] == 0x0)
		{
			RtlCopyMemory(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi, "UnknownModule.sys", sizeof("UnknownModule.sys"));
		}
	}
	else
	{
		for (USHORT j = 0; j < USER_MODE_MODULES_COUNT_LIMIT; j++)
		{
			PIN_LOAD_USER_MODE_MODULES mod = &g_dbgGlobal_debuggeeModules[j];

			if (mod->existingHeader != MAGIC_NUMBER_OF_USER_MODULE_EXISTING)
				break;

			ULONG_PTR base = mod->moduleBase;
			ULONG_PTR end = base + mod->moduleImageSize;

			if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress >= base && g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress < end)
			{
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule = (SIZE_T)(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress - base);

				UCHAR moduleAnsiName[MAX_ANSI_NAME_LENGTH_OF_ALL_MODE_MODULE] = { 0 };
				transWideModuleNameStringToAnsi(g_dbgGlobal_debuggeeModules[j].moduleNameInWideChar, &moduleAnsiName);
				moduleAnsiName[FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE - 1] = 0x0;
				RtlCopyMemory(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi, moduleAnsiName, FXGZ_MAX_ANSI_SIZE_OF_ACCESSEE_ADDRESS_LOCATED_MODULE);

				break;
			}
		}

		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule == 0)
		{
			g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule = 0xFFFFFFFFFFFFFFFF;
		}

		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi[0] == 0x0)
		{
			RtlCopyMemory(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi, "UnknownModule.exe", sizeof("UnknownModule.exe"));
		}
	}

	return;
}

VOID printRegisteredAccesseeInfo(
	ULONG_PTR accesseeAddress,
	USHORT accessedType
)
{
	for (SIZE_T currentEnptyEntryIndex = 0; currentEnptyEntryIndex < YJDD_MAX_ACCESSORS_COUNT; currentEnptyEntryIndex++)
	{
		if (g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress == accesseeAddress && g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedType == accessedType)
		{
			DbgPrint("位于地址0x%llX(%s + 0x%llX)的数据被0x%llX %s 了共计%hu次\n",
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accesseeAddress,
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].moduleNameAnsi,
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].offsetOfAccesseeAddressInModule,
				g_FXGZ_addressBeginBackwardTracingVirtual,
				(g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedType == 1) ? "读" : "写",
				g_FXGZ_allAccesseeInRecord[currentEnptyEntryIndex].accessedTimes
			);
			return;
		}
	}
	//理应不会打印，除非记录的个数超过了YJDD_MAX_ACCESSORS_COUNT
	//但是这个情况太少见了
	DbgPrint("此accessesAddress: 0x%llX未注册！\n", accesseeAddress);
	return;
}

VOID MTF_FXGZ_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	//此MTF例程仅会在非目标RIP试图执行时触发
	//目的只是为了单步放行，无特别意义
	//反向跟踪的核心全在EPT_VIOLATION中，不要过多关注此函数
	disableMTF();

	fixSinglePte(
		g_tpPT + g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned * sizeof(ULONG_PTR),
		g_FXGZ_addressBeginBackwardTracingPhysicalAligned,
		RW
	);

	return;
}

VOID EPT_VIOLATION_FXGZ_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG_PTR faultPhysicalAddress = 0;
	__vmx_vmread(VEIF_GUEST_PHYSICAL_ADDRESS, &faultPhysicalAddress);

	ULONG_PTR faultLinerlAddress = 0;
	__vmx_vmread(VEIF_GUEST_LINER_ADDRESS, &faultLinerlAddress);

	ULONG_PTR exitQualification = 0;
	__vmx_vmread(VEIF_EXIT_QUALIFICATION, &exitQualification);

	ULONG_PTR guestRsp = 0;
	__vmx_vmread(GUEST_RSP, &guestRsp);

	//检查是否是由于执行引起
	//在FXGZ的【开始】控件事件中，会提前把g_FXGZ_addressBeginBackwardTracingPhysical所在的页面标记为RW禁X
	if ((exitQualification & 0x4ull)) 
	{
		//注意：以RIP -> <mov> 'regDes', 'regSrc'举例
		//！！此时的fault相关局部变量是RIP的派生变量！！

		if (faultLinerlAddress == g_FXGZ_addressBeginBackwardTracingVirtual)
		{
			//如果在执行异常页面内的RIP和用户输入相同(这里比较的是物理地址，和UI输入不同！)
			//那么就把此指令上下文的所有通用寄存器所在的物理页面设为禁RW仅X，构造读写陷阱
			//注意注意：只有从此之后，EPT_VIOLATION才会出现【由于读写导致的EPT违规】！
			for (SIZE_T j = 0; j < 15; j++)
			{
				ULONG_PTR _reg = *(ULONG_PTR*)(_rbp + 0x90 - j * sizeof(ULONG64));
				ULONG_PTR _reg_physical = getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, _reg);
				if (_reg_physical != 0)
				{
					SIZE_T pteIndexReg = getPteIndexByPhysicalAddressAligned(_reg_physical & ~0xFFFull);
					if (pteIndexReg != 0)
					{
						fixSinglePte(g_tpPT + pteIndexReg * sizeof(ULONG_PTR), _reg_physical & ~0xFFFull, X);
					}
				}
				else
				{
					//如果寄存器的值无效导致无法转换出物理地址，那么就不处理这个寄存器了，继续处理下一个寄存器
					continue;
				}
			}
			ULONG_PTR _guestRsp_physical = getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, guestRsp);
			if (_guestRsp_physical != 0)
			{
				SIZE_T pteIndexGuestRsp = getPteIndexByPhysicalAddressAligned(_guestRsp_physical & ~0xFFFull);
				if (pteIndexGuestRsp != 0)
				{
					fixSinglePte(g_tpPT + pteIndexGuestRsp * sizeof(ULONG_PTR), _guestRsp_physical & ~0xFFFull, X);
				}
			}
		}
		else
		{
			//如果在异常页面内试图执行的地址和用户输入不同，单步MTF放行即可，无特殊处理
			enableMTF();
		}

		fixSinglePte(
			g_tpPT + g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned * sizeof(ULONG_PTR),
			g_FXGZ_addressBeginBackwardTracingPhysicalAligned,
			RWX
		);
	}
	else
	{
		//由于读写导致的异常分发到这里
		//注意：读写异常只会【在执行异常页面内的RIP和用户输入相同】之后触发！
		//除了【在执行异常页面内的RIP和用户输入相同】情况之外不可能触发EPT的读写违规！
		//注意：以RIP -> <mov> 'regDes', 'regSrc'举例
		//！！此时的fault相关局部变量是'regDes'或者'regSrc'的派生变量！！
		if (_rip == g_FXGZ_addressBeginBackwardTracingVirtual && isCurrentProcessorInDebuggeeContext(_cr3, getKernelDiscriptorTableBaseRawInKprcb(_kprcb)) == TRUE)
		{
			//在执行异常页面内，试图执行RIP -> <mov> 'regDes', 'regSrc'时，执行被EPT接管
			//设置所有寄存器页面不可读写后返回
			//关键：回到GUEST后，一摸一样的指令“RIP -> <mov> 'regDes', 'regSrc'”会【立刻】再次触发EPT违规，原因为所有寄存器不可读写
			//理应来说，这个“过程”(从EPT执行异常处理后vmresume到检测到读写违规再次触发EPT)是非常之短的
			//因此理论上，从fix 15个寄存器返回后立刻就会执行到这里，此if表达式很快就会为TRUE进行执行，中间的“过程”非常短！！
			for (SIZE_T j = 0; j < 15; j++)
			{
				ULONG_PTR _reg = *(ULONG_PTR*)(_rbp + 0x90 - j * sizeof(PVOID));
				ULONG_PTR _reg_physical = getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, _reg);
				if (_reg_physical != 0)
				{
					SIZE_T pteIndexReg = getPteIndexByPhysicalAddressAligned(_reg_physical & ~0xFFFull);
					if (pteIndexReg != 0)
					{
						fixSinglePte(g_tpPT + pteIndexReg * sizeof(ULONG_PTR), _reg_physical & ~0xFFFull, RWX);
					}
				}
				else
				{
					//如果寄存器的值无效导致无法转换出物理地址，那么就不处理这个寄存器了，继续处理下一个寄存器
					continue;
				}
			}
			ULONG_PTR _guestRsp_physical = getPhysicalAddressByCR3AndVirtualAddress(g_dbgGlobal_debuggeeCr3, guestRsp);
			if (_guestRsp_physical != 0)
			{
				SIZE_T pteIndexGuestRsp = getPteIndexByPhysicalAddressAligned(_guestRsp_physical & ~0xFFFull);
				if (pteIndexGuestRsp != 0)
				{
					fixSinglePte(g_tpPT + pteIndexGuestRsp * sizeof(ULONG_PTR), _guestRsp_physical & ~0xFFFull, RWX);
				}
			}
			USHORT accessedType = 0;
			if (exitQualification & 0x1ull)
			{
				accessedType = 1;
			}
			if (exitQualification & 0x2ull)
			{
				accessedType = 2;
			}

			registerAccesseeInfo(faultLinerlAddress, accessedType);
			printRegisteredAccesseeInfo(faultLinerlAddress, accessedType);

			enableMTF();
		}
		else
		{
			// 临界区误报终极解决方案：必须拦截 RSP 所在的页面！
			// 未拦截 RSP 前：临界区一秒刷 3000+ 条误报（栈访问触发）
			// 原因：call/ret/push/pop/mov [rsp+xx] 等指令必然访问栈

			// --------------------------------------------------------------
			// 临界区竞争条件分析
			// --------------------------------------------------------------
			// 
			// 定义：临界区 = 是【从恢复目标页面执行权限】到【再次触发读写EPT Violation之间】的极短暂时间窗口。
			// 
			// 时间线分析：
			// 1. t0: 目标指令首次执行尝试 → 触发【执行EPT违规】
			// 2. t1: 修复目标页面为RWX，修改相关15个寄存器物理页面为X
			// 3. t2: VMRESUME返回Guest
			// 4. t3: 目标指令再次执行，但访问被保护内存 → 触发【读写EPT违规】
			// 
			// 临界区范围：t1 → t3 (通常<500ns，虚拟机中可能延长至2-5μs)
			// 
			// 竞争条件：
			// - 在此期间，我们修改了至多15个物理页面的权限(设为X，仅执行)
			// - 其他CPU核心/线程/中断可能在此时访问这些页面
			// - 导致非预期的【读写EPT违规】，但并非目标指令访问
			// 
			// 稳定性保证：
			// 1. 自恢复性：目标指令会在极短时间内(t3)再次触发违规
			// 2. 快速修复：一旦进入读写违规处理，立即恢复所有页面权限
			// 3. 时间优势：单次VM轮回(t2→t3) << 看门狗超时(通常1-10ms)
			// 
			// 观察记录：
			// - 物理机：临界区极短，竞争概率极低(实际未观测到)
			// - 虚拟机：嵌套虚拟化延长临界区，曾观测到此日志
			// 
			// 此日志记录的就是非目标核心在临界区内访问被保护页面的情况
			// --------------------------------------------------------------

			/*
				一条指令的重新执行：

				从 VM-Exit
				到 handler 处理
				到 EPT 修改
				到 vmresume
				到 guest 再次执行这条指令触发读写 Violation

				是一个完整的临界区
			*/
			DbgPrint("在临界区间隔内，位于地址0x%llX处的指令访问了地址0x%llX\n", _rip, faultLinerlAddress);
		}
	}

	return;
}

VOID CPUID_FXGZ_HANDLER(
	ULONG eaxNumber,
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG64 rcx = *(ULONG64*)(_rbp + 0x80);
	ULONG64 rdx = *(ULONG64*)(_rbp + 0x78);

	switch (eaxNumber)
	{
		case 0xCCC00001: //__asm__FXGZ__lockPidCallBack PROC; rcx == debuggeePid
		{
			getDebuggeePidCr3AndModulesInfo((ULONG)rcx);
			break;
		}
		case 0xCCC00010: //__asm__FXGZ__unlockPidCallBack PROC ;none
		{
			//清空FXGZ模块私有数据
			g_FXGZ_addressBeginBackwardTracingVirtual = 0;
			g_FXGZ_addressBeginBackwardTracingPhysical = 0;
			g_FXGZ_addressBeginBackwardTracingPhysicalAligned = 0;
			g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned = 0;
			//恢复被调试程序(debuggee)相关的五大全局变量(dbgGlobal)
			restoreDebuggeeFiveGlobalAttributesInitialState();
			break;
		}
		case 0xCCC00100: //__asm__FXGZ__startBackwardTracingCallBack PROC ;rcx == addressBackwardTracing, rdx == uIsUserSpaceOnly
		{
			//进行反向跟踪专用全局变量赋值
			g_FXGZ_addressBeginBackwardTracingVirtual = rcx;
			g_FXGZ_addressBeginBackwardTracingPhysical = getPhysicalAddressByCR3AndVirtualAddress(
				g_dbgGlobal_debuggeeCr3,
				g_FXGZ_addressBeginBackwardTracingVirtual
			);
			g_FXGZ_addressBeginBackwardTracingPhysicalAligned = g_FXGZ_addressBeginBackwardTracingPhysical & ~0xFFFull;
			g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned = getPteIndexByPhysicalAddressAligned(g_FXGZ_addressBeginBackwardTracingPhysicalAligned);
			//进行仅用户空间判定赋值
			g_dbgGlobal_isUserSpaceOnly = (ULONG)rdx;
			//修改pte为RW，禁止执行
			if (g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned != 0)
			{
				fixSinglePte(
					g_tpPT + g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned * sizeof(ULONG_PTR),
					g_FXGZ_addressBeginBackwardTracingPhysicalAligned,
					RW
				);
			}
			break;
		}
		case 0xCCC01000: //__asm__FXGZ__stopBackwardTracingCallBack PROC ;none
		{
			//UI不允许重复点击此控件，点击此控件后只能点击【开始】和【解锁】按钮
			//因此在这里不必清零全局变量
			if (g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned != 0)
			{
				//如果不检查index, 直接调用fixSinglePte修复pte, 那么当用户输入的地址无效导致pteIndexOfAddressBeginBackwardTracingPhysicalAligned为0时, 就会把第0个pte修改为RWX, 导致系统崩溃!
				fixSinglePte(
					g_tpPT + g_FXGZ_pteIndexOfAddressBeginBackwardTracingPhysicalAligned * sizeof(ULONG_PTR),
					g_FXGZ_addressBeginBackwardTracingPhysicalAligned,
					RWX
				);
			}
			break;
		}
		default:
		{
			break;
		}
	}
	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//内存读写专用相关结构和变量 7FF6A764304C
//测试用例：进程写入使用thirdPartyProcess.exe写入；文件写入使用ttt.txt写入
//UI中，读取内存Panel中，地址读取输入框内容
ULONG_PTR g_NCDX_Read_addressRead;
//UI中，读取内存Panel中，读取大小输入框内容
ULONG_PTR g_NCDX_Read_readSize;
//UI中，写入内存Panel中，写入地址输入框内容
ULONG_PTR g_NCDX_Write_addressWriteDes;
//UI中，写入内存Panel中，写入大小输入框内容
ULONG_PTR g_NCDX_Write_writeSize;
//从DLL中转来的，统一接口的写入缓冲区
ULONG_PTR g_NCDX_Write_addressWriteSrc;
//注意：无论UI选择何种方式写入内存，经过DLL处理后回到驱动只会抽象成一个源指针，不记录长度
KEVENT g_NCDX_readEvent;
VOID g_NCDX_readMemoryTriggerdCallBack(
	PVOID param
)
{
	UNREFERENCED_PARAMETER(param);
	while (1)
	{
		KeWaitForSingleObject(&g_NCDX_readEvent, Executive, KernelMode, FALSE, NULL);

		if (g_dbgGlobal_isDriverUnloadCalled == TRUE)
		{
			DbgPrint("[内存读写] 读内存被动线程已退出\n");
			return;
		}

		UCHAR* tempBuffer = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_NCDX_Read_readSize, 'mmdr');
		RtlZeroMemory(tempBuffer, g_NCDX_Read_readSize);
		readUserMemorySafe(
			(HANDLE)g_dbgGlobal_debuggeePid,
			(PVOID)g_NCDX_Read_addressRead,
			tempBuffer,
			g_NCDX_Read_readSize
		);
		for (SIZE_T j = 0; j < g_NCDX_Read_readSize; j++)
		{
			DbgPrint("%hhX\n", tempBuffer[j]);
		}
		ExFreePool(tempBuffer);
		tempBuffer = NULL;

		LARGE_INTEGER time = { 0 };
		time.QuadPart = -5000;
		KeDelayExecutionThread(KernelMode, FALSE, &time);
	}
}
KEVENT g_NCDX_writeEvent;
VOID g_NCDX_writeMemoryTriggerdCallBack(
	PVOID param
)
{
	UNREFERENCED_PARAMETER(param);
	while (1)
	{
		KeWaitForSingleObject(&g_NCDX_writeEvent, Executive, KernelMode, FALSE, NULL);

		if (g_dbgGlobal_isDriverUnloadCalled == TRUE)
		{
			DbgPrint("[内存读写] 写内存被动线程已退出\n");
			return;
		}

		UCHAR* tempBuffer = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_NCDX_Write_writeSize, 'mmtw');
		RtlZeroMemory(tempBuffer, g_NCDX_Write_writeSize);

		readUserMemorySafe(
			(HANDLE)g_dbgGlobal_debuggerPid,
			(PVOID)g_NCDX_Write_addressWriteSrc,
			tempBuffer,
			g_NCDX_Write_writeSize
		);

		writeUserMemorySafe(
			(HANDLE)g_dbgGlobal_debuggeePid,
			(PVOID)g_NCDX_Write_addressWriteDes,
			tempBuffer,
			g_NCDX_Write_writeSize
		);

		ExFreePool(tempBuffer);
		tempBuffer = NULL;

		LARGE_INTEGER time = { 0 };
		time.QuadPart = -5000;
		KeDelayExecutionThread(KernelMode, FALSE, &time);
	}
}
//内存读写API
VOID MTF_NCDX_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);
	return;
}

VOID EPT_VIOLATION_NCDX_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);
	return;
}

VOID CPUID_NCDX_HANDLER(
	ULONG eaxNumber,
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG64 rcx = *(ULONG64*)(_rbp + 0x80);
	ULONG64 rdx = *(ULONG64*)(_rbp + 0x78);
	ULONG64 r8 = *(ULONG64*)(_rbp + 0x58);

	switch (eaxNumber)
	{
		case 0xDCC00001: //__asm__NCDX__lockPidCallBack PROC ;rcx == debuggeePid
		{
			getDebuggeePidCr3AndModulesInfo((ULONG)rcx);
			break;
		}
		case 0xDCC00010: //__asm__NCDX__unlockPidCallBack PROC ;none
		{
			//清空NCDX模块私有数据
			g_NCDX_Read_addressRead = 0;
			g_NCDX_Read_readSize = 0;
			g_NCDX_Write_addressWriteDes = 0;
			g_NCDX_Write_writeSize = 0;
			g_NCDX_Write_addressWriteSrc = 0;
			//恢复被调试程序(debuggee)相关的五大全局变量(dbgGlobal)
			restoreDebuggeeFiveGlobalAttributesInitialState();
			//返回
			break;
		}
		case 0xDCC00100: //__asm__NCDX__readMemoryCallBack PROC ;rcx == addressWannaRead, rdx == sizeOfRead
		{
			//进行内存读取专用全局变量赋值
			g_NCDX_Read_addressRead = rcx;
			g_NCDX_Read_readSize = rdx;

			KeSetEvent(&g_NCDX_readEvent, 0, FALSE);

			break;
		}
		case 0xDCC01000: //__asm__NCDX__writeMemoryCallBack PROC ;rcx == addressWannaWrite, rdx == sizeOfWrite, r8 == debuggerTempBufferVirtual
		{
			//进行内存写入专用全局变量赋值
			g_NCDX_Write_addressWriteDes = rcx;
			g_NCDX_Write_writeSize = rdx;
			g_NCDX_Write_addressWriteSrc = r8;

			KeSetEvent(&g_NCDX_writeEvent, 0, FALSE);

			break;
		}
		default:
		{
			break;
		}
	}
	return;
}
/*************************************************************************************************/

/*************************************************************************************************/
//模块注入专用相关结构和变量
//测试用例：任意x64进程，任意x64dll
typedef INT (*PUSER_THREAD_START_ROUTINE)(PVOID param);
SIZE_T g_MKZR_ldrLoadDllRoutineOffsetToNtdll;
ULONG_PTR g_MKZR_userModeDllFullPathAnsi;
SIZE_T g_MKZR_dllFullPathAnsiSizeIncludeEndMark;
KEVENT g_MKZR_dllInjectionEvent;
VOID g_MKZR_dllInjectionTriggerdCallBack(PVOID param)
{
	UNREFERENCED_PARAMETER(param);
	while (1)
	{
		KeWaitForSingleObject(&g_MKZR_dllInjectionEvent, Executive, KernelMode, FALSE, NULL);

		if (g_dbgGlobal_isDriverUnloadCalled == TRUE)
		{
			DbgPrint("[模块注入] 模块注入被动线程已退出\n");
			return;
		}

		UCHAR* krnlMappedDllFullPathAnsi = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, 'zzaa');
		RtlZeroMemory(krnlMappedDllFullPathAnsi, PAGE_SIZE);

		readUserMemorySafe(
			(HANDLE)g_dbgGlobal_debuggerPid,
			(PVOID)g_MKZR_userModeDllFullPathAnsi,
			krnlMappedDllFullPathAnsi,
			g_MKZR_dllFullPathAnsiSizeIncludeEndMark
		);

		HANDLE debuggeeProcessHandle = NULL;
		OBJECT_ATTRIBUTES debuggeeProcessObja = { 0 };
		InitializeObjectAttributes(&debuggeeProcessObja, NULL, 0, NULL, NULL);
		CLIENT_ID cidInput = { 0 };
		cidInput.UniqueProcess = (HANDLE)g_dbgGlobal_debuggeePid;
		cidInput.UniqueThread = NULL;
		ZwOpenProcess(&debuggeeProcessHandle, PROCESS_ALL_ACCESS, &debuggeeProcessObja, &cidInput);

		PVOID remoteMemory = NULL;
		SIZE_T remoteMemorySize = PAGE_SIZE;
		ZwAllocateVirtualMemory(debuggeeProcessHandle, &remoteMemory, 0, &remoteMemorySize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

		UCHAR* tempPage = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, 'zzaa');
		RtlZeroMemory(tempPage, PAGE_SIZE);

		UNICODE_STRING ntdll = RTL_CONSTANT_STRING(L"ntdll.dll");
		SIZE_T ntdllIndex = getUserModuleEntryIndexByUnicodeModuleName(&ntdll);
		ULONG_PTR ldrLoadDllAddress = g_dbgGlobal_debuggeeModules[ntdllIndex].moduleBase + g_MKZR_ldrLoadDllRoutineOffsetToNtdll;

		UCHAR shellCode[] =
		{
			0x48, 0x83, 0xEC, 0x48,                                     // sub rsp, 48h
			0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax, <ntdll!LdrLoadDll>
			0x48, 0x31, 0xC9,                                           // xor rcx, rcx
			0x48, 0x31, 0xD2,                                           // xor rdx, rdx
			0x49, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov r8, <pUnicodeStringStruct>
			0x49, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov r9, <pDllRetHandle>
			0xFF, 0xD0,                                                 // call rax
			0x48, 0x83, 0xC4, 0x48,                                     // add rsp, 48h
			0x31, 0xC0,                                                 // xor rax, rax
			0xC3                                                        // ret                      
		};

		*(ULONG_PTR*)((ULONG_PTR)shellCode + 6) = ldrLoadDllAddress; //<ntdll!LdrLoadDll>
		ULONG_PTR remoteUnicodeStringStruct = (ULONG_PTR)remoteMemory + 0x100;
		*(ULONG_PTR*)((ULONG_PTR)shellCode + 22) = remoteUnicodeStringStruct; //<pUnicodeStringStruct>
		ULONG_PTR remoteDllRetHandle = (ULONG_PTR)remoteMemory + 0x200;
		*(ULONG_PTR*)((ULONG_PTR)shellCode + 32) = remoteDllRetHandle; //<pDllRetHandle>
		RtlCopyMemory(tempPage, shellCode, sizeof(shellCode));

		UCHAR* wstrDllFullPath = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, 'zzaa');
		RtlZeroMemory(wstrDllFullPath, PAGE_SIZE);

		SIZE_T tempAnsiIndex = 0;
		for (SIZE_T j = 0; krnlMappedDllFullPathAnsi[tempAnsiIndex] != '\0'; j += 2)
		{
			wstrDllFullPath[j] = krnlMappedDllFullPathAnsi[tempAnsiIndex];
			tempAnsiIndex++;
		}

		UNICODE_STRING krnlUnicodeStringTemp = { 0 };
		RtlCreateUnicodeString(&krnlUnicodeStringTemp, (PCWSTR)wstrDllFullPath);

		SIZE_T usBaseOffset = 0x100;
		*(USHORT*)((ULONG_PTR)tempPage + usBaseOffset + 0x0) = krnlUnicodeStringTemp.Length;
		*(USHORT*)((ULONG_PTR)tempPage + usBaseOffset + 0x2) = krnlUnicodeStringTemp.MaximumLength;
		*(ULONG*)((ULONG_PTR)tempPage + usBaseOffset + 0x4) = 0x00000000ul;
		*(ULONG_PTR*)((ULONG_PTR)tempPage + usBaseOffset + 0x8) = (ULONG_PTR)remoteMemory + usBaseOffset + 0x10;
		RtlCopyMemory((PVOID)((ULONG_PTR)tempPage + usBaseOffset + 0x10), krnlUnicodeStringTemp.Buffer, krnlUnicodeStringTemp.MaximumLength);

		writeUserMemorySafe(
			(HANDLE)g_dbgGlobal_debuggeePid,
			(PVOID)remoteMemory,
			tempPage,
			0x250
		);

		HANDLE debuggeeThreadHandle = NULL;
		OBJECT_ATTRIBUTES debuggeeThreadObja = { 0 };
		InitializeObjectAttributes(&debuggeeThreadObja, NULL, 0, NULL, NULL);

		NTSTATUS st = ((NTSTATUS(*)(
			_Out_ PHANDLE ThreadHandle,
			_In_ ACCESS_MASK DesiredAccess,
			_In_opt_ PCOBJECT_ATTRIBUTES ObjectAttributes,
			_In_ HANDLE ProcessHandle,
			_In_ PUSER_THREAD_START_ROUTINE StartRoutine,
			_In_opt_ PVOID Argument,
			_In_ ULONG CreateFlags,
			_In_ SIZE_T ZeroBits,
			_In_ SIZE_T StackSize,
			_In_ SIZE_T MaximumStackSize,
			_In_opt_ PVOID AttributeList
			))g_NtCreateThreadEx)(
				&debuggeeThreadHandle,
				THREAD_ALL_ACCESS,
				&debuggeeThreadObja,
				debuggeeProcessHandle,
				(PUSER_THREAD_START_ROUTINE)remoteMemory,
				NULL,
				0x0, //THREAD_EXECUTE_WHEN_CREATED.
				0,
				0,
				0,
				NULL
		);

		if (NT_SUCCESS(st))
		{
			DbgPrint("注入线程执行成功！\n");
		}

		LARGE_INTEGER time = { 0 };
		time.QuadPart = -500000;
		KeDelayExecutionThread(KernelMode, FALSE, &time);

		ZwWaitForSingleObject(debuggeeThreadHandle, FALSE, NULL);

		ZwClose(debuggeeThreadHandle);
		debuggeeThreadHandle = NULL;

		SIZE_T sizeMustBeZeroZwFreeMemoryRequired = 0;
		ZwFreeVirtualMemory(debuggeeProcessHandle, &remoteMemory, &sizeMustBeZeroZwFreeMemoryRequired, MEM_RELEASE);
		remoteMemory = NULL;

		ExFreePool(wstrDllFullPath);
		wstrDllFullPath = NULL;

		ExFreePool(tempPage);
		tempPage = NULL;

		ZwClose(debuggeeProcessHandle);
		debuggeeProcessHandle = NULL;

		ExFreePool(krnlMappedDllFullPathAnsi);
		krnlMappedDllFullPathAnsi = NULL;
	}
}
//模块注入API
VOID MTF_MKZR_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);
	return;
}

VOID EPT_VIOLATION_MKZR_HANDLER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rbp);
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);
	return;
}

VOID CPUID_MKZR_HANDLER(
	ULONG eaxNumber,
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	UNREFERENCED_PARAMETER(_rip);
	UNREFERENCED_PARAMETER(_cr3);
	UNREFERENCED_PARAMETER(_kprcb);

	ULONG64 rcx = *(ULONG64*)(_rbp + 0x80);
	ULONG64 rdx = *(ULONG64*)(_rbp + 0x78);
	ULONG64 r8 = *(ULONG64*)(_rbp + 0x58);

	switch (eaxNumber)
	{
		case 0xECC00001: //__asm__MKZR__lockPidCallBack PROC ;rcx == debuggeePid
		{
			getDebuggeePidCr3AndModulesInfo((ULONG)rcx);
			break;
		}
		case 0xECC00010: //__asm__MKZR__unlockPidCallBack PROC ;none
		{
			//清空MKZR模块私有数据
			g_MKZR_ldrLoadDllRoutineOffsetToNtdll = 0;
			g_MKZR_userModeDllFullPathAnsi = 0;
			g_MKZR_dllFullPathAnsiSizeIncludeEndMark = 0;
			//恢复被调试程序(debuggee)相关的五大全局变量(dbgGlobal)
			restoreDebuggeeFiveGlobalAttributesInitialState();
			//返回
			break;
		}
		case 0xECC00100: //__asm__MKZR__dllInjectionCallBack PROC ;rcx == ldrLoadDllRoutineOffsetToNtdll, rdx == dllFullPathAnsiUser, r8 == sizeOfDllFullPathIncludeEndMark
		{
			g_MKZR_ldrLoadDllRoutineOffsetToNtdll = rcx;
			g_MKZR_userModeDllFullPathAnsi = rdx;
			g_MKZR_dllFullPathAnsiSizeIncludeEndMark = r8;
			KeSetEvent(&g_MKZR_dllInjectionEvent, 0, FALSE);
			break;
		}
		default:
		{
			break;
		}
	}
	return;
}
/*************************************************************************************************/

//调试器分发API
VOID CPUID_C_DISPATCHER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	ULONG eax = *(ULONG*)(_rbp + 0x90);
	ULONG currentProcessorNumber = KeGetCurrentProcessorNumber();
	switch (eax)
	{
	case 0xAAAAAAAA:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		CPUID_LOAD_DLL_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case 0xACC00001:
	case 0xACC00010:
	case 0xACC00100:
	case 0xACC01000:
	case 0xACC10000:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		g_dbgGlobal_funcId = DBG_FUNC_ID_YJDD;
		CPUID_YJDD_HANDLER(eax, _rbp, _rip, _cr3, _kprcb);
		break;
	case 0xBCC00001:
	case 0xBCC00010:
	case 0xBCC00100:
	case 0xBCC01000:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		g_dbgGlobal_funcId = DBG_FUNC_ID_ZLGZ;
		CPUID_ZLGZ_HANDLER(eax, _rbp, _rip, _cr3, _kprcb);
		break;
	case 0xCCC00001:
	case 0xCCC00010:
	case 0xCCC00100:
	case 0xCCC01000:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		g_dbgGlobal_funcId = DBG_FUNC_ID_FXGZ;
		CPUID_FXGZ_HANDLER(eax, _rbp, _rip, _cr3, _kprcb);
		break;
	case 0xDCC00001:
	case 0xDCC00010:
	case 0xDCC00100:
	case 0xDCC01000:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		g_dbgGlobal_funcId = DBG_FUNC_ID_NCDX;
		CPUID_NCDX_HANDLER(eax, _rbp, _rip, _cr3, _kprcb);
		break; 
	case 0xECC00001:
	case 0xECC00010:
	case 0xECC00100:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 0;
		CPUID_MKZR_HANDLER(eax, _rbp, _rip, _cr3, _kprcb);
		break;
	default:
		g_whetherCpuidNeedExecuteReally[currentProcessorNumber] = 1;
		break;
	}
	return;
}

VOID MTF_C_DISPATCHER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	switch (g_dbgGlobal_funcId)
	{
	case DBG_FUNC_ID_YJDD:
		MTF_YJDD_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_ZLGZ:
		MTF_ZLGZ_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_FXGZ:
		MTF_FXGZ_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_NCDX:
		MTF_NCDX_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_MKZR:
		MTF_MKZR_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	default:
		break;
	}
	return;
}

VOID EPT_VIOLATION_C_DISPATCHER(
	ULONG_PTR _rbp,
	ULONG_PTR _rip,
	ULONG_PTR _cr3,
	ULONG_PTR _kprcb
)
{
	switch (g_dbgGlobal_funcId)
	{
	case DBG_FUNC_ID_YJDD:
		EPT_VIOLATION_YJDD_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_ZLGZ:
		EPT_VIOLATION_ZLGZ_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_FXGZ:
		EPT_VIOLATION_FXGZ_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_NCDX:
		EPT_VIOLATION_NCDX_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	case DBG_FUNC_ID_MKZR:
		EPT_VIOLATION_MKZR_HANDLER(_rbp, _rip, _cr3, _kprcb);
		break;
	default:
		break;
	}
	return;
}