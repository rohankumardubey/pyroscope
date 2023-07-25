#ifndef PYPERF_H
#define PYPERF_H

#include "vmlinux.h"
#include "bpf_helpers.h"

#include "ume.h"

#define PYTHON_STACK_FRAMES_PER_PROG 25
#define PYTHON_STACK_PROG_CNT 3
#define STACK_MAX_LEN (PYTHON_STACK_FRAMES_PER_PROG * PYTHON_STACK_PROG_CNT)
#define CLASS_NAME_LEN 32
#define FUNCTION_NAME_LEN 64
#define FILE_NAME_LEN 128
#define TASK_COMM_LEN 16

enum {
    STACK_STATUS_COMPLETE = 0,
    STACK_STATUS_ERROR = 1,
    STACK_STATUS_TRUNCATED = 2,
};

enum {
    GIL_STATE_NO_INFO = 0,
    GIL_STATE_ERROR = 1,
    GIL_STATE_UNINITIALIZED = 2,
    GIL_STATE_NOT_LOCKED = 3,
    GIL_STATE_THIS_THREAD = 4,
    GIL_STATE_GLOBAL_CURRENT_THREAD = 5,
    GIL_STATE_OTHER_THREAD = 6,
    GIL_STATE_NULL = 7,
};

enum {
    THREAD_STATE_UNKNOWN = 0,
    THREAD_STATE_MATCH = 1,
    THREAD_STATE_MISMATCH = 2,
    THREAD_STATE_THIS_THREAD_NULL = 3,
    THREAD_STATE_GLOBAL_CURRENT_THREAD_NULL = 4,
    THREAD_STATE_BOTH_NULL = 5,
};

enum {
    PTHREAD_ID_UNKNOWN = 0,
    PTHREAD_ID_MATCH = 1,
    PTHREAD_ID_MISMATCH = 2,
    PTHREAD_ID_THREAD_STATE_NULL = 3,
    PTHREAD_ID_NULL = 4,
    PTHREAD_ID_ERROR = 5,
};

typedef struct {
    int64_t PyObject_type;
    int64_t PyTypeObject_name;
    int64_t PyThreadState_frame;
    int64_t PyThreadState_thread;
    int64_t PyFrameObject_back;
    int64_t PyFrameObject_code;
    int64_t PyFrameObject_lineno;
    int64_t PyFrameObject_localsplus;
    int64_t PyCodeObject_filename;
    int64_t PyCodeObject_name;
    int64_t PyCodeObject_varnames;
    int64_t PyTupleObject_item;
    int64_t String_data;
    int64_t String_size;
} py_offset_config;

typedef struct {
    uintptr_t current_state_addr; // virtual address of _PyThreadState_Current
    uintptr_t tls_key_addr; // virtual address of autoTLSkey for pthreads TLS
    uintptr_t gil_locked_addr; // virtual address of gil_locked
    uintptr_t gil_last_holder_addr; // virtual address of gil_last_holder
    py_offset_config offsets;
} py_pid_data;

typedef struct {
    char classname[CLASS_NAME_LEN];
    char name[FUNCTION_NAME_LEN];
    char file[FILE_NAME_LEN];
    // NOTE: PyFrameObject also has line number but it is typically just the
    // first line of that function and PyCode_Addr2Line needs to be called
    // to get the actual line
} py_symbol;

typedef struct {
    uint32_t pid;
    uint32_t tid;
    char comm[TASK_COMM_LEN];
    uint8_t thread_state_match;
    uint8_t gil_state;
    uint8_t pthread_id_match;
    uint8_t stack_status;
    // instead of storing symbol name here directly, we add it to another
    // hashmap with Symbols and only store the ids here
    int64_t stack_len;
    uint32_t stack[STACK_MAX_LEN];
} py_event;

#define _STR_CONCAT(str1, str2) str1##str2
#define STR_CONCAT(str1, str2) _STR_CONCAT(str1, str2)
#define FAIL_COMPILATION_IF(condition)            \
  typedef struct {                                \
    char _condition_check[1 - 2 * !!(condition)]; \
  } STR_CONCAT(compile_time_condition_check, __COUNTER__);
