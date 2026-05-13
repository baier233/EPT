EXTERN g_bugCheck: QWORD

EXTERN GUEST_FS_BASE_ADDRESS_INDEX: DWORD
EXTERN HOST_FS_BASE_ADDRESS_INDEX: DWORD
EXTERN GUEST_CR0_INDEX: DWORD
EXTERN GUEST_CR3_INDEX: DWORD
EXTERN GUEST_CR4_INDEX: DWORD
EXTERN GUEST_RIP_INDEX: DWORD
EXTERN GUEST_RSP_INDEX: DWORD
EXTERN GUEST_RFLAGS_INDEX: DWORD
EXTERN VM_EXIT_REASON_INDEX: DWORD
EXTERN VM_INSTRUCTION_LENGTH_INDEX: DWORD
EXTERN VEnCF_VM_ENTRY_INTERRUPTION_INFORMATION_FIELD_INDEX: DWORD
EXTERN VEnCF_VM_ENTRY_EXCEPTION_ERROR_CODE_INDEX: DWORD
EXTERN g_PER_CPU_REGS: QWORD
EXTERN g_ept: QWORD
EXTERN g_whetherCpuidNeedExecuteReally: QWORD
EXTERN CPUID_C_DISPATCHER: QWORD
EXTERN MTF_C_DISPATCHER: QWORD
EXTERN EPT_VIOLATION_C_DISPATCHER: QWORD
EXTERN dbgAsm: QWORD

