/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


#include "clog.h"
#include "method_lookup.h"
#include "Environment.h"
#include "exceptions.h"
#include "exceptions_jit.h"
#include "interpreter_exports.h"
#include "stack_iterator.h"
#include "stack_dump.h"
#include "jvmti_break_intf.h"
#include "m2n.h"
#include "open/hythread_ext.h"

// Windows specific
#include <string>
#include <excpt.h>

#include "exception_filter.h"


#if INSTRUMENTATION_BYTE == INSTRUMENTATION_BYTE_INT3
#define JVMTI_EXCEPTION_STATUS STATUS_BREAKPOINT
#elif INSTRUMENTATION_BYTE == INSTRUMENTATION_BYTE_HLT || INSTRUMENTATION_BYTE == INSTRUMENTATION_BYTE_CLI
#define JVMTI_EXCEPTION_STATUS STATUS_PRIVILEGED_INSTRUCTION
#else
#error Unknown value of INSTRUMENTATION_BYTE
#endif

static void print_callstack(LPEXCEPTION_POINTERS nt_exception) {
    PCONTEXT context = nt_exception->ContextRecord;
    Registers regs;
    nt_to_vm_context(context, &regs);
    st_print_stack(&regs);
}


static LONG process_crash(LPEXCEPTION_POINTERS nt_exception, const char* msg = NULL)
{
static DWORD saved_eip_index = TlsAlloc();
static BOOL UNREF tmp_init = TlsSetValue(saved_eip_index, (LPVOID)0);

    Registers regs;
    nt_to_vm_context(nt_exception->ContextRecord, &regs);

    // Check crash location to prevent infinite recursion
    if (regs.get_ip() == (void*)TlsGetValue(saved_eip_index))
        return EXCEPTION_CONTINUE_SEARCH;
    // Store registers to compare IP in future
    TlsSetValue(saved_eip_index, (LPVOID)regs.get_ip());

    switch (nt_exception->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
        break;

    case EXCEPTION_STACK_OVERFLOW:
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // We can't obtain a value of property if loader_env is NULL
    if (VM_Global_State::loader_env == NULL ||
        get_boolean_property("vm.assert_dialog", TRUE, VM_PROPERTIES))
        return EXCEPTION_CONTINUE_SEARCH;

    print_state(nt_exception, msg);
    print_callstack(nt_exception);
    LOGGER_EXIT(-1);
    return EXCEPTION_CONTINUE_EXECUTION;
}


/*
 * Information about stack
 */
inline void* find_stack_addr() {
    void* stack_addr;
    size_t reg_size;
    MEMORY_BASIC_INFORMATION memory_information;

    VirtualQuery(&memory_information, &memory_information, sizeof(memory_information));
    reg_size = memory_information.RegionSize;
    stack_addr =((char*) memory_information.BaseAddress) + reg_size;

    return stack_addr;
}

inline size_t find_guard_page_size() {
    size_t  guard_size;
    SYSTEM_INFO system_info;

    GetSystemInfo(&system_info);
    guard_size = system_info.dwPageSize;

    return guard_size;
}

inline size_t find_guard_stack_size() {
    // guaerded stack size on windows can be equals one page size only :(
    return find_guard_page_size();
}

static size_t common_guard_stack_size;
static size_t common_guard_page_size;

inline void* get_stack_addr() {
    return p_TLS_vmthread->stack_addr;
}

inline size_t get_stack_size() {
  return p_TLS_vmthread->stack_size;
}

inline size_t get_guard_stack_size() {
    return common_guard_stack_size;
}

inline size_t get_guard_page_size() {
    return common_guard_page_size;
}


void init_stack_info() {
    p_TLS_vmthread->stack_addr = find_stack_addr();
    p_TLS_vmthread->stack_size = hythread_get_thread_stacksize(hythread_self());
    common_guard_stack_size = find_guard_stack_size();
    common_guard_page_size = find_guard_page_size();
}

void set_guard_stack() {
    void* stack_addr = get_stack_addr();
    size_t stack_size = get_stack_size();
    size_t page_size = get_guard_page_size();
    assert(((size_t)(&stack_addr)) > ((size_t)((char*)stack_addr - stack_size + 3 * page_size)));

    if (!VirtualFree((char*)stack_addr - stack_size + page_size,
        page_size, MEM_DECOMMIT)) {
        // should be successful always
        assert(0);
    }

    DWORD oldProtect;

    if (!VirtualProtect((char*)stack_addr - stack_size + page_size + page_size,
        page_size, PAGE_GUARD | PAGE_READWRITE, &oldProtect)) {
        // should be successful always
        assert(0);
    }

    p_TLS_vmthread->restore_guard_page = false;
}

void remove_guard_stack() {
    void* stack_addr = get_stack_addr();
    size_t stack_size = get_stack_size();
    size_t page_size = get_guard_page_size();
    DWORD oldProtect;

    assert(((size_t)(&stack_addr)) > ((size_t)((char*)stack_addr - stack_size + 3 * page_size)));
    p_TLS_vmthread->restore_guard_page = true;

    if (!VirtualProtect((char*)stack_addr - stack_size + page_size + page_size,
        page_size, PAGE_READWRITE, &oldProtect)) {
        // should be successful always
        assert(0);
    }
}

size_t get_available_stack_size() {
    char* stack_addr = (char*) get_stack_addr();
    size_t used_stack_size = ((size_t)stack_addr) - ((size_t)(&stack_addr));
    int available_stack_size =
            get_stack_size() - used_stack_size
            - 2 * get_guard_page_size() - get_guard_stack_size();

    if (available_stack_size > 0) {
        return (size_t) available_stack_size;
    } else {
        return 0;
    }
}
size_t get_default_stack_size() {
    size_t default_stack_size = get_stack_size();
    return default_stack_size;
}
bool check_available_stack_size(size_t required_size) {
    size_t available_stack_size = get_available_stack_size();
    if (available_stack_size < required_size) {
        if (available_stack_size < get_guard_stack_size()) {
            remove_guard_stack();
        }
        Global_Env *env = VM_Global_State::loader_env;
        exn_raise_by_class(env->java_lang_StackOverflowError_Class);
        return false;
    } else {
        return true;
    }
}

size_t get_restore_stack_size() {
    return 0x0100;
}

bool check_stack_size_enough_for_exception_catch(void* sp) {
    char* stack_adrr = (char*) get_stack_addr();
    size_t used_stack_size = ((size_t)stack_adrr) - ((size_t)sp);
    size_t available_stack_size =
            get_stack_size() - used_stack_size
            - 2 * get_guard_page_size() - get_guard_stack_size();
    return get_restore_stack_size() < available_stack_size;
}


// exception catch callback to restore stack after Stack Overflow Error
void __cdecl exception_catch_callback_wrapper(){
    exception_catch_callback();
}

// exception catch support for JVMTI
void __cdecl jvmti_exception_catch_callback_wrapper(){
	Registers regs = {0};
	if (p_TLS_vmthread->regs) {
		regs = *(Registers*)p_TLS_vmthread->regs;
	}
    jvmti_exception_catch_callback(&regs);
}

LONG NTAPI vectored_exception_handler_internal(LPEXCEPTION_POINTERS nt_exception)
{
    DWORD code = nt_exception->ExceptionRecord->ExceptionCode;
    PCONTEXT context = nt_exception->ContextRecord;
    Registers regs;
    bool flag_replaced = false;
    VM_thread* vmthread = p_TLS_vmthread;

    // Convert NT context to Registers
    nt_to_vm_context(context, &regs);
    POINTER_SIZE_INT saved_eip = (POINTER_SIZE_INT)regs.get_ip();

    bool in_java = false;

    if (vmthread && (&vmthread->jvmti_thread))
    {
        // If exception is occured in processor instruction previously
        // instrumented by breakpoint, the actual exception address will reside
        // in jvmti_jit_breakpoints_handling_buffer
        // We should replace exception address with saved address of instruction
        POINTER_SIZE_INT break_buf =
            (POINTER_SIZE_INT)vmthread->jvmti_thread.jvmti_jit_breakpoints_handling_buffer;
        if (saved_eip >= break_buf &&
            saved_eip < break_buf + TM_JVMTI_MAX_BUFFER_SIZE)
        {
            flag_replaced = true;
            regs.set_ip(vm_get_ip_from_regs(vmthread));
            vm_to_nt_context(&regs, context);
        }

        in_java = (vm_identify_eip(regs.get_ip()) == VM_TYPE_JAVA);
    }

    // the possible reasons for hardware exception are
    //  - segfault or division by zero in java code
    //     => NullPointerException or ArithmeticException
    //
    //  - breakpoint or privileged instruction in java code
    //    => send jvmti breakpoint event
    //
    //  - stack overflow, either in java or in native
    //    => StackOverflowError
    //
    //  - other (internal VM error or debugger breakpoint)
    //    => delegate to default handler

    // delegate "other" cases to crash handler
    // Crash handler shouls be invoked when VM_thread is not attached to VM
    // or exception has occured in native code and it's not STACK_OVERFLOW
    if (!vmthread ||
        (!in_java && code != STATUS_STACK_OVERFLOW))
    {
        LONG result = process_crash(nt_exception);
        regs.set_ip((void*)saved_eip);
        vm_to_nt_context(&regs, context);
        return result;
    }

    TRACE2("signals", ("VEH received an exception: code = %x, ip = %p, sp = %p",
        nt_exception->ExceptionRecord->ExceptionCode, regs.get_ip(), regs_get_sp(&regs)));

    // if HWE occured in java code, suspension should also have been disabled
    assert(!in_java || !hythread_is_suspend_enabled());

    Global_Env *env = VM_Global_State::loader_env;
    // the actual exception object will be created lazily,
    // we determine only exception class here
    Class *exn_class = 0;

    switch(nt_exception->ExceptionRecord->ExceptionCode)
    {
    case STATUS_STACK_OVERFLOW:
        {
            TRACE2("signals",
                ("StackOverflowError detected at ip = %p, esp = %p",
                 regs.get_ip(), regs_get_sp(&regs)));

            vmthread->restore_guard_page = true;
            exn_class = env->java_lang_StackOverflowError_Class;
            if (in_java) {
                // stack overflow occured in java code:
                // nothing special to do
            } else if (is_unwindable()) {
                // stack overflow occured in native code that can be unwound
                // safely.
                // Throwing exception requires suspend disabled status
                if (hythread_is_suspend_enabled())
                    hythread_suspend_disable();
            } else {
                // stack overflow occured in native code that
                // cannot be unwound.
                // Mark raised exception in TLS and resume execution
                exn_raise_by_class(env->java_lang_StackOverflowError_Class);
                regs.set_ip((void*)saved_eip);
                vm_to_nt_context(&regs, context);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        break;
    case STATUS_ACCESS_VIOLATION:
        {
            TRACE2("signals",
                ("NullPointerException detected at ip = %p", regs.get_ip()));
            exn_class = env->java_lang_NullPointerException_Class;
        }
        break;

    case STATUS_INTEGER_DIVIDE_BY_ZERO:
        {
            TRACE2("signals",
                ("ArithmeticException detected at ip = %p", regs.get_ip()));
            exn_class = env->java_lang_ArithmeticException_Class;
        }
        break;
    case JVMTI_EXCEPTION_STATUS:
        // JVMTI breakpoint in JITted code
        {
            // Breakpoints should not occur in breakpoint buffer
            assert(!flag_replaced);

            TRACE2("signals",
                ("JVMTI breakpoint detected at ip = %p", regs.get_ip()));
            bool handled = jvmti_jit_breakpoint_handler(&regs);
            if (handled)
            {
                vm_to_nt_context(&regs, context);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            else
                return EXCEPTION_CONTINUE_SEARCH;
        }
    default:
        // unexpected hardware exception occured in java code
        LONG result = process_crash(nt_exception);
        regs.set_ip((void*)saved_eip);
        vm_to_nt_context(&regs, context);
        return result;
    }

    // we must not call potentially blocking or suspendable code
    // (i.e. java code of exception constructor) from exception
    // handler, because this handler may hold a system-wide lock,
    // and this may result in a deadlock.

    // it was reported that exception handler grabs a system
    // lock on Windows XPsp2 and 2003sp0, but not on a 2003sp1

    // save register context of hardware exception site
    // into thread-local registers snapshot
    vm_set_exception_registers(vmthread, regs);

    // __cdecl <=> push parameters in the reversed order
    // push in_java argument onto stack
    regs_push_param(&regs, in_java, 1/*2nd arg */);
    // push the exn_class argument onto stack
    assert(exn_class);
    regs_push_param(&regs, (POINTER_SIZE_INT)exn_class, 0/* 1st arg */);
    // imitate return IP on stack
    regs_push_return_address(&regs, NULL);

    // set up the real exception handler address
    regs.set_ip(asm_c_exception_handler);
    // Store changes into NT context
    vm_to_nt_context(&regs, context);

    // exit NT exception handler and transfer
    // control to VM exception handler
    return EXCEPTION_CONTINUE_EXECUTION;
}

void __cdecl c_exception_handler(Class* exn_class, bool in_java)
{
    // this exception handler is executed *after* NT exception handler returned
    DebugUtilsTI* ti = VM_Global_State::loader_env->TI;
    // Create local copy for registers because registers in TLS can be changed
    Registers regs = {0};
    VM_thread *thread = p_TLS_vmthread;
    assert(thread);
    if (thread->regs) {
        regs = *(Registers*)thread->regs;
    }

    M2nFrame* prev_m2n = m2n_get_last_frame();
    M2nFrame* m2n = NULL;
    if (in_java)
        m2n = m2n_push_suspended_frame(&regs);

    TRACE2("signals", ("should throw exception %p at IP=%p, SP=%p",
                exn_class, regs.get_ip(), regs_get_sp(&regs)));
    exn_athrow_regs(&regs, exn_class, false);

    if (ti->get_global_capability(DebugUtilsTI::TI_GC_ENABLE_EXCEPTION_EVENT)) {
        // Set return address to current IP
        regs_push_return_address(&regs, regs.get_ip());
        // Set IP to callback address
        regs.set_ip(asm_jvmti_exception_catch_callback);
    } else if (thread->restore_guard_page) {
        // Set return address to current IP
        regs_push_return_address(&regs, regs.get_ip());
        // Set IP to callback address
        regs.set_ip(asm_exception_catch_callback);
    }

    StackIterator *si =
        si_create_from_registers(&regs, false, prev_m2n);

    if (m2n)
        STD_FREE(m2n);

    vm_set_exception_registers(thread, regs);
    si_transfer_control(si);
    assert(!"si_transfer_control should not return");
}
