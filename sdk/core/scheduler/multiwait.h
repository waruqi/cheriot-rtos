// Copyright Microsoft and CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once
#include "thread.h"
#include <errno.h>
#include <multiwaiter.h>
#include <riscvreg.h>
#include <stdint.h>
#include <type_traits>

namespace sched
{
	using namespace CHERI;
	class Queue;
	class Event;

	/**
	 * Structure describing state for waiting for a single event source.
	 *
	 * This is roughly analogous to a knote in kqueue: the structure that holds
	 * state related to a specific event trigger.
	 */
	struct EventWaiter
	{
		/**
		 * The object (queue, event channel, or `uint32_t` address for futexes)
		 * that is monitored by this event waiter.
		 */
		void *eventSource = nullptr;
		/**
		 * Event-type value.
		 */
		uint32_t eventValue = 0;
		/**
		 * The kind of event source.
		 */
		EventWaiterKind kind : 2;
		/**
		 * Event-type-specific flags.
		 */
		unsigned flags : 6 = 0;
		/**
		 * Value indicating the events that have occurred.  The zero value is
		 * reserved to indicate that this event has not been triggered,
		 * subclasses are responsible for defining interpretations of others.
		 */
		unsigned readyEvents : 24 = 0;
		/**
		 * Set some of the bits in the readyEvents field.  Any bits set in
		 * `value` will be set, in addition to any that are already set.
		 */
		void set_ready(unsigned value)
		{
			Debug::Invariant((value & 0xFF000000) == 0,
			                 "{} is out of range for a delivered event",
			                 value);
			readyEvents |= value;
		}
		/**
		 * Returns true if this event has fired, false otherwise.
		 */
		bool has_fired()
		{
			return readyEvents != 0;
		}

		/**
		 * Reset methods.  Each overload takes a pointer to the event source
		 * and the user-provided word describing when it should fire.
		 */
		///@{
		bool reset(Queue *queue, uint32_t conditions);
		/*
		 * Event channel reset depends on event->bits_get(), so the definition
		 * of reset() is in event.h.
		 */
		bool reset(Event *event, uint32_t bits);
		bool reset(uint32_t *address, uint32_t value)
		{
			eventSource = reinterpret_cast<void *>(
			  static_cast<uintptr_t>(Capability{address}.address()));
			eventValue  = value;
			flags       = 0;
			kind        = EventWaiterFutex;
			readyEvents = 0;
			if (*address != value)
			{
				set_ready(1);
				return true;
			}
			return false;
		}
		///@}

		/**
		 * Each of the trigger methods is called when an event source is
		 * triggered.  They return true if this event has fired (and so the
		 * corresponding thread should be woken), false otherwise.
		 *
		 * Each trigger method does nothing if the argument does not match the
		 * registered event type.
		 */
		///@{
		bool trigger(Queue *queue);
		bool trigger(Event *event, uint32_t info);

		bool trigger(uint32_t *address)
		{
			ptraddr_t sourceAddress    = Capability{eventSource}.address();
			ptraddr_t triggeredAddress = Capability{address}.address();
			if ((kind != EventWaiterFutex) ||
			    (sourceAddress != triggeredAddress))
			{
				return false;
			}
			set_ready(1);
			return true;
		}
		///@}
	};

	static_assert(
	  sizeof(EventWaiter) == (2 * sizeof(void *)),
	  "Each waited event should consume only two pointers worth of memory");

	/**
	 * Multiwaiter object.  This contains space for all of the triggers.
	 */
	class MultiWaiter : public Handle
	{
		/**
		 * Helper template for mapping from a type to the EventWaiterKind that
		 * it corresponds to.  The base case is a different type to trigger
		 * compile failures if it is used.
		 *@{
		 */
		template<typename T>
		static constexpr std::nullptr_t KindFor = nullptr;
		template<>
		static constexpr EventWaiterKind KindFor<Queue> = EventWaiterQueue;
		template<>
		static constexpr EventWaiterKind KindFor<Event> =
		  EventWaiterEventChannel;
		template<>
		static constexpr EventWaiterKind KindFor<uint32_t> = EventWaiterFutex;
		///@}

		public:
		/**
		 * Type marker used for `Handle::unseal_as`.
		 */
		static constexpr auto TypeMarker = Handle::Type::Queue;

		private:
		/**
		 * We place a limit on the number of waiters in an event queue to
		 * bound the time spent traversing them.
		 */
		static constexpr size_t MaxMultiWaiterSize = 8;

		/**
		 * The maximum number of events in this multiwaiter.
		 */
		const uint8_t Length;
		/**
		 * The current number of events in this multiwaiter.
		 */
		uint8_t usedLength = 0;