// See comments in get_frame_data
FAIL_COMPILATION_IF(sizeof(py_symbol) == sizeof(struct bpf_perf_event_value))

typedef u64 frame_ptr_t;

typedef struct {
    py_offset_config offsets;
    uint64_t cur_cpu;
    int64_t symbol_counter;
    frame_ptr_t frame_ptr;
    int64_t python_stack_prog_call_cnt;
    py_event event;
} py_sample_state_t;

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, u32);
    __type(value, py_sample_state_t);
    __uint(max_entries, 1);
} py_state_heap SEC(".maps");


struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, py_symbol);
    __type(value, int32_t);
    __uint(max_entries, 16384);
} py_symbols SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, pid_t);
    __type(value, py_pid_data);
    __uint(max_entries, 10240); // todo
} py_pid_config SEC(".maps");

#define PYTHON_PROG_IDX_ON_EVENT 0
#define PYTHON_PROG_IDX_READ_PYTHON_STACK 1
int read_python_stack(struct pt_regs* ctx);
int on_event(struct pt_regs* ctx);

struct {
    __uint(type, BPF_MAP_TYPE_PROG_ARRAY);
    __uint(max_entries, 2);
    __type(key, int);
    __array(values, int (void *)); //todo
} py_progs SEC(".maps") = {
        .values = {
                [PYTHON_PROG_IDX_ON_EVENT] = (void *)&on_event,
                [PYTHON_PROG_IDX_READ_PYTHON_STACK] = (void *)&read_python_stack,
        },
};


struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(u32));
    __uint(value_size, sizeof(u32));
} py_events SEC(".maps") ;

static inline __attribute__((__always_inline__)) void* get_thread_state(
        void* tls_base,
        py_pid_data* pid_data) {
    // Python sets the thread_state using pthread_setspecific with the key
    // stored in a global variable autoTLSkey.
    // We read the value of the key from the global variable and then read
    // the value in the thread-local storage. This relies on pthread implementation.
    // This is basically the same as running the following in GDB:
    //  p *(PyThreadState*)((struct pthread*)pthread_self())->
    //    specific_1stblock[autoTLSkey]->data
    int key;
    bpf_probe_read_user(&key, sizeof(key), (void*)pid_data->tls_key_addr);

    bpf_printk("pid_data->tls_key_addr = %x, key=%x", pid_data->tls_key_addr, key);
    // This assumes autoTLSkey < 32, which means that the TLS is stored in
    //   pthread->specific_1stblock[autoTLSkey]
    // 0x310 is offsetof(struct pthread, specific_1stblock),
    // 0x10 is sizeof(pthread_key_data)
    // 0x8 is offsetof(struct pthread_key_data, data)
    // 'struct pthread' is not in the public API so we have to hardcode
    // the offsets here
    void* thread_state;
    bpf_probe_read_user(
            &thread_state,
            sizeof(thread_state),
            tls_base + 0x310 + key * 0x10 + 0x08);
    bpf_printk("thread_state=%x", thread_state);

    return thread_state;
}

static inline __attribute__((__always_inline__)) int submit_sample(
        struct pt_regs* ctx,
        py_sample_state_t* state) {
    bpf_perf_event_output(ctx, &py_events, BPF_F_CURRENT_CPU, &state->event, sizeof(py_event));
    return 0; // todo return value
}

// this function is trivial, but we need to do map lookup in separate function,
// because BCC doesn't allow direct map calls (including lookups) from inside
// a macro (which we want to do in GET_STATE() macro below)
static inline __attribute__((__always_inline__)) py_sample_state_t* get_state() {
    int zero = 0;
    return bpf_map_lookup_elem(&py_state_heap, &zero);
}

#define GET_STATE()                     \
  py_sample_state_t* state = get_state();  \
  if (!state) {                         \
    return 0; /* should never happen */ \
  }

