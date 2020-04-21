//
// Created by Yves on 2020-03-11.
//

#include <dlfcn.h>
#include <unordered_map>
#include <StackTrace.h>
#include <cxxabi.h>
#include <sstream>
#include <iostream>
#include <xhook.h>
#include <cinttypes>
#include <regex>
#include <set>
#include <regex.h>
#include <utils.h>
#include "PthreadHook.h"
#include "pthread.h"
#include "log.h"
#include "JNICommon.h"
#include "cJSON.h"

#define ORIGINAL_LIB "libc.so"
#define TAG "PthreadHook"

#define THREAD_NAME_LEN 16

typedef void *(*pthread_routine_t)(void *);

//typedef struct {
//
//    std::vector<unwindstack::FrameData> native_stacktrace;
//
//    std::atomic<char *> java_stacktrace;
//
//} pthread_stacktrace_t;

struct pthread_meta_t {
    pid_t tid;
    char  *thread_name;
    char  *parent_name;

    uint64_t hash;

    std::vector<unwindstack::FrameData> native_stacktrace;

    std::atomic<char *> java_stacktrace;

    pthread_meta_t() : tid(0),
                       thread_name(nullptr),
                       parent_name(nullptr),
                       hash(0),
                       java_stacktrace(nullptr) {
    };

    ~pthread_meta_t() = default;

    pthread_meta_t(const pthread_meta_t &src) {
        tid               = src.tid;
        thread_name       = src.thread_name;
        parent_name       = src.parent_name;
        hash              = src.hash;
        native_stacktrace = src.native_stacktrace;
        java_stacktrace.store(src.java_stacktrace.load(std::memory_order_acquire),
                              std::memory_order_release);
    }
};

// fixme on_create on_setname 时序问题

typedef struct {
    pthread_routine_t origin_func;
    void              *origin_args;
}            routine_wrapper_t;

struct regex_wrapper {
    const char *regex_str;
    regex_t    regex;

    regex_wrapper(const char *regexStr, const regex_t &regex) : regex_str(regexStr), regex(regex) {}

    friend bool operator<(const regex_wrapper &left, const regex_wrapper &right) {
        return static_cast<bool>(strcmp(left.regex_str, right.regex_str));
    }
};

static pthread_mutex_t     m_pthread_meta_mutex;
static pthread_mutexattr_t attr;

static std::map<pthread_t, pthread_meta_t> m_pthread_metas;

static std::set<regex_wrapper> m_hook_thread_name_regex;

static pthread_key_t m_key;

static void on_pthread_destroy(void *__specific);

void pthread_hook_init() {
    LOGD(TAG, "pthread_hook_init");
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_pthread_meta_mutex, &attr);

    if (!m_key) {
        pthread_key_create(&m_key, on_pthread_destroy);
    }
}

void add_hook_thread_name(const char *__regex_str) {
//    std::regex regex(__regex_str);
    regex_t regex;
    if (0 != regcomp(&regex, __regex_str, REG_NOSUB)) {
        LOGE("PthreadHook", "regex compiled error: %s", __regex_str);
        return;
    }
    size_t len          = strlen(__regex_str) + 1;
    char   *p_regex_str = static_cast<char *>(malloc(len));
    strncpy(p_regex_str, __regex_str, len);
    regex_wrapper w_regex(p_regex_str, regex);
    m_hook_thread_name_regex.insert(w_regex);
    LOGD(TAG, "parent name regex: %s -> %s, len = %zu", __regex_str, p_regex_str, len);
}

static int read_thread_name(pthread_t __pthread, char *__buf, size_t __n) {
    if (!__buf) {
        return -1;
    }

    char proc_path[128];

    sprintf(proc_path, "/proc/self/task/%d/stat", pthread_gettid_np(__pthread));

    FILE *file = fopen(proc_path, "r");

    if (!file) {
        LOGD(TAG, "file not found: %s", proc_path);
        return -1;
    }

    fscanf(file, "%*d (%[^)]", __buf);

    LOGD(TAG, "read thread name %s, len %zu", __buf, strlen(__buf));

    fclose(file);

    return 0;
}