		/**
		 * Bitmap of `1<<EventWaiterKind` values indicating the kinds of object
		 * that this waiter contains.
		 */
		uint8_t containedKinds = 0;

		/**
		 * Multiwaiters are added to a list in between being triggered
		 */
		MultiWaiter *next = nullptr;

		/**
		 * The array of events that we're waiting for.  This is variable sized
		 * and must be the last field of the structure.
		 */
		EventWaiter events[];

		public:
		/**
		 * Returns an iterator to the event waiters that this multiwaiter
		 * contains.
		 */
		EventWaiter *begin()
		{
			return events;
		}

		/**
		 * Returns an end iterator to the event waiters that this multiwaiter
		 * contains.
		 */
		EventWaiter *end()
		{
			return events + usedLength;
		}

		/**
		 * Returns the maximum number of event waiters that this is permitted
		 * to hold.
		 */
		size_t capacity()
		{
			return Length;
		}

		/**
		 * Returns the number of event waiters that this holds.
		 */
		size_t size()
		{
			return usedLength;
		}

		/**
		 * Factory method.  Creates a multiwaiter of the specified size.  On
		 * failure, sets `error` to the errno constant corresponding to the
		 * failure reason and return `nullptr`.
		 */
		static std::unique_ptr<MultiWaiter> create(size_t length, int &error)
		{
			static_assert(sizeof(MultiWaiter) <= 2 * sizeof(void *),
			              "Header for event queue is too large");
			if (length > MaxMultiWaiterSize)
			{
				error = -EINVAL;
				return nullptr;
			}
			Timeout t{0};
			void   *q = heap_allocate(
			    sizeof(MultiWaiter) + (length * sizeof(EventWaiter)), &t);
			if (q == nullptr)
			{
				error = -ENOMEM;
				return nullptr;
			}
			error = 0;
			return std::unique_ptr<MultiWaiter>{new (q) MultiWaiter(length)};
		}

		/**
		 * Tri-state return from `set_events`.
		 */
		enum class EventOperationResult
		{
			/// Failure, report an error.
			Error,
			/// Success and an event fired already.
			Wake,
			/// Success but no events fired, sleep until one does.
			Sleep
		};

		/**
		 * Set the events provided by the user.  The caller is responsible for
		 * ensuring that `newEvents` is a valid and usable capability and that
		 * `count` is within the capacity of this object.
		 */
		EventOperationResult set_events(EventWaiterSource *newEvents,
		                                size_t             count)
		{
			// Has any event triggered yet?
			bool eventTriggered = false;
			// Reset the kinds of event source that this contains.
			containedKinds = 0;
			for (size_t i = 0; i < count; i++)
			{
				void *ptr = newEvents[i].eventSource;
				switch (newEvents[i].kind)
				{
					default:
						return EventOperationResult::Error;
					case EventWaiterQueue:
					{
						auto *queue = Handle::unseal<Queue>(ptr);
						if (queue == nullptr)
						{
							return EventOperationResult::Error;
						}
						eventTriggered |=
						  events[i].reset(queue, newEvents[i].value);
						break;
					}
					case EventWaiterEventChannel:
					{
						auto *event = Handle::unseal<Event>(ptr);
						if (event == nullptr ||
						    (newEvents[i].value & 0xffffff) == 0)
						{
							return EventOperationResult::Error;
						}
						eventTriggered |=
						  events[i].reset(event, newEvents[i].value);
						break;
					}
					case EventWaiterFutex:
					{
						auto *address = static_cast<uint32_t *>(ptr);
						if (!check_pointer<PermissionSet{Permission::Load}>(
						      address))
						{
							return EventOperationResult::Error;
						}
						eventTriggered |=
						  events[i].reset(address, newEvents[i].value);
						break;
					}
				}
				// If we successfully registered this event, we have at least
				// one event of this kind.
				containedKinds |= 1 << newEvents[i].kind;
			}
			usedLength = count;
			return eventTriggered ? EventOperationResult::Wake
			                      : EventOperationResult::Sleep;
		}

		/**
		 * Destructor, ensures that nothing is waiting on this.
		 */
		~MultiWaiter()
		{
			// Remove from the pending-wake list
			remove_from_pending_wake_list();
			// If this is on any threads that it's waiting on.
			Thread::walk_thread_list(threads, [&](Thread *thread) {
				if (thread->multiWaiter == this)
				{
					thread->multiWaiter = nullptr;
					thread->ready(Thread::WakeReason::Timer);
				}
			});
		}