static inline __attribute__((__always_inline__)) int get_thread_state_match(
        void* this_thread_state,
        void* global_thread_state) {
    if (this_thread_state == 0 && global_thread_state == 0) {
        return THREAD_STATE_BOTH_NULL;
    }
    if (this_thread_state == 0) {
        return THREAD_STATE_THIS_THREAD_NULL;
    }
    if (global_thread_state == 0) {
        return THREAD_STATE_GLOBAL_CURRENT_THREAD_NULL;
    }
    if (this_thread_state == global_thread_state) {
        return THREAD_STATE_MATCH;
    } else {
        return THREAD_STATE_MISMATCH;
    }
}

static inline __attribute__((__always_inline__)) int get_gil_state(
        void* this_thread_state,
        void* global_thread_state,
        py_pid_data* pid_data) {
    // Get information of GIL state
    if (pid_data->gil_locked_addr == 0 || pid_data->gil_last_holder_addr == 0) {
        return GIL_STATE_NO_INFO;
    }

    int gil_locked = 0;
    void* gil_thread_state = 0;
    if (bpf_probe_read_user(
            &gil_locked, sizeof(gil_locked), (void*)pid_data->gil_locked_addr)) {
        return GIL_STATE_ERROR;
    }

    switch (gil_locked) {
        case -1:
            return GIL_STATE_UNINITIALIZED;
        case 0:
            return GIL_STATE_NOT_LOCKED;
        case 1:
            // GIL is held by some Thread
            bpf_probe_read_user(
                    &gil_thread_state,
                    sizeof(void*),
                    (void*)pid_data->gil_last_holder_addr);
            if (gil_thread_state == this_thread_state) {
                return GIL_STATE_THIS_THREAD;
            } else if (gil_thread_state == global_thread_state) {
                return GIL_STATE_GLOBAL_CURRENT_THREAD;
            } else if (gil_thread_state == 0) {
                return GIL_STATE_NULL;
            } else {
                return GIL_STATE_OTHER_THREAD;
            }
        default:
            return GIL_STATE_ERROR;
    }
}

static inline __attribute__((__always_inline__)) int
get_pthread_id_match(void* thread_state, void* tls_base, py_pid_data* pid_data) {
    if (thread_state == 0) {
        return PTHREAD_ID_THREAD_STATE_NULL;
    }

    uint64_t pthread_self, pthread_created;

    bpf_probe_read_user(
            &pthread_created,
            sizeof(pthread_created),
            thread_state + pid_data->offsets.PyThreadState_thread);
    if (pthread_created == 0) {
        return PTHREAD_ID_NULL;
    }

    // 0x10 = offsetof(struct pthread, header.self)
    bpf_probe_read_user(&pthread_self, sizeof(pthread_self), tls_base + 0x10);
    if (pthread_self == 0) {
        return PTHREAD_ID_ERROR;
    }

    if (pthread_self == pthread_created) {
        return PTHREAD_ID_MATCH;
    } else {
        return PTHREAD_ID_MISMATCH;
    }
}