inline int wrap_pthread_getname_np(pthread_t __pthread, char *__buf, size_t __n) {
#if __ANDROID_API__ >= 26
    return pthread_getname_np(__pthread, __buf, __n);
#else
    return read_thread_name(__pthread, __buf, __n);
#endif
}

static bool test_match_thread_name(pthread_meta_t &__meta) {
    for (const auto &w : m_hook_thread_name_regex) {
        if (__meta.thread_name && 0 == regexec(&w.regex, __meta.thread_name, 0, NULL, 0)) {
            LOGD(TAG, "%s matches regex %s", __meta.thread_name, w.regex_str);
            return true;
        }
    }
    return false;
}

static void unwind_native_stacktrace(pthread_meta_t &__meta) {
    unwindstack::do_unwind(__meta.native_stacktrace);
}

static void unwind_java_stacktrace(pthread_meta_t *__meta) {
    const size_t BUF_SIZE = 1024;
    char         *buf     = static_cast<char *>(malloc(BUF_SIZE));

    if (buf) {
        get_java_stacktrace(buf, BUF_SIZE);
    }
    __meta->java_stacktrace.store(buf, std::memory_order_release);
}

static void on_pthread_create(const pthread_t __pthread_ptr) {

    pthread_t pthread = __pthread_ptr;
    pid_t     tid     = pthread_gettid_np(pthread);

    LOGD(TAG, "+++++++ on_pthread_create tid: %d", tid);
    pthread_mutex_lock(&m_pthread_meta_mutex);

    if (m_pthread_metas.count(pthread)) {
        LOGD(TAG, "on_pthread_create: thread already recorded");
        pthread_mutex_unlock(&m_pthread_meta_mutex);
        return;
    }

    pthread_meta_t &meta = m_pthread_metas[pthread];

    meta.tid = tid;

    char *parent_name = static_cast<char *>(malloc(sizeof(char) * THREAD_NAME_LEN));

    if (wrap_pthread_getname_np(pthread, parent_name, THREAD_NAME_LEN) == 0) {
    } else {
        strncpy(parent_name, "(null)", THREAD_NAME_LEN);
    }

    meta.parent_name = parent_name;

    if (!meta.thread_name) { // 如果还没 setname, 则继承父线程名
        meta.thread_name = meta.parent_name;
    }

    LOGD(TAG, "on_pthread_create: pthread = %ld, parent name: %s, thread name: %s", pthread,
         meta.parent_name,
         meta.thread_name);

    bool need_unwind = test_match_thread_name(meta);

    uint64_t native_hash = 0;
    uint64_t java_hash   = 0;

    if (need_unwind && meta.native_stacktrace.empty()) {
        unwind_native_stacktrace(meta);
        native_hash = hash_stack_frames(meta.native_stacktrace);
        LOGD(TAG, "on_pthread_create: native hash = %lu", native_hash);
    }

    pthread_mutex_unlock(&m_pthread_meta_mutex);

    if (need_unwind && !meta.java_stacktrace.load(std::memory_order_acquire)) {
        // 反射 Java 获取堆栈时加锁会造成死锁
        unwind_java_stacktrace(&meta);

        const char *java_stacktrace = meta.java_stacktrace.load(std::memory_order_acquire);
        if (java_stacktrace) {
            java_hash = hash_str(java_stacktrace);
            LOGD(TAG, "on_pthread_create: java hash = %lu", java_hash);
        }
    }

    pthread_mutex_lock(&m_pthread_meta_mutex);

    if (native_hash && java_hash) {
        meta.hash = hash_combine(native_hash, java_hash);
    }

    pthread_mutex_unlock(&m_pthread_meta_mutex);
    LOGD(TAG, "------ on_pthread_create end");
}

/**
 * on_pthread_setname 有可能在 on_pthread_create 之前先执行
 * @param __pthread
 * @param __name
 */
