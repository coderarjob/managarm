#pragma once

#include <thor-internal/address-space.hpp>

namespace thor {

struct FbInfo {
	uint64_t address;
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
	
	frigg::SharedPtr<MemoryView> memory;
};

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type, void *early_window);
void transitionBootFb();

} // namespace thor
