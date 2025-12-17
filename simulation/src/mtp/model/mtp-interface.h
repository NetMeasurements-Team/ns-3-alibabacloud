/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef MTP_INTERFACE_H
#define MTP_INTERFACE_H

#include "logical-process.h"

#include "ns3/global-value.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"

#include <pthread.h>

namespace ns3
{

class MtpInterface
{
  public:
    class CriticalSection
    {
      public:
        /**
         * @brief Acquires the lock upon construction.
         *
         * Spins in a loop until it successfully sets the atomic flag from 'false'
         * to 'true', effectively waiting until the lock is free and then taking it.
         * The 'acquire' memory order ensures that later memory operations are
         * not moved before this point.
         */
        CriticalSection()
        {
            while (g_inCriticalSection.exchange(true, std::memory_order_acquire))
                ;
        }

        /**
         * @brief Releases the lock upon destruction.
         *
         * The destructor ensures the lock is released when the object goes out of
         * scope (RAII). The 'release' memory order ensures that preceding memory
         * operations are not moved after this point.
         */
        ~CriticalSection()
        {
            ExitSection();
        }

        CriticalSection(const CriticalSection&) = delete;
        CriticalSection& operator=(const CriticalSection&) = delete;
        CriticalSection(CriticalSection&&) = delete;
        CriticalSection& operator=(CriticalSection&&) = delete;

      private:
        /**
         * @brief Releases the lock.
         *
         * This method is private as it should never be called outside the destructor
         * to prevent a thread from releasing the lock twice (possibly releasing it while
         * someone else was holding it).
         */
        static void ExitSection()
        {
            g_inCriticalSection.store(false, std::memory_order_release);
        }
    };

    /**
     * Unlike CriticalSection, this class does not automatically release the
     * lock on destruction. The caller must explicitly call ExitSection() to
     * release the lock once the critical section is complete.
     */
    class ExplicitCriticalSection
    {
      public:
        /**
         * @brief Acquires the lock upon construction.
         *
         * Spins in a loop until it successfully sets the atomic flag from 'false'
         * to 'true', effectively waiting until the lock is free and then taking it.
         * The 'acquire' memory order ensures that later memory operations are
         * not moved before this point.
         */
        ExplicitCriticalSection()
        {
            while (g_inCriticalSection.exchange(true, std::memory_order_acquire))
                ;
        }

        /**
         * @brief Default destructor that does not release the lock.
         *
         * This destructor intentionally does not modify the lock state. This allows
         * the user to control precisely when the lock is released by calling
         * ExitSection() at the appropriate time. Failing to call ExitSection()
         * after entering the critical section results in the lock remaining held.
         */
        ~ExplicitCriticalSection() = default;

        /**
         * @brief Releases the lock explicitly.
         *
         * The user must call this static method to release the lock acquired in the
         * constructor.
         *
         * Calling ExitSection() without having successfully acquired the lock first
         * results in undefined behavior, as it would release a lock that the caller
         * does not hold.
         */
        static void ExitSection()
        {
            g_inCriticalSection.store(false, std::memory_order_release);
        }
    };

    static void Enable();                           // auto topology partition
    static void Enable(const uint32_t threadCount); // auto partition, specify thread count
    static void Enable(const uint32_t threadCount, const uint32_t systemCount); // manual partition
    static void EnableNew(const uint32_t newSystemCount); // add LPs for dynamic added node
    static void Disable();
    static void Run();
    static void RunBefore();
    static void ProcessOneRound();
    static void CalculateSmallestTime();
    static void RunAfter();
    static bool isEnabled();
    static bool isPartitioned();
    static void CalculateLookAhead();

    // get current thread's executing logical process
    inline static LogicalProcess* GetSystem()
    {
        return static_cast<LogicalProcess*>(pthread_getspecific(g_key));
    }

    inline static LogicalProcess* GetSystem(const uint32_t systemId)
    {
        return &g_systems[systemId];
    }

    // set current thread's executing logical process
    inline static void SetSystem(const uint32_t systemId)
    {
        pthread_setspecific(g_key, &g_systems[systemId]);
    }

    inline static uint32_t GetSize()
    {
        return g_systemCount + 1;
    }

    inline static uint32_t GetRound()
    {
        return g_round;
    }

    inline static Time GetSmallestTime()
    {
        return g_smallestTime;
    }

    inline static void SetSmallestTime(const Time smallestTime)
    {
        g_smallestTime = smallestTime;
    }

    inline static Time GetNextPublicTime()
    {
        return g_nextPublicTime;
    }

    inline static bool isFinished()
    {
        return g_globalFinished;
    }

    template <
        typename FUNC,
        typename std::enable_if<!std::is_convertible<FUNC, Ptr<EventImpl>>::value, int>::type,
        typename std::enable_if<!std::is_function<typename std::remove_pointer<FUNC>::type>::value,
                                int>::type,
        typename... Ts>
    inline static void ScheduleGlobal(FUNC f, Ts&&... args)
    {
        CriticalSection cs;
        g_systems[0].ScheduleAt(Simulator::NO_CONTEXT,
                                Min(g_smallestTime, g_nextPublicTime),
                                MakeEvent(f, std::forward<Ts>(args)...));
    }

    template <typename... Us, typename... Ts>
    inline static void ScheduleGlobal(void (*f)(Us...), Ts&&... args)
    {
        CriticalSection cs;
        g_systems[0].ScheduleAt(Simulator::NO_CONTEXT,
                                Min(g_smallestTime, g_nextPublicTime),
                                MakeEvent(f, std::forward<Ts>(args)...));
    }

  private:
    static void* ThreadFunc(void* arg);

    // determine logical process priority
    static bool SortByExecutionTime(const uint32_t& i, const uint32_t& j);
    static bool SortByEventCount(const uint32_t& i, const uint32_t& j);
    static bool SortByPendingEventCount(const uint32_t& i, const uint32_t& j);
    static bool SortBySimulationTime(const uint32_t& i, const uint32_t& j);
    static bool (*g_sortFunc)(const uint32_t&, const uint32_t&);
    static GlobalValue g_sortMethod;
    static GlobalValue g_sortPeriod;
    static uint32_t g_period;

    static pthread_t* g_threads;
    static LogicalProcess* g_systems;
    static uint32_t g_threadCount;
    static uint32_t g_systemCount;

    static uint32_t* g_sortedSystemIndices;
    static std::atomic<uint32_t> g_systemIndex;
    static std::atomic<uint32_t> g_finishedSystemCount;

    static uint32_t g_round;
    static Time g_smallestTime;
    static Time g_nextPublicTime;
    static bool g_recvMsgStage;
    static bool g_globalFinished;
    static bool g_enabled;

    static pthread_key_t g_key;
    static std::atomic<bool> g_inCriticalSection;
};

} // namespace ns3

#endif /* MTP_INTERFACE_H */