static void on_pthread_setname(pthread_t __pthread, const char *__name) {
    if (NULL == __name) {
        LOGE(TAG, "setting name null");
        return;
    }

    const size_t name_len = strlen(__name);

    if (0 == name_len || name_len >= THREAD_NAME_LEN) {
        LOGE(TAG, "pthread name is illegal, just ignore. len(%s)", __name);
        return;
    }

    LOGD(TAG, "++++++++ pre on_pthread_setname tid: %d, %s", pthread_gettid_np(__pthread), __name);

    pthread_mutex_lock(&m_pthread_meta_mutex);

    if (!m_pthread_metas.count(__pthread)) {
        auto lost_thread_name = static_cast<char *>(malloc(sizeof(char) * THREAD_NAME_LEN));
        wrap_pthread_getname_np(__pthread, lost_thread_name, THREAD_NAME_LEN);
        LOGE(TAG, "on_pthread_setname: pthread hook lost: {%s}", lost_thread_name);
        free(lost_thread_name);
        pthread_mutex_unlock(&m_pthread_meta_mutex);
        return;
    }

    pthread_meta_t &meta = m_pthread_metas.at(__pthread);

    // 如果 match, 则说明在父线程 unwind 了, 否则应该重新 unwind
    bool unwind_in_parent = test_match_thread_name(meta);

    LOGD(TAG, "on_pthread_setname: %s -> %s, tid:%d", meta.thread_name, __name, meta.tid);

    meta.thread_name = static_cast<char *>(malloc(sizeof(char) * (THREAD_NAME_LEN + 1)));
    strncpy(meta.thread_name, __name, THREAD_NAME_LEN + 1);

    // 子线程与父线程名不一致时，并且新线程名 match 正则，则需要 unwind
    bool need_unwind = (!unwind_in_parent
                        && 0 != strncmp(meta.parent_name, meta.thread_name, THREAD_NAME_LEN)
                        && test_match_thread_name(meta));

    uint64_t native_hash = 0;
    uint64_t java_hash   = 0;

    if (need_unwind && meta.native_stacktrace.empty()) {
        unwind_native_stacktrace(meta);
        native_hash = hash_stack_frames(meta.native_stacktrace);
        LOGD(TAG, "on_pthread_setname: unwind native");
    }

    pthread_mutex_unlock(&m_pthread_meta_mutex);

    if (need_unwind && !meta.java_stacktrace.load(std::memory_order_acquire)) {
        LOGD(TAG, "on_pthread_setname: unwinding java");
        unwind_java_stacktrace(&meta);

        const char *java_stacktrace = meta.java_stacktrace.load(std::memory_order_acquire);
        if (java_stacktrace) {
            java_hash = hash_str(java_stacktrace);
            LOGD(TAG, "on_pthread_setname: java hash = %lu", java_hash);
        }
    }

    pthread_mutex_lock(&m_pthread_meta_mutex);

    if (native_hash && java_hash) {
        meta.hash = hash_combine(native_hash, java_hash);
    }

    pthread_mutex_unlock(&m_pthread_meta_mutex);

    LOGD(TAG, "--------------------------");
}