.code

	__vasm__detectMaxPhysicalAddressLength PROC
		push rbx
		xor rax, rax
		mov eax, 80000008h
		cpuid
		and rax, 0FFh
		pop rbx
		ret
	__vasm__detectMaxPhysicalAddressLength ENDP
	
	__vsm__getRAX PROC
		mov rax, rax
		ret
	__vsm__getRAX ENDP

	__vsm__INT3 PROC
		int 3
		ret
	__vsm__INT3 ENDP

	__vsm__wrmsr PROC
		mov eax, r8d
		wrmsr
		ret
	__vsm__wrmsr ENDP

	__vsm__CLI PROC
		cli
		ret
	__vsm__CLI ENDP

	__vsm__STI PROC
		sti
		ret
	__vsm__STI ENDP

	__vasm__isVMXOperationsSupported PROC
		mov eax, 1
		cpuid
		shr ecx, 5
		and ecx, 1
		mov rax, rcx
		ret
	__vasm__isVMXOperationsSupported ENDP

	__vasm__setCR4VMXEBit PROC
		mov rax, cr4
		mov ecx, 1
		shl rcx, 13
		or rax, rcx
		mov cr4, rax
		ret
	__vasm__setCR4VMXEBit ENDP

	__vasm__clearCR4VMXEBit PROC
		mov rax, cr4
		mov ecx, 1
		shl rcx, 13
		not rcx
		and rax, rcx
		mov cr4, rax
		ret
	__vasm__clearCR4VMXEBit ENDP

	__vsm__testX2APICmode PROC
		mov eax, 1
		cpuid
		mov eax, ecx
		ret
	__vsm__testX2APICmode ENDP

	__vsm__vmlaunchSaveRegisters PROC
		sub rsp, 38h
		push rax
		push rcx

		mov rax, gs: [20h]
		mov eax, dword ptr [rax + 24h]
		shl eax, 3
		mov rcx, g_PER_CPU_REGS
		add rcx, rax
		mov rcx, qword ptr [rcx]

		mov qword ptr [rcx], rsp

		vmlaunch

		mov ecx, 4400h
		vmread rcx, rcx
		or ecx, 0CCDD0000h
		call g_bugCheck
	__vsm__vmlaunchSaveRegisters ENDP

	__vsm__guestEntry PROC
		mov rax, gs: [20h]
		mov eax, dword ptr [rax + 24h]
		shl eax, 3
		mov rcx, g_PER_CPU_REGS
		add rcx, rax
		mov rcx, qword ptr [rcx]

		mov rsp, qword ptr [rcx]
		pop rcx
		pop rax
		add rsp, 38h

		ret
	__vsm__guestEntry ENDP

	__vsm__getCR0 PROC
		mov rax, cr0
		ret
	__vsm__getCR0 ENDP

	__vsm__getCR3 PROC
		mov rax, cr3
		ret
	__vsm__getCR3 ENDP		

	__vsm__getCR4 PROC
		mov rax, cr4
		ret
	__vsm__getCR4 ENDP

	__vsm__restoreCR4 PROC
		mov rax, cr4
		mov ecx, 1
		shl ecx, 13
		not ecx
		and rax, rcx
		mov cr4, rax
		ret
	__vsm__restoreCR4 ENDP
		
	__vsm__getDR7 PROC
		mov rax, dr7
		ret
	__vsm__getDR7 ENDP
		
	__vsm__getRSP PROC
		mov rax, rsp
		ret
	__vsm__getRSP ENDP
	
	__vsm__getRFLAGS PROC
		pushfq
		pop rax
		ret
	__vsm__getRFLAGS ENDP
	
	__vsm__getCsSelector PROC
		mov eax, cs
		ret
	__vsm__getCsSelector ENDP
		
	__vsm__getSsSelector PROC
		mov eax, ss
		ret
	__vsm__getSsSelector ENDP
		
	__vsm__getDsSelector PROC
		mov eax, ds
		ret
	__vsm__getDsSelector ENDP
		
	__vsm__getEsSelector PROC
		mov eax, es
		ret
	__vsm__getEsSelector ENDP
		
	__vsm__getFsSelector PROC
		mov eax, fs
		ret
	__vsm__getFsSelector ENDP
		
	__vsm__getGsSelector PROC
		mov eax, gs
		ret
	__vsm__getGsSelector ENDP
		
	__vsm__getLDTR PROC
		xor eax, eax
		sldt ax
		ret
	__vsm__getLDTR ENDP
		
	__vsm__getTrSelector PROC
		xor eax, eax
		str ax
		ret
	__vsm__getTrSelector ENDP

	__vsm__getGDTbase PROC
		LOCAL gdtr[10]:BYTE
		sgdt gdtr
		mov rax, qword ptr gdtr[2]
		ret
	__vsm__getGDTbase ENDP

	__vsm__getIDTbase PROC
		LOCAL idtr[10]:BYTE
		sidt idtr
		mov	rax, qword ptr idtr[2]
		ret
	__vsm__getIDTbase ENDP

	__vsm__getGDTlimit PROC
		LOCAL gdtr[10]:BYTE
		sgdt gdtr
		xor rax, rax
		mov	ax, word ptr gdtr[0]
		ret
	__vsm__getGDTlimit ENDP

	__vsm__getIDTlimit PROC
		LOCAL idtr[10]:BYTE
		sidt idtr
		xor rax, rax
		mov	ax, word ptr idtr[0]
		ret
	__vsm__getIDTlimit ENDP

	__vsm__readGdtr PROC ;__vsm__readGdtr(ULONG_PTR pGdtrSpace)
		sub rsp, 68h

		sgdt qword ptr [rcx]

		add rsp, 68h
		ret
	__vsm__readGdtr ENDP

	__vsm__writeGdtr PROC ;__vsm__writeGdtr(ULONG_PTR pGdtrSpace)
		sub rsp, 68h

		lgdt fword ptr [rcx]

		add rsp, 68h
		ret
	__vsm__writeGdtr ENDP

	__vsm__readIdtr PROC ;__vsm__readGdtr(ULONG_PTR pGdtrSpace)
		sub rsp, 68h

		sidt qword ptr [rcx]

		add rsp, 68h
		ret
	__vsm__readIdtr ENDP

	__vsm__writeIdtr PROC ;__vsm__writeGdtr(ULONG_PTR pGdtrSpace)
		sub rsp, 68h

		lidt fword ptr [rcx]

		add rsp, 68h
		ret
	__vsm__writeIdtr ENDP

	__vsm__flushEptCache PROC
		sub rsp, 68h

		sub rsp, 20h
		mov qword ptr [rsp + 0h], 0
		mov qword ptr [rsp + 8h], 0
		mov rax, g_ept
		mov qword ptr [rsp + 0h], rax
		mov rax, 1
		invept rax, oword ptr [rsp + 0h]
		add rsp, 20h				;刷新EPT缓存

		add rsp, 68h
		ret
	__vsm__flushEptCache ENDP

	__vsm__vmxoffSaveRegisters PROC
		vmcall ;for executing vmxoff under root-operation mode
	__vsm__vmxoffSaveRegisters ENDP

	__vsm__hostEntry PROC
		push rax ;$ + 90
		push rbx ;$ + 88
		push rcx ;$ + 80
		push rdx ;$ + 78
		push rsi ;$ + 70
		push rdi ;$ + 68
		push rbp ;$ + 60
		push r8  ;$ + 58
		push r9  ;$ + 50
		push r10 ;$ + 48
		push r11 ;$ + 40
		push r12 ;$ + 38
		push r13 ;$ + 30
		push r14 ;$ + 28
		push r15 ;$ + 20h, rsp: x8h
		mov ecx, GUEST_RIP_INDEX
		vmread rcx, rcx
		push rcx		
		mov ecx, GUEST_RFLAGS_INDEX
		vmread rcx, rcx
		push rcx
		mov ecx, VM_EXIT_REASON_INDEX
		vmread rcx, rcx
		push rcx
		mov ecx, VM_INSTRUCTION_LENGTH_INDEX
		vmread rcx, rcx
		push rcx ;$, rsp: x8h

		mov rbp, rsp

		;save x87fpu, SSE, AVX states
		sub rsp, 248h ;x0h
		fxsave qword ptr [rsp] ;至少需要200h(= 512)字节的空间
		sub rsp, 60h ;x0h		

		mov ecx, GUEST_FS_BASE_ADDRESS_INDEX
		vmread rcx, rcx
		;IA-32e host(64 bit-mode if IA-32e) never use FS base address.
		;so we can fix this value to any value we want.
		;but here, we make this operation to store the FS base address of application in compatibility mode of IA-32e.
		mov eax, HOST_FS_BASE_ADDRESS_INDEX
		vmwrite rax, rcx

		sub rsp, 40h
		call VM_EXIT_HANDLER
		add rsp, 40h

		;restore guest Rip pointer
		mov rcx, qword ptr [rbp + 0h]
		mov rax, qword ptr [rbp + 18h]
		add rax, rcx
		mov ecx, GUEST_RIP_INDEX
		vmwrite rcx, rax
		
		add rsp, 60h ;x0h
		fxrstor qword ptr [rsp]
		add rsp, 248h ;x0h		

		;skip vmx-related fields
		add rsp, 20h

		;restore general purpose registers
		pop r15
		pop r14
		pop r13
		pop r12
		pop r11
		pop r10
		pop r9
		pop r8
		pop rbp
		pop rdi
		pop rsi
		pop rdx
		pop rcx
		pop rbx
		pop rax

		;resume guest
		vmresume
	__vsm__hostEntry ENDP

	VM_EXIT_HANDLER PROC
		sub rsp, 48h
		
		cmp word ptr [rbp + 8h], 10
	jz  REASON_CPUID
		cmp word ptr [rbp + 8h], 18
	jz  REASON_VMCALL
		cmp word ptr [rbp + 8h], 31
	jz  REASON_RDMSR
		cmp word ptr [rbp + 8h], 32
	jz  REASON_WRMSR
		cmp word ptr [rbp + 8h], 37
	jz  REASON_MTF
		cmp word ptr [rbp + 8h], 48
	jz  REASON_EPT_VIOLATION
		cmp word ptr [rbp + 8h], 49
	jz  REASON_EPT_MISCONFIGURATION
		mov rcx, qword ptr [rbp + 8h] ;VM_EXIT_REASON
		or rcx, 0ABCD0000h
		call g_bugCheck

