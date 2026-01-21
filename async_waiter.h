#pragma once

#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <limits>
#include <memory>
#include <condition_variable>

namespace HSLL
{
	class CAsyncValueBase
	{
	public:
		virtual ~CAsyncValueBase()
		{
			Delete();
		}

		virtual void Delete()
		{
			{
				std::lock_guard<std::mutex> oLock(m_oMutex);

				if (m_bReady || m_bDeleted)
				{
					return;
				}

				m_bDeleted = true;
			}

			m_oCV.notify_all();
		}

		virtual std::chrono::steady_clock::time_point GetExpirationTime() const
		{
			return m_stExpirationTime;
		}

		bool IsReady() const
		{
			std::lock_guard<std::mutex> oLock(m_oMutex);
			return m_bReady;
		}

		bool IsCanceled() const
		{
			std::lock_guard<std::mutex> oLock(m_oMutex);
			return m_bDeleted;
		}

	protected:
		CAsyncValueBase(uint32_t uTimeoutMs)
		{
			m_bReady = false;
			m_bDeleted = false;

			if (uTimeoutMs == std::numeric_limits<uint32_t>::max())
			{
				m_stExpirationTime = std::chrono::steady_clock::time_point::max();
			}
			else
			{
				m_stExpirationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(uTimeoutMs);
			}
		}

	protected:
		bool m_bReady;
		bool m_bDeleted;
		mutable std::mutex m_oMutex;
		std::condition_variable m_oCV;
		std::chrono::steady_clock::time_point m_stExpirationTime;
	};

	template<typename Value>
	class CAsyncValueImpl : public CAsyncValueBase
	{
	public:
		explicit CAsyncValueImpl(uint32_t uTimeoutMs) : CAsyncValueBase(uTimeoutMs)
		{
		}

		virtual ~CAsyncValueImpl()
		{
			std::lock_guard<std::mutex> oLock(m_oMutex);

			if (m_bReady)
			{
				reinterpret_cast<Value*>(m_aStorage)->~Value();
			}
		}

		template<typename Rep, typename Period>
		bool WaitValue(Value& tValue, const std::chrono::duration<Rep, Period>& stTimeout, bool bResetAfterFetch = false)
		{
			std::unique_lock<std::mutex> oLock(m_oMutex);

			if (!m_oCV.wait_for(oLock, stTimeout, [this] { return m_bReady || m_bDeleted; }))
			{
				return false;
			}

			if (m_bDeleted || !m_bReady)
			{
				return false;
			}


			if (bResetAfterFetch)
			{
				m_bReady = false;
				tValue = std::move(*reinterpret_cast<Value*>(m_aStorage));
				reinterpret_cast<Value*>(m_aStorage)->~Value();
			}
			else
			{
				tValue = *reinterpret_cast<Value*>(m_aStorage);
			}

			return true;
		}

		template<typename V>
		bool SetValue(V&& tValue)
		{
			{
				std::lock_guard<std::mutex> oLock(m_oMutex);

				if (m_bDeleted)
				{
					return false;
				}

				if (m_bReady)
				{
					reinterpret_cast<Value*>(m_aStorage)->~Value();
				}
				else
				{
					m_bReady = true;
				}

				new(m_aStorage) Value(std::forward<V>(tValue));
			}

			m_oCV.notify_all();
			return true;
		}

	private:
		alignas(alignof(Value)) uint8_t m_aStorage[sizeof(Value)];
	};

	template<>
	class CAsyncValueImpl<void> : public CAsyncValueBase
	{
	public:
		explicit CAsyncValueImpl(uint32_t uTimeoutMs) : CAsyncValueBase(uTimeoutMs)
		{
		}

		template<typename Rep, typename Period>
		bool Wait(const std::chrono::duration<Rep, Period>& stTimeout, bool bResetAfterFetch = false)
		{
			std::unique_lock<std::mutex> oLock(m_oMutex);

			if (!m_oCV.wait_for(oLock, stTimeout, [this] {return m_bReady || m_bDeleted; }))
			{
				return false;
			}

			if (m_bDeleted)
			{
				return false;
			}

			if (bResetAfterFetch)
			{
				m_bReady = false;
			}

			return true;
		}

		bool Set()
		{
			{
				std::lock_guard<std::mutex> oLock(m_oMutex);

				if (m_bDeleted)
				{
					return false;
				}

				m_bReady = true;
			}

			m_oCV.notify_all();
			return true;
		}
	};

	/**
	 * @class CAsyncWaiter
	 * @brief Manages asynchronous events with associated keys and expiration times.
	 *
	 * This class provides a thread-safe mechanism to create, set, wait for, and remove
	 * asynchronous events identified by unique keys. It supports both void and typed
	 * events, and includes automatic cleanup of expired events through a background
	 * monitoring thread.
	 *
	 * @tparam Key The type used to uniquely identify asynchronous events.
	 */
	template<typename Key>
	class CAsyncWaiter
	{
		constexpr static uint32_t MAX_U32 = std::numeric_limits<uint32_t>::max();

