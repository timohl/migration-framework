/*
 * This file is part of migration-framework.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#include "libvirt_hypervisor.hpp"

#include "pci_device_handler.hpp"
#include "memory_ballooning.hpp"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <boost/log/trivial.hpp>

#include <stdexcept>
#include <memory>

// Some deleter to be used with smart pointers.

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

//
// Libvirt_hypervisor implementation
//

/// TODO: Get hostname dynamically.
Libvirt_hypervisor::Libvirt_hypervisor() :
	pci_device_handler(std::make_shared<PCI_device_handler>()),
	run_event_loop(true)
{
	BOOST_LOG_TRIVIAL(trace) << "Initialize libvirt.";
	if (virInitialize() < 0) {
	    throw std::runtime_error("Failed to initialize libvirt");
	}

	BOOST_LOG_TRIVIAL(trace) << "Register event implementation.";
	if (virEventRegisterDefaultImpl() < 0) {
		virErrorPtr err = virGetLastError();
		throw std::runtime_error("Failed to register event implementation: " + 
				std::string(err && err->message ? err->message: "Unknown error"));
	}

	BOOST_LOG_TRIVIAL(trace) << "Connect to hypervisor.";
	if ((local_host_conn = virConnectOpen("qemu:///system")) == nullptr) {
		throw std::runtime_error("Failed to connect to qemu on local host.");
	}

	BOOST_LOG_TRIVIAL(trace) << "Start libvirt event loop.";
	auto &run_event_loop = this->run_event_loop;
	event_loop = std::async(std::launch::async, [&run_event_loop]{
		while (run_event_loop) {
			if (virEventRunDefaultImpl() < 0) {
				virErrorPtr err = virGetLastError();
				BOOST_LOG_TRIVIAL(error) << "Failed to run event loop: ";
				BOOST_LOG_TRIVIAL(error) << (err && err->message ? err->message : "Unknown error") << std::endl;
			}
		}
	});
}

Libvirt_hypervisor::~Libvirt_hypervisor()
{
	run_event_loop = false;
	BOOST_LOG_TRIVIAL(trace) << "Close connection to hypervisor.";
	if (virConnectClose(local_host_conn)) {
		BOOST_LOG_TRIVIAL(warning) << "Warning: Some qemu connections have not been closed after destruction of hypervisor wrapper!";
	}
}

void Libvirt_hypervisor::start(const std::string &vm_name, unsigned int vcpus, unsigned long memory, const std::vector<PCI_id> &pci_ids)
{
	// Get domain by name
	BOOST_LOG_TRIVIAL(trace) << "Get domain by name.";
	std::unique_ptr<virDomain, Deleter_virDomain> domain(
		virDomainLookupByName(local_host_conn, vm_name.c_str())
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	// Get domain info + check if in shutdown state
	BOOST_LOG_TRIVIAL(trace) << "Get domain info + check if in shutdown state.";
	virDomainInfo domain_info;
	if (virDomainGetInfo(domain.get(), &domain_info) == -1)
		throw std::runtime_error("Failed getting domain info.");
	if (domain_info.state != VIR_DOMAIN_SHUTOFF)
		throw std::runtime_error("Wrong domain state: " + std::to_string(domain_info.state));
	// Set memory
	BOOST_LOG_TRIVIAL(trace) << "Set memory.";
	if (virDomainSetMemoryFlags(domain.get(), memory, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_MEM_MAXIMUM) == -1)
		throw std::runtime_error("Error setting maximum amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	if (virDomainSetMemoryFlags(domain.get(), memory, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory) + " KiB for domain " + vm_name);
	// Set VCPUs
	BOOST_LOG_TRIVIAL(trace) << "Set VCPUs.";
	if (virDomainSetVcpusFlags(domain.get(), vcpus, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM) == -1)
		throw std::runtime_error("Error setting maximum number of vcpus to " + std::to_string(vcpus) + " for domain " + vm_name);
	if (virDomainSetVcpusFlags(domain.get(), vcpus, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting number of vcpus to " + std::to_string(vcpus) + " for domain " + vm_name);
	int memory_stats_period = 1;
	if (virDomainSetMemoryStatsPeriod(domain.get(), memory_stats_period, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting memory stats period to " + std::to_string(memory_stats_period) + " for domain " + vm_name);
	// Create domain
	BOOST_LOG_TRIVIAL(trace) << "Create domain.";
	if (virDomainCreate(domain.get()) == -1)
		throw std::runtime_error(std::string("Error creating domain: ") + virGetLastErrorMessage());
	// Attach device
	BOOST_LOG_TRIVIAL(trace) << "Attach " << pci_ids.size() << " devices.";
	for (auto &pci_id : pci_ids) {
		BOOST_LOG_TRIVIAL(trace) << "Attach device with PCI-ID " << pci_id.str();
		pci_device_handler->attach(domain.get(), pci_id);
	}
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
	pci_device_handler->detach(domain.get());
	// Destroy domain
	if (virDomainDestroy(domain.get()) == -1)
		throw std::runtime_error("Error destroying domain.");
}

void Libvirt_hypervisor::migrate(const std::string &vm_name, const std::string &dest_hostname, bool live_migration, bool rdma_migration, bool memory_ballooning)
{
	BOOST_LOG_TRIVIAL(trace) << "Migrate " << vm_name << " to " << dest_hostname << ".";
	BOOST_LOG_TRIVIAL(trace) << std::boolalpha << "live-migration=" << live_migration;
	BOOST_LOG_TRIVIAL(trace) << std::boolalpha << "rdma-migration=" << rdma_migration;

	// Get domain by name
	BOOST_LOG_TRIVIAL(trace) << "Get domain by name.";
	std::unique_ptr<virDomain, Deleter_virDomain> domain(
		virDomainLookupByName(local_host_conn, vm_name.c_str())
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	
	// Get domain info + check if in running state
	BOOST_LOG_TRIVIAL(trace) << "Get domain info and check if in running state.";
	auto domain_info = get_domain_info(domain.get());
	if (domain_info.state != VIR_DOMAIN_RUNNING)
		throw std::runtime_error("Domain not running.");

	// Guard migration of PCI devices.
	BOOST_LOG_TRIVIAL(trace) << "Create guard for device migration.";
	Migrate_devices_guard dev_guard(pci_device_handler, domain.get());

	// Reduce memory (disabled if memory_ballooning==false)
	Memory_ballooning_guard mem_ballooning_guard(domain.get(), memory_ballooning);

	// Connect to destination
	BOOST_LOG_TRIVIAL(trace) << "Connect to destination.";
	std::unique_ptr<virConnect, Deleter_virConnect> dest_connection(
		virConnectOpen(("qemu+ssh://" + dest_hostname + "/system").c_str())
	);
	if (!dest_connection)
		throw std::runtime_error("Cannot establish connection to " + dest_hostname);

	// Set migration flags
	unsigned long flags = 0;
	flags |= live_migration ? VIR_MIGRATE_LIVE : 0;

	// Create migrate uri for rdma migration
	std::string migrate_uri = rdma_migration ? "rdma://" + dest_hostname + "-ib" : "";
	BOOST_LOG_TRIVIAL(trace) << (rdma_migration ? "Use migrate uri: " + migrate_uri + "." : "Use default migrate uri.");

	// Migrate domain
	BOOST_LOG_TRIVIAL(trace) << "Migrate domain.";
	std::unique_ptr<virDomain, Deleter_virDomain> dest_domain(
		virDomainMigrate(domain.get(), dest_connection.get(), flags, 0, rdma_migration ? migrate_uri.c_str() : nullptr, 0)
	);
	if (!dest_domain)
		throw std::runtime_error(std::string("Migration failed: ") + virGetLastErrorMessage());

	// Set destination domain for guards
	BOOST_LOG_TRIVIAL(trace) << "Set destination domain for guards.";
	mem_ballooning_guard.set_destination_domain(dest_domain.get());
	dev_guard.set_destination_domain(dest_domain.get());

	// Reset memory
	BOOST_LOG_TRIVIAL(trace) << "Reset memory on destination.";
	mem_ballooning_guard.reset_memory();
	
	// Reattach devices on destination.
	BOOST_LOG_TRIVIAL(trace) << "Reattach devices on destination.";
	dev_guard.reattach();
}
