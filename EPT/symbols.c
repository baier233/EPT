#include "symbols.h"

#pragma warning(disable: 4702)

PLIST_ENTRY g_psLoadedModuleListHead;

IN_LOAD_KRNL_MODE_MODULES g_krnlModules[KRNL_MODE_MODULES_COUNT_LIMIT];

VOID getPsLoadedModuleListHeadAddress(
	PDRIVER_OBJECT myDriverObj
)
{
	ULONG_PTR currDriverObj = (ULONG_PTR)myDriverObj;
	ULONG_PTR pDriverSection = currDriverObj + 0x28; // Windows 10 x64 DriverObject + 0x28 = DriverSection
	ULONG_PTR pLdrDataTableEntry = *(ULONG_PTR*)pDriverSection;
	PLIST_ENTRY inLoadOrderLinks = (PLIST_ENTRY)(pLdrDataTableEntry + 0x0); // Windows 10 x64 LdrDataTableEntry + 0x20 = InLoadOrderLinks
	g_psLoadedModuleListHead = inLoadOrderLinks->Flink;
	return;
}

VOID fillInLoadKrnlModulesArray()
{
	PLIST_ENTRY currEntry = g_psLoadedModuleListHead->Flink;
	SIZE_T idx = 0;
	while (currEntry != g_psLoadedModuleListHead)
	{
		g_krnlModules[idx].existingHeader = MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING;
		g_krnlModules[idx].moduleBase = *(ULONG_PTR*)((ULONG_PTR)currEntry + 0x30);
		g_krnlModules[idx].moduleImageSize = *(ULONG_PTR*)((ULONG_PTR)currEntry + 0x40);

		PUNICODE_STRING tempUnicodeStringStruct = (PUNICODE_STRING)((ULONG_PTR)currEntry + 0x58);

		RtlCopyMemory(
			g_krnlModules[idx].moduleNameInWideChar,
			tempUnicodeStringStruct->Buffer,
			min(tempUnicodeStringStruct->MaximumLength, KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS - 2)
		);

		idx++;
		currEntry = currEntry->Flink;
	}
	return;
}

VOID printInLoadKrnlModulesInfo()
{
	for (SIZE_T j = 0; j < KRNL_MODE_MODULES_COUNT_LIMIT; j++)
	{
		if (g_krnlModules[j].existingHeader == MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING)
		{
			UCHAR tempModuleNameInAnsi[(KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS / 2) + 2] = { 0 };
			RtlZeroMemory(tempModuleNameInAnsi, (KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS / 2) + 2);
			SIZE_T idx = 0;
			for (SIZE_T i = 0; i < KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS; i += 2)
			{
				if (g_krnlModules[j].moduleNameInWideChar[i] != 0x0)
				{
					tempModuleNameInAnsi[idx++] = g_krnlModules[j].moduleNameInWideChar[i];
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
				g_krnlModules[j].moduleBase,
				g_krnlModules[j].moduleImageSize
			);
		}
		else
		{
			return;
		}
	}
}

UCHAR hexChar2Nibble(
	CHAR c
)
{
	if (c >= '0' && c <= '9')
		return (UCHAR)(c - '0');

	if (c >= 'A' && c <= 'F')
		return (UCHAR)(c - 'A' + 10);

	if (c >= 'a' && c <= 'f')
		return (UCHAR)(c - 'a' + 10);

	return 0;
}

VOID pattern2Array(
	UCHAR* receivedBuffer,
	CONST CHAR* pattern,
	SIZE_T size
)
{
	CONST CHAR* current = pattern;
	SIZE_T index = 0;

	while (*current && index < size)
	{
		if (*current == '?')
		{
			++current;
			if (*current == '?')
				++current;

			receivedBuffer[index++] = 0xFF;
		}
		else
		{
			UCHAR high, low;

			if (*current == ' ')
			{
				++current;
				continue;
			}

			high = hexChar2Nibble(*current++);
			low = hexChar2Nibble(*current++);

			receivedBuffer[index++] = (UCHAR)((high << 4) | low);
		}
	}

	return;
}

ULONG_PTR searchOpcodeInKrnlModule(
	ULONG_PTR krnlModuleBase,
	SIZE_T krnlModuleImageSize,
	CONST CHAR* opcodeStringStream,
	SIZE_T opcodeSize
)
{
	PUCHAR scanBase = (PUCHAR)krnlModuleBase;

	UCHAR opcodeHexBuffer[PAGE_SIZE];
	RtlZeroMemory(opcodeHexBuffer, PAGE_SIZE);

	pattern2Array(opcodeHexBuffer, opcodeStringStream, opcodeSize);

	for (SIZE_T j = 0; j < krnlModuleImageSize - opcodeSize; ++j)
	{
		SIZE_T tempLen = 0;
		for (SIZE_T i = 0; i < opcodeSize; ++i)
		{
			if (MmIsAddressValid(&scanBase[j + i]) == TRUE)
			{
				if (scanBase[j + i] == opcodeHexBuffer[i])
				{
					tempLen++;
				}
				else
				{
					if (opcodeStringStream[i * 3] == '?' && opcodeHexBuffer[i] == 0xFF)
					{
						tempLen++;
					}
					else
					{
						break;
					}
				}
			}
			else
			{
				continue;
			}
		}

		if (tempLen == opcodeSize)
		{
			return (ULONG_PTR)(scanBase + j);
		}
		else
		{
			tempLen = 0;
		}
	}

	return 0;
}

SIZE_T getKrnlModuleEntryIndexByUnicodeModuleName(
	PUNICODE_STRING krnlModuleNameUnicode
)
{
	if (!krnlModuleNameUnicode || !krnlModuleNameUnicode->Buffer)
		return INVALID_KRNL_MODULE_INDEX;

	SIZE_T krnlLenChars = krnlModuleNameUnicode->Length / sizeof(WCHAR);
	if (krnlLenChars == 0)
		return INVALID_KRNL_MODULE_INDEX;

	SIZE_T maxModuleChars =
		KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS / sizeof(WCHAR);

	for (SIZE_T i = 0; i < KRNL_MODE_MODULES_COUNT_LIMIT; ++i)
	{
		PIN_LOAD_KRNL_MODE_MODULES mod = &g_krnlModules[i];

		if (mod->existingHeader != MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING)
			break;

		PWCHAR moduleName = (PWCHAR)mod->moduleNameInWideChar;

		SIZE_T j = 0;
		for (; j < krnlLenChars && j < maxModuleChars; ++j)
		{
			WCHAR c1 = moduleName[j];
			WCHAR c2 = krnlModuleNameUnicode->Buffer[j];

			if (c1 == L'\0')
				break;

			if (c1 >= L'A' && c1 <= L'Z') c1 += (L'a' - L'A');
			if (c2 >= L'A' && c2 <= L'Z') c2 += (L'a' - L'A');

			if (c1 != c2)
				break;
		}

		if (j == krnlLenChars && moduleName[j] == L'\0')
		{
			return i;
		}
	}

	return INVALID_KRNL_MODULE_INDEX;
}