void pthread_dump_impl(FILE *__log_file) {
    if (!__log_file) {
        LOGE(TAG, "open file failed");
        return;
    }

    for (auto &i: m_pthread_metas) {
        auto &meta = i.second;
        LOGD(TAG, "========> RETAINED PTHREAD { name : %s, parent: %s, tid: %d }", meta.thread_name,
             meta.parent_name, meta.tid);
        fprintf(__log_file, "========> RETAINED PTHREAD { name : %s, parent: %s, tid: %d }\n",
                meta.thread_name, meta.parent_name, meta.tid);
        std::stringstream stack_builder;

        if (meta.native_stacktrace.empty()) {
            continue;
        }

        LOGD(TAG, "native stacktrace:");
        fprintf(__log_file, "native stacktrace:\n");

        for (auto &p_frame : meta.native_stacktrace) {
            Dl_info stack_info;
            dladdr((void *) p_frame.pc, &stack_info);

            std::string so_name = std::string(stack_info.dli_fname);

            int  status          = 0;
            char *demangled_name = abi::__cxa_demangle(stack_info.dli_sname, nullptr, 0,
                                                       &status);

            LOGD(TAG, "  #pc %"
                    PRIxPTR
                    " %s (%s)", p_frame.rel_pc,
                 demangled_name ? demangled_name : "(null)", stack_info.dli_fname);
            fprintf(__log_file, "  #pc %" PRIxPTR " %s (%s)\n", p_frame.rel_pc,
                    demangled_name ? demangled_name : "(null)", stack_info.dli_fname);

            free(demangled_name);
        }

        LOGD(TAG, "java stacktrace:\n%s", meta.java_stacktrace.load(std::memory_order_acquire));
        fprintf(__log_file, "java stacktrace:\n%s\n",
                meta.java_stacktrace.load(std::memory_order_acquire));
    }
}

void pthread_dump(const char *__path) {
    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump begin <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
    pthread_mutex_lock(&m_pthread_meta_mutex);

    FILE *log_file = fopen(__path, "w+");
    LOGD(TAG, "pthread dump path = %s", __path);

    pthread_dump_impl(log_file);

    fclose(log_file);

    pthread_mutex_unlock(&m_pthread_meta_mutex);

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump end <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
}


char *pthread_dump_json_impl(FILE *__log_file) {

    std::map<uint64_t, std::vector<pthread_meta_t>> pthread_metas_by_hash;

    for (auto &i : m_pthread_metas) {
        auto &meta        = i.second;
        auto &hash_bucket = pthread_metas_by_hash[meta.hash];
        hash_bucket.emplace_back(meta);
    }

    char  *json_str    = NULL;
    cJSON *threads_arr = NULL;

    cJSON *json_obj = cJSON_CreateObject();

    if (!json_obj) {
        goto err;
    }

    threads_arr = cJSON_AddArrayToObject(json_obj, "PthreadHook");

    if (!threads_arr) {
        goto err;
    }

    for (auto &i : pthread_metas_by_hash) {
        auto &hash  = i.first;
        auto &metas = i.second;

        cJSON *hash_obj = cJSON_CreateObject();

        if (!hash_obj) {
            goto err;
        }

        cJSON_AddStringToObject(hash_obj, "hash", std::to_string(hash).c_str());
        assert(!metas.empty());

        std::stringstream stack_builder;
        for (auto         &frame : metas.front().native_stacktrace) {
            Dl_info stack_info;
            dladdr((void *) frame.pc, &stack_info);

            int  status          = 0;
            char *demangled_name = abi::__cxa_demangle(stack_info.dli_sname, nullptr, 0,
                                                       &status);

            stack_builder << "#pc " << std::hex << frame.rel_pc << " "
                          << (demangled_name ? demangled_name : "(null)")
                          << " (" << (stack_info.dli_fname ? stack_info.dli_fname : "(null)")
                          << ");";

            free(demangled_name);
        }
        cJSON_AddStringToObject(hash_obj, "native", stack_builder.str().c_str());

        cJSON_AddStringToObject(hash_obj, "java", metas.front().java_stacktrace);

        cJSON_AddStringToObject(hash_obj, "count", std::to_string(metas.size()).c_str());

        cJSON *same_hash_metas_arr = cJSON_AddArrayToObject(hash_obj, "threads");

        if (!same_hash_metas_arr) {
            goto err;
        }

        for (auto &meta: metas) {
            cJSON *meta_obj = cJSON_CreateObject();

            if (!meta_obj) {
                goto err;
            }

            cJSON_AddStringToObject(meta_obj, "tid", std::to_string(meta.tid).c_str());
            cJSON_AddStringToObject(meta_obj, "name", meta.thread_name);
            cJSON_AddStringToObject(meta_obj, "parent", meta.parent_name);

            cJSON_AddItemToArray(same_hash_metas_arr, meta_obj);
        }

        cJSON_AddItemToArray(threads_arr, hash_obj);

        LOGD(TAG, "%s", cJSON_Print(hash_obj));
    }

    json_str = cJSON_PrintUnformatted(json_obj);

    cJSON_Delete(json_obj);

    fprintf(__log_file, "%s", json_str);

    return json_str;

    err:
    LOGD(TAG, "ERROR: create cJSON object failed");
    cJSON_Delete(json_obj);

    return nullptr;
}