		/**
		 * Function to handle the end of a multi-wait operation.  This collects
		 * all of the results from each of the events and propagates them to
		 * the query list.  The caller is responsible for ensuring that
		 * `newEvents` is valid.
		 */
		bool get_results(EventWaiterSource *newEvents, size_t count)
		{
			// Remove ourself from the list of waiters.
			remove_from_pending_wake_list();
			// Collect all events that have fired.
			Debug::Assert(
			  count <= Length, "Invalid length {} > {}", count, Length);
			bool found = false;
			for (size_t i = 0; i < count; i++)
			{
				newEvents[i].value = events[i].readyEvents;
				found |= (events[i].readyEvents != 0);
			}
			return found;
		}

		/**
		 * Helper that should be called whenever an event of type `T` is ready.
		 * This will always notify any waiters that have already been woken but
		 * have not yet returned.  The `maxWakes` parameter can be used to
		 * restrict the number of threads that are woken as a result of this
		 * call.
		 */
		template<typename T>
		static uint32_t
		wake_waiters(T       *source,
		             uint32_t info     = 0,
		             uint32_t maxWakes = std::numeric_limits<uint32_t>::max())
		{
			// Trigger any multiwaiters whose threads have been woken but which
			// have not yet been scheduled.
			for (auto *mw = wokenMultiwaiters; mw != nullptr; mw = mw->next)
			{
				if constexpr (std::is_same_v<T, Event>)
				{
					mw->trigger(source, info);
				}
				else
				{
					mw->trigger(source);
				}
			}
			// Look at any threads that are waiting on multiwaiters.  This
			// should happen after waking the multiwaiters so that we don't
			// visit multiwaiters twice
			uint32_t woken = 0;
			Thread::walk_thread_list(
			  threads,
			  [&](Thread *thread) {
				  bool threadReady;
				  if constexpr (std::is_same_v<T, Event>)
				  {
					  threadReady = thread->multiWaiter->trigger(source, info);
				  }
				  else
				  {
					  threadReady = thread->multiWaiter->trigger(source);
				  }
				  if (threadReady)
				  {
					  thread->ready(Thread::WakeReason::MultiWaiter);
					  woken++;
					  thread->multiWaiter->next = wokenMultiwaiters;
					  wokenMultiwaiters         = thread->multiWaiter;
				  }
			  },
			  [&]() { return woken >= maxWakes; });
			return woken;
		}

		/**
		 * Wait on this multi-waiter object until either the timeout expires or
		 * one or more events have fired.
		 */
		void wait(Timeout *timeout)
		{
			Thread *currentThread      = Thread::current_get();
			currentThread->multiWaiter = this;
			currentThread->suspend(timeout, &sched::MultiWaiter::threads);
			currentThread->multiWaiter = nullptr;
		}

		private:
		/**
		 * Helper to remove this object from the list maintained for
		 * multiwaiters that have been triggered but whose threads have not yet
		 * been scheduled.
		 */
		void remove_from_pending_wake_list()
		{
			MultiWaiter **prev = &wokenMultiwaiters;
			while ((prev != nullptr) && (*prev != nullptr) && ((*prev) != this))
			{
				prev = &((*prev)->next);
			}
			if (prev != nullptr)
			{
				*prev = next;
			}
			next = nullptr;
		}
		/**
		 * Deliver an event from the source to all possible waiting events in
		 * this set.  This returns true if any of the event sources matches
		 * this multiwaiter and the thread should be awoken.
		 */
		template<typename T>
		bool trigger(T *source, uint32_t info = 0)
		{
			// If we're not waiting on any of this kind of thing, skip scanning
			// the list.
			if ((containedKinds & (1 << KindFor<T>)) == 0)
			{
				return false;
			}
			bool shouldWake = false;
			for (auto &registeredSource : *this)
			{
				if constexpr (std::is_same_v<T, Event>)
				{
					shouldWake |= registeredSource.trigger(source, info);
				}
				else
				{
					shouldWake |= registeredSource.trigger(source);
				}
			}
			return shouldWake;
		}

		/**
		 * Private constructor, called only from the factory method (`create`).
		 */
		MultiWaiter(size_t length) : Handle(TypeMarker), Length(length) {}

		/**
		 * Priority-sorted wait queue for threads that are blocked on a
		 * multiwaiter.
		 */
		static inline Thread *threads;

		/**
		 * List of multiwaiters whose threads have been woken but not yet run.
		 */
		static inline MultiWaiter *wokenMultiwaiters = nullptr;
	};

} // namespace sched

#include "event.h"
#include "queue.h"
