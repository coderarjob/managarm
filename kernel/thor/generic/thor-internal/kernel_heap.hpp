#pragma once

#include <frg/slab.hpp>
#include <frigg/atomic.hpp>
#include <frigg/initializer.hpp>
#include <physical-buddy.hpp>
#include <thor-internal/arch/stack.hpp>

namespace thor {

struct IrqSpinlock {
	constexpr IrqSpinlock() = default;

	void lock();
	void unlock();

private:
	frigg::TicketLock _spinlock;
};

struct KernelVirtualMemory {
	using Mutex = frigg::TicketLock;
public:
	static KernelVirtualMemory &global();

	// TODO: make this private
	KernelVirtualMemory();

	KernelVirtualMemory(const KernelVirtualMemory &other) = delete;
	
	KernelVirtualMemory &operator= (const KernelVirtualMemory &other) = delete;

	void *allocate(size_t length);
	void deallocate(void *pointer, size_t length);

private:
	Mutex mutex_;
	BuddyAccessor buddy_;
};

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);

	bool enable_trace() {
#ifdef KERNEL_LOG_ALLOCATIONS
		return true;
#else
		return false;
#endif // KERNEL_LOG_ALLOCATIONS
	}

	template <typename F>
	void walk_stack(F functor) {
		walkThisStack(functor);
	}

	void unpoison(void *pointer, size_t size);
	void unpoison_expand(void *pointer, size_t size);
	void poison(void *pointer, size_t size);

	void output_trace(uint8_t val);
};

using KernelAlloc = frg::slab_allocator<KernelVirtualAlloc, IrqSpinlock>;

extern frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;

extern frigg::LazyInitializer<
	frg::slab_pool<
		KernelVirtualAlloc,
		IrqSpinlock
	>
> kernelHeap;

extern frigg::LazyInitializer<KernelAlloc> kernelAlloc;

struct Allocator {
	void *allocate(size_t size) {
		return kernelAlloc->allocate(size);
	}

	void deallocate(void *p, size_t size) {
		kernelAlloc->deallocate(p, size);
	}
};

} // namespace thor