char *pthread_dump_json(const char *__path) {

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump json begin <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
    pthread_mutex_lock(&m_pthread_meta_mutex);

    FILE *log_file = fopen(__path, "w+");
    LOGD(TAG, "pthread dump path = %s", __path);

    pthread_dump_json_impl(log_file);

    fclose(log_file);

    pthread_mutex_unlock(&m_pthread_meta_mutex);

    LOGD(TAG,
         ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pthread dump json end <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");

    char *ret;


    return ret;
}

void pthread_hook_on_dlopen(const char *__file_name) {
    LOGD(TAG, "pthread_hook_on_dlopen");
    unwindstack::update_maps();

    LOGD(TAG, "pthread_hook_on_dlopen end");
}

static void on_pthread_destroy(void *__specific) {
    LOGD(TAG, "on_pthread_destroy++++");
    pthread_mutex_lock(&m_pthread_meta_mutex);

    pthread_t destroying_thread = pthread_self();

    if (!m_pthread_metas.count(destroying_thread)) {
        LOGD(TAG, "on_pthread_destroy: thread not found");
        pthread_mutex_unlock(&m_pthread_meta_mutex);
        return;
    }

    pthread_meta_t &meta = m_pthread_metas.at(destroying_thread);
    LOGD(TAG, "removing thread {%ld, %s, %s, %d}", destroying_thread, meta.thread_name,
         meta.parent_name, meta.tid);

    if (meta.thread_name == meta.parent_name) {
        free(meta.thread_name);
    } else {
        free(meta.thread_name);
        free(meta.parent_name);
    }

    char *java_stacktrace = meta.java_stacktrace.load(std::memory_order_acquire);
    if (java_stacktrace) {
        free(java_stacktrace);
    }

    m_pthread_metas.erase(destroying_thread);
    pthread_mutex_unlock(&m_pthread_meta_mutex);

    LOGD(TAG, "__specific %c", *(char *) __specific);

    free(__specific);

    LOGD(TAG, "on_pthread_destroy end----");
}

static void *pthread_routine_wrapper(void *__arg) {

    auto *specific = (char *) malloc(sizeof(char));
    *specific = 'P';

    pthread_setspecific(m_key, specific);

    auto *args_wrapper = (routine_wrapper_t *) __arg;
    void *ret          = args_wrapper->origin_func(args_wrapper->origin_args);
    free(args_wrapper);

    return ret;
}

DEFINE_HOOK_FUN(int, pthread_create, pthread_t *__pthread_ptr, pthread_attr_t const *__attr,
                void *(*__start_routine)(void *), void *__arg) {
    auto *args_wrapper = (routine_wrapper_t *) malloc(sizeof(routine_wrapper_t));
    args_wrapper->origin_func = __start_routine;
    args_wrapper->origin_args = __arg;

    CALL_ORIGIN_FUNC_RET(int, ret, pthread_create, __pthread_ptr, __attr, pthread_routine_wrapper,
                         args_wrapper);

    on_pthread_create(*__pthread_ptr);

    return ret;
}

DEFINE_HOOK_FUN(int, pthread_setname_np, pthread_t
        __pthread, const char *__name) {
    CALL_ORIGIN_FUNC_RET(int, ret, pthread_setname_np, __pthread, __name);
    on_pthread_setname(__pthread, __name);
    return ret;
}
