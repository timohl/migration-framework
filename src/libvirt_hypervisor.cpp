/*
 * This file is part of migration-framework.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#include "libvirt_hypervisor.hpp"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <stdexcept>
#include <thread>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>

struct Deleter_virConnect
{
	void operator()(virConnectPtr ptr) const
	{
		virConnectClose(ptr);
	}
};

struct Deleter_virDomain
{
	void operator()(virDomainPtr ptr) const
	{
		virDomainFree(ptr);
	}
};

const std::string file_name = "devices/ib_pci_82_00_1.xml";

void attach_device(virDomainPtr domain)
{
	// Convert config file to string
	std::ifstream file_stream(file_name);
	std::stringstream string_stream;
	string_stream << file_stream.rdbuf(); // Filestream to stingstream conversion
	auto pci_device_xml = string_stream.str();

	// attach device
	auto ret = virDomainAttachDevice(domain, pci_device_xml.c_str());
	if (ret != 0)
		throw std::runtime_error("Failed attaching device with following xml:\n" + pci_device_xml);
}

void detach_device(virDomainPtr domain)
{
	// Convert config file to string
	std::ifstream file_stream(file_name);
	std::stringstream string_stream;
	string_stream << file_stream.rdbuf(); // Filestream to stingstream conversion
	auto pci_device_xml = string_stream.str();

	// attach device
	auto ret = virDomainDetachDevice(domain, pci_device_xml.c_str());
	if (ret != 0)
		throw std::runtime_error("Failed detaching device with following xml:\n" + pci_device_xml);
}

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

class Memory_ballooning_guard
{
};

/// TODO: Get hostname dynamically.
Libvirt_hypervisor::Libvirt_hypervisor() :
	local_host_conn(virConnectOpen("qemu:///system"))
{
	if (!local_host_conn)
		throw std::runtime_error("Failed to connect to qemu on local host.");
}

Libvirt_hypervisor::~Libvirt_hypervisor()
{
	if (virConnectClose(local_host_conn)) {
		std::cout << "Warning: Some qemu connections have not been closed after destruction of hypervisor wrapper!" << std::endl;
	}
}

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

void Libvirt_hypervisor::start(const std::string &vm_name, unsigned int vcpus, unsigned long memory)
{
	// Get domain by name
	std::unique_ptr<virDomain, Deleter_virDomain> domain(
		virDomainLookupByName(local_host_conn, vm_name.c_str())
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	// Get domain info + check if in shutdown state
	virDomainInfo domain_info;
	if (virDomainGetInfo(domain.get(), &domain_info) == -1)
		throw std::runtime_error("Failed getting domain info.");
	if (domain_info.state != VIR_DOMAIN_SHUTOFF)
		throw std::runtime_error("Wrong domain state: " + std::to_string(domain_info.state));
	// Set memory
	if (virDomainSetMemoryFlags(domain.get(), memory, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_MEM_MAXIMUM) == -1)
		throw std::runtime_error("Error setting maximum amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	if (virDomainSetMemoryFlags(domain.get(), memory, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	// Set VCPUs
	if (virDomainSetVcpusFlags(domain.get(), vcpus, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM) == -1)
		throw std::runtime_error("Error setting maximum number of vcpus to " + std::to_string(vcpus) + " for domain " + vm_name);
	if (virDomainSetVcpusFlags(domain.get(), vcpus, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting number of vcpus to " + std::to_string(vcpus) + " for domain " + vm_name);
	int memory_stats_period = 1;
	if (virDomainSetMemoryStatsPeriod(domain.get(), memory_stats_period, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting memory stats period to " + std::to_string(memory_stats_period) + " for domain " + vm_name);
	// Create domain
	if (virDomainCreate(domain.get()) == -1)
		throw std::runtime_error(std::string("Error creating domain: ") + virGetLastErrorMessage());
	// Attach device
//	attach_device(domain.get());
}

void Libvirt_hypervisor::stop(const std::string &vm_name)
{
	// Get domain by name
	std::unique_ptr<virDomain, Deleter_virDomain> domain(
		virDomainLookupByName(local_host_conn, vm_name.c_str())
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	// Get domain info + check if in running state
	virDomainInfo domain_info;
	if (virDomainGetInfo(domain.get(), &domain_info) == -1)
		throw std::runtime_error("Failed getting domain info.");
	if (domain_info.state != VIR_DOMAIN_RUNNING)
		throw std::runtime_error("Domain not running.");
	// Destroy domain
	if (virDomainDestroy(domain.get()) == -1)
		throw std::runtime_error("Error destroying domain.");
}

void Libvirt_hypervisor::migrate(const std::string &vm_name, const std::string &dest_hostname, bool live_migration, bool rdma_migration)
{
	// Get domain by name
	std::unique_ptr<virDomain, Deleter_virDomain> domain(
		virDomainLookupByName(local_host_conn, vm_name.c_str())
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	// Get domain info + check if in running state
	auto domain_info = get_domain_info(domain.get());
	if (domain_info.state != VIR_DOMAIN_RUNNING)
		throw std::runtime_error("Domain not running.");
	// Detach devices TODO: RAII handler and dynamic device recognition.
//	detach_device(domain.get());
	// Reduce memory
	Memory_stats mem_stats(domain.get());
	auto memory = mem_stats.actual_balloon - mem_stats.unused;
	std::cout << "Used memory: " << memory << std::endl;
	memory += (domain_info.maxMem - memory) > 32 * 1024 ? 16 * 1024 : 0;
	std::cout << "Memory during migration: " << memory << std::endl;
	if (virDomainSetMemoryFlags(domain.get(), memory, VIR_DOMAIN_AFFECT_LIVE) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	wait_for_memory_change(domain.get(), memory);

	// Connect to destination
	std::unique_ptr<virConnect, Deleter_virConnect> dest_connection(
		virConnectOpen(("qemu+ssh://" + dest_hostname + "/system").c_str())
	);
	if (!dest_connection)
		throw std::runtime_error("Cannot establish connection to " + dest_hostname);
	// Set migration flags
	unsigned long flags = 0;
	flags |= live_migration ? VIR_MIGRATE_LIVE : 0;
	// create migrateuri
	auto migrate_uri = rdma_migration ? ("rdma://" + dest_hostname + "-ib").c_str() : nullptr;
	// Migrate domain
	std::unique_ptr<virDomain, Deleter_virDomain> dest_domain(
		virDomainMigrate(domain.get(), dest_connection.get(), flags, 0, migrate_uri, 0)
	);
	if (!dest_domain)
		throw std::runtime_error(std::string("Migration failed: ") + virGetLastErrorMessage());
	// Reset memory
	if (virDomainSetMemoryFlags(dest_domain.get(), domain_info.maxMem, VIR_DOMAIN_AFFECT_LIVE) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	wait_for_memory_change(dest_domain.get(), domain_info.maxMem);

	// Attach device
//	attach_device(dest_domain.get());
}
