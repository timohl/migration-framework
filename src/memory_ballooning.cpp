/*
 * This file is part of migration-framework.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#include "memory_ballooning.hpp"

#include <boost/log/trivial.hpp>

#include <stdexcept>
#include <iostream>
#include <thread>

//
// Domain info helper implementation
//

virDomainInfo get_domain_info(virDomainPtr domain)
{
	virDomainInfo domain_info;
	if (virDomainGetInfo(domain, &domain_info) == -1)
		throw std::runtime_error("Failed getting domain info.");
	return domain_info;
}

std::string domain_info_memory_to_str(const virDomainInfo &domain_info)
{
	return "memory: " + std::to_string(domain_info.memory) + ", maxMem: " + std::to_string(domain_info.maxMem);
}

//
// Memory_stats implementation
//

Memory_stats::Memory_stats(virDomainPtr domain) :
	domain(domain)
{
	refresh();
}

std::string Memory_stats::str() const
{
	return "Unused: " + std::to_string(unused) + ", available: " + std::to_string(available) + ", actual: " + std::to_string(actual_balloon);
}

void Memory_stats::refresh()
{
	virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
	int statcnt;
	if ((statcnt = virDomainMemoryStats(domain, mem_stats, VIR_DOMAIN_MEMORY_STAT_RSS, 0)) == -1)
		throw std::runtime_error("Error getting memory stats");
	for (int i = 0; i != statcnt; ++i) {
		if (mem_stats[i].tag == VIR_DOMAIN_MEMORY_STAT_UNUSED)
			unused = mem_stats[i].val;
		if (mem_stats[i].tag == VIR_DOMAIN_MEMORY_STAT_AVAILABLE)
			available = mem_stats[i].val;
		if (mem_stats[i].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON)
			actual_balloon = mem_stats[i].val;
	}
}

//
// Wait for memory change helper
//

// TODO: Use event callback.
void wait_for_memory_change(virDomainPtr domain, unsigned long long expected_actual_balloon)
{
	Memory_stats current_mem_stats(domain);
	do {
		std::cout << "Check memory status" << std::endl;
		current_mem_stats.refresh();
		std::cout << current_mem_stats.str() << std::endl;
		auto current_domain_info = get_domain_info(domain);
		std::cout << domain_info_memory_to_str(current_domain_info) << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	} while(current_mem_stats.actual_balloon != expected_actual_balloon);
}

//
// Memory_ballooning_guard implementation
//

Memory_ballooning_guard::Memory_ballooning_guard(virDomainPtr domain, bool enable_memory_ballooning) :
	domain(domain),
	memory_was_reset(false),
	enable_memory_ballooning(enable_memory_ballooning)
{
	if (enable_memory_ballooning) {
		// maxMem == actual_balloon???
		initial_memory = get_domain_info(domain).maxMem;
		Memory_stats mem_stats(domain);
		auto memory = mem_stats.actual_balloon - mem_stats.unused;
		std::cout << "Used memory: " << memory << std::endl;
		memory += (initial_memory - memory) > 32 * 1024 ? 16 * 1024 : 0;
		std::cout << "Memory during migration: " << memory << std::endl;
		
		if (virDomainSetMemoryFlags(domain, memory, VIR_DOMAIN_AFFECT_LIVE) == -1)
			throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB.");
		
		wait_for_memory_change(domain, memory);
	}
}	

Memory_ballooning_guard::~Memory_ballooning_guard()
{
	try {
		if (!memory_was_reset)
			reset_memory();
	} catch (...) {
		std::cout << "Error in memory ballooning guards destructor." << std::endl;
	}
}

void Memory_ballooning_guard::set_destination_domain(virDomainPtr dest_domain)
{
	domain = dest_domain;
}

void Memory_ballooning_guard::reset_memory()
{
	memory_was_reset = true; // Used so that destructor does not reset memory again.
	if (enable_memory_ballooning) {
		if (virDomainSetMemoryFlags(domain, initial_memory, VIR_DOMAIN_AFFECT_LIVE) == -1)
			throw std::runtime_error("Error setting amount of memory to " + std::to_string(initial_memory) + " KiB.");
	
		wait_for_memory_change(domain, initial_memory);
	}
}
