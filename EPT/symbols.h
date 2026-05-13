#ifndef __SYMBOLS_HEADER__
#define __SYMBOLS_HEADER__

#include "base.h"

#define KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS  (100) 
#define KRNL_MODE_MODULES_COUNT_LIMIT (1000)
#define MAGIC_NUMBER_OF_KRNL_MODULE_EXISTING (0xAABBCCDD) 
#define INVALID_KRNL_MODULE_INDEX (0xFFFFFFFFFFFFFFFF)

typedef struct _IN_LOAD_KRNL_MODE_MODULES
{
	ULONG existingHeader; //存在性标头。如果此模块条目存在，此值会被定义为宏MAGIC_NUMBER_OF_USER_MODULE_EXISTING
	ULONG_PTR moduleBase; //模块在内存中加载的基地址
	UCHAR moduleNameInWideChar[KRNL_MODE_MODULE_NAME_MAX_ANSI_CHARS]; //注意：以WCHAR存储！例如abc.exe\0，那么moduleNameInWideChar->a 0 b 0 c 0 . 0 e 0 x 0 e 0 \0 0
	SIZE_T moduleImageSize; //模块在内存中的映像大小
}IN_LOAD_KRNL_MODE_MODULES, * PIN_LOAD_KRNL_MODE_MODULES;

EXTERNAL_MODULE_EXTERN PDRIVER_OBJECT g_driverObject; //from entry.c

LOCAL_MODULE_EXTERN PLIST_ENTRY g_psLoadedModuleListHead;

LOCAL_MODULE_EXTERN IN_LOAD_KRNL_MODE_MODULES g_krnlModules[KRNL_MODE_MODULES_COUNT_LIMIT];

VOID getPsLoadedModuleListHeadAddress(
	PDRIVER_OBJECT myDriverObj
);

VOID fillInLoadKrnlModulesArray();

VOID printInLoadKrnlModulesInfo();

UCHAR hexChar2Nibble(
	CHAR c
);

VOID pattern2Array(
	UCHAR* receivedBuffer,
	CONST CHAR* pattern,
	SIZE_T size
);

ULONG_PTR searchOpcodeInKrnlModule(
	ULONG_PTR krnlModuleBase,
	SIZE_T krnlModuleImageSize,
	CONST CHAR* opcodeStringStream,
	SIZE_T opcodeSize
); 

SIZE_T getKrnlModuleEntryIndexByUnicodeModuleName(
	PUNICODE_STRING krnlModuleNameUnicode
);

#endif