REASON_CPUID:
		call CPUID_EXIT
	jmp RETURN
REASON_VMCALL:
		call VMCALL_EXIT
	jmp RETURN
REASON_RDMSR:
		call RDMSR_EXIT
	jmp RETURN
REASON_WRMSR:
		call WRMSR_EXIT
	jmp RETURN
REASON_MTF:
		call MTF_EXIT
	jmp RETURN
REASON_EPT_VIOLATION:
		call EPT_VIOLATION_EXIT
	jmp RETURN
REASON_EPT_MISCONFIGURATION:
		call EPT_MISCONFIGURATION_EXIT
	jmp RETURN

RETURN:
		add rsp, 48h
		ret
	VM_EXIT_HANDLER ENDP

	CPUID_EXIT PROC
		sub rsp, 68h

		mov rcx, rbp
		mov rdx, qword ptr [rbp + 18h]
		mov eax, GUEST_CR3_INDEX
		vmread r8, rax
		mov r9, gs: [20h]
		lea rax, CPUID_C_DISPATCHER
		call rax
		
		;ULONG g_whetherCpuidNeedExecuteReally[TOTAL_CPU_COUNT];
		mov rax, gs: [20h]
		mov eax, dword ptr [rax + 24h] ;eax == cpuIndex
		shl eax, 2 ;cpuIndex * sizeof(ULONG) (4)
		lea rcx, g_whetherCpuidNeedExecuteReally
		mov ecx, dword ptr [rcx + rax]
		cmp ecx, 0
	jz	SKIP_CPUID_EXECUTE

		mov rax, qword ptr [rbp + 90h]
		mov rcx, qword ptr [rbp + 80h]
		cpuid
		mov qword ptr [rbp + 90h], rax
		mov qword ptr [rbp + 88h], rbx
		mov qword ptr [rbp + 80h], rcx
		mov qword ptr [rbp + 78h], rdx

