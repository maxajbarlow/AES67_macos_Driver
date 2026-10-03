#include "AudioThreadPriority.h"
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <errno.h>
#include <cstring>
#include <cstdio>

namespace AES67 {

bool AudioThreadPriority::configureForRealTime() {
    return configureThreadForRealTime(pthread_self());
}

bool AudioThreadPriority::configureThreadForRealTime(pthread_t thread) {
    // On macOS, use mach thread policies for real-time audio
    thread_extended_policy_data_t extendedPolicy;
    thread_precedence_policy_data_t precedencePolicy;
    thread_affinity_policy_data_t affinityPolicy;

    // Set extended policy for real-time constraints
    extendedPolicy.timeshare = FALSE; // Don't timeshare - run at real-time priority

    kern_return_t result = thread_policy_set(
        pthread_mach_thread_np(thread),
        THREAD_EXTENDED_POLICY,
        (thread_policy_t)&extendedPolicy,
        THREAD_EXTENDED_POLICY_COUNT
    );

    if (result != KERN_SUCCESS) {
        fprintf(stderr, "AES67 AudioThreadPriority: THREAD_EXTENDED_POLICY failed (kern_return=%d: %s), falling back to nice -20\n",
                result, mach_error_string(result));
        // Fallback: try to set nice value
        setpriority(PRIO_PROCESS, 0, -20);
        return false;
    }

    // Set precedence policy for priority
    precedencePolicy.importance = 63; // High priority value (0-63)

    result = thread_policy_set(
        pthread_mach_thread_np(thread),
        THREAD_PRECEDENCE_POLICY,
        (thread_policy_t)&precedencePolicy,
        THREAD_PRECEDENCE_POLICY_COUNT
    );

    if (result != KERN_SUCCESS) {
        fprintf(stderr, "AES67 AudioThreadPriority: THREAD_PRECEDENCE_POLICY failed (kern_return=%d: %s)\n",
                result, mach_error_string(result));
        return false;
    }

    // Affinity is only a hint, and Apple Silicon does not support it at all
    // (KERN_NOT_SUPPORTED), so it never decides success: the extended and
    // precedence policies above are what make the thread real-time
    affinityPolicy.affinity_tag = 0; // Use default affinity

    result = thread_policy_set(
        pthread_mach_thread_np(thread),
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&affinityPolicy,
        THREAD_AFFINITY_POLICY_COUNT
    );

    if (result != KERN_SUCCESS && result != KERN_NOT_SUPPORTED) {
        fprintf(stderr, "AES67 AudioThreadPriority: THREAD_AFFINITY_POLICY failed (kern_return=%d: %s) - non-critical\n",
                result, mach_error_string(result));
    }

    return true;
}

void AudioThreadPriority::restoreNormalPriority() {
    // Reset to normal scheduling
    thread_extended_policy_data_t extendedPolicy;
    extendedPolicy.timeshare = TRUE; // Timeshare - normal scheduling
    
    thread_policy_set(
        mach_thread_self(),
        THREAD_EXTENDED_POLICY,
        (thread_policy_t)&extendedPolicy,
        THREAD_EXTENDED_POLICY_COUNT
    );
    
    // Restore normal nice value
    setpriority(PRIO_PROCESS, 0, 0);
}

int AudioThreadPriority::getRecommendedSchedulingPolicy() {
    // On macOS, we use Mach thread policies instead of POSIX scheduling
    return SCHED_FIFO;  // This is for reference; actual implementation uses Mach
}

int AudioThreadPriority::getRecommendedPriority() {
    // High priority for audio processing
    return 63;  // Max priority for real-time audio on macOS
}

} // namespace AES67