SEC("perf_event")
int on_event(struct pt_regs* ctx) {
    uint64_t pid_tgid = bpf_get_current_pid_tgid();
    pid_t pid = (pid_t)(pid_tgid >> 32);
    py_pid_data* pid_data = bpf_map_lookup_elem(&py_pid_config, &pid);
    if (!pid_data) {
        return 0;
    }

    GET_STATE();

    state->offsets = pid_data->offsets;
    state->cur_cpu = bpf_get_smp_processor_id();
    state->python_stack_prog_call_cnt = 0;

    py_event* event = &state->event;
    event->pid = pid;
    event->tid = (pid_t)pid_tgid;
    bpf_get_current_comm(&event->comm, sizeof(event->comm));

    // Get pointer of global PyThreadState, which should belong to the Thread
    // currently holds the GIL
    void* global_current_thread = (void*)0;
    bpf_probe_read_user(
            &global_current_thread,
            sizeof(global_current_thread),
            (void*)pid_data->current_state_addr);

    struct task_struct* task = (struct task_struct*)bpf_get_current_task();
    if (task == NULL) {
        return 0;
    }
    void* tls_base = NULL;

//todo can we read fsbase from pt_regs?
#if defined(__TARGET_ARCH_x86)
    if (pyro_bpf_core_read(&tls_base, sizeof(tls_base), &task->thread.fsbase)) {
        return 0;
    }
#elif defined(__TARGET_ARCH_arm64)
    if (pyro_bpf_core_read(&tls_base, sizeof(tls_base), &task->thread.uw.tp_value)) { // todo what is tp_value2??
        return 0;
    }
#else
    #error "Unknown architecture"
#endif

    // Read PyThreadState of this Thread from TLS
    void* thread_state = get_thread_state(tls_base, pid_data);

    // Check for matching between TLS PyThreadState and
    // the global _PyThreadState_Current
    event->thread_state_match =
            get_thread_state_match(thread_state, global_current_thread);

    // Read GIL state
    event->gil_state =
            get_gil_state(thread_state, global_current_thread, pid_data);

    // Check for matching between pthread ID created current PyThreadState and
    // pthread of actual current pthread
    event->pthread_id_match =
            get_pthread_id_match(thread_state, tls_base, pid_data);

    // pre-initialize event struct in case any subprogram below fails
    event->stack_status = STACK_STATUS_COMPLETE;
    event->stack_len = 0;

    if (thread_state != 0) {
        // Get pointer to top frame from PyThreadState
        int res = bpf_probe_read_user(
                &state->frame_ptr,
                sizeof(void*),
                thread_state + pid_data->offsets.PyThreadState_frame);
        bpf_printk("state->frame_ptr %x %d", state->frame_ptr, res);
        // jump to reading first set of Python frames
        pyro_bpf_tail_call(ctx, &py_progs, PYTHON_PROG_IDX_READ_PYTHON_STACK);
        // we won't ever get here
    }

    return submit_sample(ctx, state);
}

static inline __attribute__((__always_inline__)) void get_names(
        frame_ptr_t cur_frame,
        void* code_ptr,
        py_offset_config* offsets,
        py_symbol* symbol,
        void* ctx) {
    // Figure out if we want to parse class name, basically checking the name of
    // the first argument,
    //   ((PyTupleObject*)$frame->f_code->co_varnames)->ob_item[0]
    // If it's 'self', we get the type and it's name, if it's cls, we just get
    // the name. This is not perfect but there is no better way to figure this
    // out from the code object.
    void* args_ptr;
    bpf_probe_read_user(
            &args_ptr, sizeof(void*), code_ptr + offsets->PyCodeObject_varnames);
    bpf_probe_read_user(
            &args_ptr, sizeof(void*), args_ptr + offsets->PyTupleObject_item);
    bpf_probe_read_user_str(
            &symbol->name, sizeof(symbol->name), args_ptr + offsets->String_data);

    // compare strings as ints to save instructions
    char self_str[4] = {'s', 'e', 'l', 'f'};
    char cls_str[4] = {'c', 'l', 's', '\0'};
    bool first_self = *(int32_t*)symbol->name == *(int32_t*)self_str;
    bool first_cls = *(int32_t*)symbol->name == *(int32_t*)cls_str;

    // We re-use the same py_symbol instance across loop iterations, which means
    // we will have left-over data in the struct. Although this won't affect
    // correctness of the result because we have '\0' at end of the strings read,
    // it would affect effectiveness of the deduplication.
    // Helper bpf_perf_prog_read_value clears the buffer on error, so here we
    // (ab)use this behavior to clear the memory. It requires the size of py_symbol
    // to be different from struct bpf_perf_event_value, which we check at
    // compilation time using the FAIL_COMPILATION_IF macro.
    bpf_perf_prog_read_value(ctx, (struct bpf_perf_event_value *)symbol, sizeof(py_symbol));

    // Read class name from $frame->f_localsplus[0]->ob_type->tp_name.
    if (first_self || first_cls) {
        void* ptr;
        bpf_probe_read_user(
                &ptr, sizeof(void*), (void*)(cur_frame + offsets->PyFrameObject_localsplus));
        if (first_self) {
            // we are working with an instance, first we need to get type
            bpf_probe_read_user(&ptr, sizeof(void*), ptr + offsets->PyObject_type);
        }
        bpf_probe_read_user(&ptr, sizeof(void*), ptr + offsets->PyTypeObject_name);
        bpf_probe_read_user_str(&symbol->classname, sizeof(symbol->classname), ptr);
    }

    void* pystr_ptr;
    // read PyCodeObject's filename into symbol
    bpf_probe_read_user(
            &pystr_ptr, sizeof(void*), code_ptr + offsets->PyCodeObject_filename);
    bpf_probe_read_user_str(
            &symbol->file, sizeof(symbol->file), pystr_ptr + offsets->String_data);
    // read PyCodeObject's name into symbol
    bpf_probe_read_user(
            &pystr_ptr, sizeof(void*), code_ptr + offsets->PyCodeObject_name);
    bpf_probe_read_user_str(
            &symbol->name, sizeof(symbol->name), pystr_ptr + offsets->String_data);
}