SKIP_CPUID_EXECUTE:
		add rsp, 68h
		ret
	CPUID_EXIT ENDP

	RDMSR_EXIT PROC
		sub rsp, 68h

		;mov rcx, qword ptr [rbp + 80h]
		;rdmsr
		;mov qword ptr [rbp + 90h], rax
		;mov qword ptr [rbp + 78h], rdx

		mov ecx, VEnCF_VM_ENTRY_INTERRUPTION_INFORMATION_FIELD_INDEX
		mov eax, 80000B0Dh
		vmwrite rcx, rax
		
		mov ecx, VEnCF_VM_ENTRY_EXCEPTION_ERROR_CODE_INDEX
		mov eax, 0
		vmwrite rcx, rax

		add rsp, 68h
		ret
	RDMSR_EXIT ENDP

	WRMSR_EXIT PROC
		sub rsp, 68h
		
		;mov rax, qword ptr [rbp + 90h]
		;mov rcx, qword ptr [rbp + 80h]
		;mov rdx, qword ptr [rbp + 78h]
		;wrmsr
		
		mov ecx, VEnCF_VM_ENTRY_INTERRUPTION_INFORMATION_FIELD_INDEX
		mov eax, 80000B0Dh
		vmwrite rcx, rax
		
		mov ecx, VEnCF_VM_ENTRY_EXCEPTION_ERROR_CODE_INDEX
		mov eax, 0
		vmwrite rcx, rax

		add rsp, 68h
		ret
	WRMSR_EXIT ENDP 

	MTF_EXIT PROC		
		sub rsp, 68h
		
		mov qword ptr [rbp + 0h], 0

		mov rcx, rbp
		mov rdx, qword ptr [rbp + 18h]
		mov eax, GUEST_CR3_INDEX
		vmread r8, rax
		mov r9, gs: [20h]
		lea rax, MTF_C_DISPATCHER
		call rax
		
		add rsp, 68h
		ret
	MTF_EXIT ENDP

	EPT_VIOLATION_EXIT PROC
		sub rsp, 68h

		mov qword ptr [rbp + 0h], 0

		mov rcx, rbp
		mov rdx, qword ptr [rbp + 18h]
		mov eax, GUEST_CR3_INDEX
		vmread r8, rax
		mov r9, gs: [20h]
		lea rax, EPT_VIOLATION_C_DISPATCHER
		call rax

		add rsp, 68h
		ret
	EPT_VIOLATION_EXIT ENDP

	EPT_MISCONFIGURATION_EXIT PROC
		sub rsp, 68h
		mov rcx, 7722AACCh
		call g_bugCheck
		add rsp, 68h 
		ret
	EPT_MISCONFIGURATION_EXIT ENDP
	
	VMCALL_EXIT PROC
		mov rsp, rbp

		sub rsp, 248h
		fxrstor qword ptr [rsp]  ;at least 200h(= 512) bytes space is needed
		add rsp, 248h	

		add rsp, 20h

		pop r15
		pop r14
		pop r13
		pop r12
		pop r11
		pop r10
		pop r9
		pop r8
		pop rbp
		pop rdi
		pop rsi 
		pop rdx
		pop rcx
		pop rbx
		pop rax

		mov qword ptr [rsp + 8h], 0
		mov qword ptr [rsp + 10h], 0
		mov qword ptr [rsp + 18h], 0
		mov rax, g_ept
		mov qword ptr [rsp + 10h], rax
		mov rax, 2
		invept rax, oword ptr [rsp + 10h] ;flush ept cache
		
		mov eax, GUEST_RSP_INDEX
		vmread rsp, rax ;change stack to guest ;;guest x8h

		mov eax, GUEST_CR0_INDEX
		vmread rax, rax
		mov qword ptr [rsp + 8h], rax

		mov eax, GUEST_CR3_INDEX
		vmread rax, rax
		mov qword ptr [rsp + 10h], rax

		mov eax, GUEST_CR4_INDEX
		vmread rax, rax
		mov qword ptr [rsp + 18h], rax

		mov eax, GUEST_RFLAGS_INDEX
		vmread rax, rax
		mov qword ptr [rsp + 20h], rax

		vmxoff

		;now guest rsp -> <guestRetAddress>

		mov rax, qword ptr [rsp + 18h]
		mov cr4, rax
		mov rax, qword ptr [rsp + 10h]
		mov cr3, rax
		mov rax, qword ptr [rsp + 8h]
		mov cr0, rax
		mov rax, qword ptr [rsp + 20h]
		;DO NOT change any stack space not in [rsp - 28h, rsp]
		add rsp, 10h
		push rax
		popfq
		sub rsp, 10h

		ret
	VMCALL_EXIT ENDP

END