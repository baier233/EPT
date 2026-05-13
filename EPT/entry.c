#include "vtRoot.h"

PDRIVER_OBJECT g_driverObject;

HANDLE g_NCDX_READ_CALLBACK_THREAD_HANDLE;
HANDLE g_NCDX_WRITE_CALLBACK_THREAD_HANDLE;
HANDLE g_MKZR_CALLBACK_THREAD_HANDLE;

EXTERNAL_MODULE_EXTERN ULONG_PTR g_MiProcessLoaderEntry; //from vtRoot.c

EXTERNAL_MODULE_EXTERN ULONG g_dbgGlobal_isDriverUnloadCalled; //from debugger.c

//内存读写相关事件：此事件仅内存读写功能可用
EXTERNAL_MODULE_EXTERN KEVENT g_NCDX_readEvent; //from debugger.c
EXTERNAL_MODULE_EXTERN VOID g_NCDX_readMemoryTriggerdCallBack(PVOID param); //from debugger.c
EXTERNAL_MODULE_EXTERN KEVENT g_NCDX_writeEvent; //from debugger.c
EXTERNAL_MODULE_EXTERN VOID g_NCDX_writeMemoryTriggerdCallBack(PVOID param); //from debugger.c

//模块注入相关事件：此事件仅模块注入功能可用
EXTERNAL_MODULE_EXTERN KEVENT g_MKZR_dllInjectionEvent; //from debugger.c
EXTERNAL_MODULE_EXTERN VOID g_MKZR_dllInjectionTriggerdCallBack(PVOID param); //from debugger.c

VOID initializeGlobalDbgEventsAndCreateDbgThreads()
{
	KeInitializeEvent(&g_NCDX_readEvent, SynchronizationEvent, FALSE);
	PsCreateSystemThread(
		&g_NCDX_READ_CALLBACK_THREAD_HANDLE,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		g_NCDX_readMemoryTriggerdCallBack,
		NULL
	);

	KeInitializeEvent(&g_NCDX_writeEvent, SynchronizationEvent, FALSE);
	PsCreateSystemThread(
		&g_NCDX_WRITE_CALLBACK_THREAD_HANDLE,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		g_NCDX_writeMemoryTriggerdCallBack,
		NULL
	);

	KeInitializeEvent(&g_MKZR_dllInjectionEvent, SynchronizationEvent, FALSE);
	PsCreateSystemThread(
		&g_MKZR_CALLBACK_THREAD_HANDLE,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		g_MKZR_dllInjectionTriggerdCallBack,
		NULL
	);

	return;
}

VOID wakeupGlobalDbgEventsManullyAndShutdownDbgThreads()
{
	g_dbgGlobal_isDriverUnloadCalled = TRUE;
	//如果这个事件过短，会导致DRIVER_UNLOAD_WITHOUT_CANCELLING_PENDING_OPERATIONS
	//问题出在g_NCDX_readEvent释放线程访问了空地址g_dbgGlobal_isDriverUnloadCalled
	//稳妥起见，安排一个长时间等待
	LARGE_INTEGER shutdownTimeTolerance = { 0 };
	shutdownTimeTolerance.QuadPart = -5000000;

	KeSetEvent(&g_NCDX_readEvent, 0, FALSE);
	KeDelayExecutionThread(KernelMode, FALSE, &shutdownTimeTolerance);
	ZwClose(g_NCDX_READ_CALLBACK_THREAD_HANDLE);
	KeSetEvent(&g_NCDX_writeEvent, 0, FALSE);
	KeDelayExecutionThread(KernelMode, FALSE, &shutdownTimeTolerance);
	ZwClose(g_NCDX_WRITE_CALLBACK_THREAD_HANDLE);
	KeSetEvent(&g_MKZR_dllInjectionEvent, 0, FALSE);
	KeDelayExecutionThread(KernelMode, FALSE, &shutdownTimeTolerance);
	ZwClose(g_MKZR_CALLBACK_THREAD_HANDLE);

	return;
}

VOID driverUnload(
	PDRIVER_OBJECT driverObject
)
{
	UNREFERENCED_PARAMETER(driverObject);

	wakeupGlobalDbgEventsManullyAndShutdownDbgThreads();
	exitVtStateWithEpt();

	return;
}

NTSTATUS DriverEntry(
	PDRIVER_OBJECT driverObject,
	PUNICODE_STRING regPath
)
{
	UNREFERENCED_PARAMETER(regPath);
	driverObject->DriverUnload = driverUnload;
	g_driverObject = driverObject;

	initializeGlobalDbgEventsAndCreateDbgThreads();
	enterVtStateWithEpt();

	return STATUS_SUCCESS;
}