// get_frame_data reads current PyFrameObject filename/name and updates
// stack_info->frame_ptr with pointer to next PyFrameObject
static inline __attribute__((__always_inline__)) bool get_frame_data(
        frame_ptr_t* frame_ptr,
        py_offset_config* offsets,
        py_symbol* symbol,
        // ctx is only used to call helper to clear symbol, see documentation below
        void* ctx) {
    frame_ptr_t cur_frame = *frame_ptr;
    if (!cur_frame) {
        return false;
    }
    void* code_ptr;
    // read PyCodeObject first, if that fails, then no point reading next frame
    bpf_probe_read_user(
            &code_ptr, sizeof(void*), (void*)(cur_frame + offsets->PyFrameObject_code));
    if (!code_ptr) {
        return false;
    }

    get_names(cur_frame, code_ptr, offsets, symbol, ctx);

    // read next PyFrameObject pointer, update in place
    bpf_probe_read_user(
            frame_ptr, sizeof(void*), (void*)(cur_frame + offsets->PyFrameObject_back));

    return true;
}
// TODO NO MERGE, fix this
#define NUM_CPUS 32
// To avoid duplicate ids, every CPU needs to use different ids when inserting
// into the hashmap. NUM_CPUS is defined at PyPerf backend side and passed
// through CFlag.
static inline __attribute__((__always_inline__)) int64_t get_symbol_id(
        py_sample_state_t* state,
        py_symbol* sym) {

    bpf_printk("sym  %s ", sym->name);
    int32_t* symbol_id_ptr = bpf_map_lookup_elem(&py_symbols, sym);
    if (symbol_id_ptr) {
        bpf_printk("sym %s = %d", sym->name, *symbol_id_ptr);
        return *symbol_id_ptr;
    }
    // the symbol is new, bump the counter
    state->symbol_counter++;
    int32_t symbol_id = state->symbol_counter * NUM_CPUS + state->cur_cpu;
    bpf_map_update_elem(&py_symbols, sym, &symbol_id, BPF_ANY);
    return symbol_id;
}

SEC("perf_event")
int read_python_stack(struct pt_regs* ctx) {
    GET_STATE();

    state->python_stack_prog_call_cnt++;
    py_event* sample = &state->event;

    py_symbol sym = {};
    bool last_res = false;
#pragma unroll
    for (int i = 0; i < PYTHON_STACK_FRAMES_PER_PROG; i++) {
        last_res = get_frame_data(&state->frame_ptr, &state->offsets, &sym, ctx);
        if (last_res) {
            uint32_t symbol_id = get_symbol_id(state, &sym);
            int64_t cur_len = sample->stack_len;
            if (cur_len >= 0 && cur_len < STACK_MAX_LEN) {
                sample->stack[cur_len] = symbol_id;
                sample->stack_len++;
            }
        }
    }

    if (!state->frame_ptr) {
        sample->stack_status = STACK_STATUS_COMPLETE;
    } else {
        if (!last_res) {
            sample->stack_status = STACK_STATUS_ERROR;
        } else {
            sample->stack_status = STACK_STATUS_TRUNCATED;
        }
    }

    if (sample->stack_status == STACK_STATUS_TRUNCATED &&
        state->python_stack_prog_call_cnt < PYTHON_STACK_PROG_CNT) {
        // read next batch of frames
        pyro_bpf_tail_call(ctx, &py_progs, PYTHON_PROG_IDX_READ_PYTHON_STACK);
    }

    return submit_sample(ctx, state);
}

#endif // PYPERF_H