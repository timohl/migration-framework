/*
 * This file is part of migration-framework.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#ifndef MEMORY_BALLOONING_HPP
#define MEMORY_BALLOONING_HPP

#include <libvirt/libvirt.h>

#include <string>

// Some helper for domain info
virDomainInfo get_domain_info(virDomainPtr domain);
std::string domain_info_memory_to_str(const virDomainInfo &domain_info);

// Struct for holding memory stats of a domain.
struct Memory_stats
{
	Memory_stats(virDomainPtr domain);

	unsigned long long unused = 0;
	unsigned long long available = 0;
	unsigned long long actual_balloon = 0;
	virDomainPtr domain = nullptr;

	std::string str() const;
	void refresh();
};

// Again some helper
void wait_for_memory_change(virDomainPtr domain, unsigned long long expected_actual_balloon);

// RAII-guard to reduce memory in constructor and reset in destructor or explicit.
// If no error occures during migration the domain on destination should be set.
class Memory_ballooning_guard
{
public:
	Memory_ballooning_guard(virDomainPtr domain, bool enable_memory_ballooning);
	~Memory_ballooning_guard();
	void set_destination_domain(virDomainPtr dest_domain);
	void reset_memory();
private:
	
	virDomainPtr domain;
	unsigned long long initial_memory;
	bool memory_was_reset;
	bool enable_memory_ballooning;
};

#endif
