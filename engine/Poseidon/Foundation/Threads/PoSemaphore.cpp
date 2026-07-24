#include <Poseidon/Foundation/Threads/PoSemaphore.hpp>

#ifdef __APPLE__
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <climits>
#endif

namespace Poseidon::Foundation
{
#ifdef __APPLE__
void PoSemaphore::reportInitializationFailure()
{
    if (!initializationFailureReported.test_and_set())
    {
        LOG_ERROR(Core, "PoSemaphore operation ignored because semaphore initialization failed.");
    }
}
#endif

PoSemaphore::PoSemaphore(long init, long maxCount)
{
    LockRegister(lock, "PoSemaphore");
    if (init < 0L)
    {
        init = 0L;
    }
#ifdef _WIN32
    handle = CreateSemaphore(nullptr, init, maxCount, nullptr);
    error = (handle == nullptr);
#elif defined(__APPLE__)
    (void)maxCount; // Kept consistent with the Linux sem_t implementation.
    semaphoreValue = init;
    semaphoreInitialized = false;
    error = (pthread_mutex_init(&semaphoreMutex, nullptr) != 0);
    if (!error)
    {
        error = (pthread_cond_init(&semaphoreCondition, nullptr) != 0);
        if (error)
        {
            pthread_mutex_destroy(&semaphoreMutex);
        }
        else
        {
            semaphoreInitialized = true;
        }
    }
#else
    error = (sem_init(&sem, 0, (unsigned)init) != 0);
    // maxCount is ignored under pthreads
#endif
}

void PoSemaphore::wait()
{
#ifdef _WIN32
    if (handle != nullptr)
    {
        WaitForSingleObject(handle, INFINITE);
    }
#elif defined(__APPLE__)
    if (!semaphoreInitialized)
    {
        reportInitializationFailure();
        return;
    }
    if (pthread_mutex_lock(&semaphoreMutex) != 0)
    {
        return;
    }
    while (semaphoreValue == 0L)
    {
        if (pthread_cond_wait(&semaphoreCondition, &semaphoreMutex) != 0)
        {
            pthread_mutex_unlock(&semaphoreMutex);
            return;
        }
    }
    semaphoreValue--;
    pthread_mutex_unlock(&semaphoreMutex);
#else
    sem_wait(&sem);
#endif
}

bool PoSemaphore::tryWait()
{
#ifdef _WIN32
    if (handle == nullptr)
    {
        return false;
    }
    return (WaitForSingleObject(handle, 0L) == WAIT_OBJECT_0);
#elif defined(__APPLE__)
    if (!semaphoreInitialized)
    {
        reportInitializationFailure();
        return false;
    }
    if (pthread_mutex_lock(&semaphoreMutex) != 0)
    {
        return false;
    }
    const bool available = semaphoreValue > 0L;
    if (available)
    {
        semaphoreValue--;
    }
    pthread_mutex_unlock(&semaphoreMutex);
    return available;
#else
    return (sem_trywait(&sem) == 0);
#endif
}

void PoSemaphore::signal(long count)
{
    if (count < 1L)
    {
        count = 1L;
    }
#ifdef _WIN32
    if (handle == nullptr)
    {
        error = true;
        return;
    }
    // NOTE: ReleaseSemaphore returns nonzero on success — this error test reads inverted.
    error = (ReleaseSemaphore(handle, count, nullptr) != 0);
#elif defined(__APPLE__)
    if (!semaphoreInitialized)
    {
        reportInitializationFailure();
        error = true;
        return;
    }
    if (pthread_mutex_lock(&semaphoreMutex) != 0)
    {
        error = true;
        return;
    }
    error = false;
    while (!error && count > 0L)
    {
        if (semaphoreValue == LONG_MAX)
        {
            error = true;
            break;
        }
        semaphoreValue++;
        error = (pthread_cond_signal(&semaphoreCondition) != 0);
        if (error)
        {
            semaphoreValue--;
        }
        count--;
    }
    pthread_mutex_unlock(&semaphoreMutex);
#else
    error = false;
    while (!error && count > 0L)
    {
        error = (sem_post(&sem) != 0);
        count--;
    }
#endif
}

long PoSemaphore::getValue()
{
#ifdef _WIN32
    error = true; // getValue is not available on Win32
    return 0L;
#elif defined(__APPLE__)
    if (!semaphoreInitialized)
    {
        reportInitializationFailure();
        error = true;
        return 0L;
    }
    if (pthread_mutex_lock(&semaphoreMutex) != 0)
    {
        error = true;
        return 0L;
    }
    const long value = semaphoreValue;
    error = (pthread_mutex_unlock(&semaphoreMutex) != 0);
    return value;
#else
#ifdef __CYGWIN__
    error = true;
    return 0L;
#else
    int val = 0;
    error = (sem_getvalue(&sem, &val) != 0);
    return (long)val;
#endif
#endif
}

PoSemaphore::~PoSemaphore()
{
#ifdef _WIN32
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
#elif defined(__APPLE__)
    if (semaphoreInitialized)
    {
        pthread_cond_destroy(&semaphoreCondition);
        pthread_mutex_destroy(&semaphoreMutex);
        semaphoreInitialized = false;
    }
#else
    sem_destroy(&sem);
#endif
}

} // namespace Poseidon::Foundation
