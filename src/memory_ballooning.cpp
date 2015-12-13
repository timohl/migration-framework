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
#include <mutex>
#include <condition_variable>
#include <cstdlib>

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
//void wait_for_memory_change(virDomainPtr domain, unsigned long long expected_actual_balloon)
//{
//	Memory_stats current_mem_stats(domain);
//	do {
//		BOOST_LOG_TRIVIAL(trace) << "Check memory status";
//		current_mem_stats.refresh();
//		BOOST_LOG_TRIVIAL(trace) << current_mem_stats.str();
//		auto current_domain_info = get_domain_info(domain);
//		BOOST_LOG_TRIVIAL(trace) << domain_info_memory_to_str(current_domain_info);
//		std::this_thread::sleep_for(std::chrono::milliseconds(10));
//	} while(current_mem_stats.actual_balloon != expected_actual_balloon);
//}

class Balloon_adaption_signal
{
public:
	Balloon_adaption_signal(unsigned long long expected_actual_balloon, virDomainPtr domain);
	void register_current_actual_balloon(unsigned long long actual_balloon);
	void wait();
private:
	unsigned long long expected_actual_balloon;
	bool balloon_is_adapted;
	std::mutex balloon_is_adapted_mutex;
	std::condition_variable balloon_is_adapted_cv;
	unsigned long long last_actual_balloon;
	virDomainPtr domain;
};

Balloon_adaption_signal::Balloon_adaption_signal(unsigned long long expected_actual_balloon, virDomainPtr domain) :
	expected_actual_balloon(expected_actual_balloon),
	balloon_is_adapted(false),
	domain(domain)
{
	Memory_stats stats(domain);
	last_actual_balloon = stats.actual_balloon;
}

void Balloon_adaption_signal::register_current_actual_balloon(unsigned long long actual_balloon)
{
	auto last_rate = (actual_balloon < last_actual_balloon) ? last_actual_balloon - actual_balloon : actual_balloon - last_actual_balloon;
	last_actual_balloon = actual_balloon;
	BOOST_LOG_TRIVIAL(trace) << "Balloon: " << actual_balloon << "/" << expected_actual_balloon << " (" << last_rate << " KiB/s)";
	auto total_diff = (actual_balloon < expected_actual_balloon) ? expected_actual_balloon - actual_balloon : actual_balloon - expected_actual_balloon;
       	if (total_diff < last_rate) {
		if (total_diff != 0) {
			// TODO: Is sleeping in callback so clever?
			BOOST_LOG_TRIVIAL(trace) << "Sleep duration: " << ((total_diff * 1000) / last_rate);
			std::this_thread::sleep_for(std::chrono::milliseconds((total_diff * 1000) / last_rate));
		}
		std::unique_lock<std::mutex> lock(balloon_is_adapted_mutex);
		balloon_is_adapted = true;
		lock.unlock();
		balloon_is_adapted_cv.notify_one();
	}
}

void Balloon_adaption_signal::wait()
{
	// Wait for signal.
	std::unique_lock<std::mutex> lock(balloon_is_adapted_mutex);
	balloon_is_adapted_cv.wait(lock, [this]{return balloon_is_adapted;});
	// Confirm actual balloon and wait actively until it is actually reached.
	auto start = std::chrono::high_resolution_clock::now();
	Memory_stats stats(domain);
	while (stats.actual_balloon != expected_actual_balloon) { // TODO: Limit loop by max duration of 2 seconds
		BOOST_LOG_TRIVIAL(trace) << "Not yet adapted: " << stats.actual_balloon << "/" << expected_actual_balloon;
		auto now = std::chrono::high_resolution_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() == 1)
			break;
		std::this_thread::yield();
		stats.refresh();
	}
}

int balloon_change_callback(virConnectPtr connection, virDomainPtr domain, unsigned long long actual, void *opaque)
{
	(void) connection; (void) domain;
	auto signal = static_cast<Balloon_adaption_signal *>(opaque);
	signal->register_current_actual_balloon(actual);
	return 0;
}

void resize_memory_balloon(virDomainPtr domain, unsigned long long memory)
{
	Balloon_adaption_signal signal(memory, domain);
	auto connection = virDomainGetConnect(domain);

	BOOST_LOG_TRIVIAL(trace) << "Register event callback for balloon change.";
	auto callback_handle = virConnectDomainEventRegisterAny(
			connection,
			domain,
			VIR_DOMAIN_EVENT_ID_BALLOON_CHANGE,
			VIR_DOMAIN_EVENT_CALLBACK(balloon_change_callback),
			static_cast<void *>(&signal), nullptr);

	BOOST_LOG_TRIVIAL(trace) << "Set memory to " << memory << ".";
	if (virDomainSetMemoryFlags(domain, memory, VIR_DOMAIN_AFFECT_LIVE) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB.");

	BOOST_LOG_TRIVIAL(trace) << "Wait for balloon adaption signal.";
	signal.wait();

	BOOST_LOG_TRIVIAL(trace) << "Deregister event callback for balloon change.";
	virConnectDomainEventDeregisterAny(connection, callback_handle);
}

//
// Memory_ballooning_guard implementation
//

Memory_ballooning_guard::Memory_ballooning_guard(virDomainPtr domain, bool enable_memory_ballooning, Time_measurement &time_measurement) :
	domain(domain),
	memory_was_reset(false),
	enable_memory_ballooning(enable_memory_ballooning),
	time_measurement(time_measurement)
{
	if (enable_memory_ballooning) {
		time_measurement.tick("shrink-mem-balloon");
		Memory_stats mem_stats(domain);
		BOOST_LOG_TRIVIAL(trace) << mem_stats.str();
		initial_memory = mem_stats.actual_balloon;
		auto used_memory = mem_stats.actual_balloon - mem_stats.unused;
		BOOST_LOG_TRIVIAL(trace) << "Used memory: " << used_memory;
		// Add 5% of unused memory as buffer.
		auto migration_memory = used_memory + static_cast<unsigned long long>(mem_stats.unused * .05);
		migration_memory = (migration_memory / 4) * 4; // Align to pagesize TODO: get and use system pagesize
		BOOST_LOG_TRIVIAL(trace) << "Memory during migration: " << migration_memory;
		// Make sure memory is really decreased. TODO: Maybe unnecessary?
		if (migration_memory < initial_memory) {
			resize_memory_balloon(domain, migration_memory);
		}
		time_measurement.tock("shrink-mem-balloon");
	}
}	

Memory_ballooning_guard::~Memory_ballooning_guard()
{
	try {
		if (!memory_was_reset)
			reset_memory();
	} catch (...) {
		BOOST_LOG_TRIVIAL(error) << "Error in memory ballooning guards destructor.";
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
		time_measurement.tick("reset-mem-balloon");
		resize_memory_balloon(domain, initial_memory);
		time_measurement.tock("reset-mem-balloon");
	}
}