	public:
		/**
		 * @brief Constructs a CAsyncWaiter instance.
		 *
		 * The monitor thread is not automatically started. Call Start() to begin
		 * automatic cleanup of expired events.
		 */
		CAsyncWaiter()
		{
			m_bIsRunning = false;
			m_uCleanupIntervalMs = 50;
		}

		/**
		 * @brief Destructor that stops the monitor thread and cleans up all pending events.
		 */
		~CAsyncWaiter()
		{
			Stop();
		}

		/**
		 * @brief Starts the background cleanup thread.
		 *
		 * The cleanup thread periodically removes events that have exceeded their
		 * expiration time.
		 *
		 * @param uCleanupIntervalMs Interval in milliseconds between cleanup checks.
		 * @return true if the thread was successfully started, false if already running
		 *         or if the interval is zero.
		 */
		bool Start(uint32_t uCleanupIntervalMs = 50)
		{
			if (uCleanupIntervalMs == 0)
			{
				return false;
			}

			std::lock_guard<std::recursive_mutex> oLock(m_oMutex);

			if (m_bIsRunning)
			{
				return false;
			}

			m_bIsRunning = true;
			m_uCleanupIntervalMs = uCleanupIntervalMs;
			m_oMonitorThread = std::thread(&CAsyncWaiter<Key>::CleanupThread, this);
			return true;
		}

		/**
		 * @brief Removes an event associated with the specified key.
		 *
		 * If the event exists, it is marked for deletion and removed from the internal map.
		 *
		 * @param tKey The key identifying the event to remove.
		 * @return true if an event was found and removed, false otherwise.
		 */
		bool Remove(const Key& tKey)
		{
			std::shared_ptr<CAsyncValueBase> pEvent;

			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
				auto oIt = m_mapPendingEvents.find(tKey);

				if (oIt != m_mapPendingEvents.end())
				{
					pEvent = oIt->second;
					m_mapPendingEvents.erase(oIt);
				}
				else
				{
					return false;
				}
			}

			if (pEvent)
			{
				pEvent->Delete();
			}

			return true;
		}

		/**
		 * @brief Creates a typed asynchronous event with the specified key.
		 *
		 * @tparam V The type of value associated with the event.
		 * @param tKey The key identifying the event.
		 * @param bRemoveOld If true, replaces any existing event with the same key.
		 * @param uExpireMs Expiration time in milliseconds for the event.
		 * @return true if the event was created, false if an event already exists
		 *         and bRemoveOld is false.
		 */
		template<typename V>
		bool Create(const Key& tKey, bool bRemoveOld = false, uint32_t uExpireMs = MAX_U32)
		{
			std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
			auto oIt = m_mapPendingEvents.find(tKey);

			if (oIt != m_mapPendingEvents.end())
			{
				if (!bRemoveOld)
				{
					return false;
				}

				m_mapPendingEvents.erase(oIt);
			}

			m_mapPendingEvents[tKey] = std::make_shared<CAsyncValueImpl<std::decay_t<V>>>(uExpireMs);
			return true;
		}

		/**
		 * @brief Creates a void asynchronous event with the specified key.
		 *
		 * @param tKey The key identifying the event.
		 * @param bRemoveOld If true, replaces any existing event with the same key.
		 * @param uExpireMs Expiration time in milliseconds for the event.
		 * @return true if the event was created, false if an event already exists
		 *         and bRemoveOld is false.
		 */
		bool Create(const Key& tKey, bool bRemoveOld = false, uint32_t uExpireMs = MAX_U32)
		{
			std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
			auto oIt = m_mapPendingEvents.find(tKey);

			if (oIt != m_mapPendingEvents.end())
			{
				if (!bRemoveOld)
				{
					return false;
				}

				m_mapPendingEvents.erase(oIt);
			}

			m_mapPendingEvents[tKey] = std::make_shared<CAsyncValueImpl<void>>(uExpireMs);
			return true;
		}

		/**
		 * @brief Sets the value for a typed asynchronous event.
		 *
		 * @tparam V The type of value to set.
		 * @param tKey The key identifying the event.
		 * @param tValue The value to set.
		 * @return true if the value was successfully set, false if the event doesn't
		 *         exist, has been deleted, or is of incompatible type.
		 */
		template<typename V>
		bool SetValue(const Key& tKey, V&& tValue)
		{
			std::shared_ptr<CAsyncValueBase> pEvent;

			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
				auto oIt = m_mapPendingEvents.find(tKey);

				if (oIt == m_mapPendingEvents.end())
				{
					return false;
				}

				pEvent = oIt->second;
			}

			auto pImpl = std::dynamic_pointer_cast<CAsyncValueImpl<std::decay_t<V>>>(pEvent);

			if (!pImpl)
			{
				return false;
			}

