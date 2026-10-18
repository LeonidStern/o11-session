// o11proto - the scheduling of a thread (realtime.h).
#include "o11proto/realtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/task_policy.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace o11proto {
namespace thread {

bool makeAudioThread(double period_seconds) {
#if defined(__APPLE__)
    if (!(period_seconds > 0)) return false;
    mach_timebase_info_data_t tb{};
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0) return false;
    auto ticks = [&](double s) {
        return static_cast<std::uint32_t>(s * 1e9 * static_cast<double>(tb.denom) / static_cast<double>(tb.numer));
    };
    const double period = std::min(period_seconds, 0.1);
    thread_time_constraint_policy_data_t p{};
    p.period = ticks(period);
    p.computation = ticks(std::max(0.0002, std::min(0.5 * period, 0.005)));
    p.constraint = ticks(period);
    p.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#elif defined(__linux__)
    (void)period_seconds;                      // the least timer slack: its waits end on time
    return prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) == 0;
#else
    (void)period_seconds;
    return false;
#endif
}

bool makeLatencyCritical() {
#if defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#elif defined(__linux__)
    return prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL) == 0;
#else
    return false;
#endif
}

namespace {

#if defined(__APPLE__)
const char* qosName(qos_class_t q) {
    switch (q) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    case QOS_CLASS_UNSPECIFIED: return "unspecified";
    default: return "other";
    }
}
#endif

// The median and the longest of `samples` waits of `ms`, in ms.
void waits(int ms, int samples, double* median, double* longest) {
    std::vector<double> took;
    for (int i = 0; i < samples; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        took.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(took.begin(), took.end());
    *median = took.empty() ? 0 : took[took.size() / 2];
    *longest = took.empty() ? 0 : took.back();
}

}  // namespace

std::string describe(int samples) {
    char buf[512];
    std::string s;
#if defined(__APPLE__)
    const qos_class_t q = qos_class_self();
    thread_time_constraint_policy_data_t p{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t get_default = FALSE;
    const kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&p), &count, &get_default);
    const bool rt = kr == KERN_SUCCESS && !get_default;
    task_category_policy_data_t cat{};
    mach_msg_type_number_t ccount = TASK_CATEGORY_POLICY_COUNT;
    boolean_t cdefault = FALSE;
    const kern_return_t ck = task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                                             reinterpret_cast<task_policy_t>(&cat), &ccount, &cdefault);
    int bg_process = -1, bg_thread = -1;
#if defined(PRIO_DARWIN_PROCESS)
    bg_process = getpriority(PRIO_DARWIN_PROCESS, getpid());
#endif
#if defined(PRIO_DARWIN_THREAD)
    bg_thread = getpriority(PRIO_DARWIN_THREAD, 0);
#endif
    std::snprintf(buf, sizeof(buf), "QoS %s, %s, task role %d%s, darwin background: process %d, thread %d; ",
                  qosName(q), rt ? "time-constraint" : "not time-constraint", ck == KERN_SUCCESS ? cat.role : -1,
                  ck == KERN_SUCCESS && cdefault ? " (default)" : "", bg_process, bg_thread);
    s += buf;
#elif defined(__linux__)
    const int slack = prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
    std::snprintf(buf, sizeof(buf), "timer slack %d ns; ", slack);
    s += buf;
#endif
    s += "waits of";
    for (int ms : {1, 2, 5}) {
        double median = 0, longest = 0;
        waits(ms, samples, &median, &longest);
        std::snprintf(buf, sizeof(buf), "%s %d ms took %.2f ms (the longest %.2f)", ms == 1 ? "" : ",", ms, median,
                      longest);
        s += buf;
    }
    return s;
}

}  // namespace thread
}  // namespace o11proto