			return pImpl->SetValue(std::forward<V>(tValue));
		}

		/**
		 * @brief Sets a void asynchronous event as ready.
		 *
		 * @param tKey The key identifying the event.
		 * @return true if the event was successfully set, false if the event doesn't
		 *         exist, has been deleted, or is not a void event.
		 */
		bool Set(const Key& tKey)
		{
			std::shared_ptr<CAsyncValueBase> pEvent;

			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
				auto oIt = m_mapPendingEvents.find(tKey);

				if (oIt == m_mapPendingEvents.end())
				{
					return false;
				}

				pEvent = oIt->second;
			}

			auto pImpl = std::dynamic_pointer_cast<CAsyncValueImpl<void>>(pEvent);

			if (!pImpl)
			{
				return false;
			}

			return pImpl->Set();
		}

		/**
		 * @brief Waits for a typed event and retrieves its value.
		 *
		 * @tparam V The type of value to retrieve.
		 * @param tKey The key identifying the event.
		 * @param tValue Reference to store the retrieved value.
		 * @param bRemoveAfterFetch If true, removes the event after successfully
		 *                          retrieving the value.
		 * @param uTimeoutMs Maximum time to wait in milliseconds.
		 * @return true if the value was successfully retrieved within the timeout,
		 *         false otherwise.
		 */
		template<typename V>
		bool WaitValue(const Key& tKey, V& tValue, bool bRemoveAfterFetch = true, uint32_t uTimeoutMs = MAX_U32)
		{
			std::shared_ptr<CAsyncValueBase> pEvent;

			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
				auto oIt = m_mapPendingEvents.find(tKey);

				if (oIt == m_mapPendingEvents.end())
				{
					return false;
				}

				pEvent = oIt->second;
			}

			auto pImpl = std::dynamic_pointer_cast<CAsyncValueImpl<std::decay_t<V>>>(pEvent);

			if (!pImpl)
			{
				return false;
			}

			bool bRet = pImpl->WaitValue(tValue, std::chrono::milliseconds(uTimeoutMs), bRemoveAfterFetch);

			if (bRemoveAfterFetch)
			{
				Remove(tKey);
			}

			return bRet;
		}

		/**
		 * @brief Waits for a void event to be set.
		 *
		 * @param tKey The key identifying the event.
		 * @param bRemoveAfterFetch If true, removes the event after it becomes ready.
		 * @param uTimeoutMs Maximum time to wait in milliseconds.
		 * @return true if the event was set within the timeout, false otherwise.
		 */
		bool Wait(const Key& tKey, bool bRemoveAfterFetch = true, uint32_t uTimeoutMs = MAX_U32)
		{
			std::shared_ptr<CAsyncValueBase> pEvent;

			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);
				auto oIt = m_mapPendingEvents.find(tKey);

				if (oIt == m_mapPendingEvents.end())
				{
					return false;
				}

				pEvent = oIt->second;
			}

			auto pImpl = std::dynamic_pointer_cast<CAsyncValueImpl<void>>(pEvent);

			if (!pImpl)
			{
				return false;
			}

			bool bRet = pImpl->Wait(std::chrono::milliseconds(uTimeoutMs), bRemoveAfterFetch);

			if (bRemoveAfterFetch)
			{
				Remove(tKey);
			}

			return bRet;
		}

		/**
		 * @brief Stops the background cleanup thread and cleans up all pending events.
		 *
		 * This function blocks until the monitor thread has completed.
		 */
		void Stop()
		{
			{
				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);

				if (!m_bIsRunning)
				{
					return;
				}

				m_bIsRunning = false;

				for (auto& pair : m_mapPendingEvents)
				{
					pair.second->Delete();
				}

				m_mapPendingEvents.clear();
			}

			if (m_oMonitorThread.joinable())
			{
				m_oMonitorThread.join();
			}
		}

	private:
		/**
		 * @brief Background thread function for cleaning up expired events.
		 *
		 * Periodically checks all pending events and removes those that have
		 * exceeded their expiration time.
		 */
		void CleanupThread()
		{
			while (true)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(m_uCleanupIntervalMs));

				std::lock_guard<std::recursive_mutex> oLock(m_oMutex);

				if (!m_bIsRunning)
				{
					break;
				}

				auto stCurrentTime = std::chrono::steady_clock::now();
				auto oIt = m_mapPendingEvents.begin();
				std::vector<std::shared_ptr<CAsyncValueBase>> vecWaitDelete;

				while (oIt != m_mapPendingEvents.end())
				{
					auto& pEvent = oIt->second;
					auto stExpirationTime = pEvent->GetExpirationTime();

					if (stExpirationTime != std::chrono::steady_clock::time_point::max() && stCurrentTime >= stExpirationTime)
					{
						vecWaitDelete.push_back(pEvent);
						oIt = m_mapPendingEvents.erase(oIt);
					}
					else
					{
						++oIt;
					}
				}
			}
		}

	private:
		std::recursive_mutex m_oMutex; ///< Mutex for thread-safe access to the event map.
		std::thread m_oMonitorThread;  ///< Background thread for cleanup.

		bool m_bIsRunning;             ///< Flag indicating if the monitor thread is running.
		uint32_t m_uCleanupIntervalMs; ///< Cleanup check interval in milliseconds.

		/**
		 * @brief Map storing pending asynchronous events.
		 *
		 * The key identifies the event, and the value is a base pointer to the
		 * event implementation.
		 */
		std::map<Key, std::shared_ptr<CAsyncValueBase>> m_mapPendingEvents;
	};
}