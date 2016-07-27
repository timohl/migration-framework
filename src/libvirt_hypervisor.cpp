/*
 * This file is part of migration-framework.
 * Copyright (C) 2015 RWTH Aachen University - ACS
 *
 * This file is licensed under the GNU Lesser General Public License Version 3
 * Version 3, 29 June 2007. For details see 'LICENSE.md' in the root directory.
 */

#include "libvirt_hypervisor.hpp"

#include "pci_device_handler.hpp"
#include "libvirt_utility.hpp"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <fast-lib/log.hpp>
#include <libssh/libsshpp.hpp>

#include <stdexcept>
#include <memory>
#include <thread>
#include <chrono>

using namespace fast::msg::migfra;

FASTLIB_LOG_INIT(libvirt_hyp_log, "Libvirt_hypervisor")
FASTLIB_LOG_SET_LEVEL_GLOBAL(libvirt_hyp_log, trace);

//
// Helper functions
//

void probe_ssh_connection(virDomainPtr domain)
{
	auto host = virDomainGetName(domain);
	ssh::Session session;
	session.setOption(SSH_OPTIONS_HOST, host);
	bool success = false;
	do {
		try {
			FASTLIB_LOG(libvirt_hyp_log, trace) << "Try to connect to domain with SSH.";
			session.connect();
			FASTLIB_LOG(libvirt_hyp_log, trace) << "Domain is ready.";
			success = true;
		} catch (ssh::SshException &e) {
			FASTLIB_LOG(libvirt_hyp_log, debug) << "Exception while connecting with SSH: " << e.getError();
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		session.disconnect();
	} while (!success);
}

std::shared_ptr<virConnect> connect(const std::string &host, const std::string &driver, const std::string transport = "")
{
	std::string plus_transport = (transport != "") ? ("+" + transport) : "";
	std::string uri = driver + plus_transport + "://" + host + "/system";
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Connect to " + uri;
	std::shared_ptr<virConnect> conn(
			virConnectOpen(uri.c_str()),
			Deleter_virConnect()
	);
	if (!conn)
		throw std::runtime_error("Failed to connect to libvirt with uri: " + uri);
	return conn;
}

std::shared_ptr<virDomain> define_from_xml(virConnectPtr conn, const std::string &xml)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Define persistant domain from xml";
	std::shared_ptr<virDomain> domain(
			virDomainDefineXML(conn, xml.c_str()),
			Deleter_virDomain()
	);
	if (!domain)
		throw std::runtime_error("Error defining domain from xml.");
	return std::move(domain);
}

std::shared_ptr<virDomain> create_from_xml(virConnectPtr conn, const std::string &xml)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Create domain from xml";
	std::shared_ptr<virDomain> domain(
			virDomainCreateXML(conn, xml.c_str(), VIR_DOMAIN_NONE),
			Deleter_virDomain()
	);
	if (!domain)
		throw std::runtime_error("Error creating domain from xml.");
	return std::move(domain);
}

std::shared_ptr<virDomain> find_by_name(virConnectPtr conn, const std::string &name)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Get domain by name.";
	std::shared_ptr<virDomain> domain(
		virDomainLookupByName(conn, name.c_str()),
		Deleter_virDomain()
	);
	if (!domain)
		throw std::runtime_error("Domain not found.");
	return std::move(domain);
}

void create(virDomainPtr domain)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Create domain.";
	if (virDomainCreate(domain) == -1)
		throw std::runtime_error(std::string("Error creating domain: ") + virGetLastErrorMessage());
}

unsigned char get_domain_state(virDomainPtr domain)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Get domain info.";
	virDomainInfo domain_info;
	if (virDomainGetInfo(domain, &domain_info) == -1)
		throw std::runtime_error("Failed getting domain info.");
	return domain_info.state;
}

struct Domain_state_error :
	public std::runtime_error
{
	explicit Domain_state_error(const std::string &what_arg) :
		std::runtime_error(what_arg)
	{
	}
};

void check_state(virDomainPtr domain, virDomainState expected_state)
{
	auto state = get_domain_state(domain);
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Check domain state.";
	if (state != expected_state)
		throw Domain_state_error("Wrong domain state: " + std::to_string(state));
}

void check_remote_state(const std::string &name, const std::vector<std::string> &nodes, virDomainState expected_state)
{
	for (const auto &node : nodes) {
		FASTLIB_LOG(libvirt_hyp_log, trace) << "Check domain state on " + node + ".";
		auto conn = connect(node, "qemu", "ssh");
		try {
			auto domain = find_by_name(conn.get(), name);
			check_state(domain.get(), expected_state);
		} catch (const Domain_state_error &e) {
			throw std::runtime_error("Domain already running on " + node);
		} catch (const std::runtime_error &e) {
			if (e.what() != std::string("Domain not found."))
				throw;
		}
	}
}

void wait_for_state(virDomainPtr domain, virDomainState expected_state)
{
	// TODO: Implement timeout
	while (get_domain_state(domain) != expected_state) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

void set_memory(virDomainPtr domain, unsigned long memory)
{
	if (virDomainSetMemoryFlags(domain, memory, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting amount of memory to " + std::to_string(memory)
				+ " KiB.");
}

void set_max_memory(virDomainPtr domain, unsigned long memory)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Set memory.";
	if (virDomainSetMemoryFlags(domain, memory, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_MEM_MAXIMUM) == -1) {
		throw std::runtime_error("Error setting maximum amount of memory to " + std::to_string(memory) 
				+ " KiB.");
	}
}

void set_max_vcpus(virDomainPtr domain, unsigned int vcpus)
{
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Set VCPUs.";
	if (virDomainSetVcpusFlags(domain, vcpus, VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM) == -1)
		throw std::runtime_error("Error setting maximum number of vcpus to " + std::to_string(vcpus)
				+ ".");
}

void set_vcpus(virDomainPtr domain, unsigned int vcpus)
{
	if (virDomainSetVcpusFlags(domain, vcpus, VIR_DOMAIN_AFFECT_CONFIG) == -1)
		throw std::runtime_error("Error setting number of vcpus to " + std::to_string(vcpus)
				+ ".");
}
// Libvirt sometimes returns a dynamically allocated cstring.
// As we prefer std::string this function converts and frees.
std::string convert_and_free_cstr(char *cstr)
{
	std::string str;
	if (cstr) {
		str.assign(cstr);
		free(cstr);
	}
	return str;
}

// TODO: Implement deleter for virDomainSnapshotPtr
void snapshot_migration(const std::string &local_domain_name, const std::string &remote_domain_name, const std::string &remote_host)
{
	// Get domains	
	auto driver = task.driver.is_valid() ? task.driver.get() : default_driver;
	auto local_conn = connect("", driver);
	auto remote_conn = connect("", driver, "ssh");
	auto local_domain = find_by_name(local_conn.get(), local_domain_name);
	auto remote_domain = find_by_name(remote_conn.get(), remote_domain_name);
	// Check states TODO: Implement
	// Compare size TODO: Replace with real implementation
	auto &name1 = local_domain_name;
	auto &name2 = remote_domain_name;
	auto &conn1 = local_conn;
	auto &conn2 = remote_conn;
	auto &domain1 = local_domain;
	auto &domain2 = remote_domain;
	// Pause vm1 TODO: move to function
	virDomainSuspend(domain1.get());
	// Take snapshot of vm1 TODO: move to function
	auto snapshot = virDomainSnapshotCreateXML(domain1.get(), "", nullptr);
	// destroy vm1 TODO: move to function
	virDomainDestroy(domain1.get());
	// Migrate vm2 TODO: handle flags and migrateuri and check success
	std::unique_ptr<virDomain, Deleter_virDomain> dest_domain2(
		virDomainMigrate(domain2.get(), conn1.get(), nullptr, 0, nullptr, 0)
	);
	// Get snapshotted domain on dest
	auto dest_domain1 = find_by_name(conn2.get(), name1);
	// Redefine snapshot on remote
	auto xml = convert_and_free_cstr(virDomainSnapshotGetXMLDesc(snapshot, VIR_DOMAIN_XML_MIGRATABLE));
	auto dest_snapshot = virDomainSnapshotCreateXML(dest_domain1, xml.c_str(), VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE);
	// Remove snapshot from src
	virDomainSnapshotDelete(snapshot)
	// Revert to snapshot
	virDomainSnapshotRevert(dest_snapshot, VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING);

}

//
// Libvirt_hypervisor implementation
//

Libvirt_hypervisor::Libvirt_hypervisor(std::vector<std::string> nodes, std::string default_driver, std::string default_transport) :
	pci_device_handler(std::make_shared<PCI_device_handler>()),
	nodes(std::move(nodes)),
	default_driver(std::move(default_driver)),
	default_transport(std::move(default_transport))
{
}

void Libvirt_hypervisor::start(const Start &task, Time_measurement &time_measurement)
{
	(void) time_measurement;
	// Connect to libvirt to libvirt
	auto driver = task.driver.is_valid() ? task.driver.get() : default_driver;
	auto conn = connect("", driver);
	// Get domain
	std::shared_ptr<virDomain> domain;
	if (task.xml.is_valid()) {
		// Define domain from XML
		domain = define_from_xml(conn.get(), task.xml);
	} else if (task.vm_name.is_valid()) {
		// Find existing domain
		domain = find_by_name(conn.get(), task.vm_name);
		// Get domain info + check if in shutdown state
		check_state(domain.get(), VIR_DOMAIN_SHUTOFF);
	} else {
		throw std::runtime_error("Neither vm-name nor xml is specified in start task.");
	}
	// Check if domain already running on a remote host
	auto name_cstr = virDomainGetName(domain.get());
	if (!name_cstr)
		throw std::runtime_error("Cannot get domain name.");
	auto name = std::string(name_cstr);
	check_remote_state(name, nodes, VIR_DOMAIN_SHUTOFF);
	// Set memory
	if (task.memory.is_valid()) {
		// TODO: Add separat max memory option
		set_max_memory(domain.get(), task.memory);
		set_memory(domain.get(), task.memory);
	}
	// Set VCPUs
	if (task.vcpus.is_valid()) {
		// TODO: Add separat max vcpus option
		set_max_vcpus(domain.get(), task.vcpus);
		set_vcpus(domain.get(), task.vcpus);
	}
	// Start domain
	create(domain.get());
	// Attach devices
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Attach " << task.pci_ids.size() << " devices.";
	for (auto &pci_id : task.pci_ids) {
		FASTLIB_LOG(libvirt_hyp_log, trace) << "Attach device with PCI-ID " << pci_id.str();
		pci_device_handler->attach(domain.get(), pci_id);
	}
	// Wait for domain to boot
	probe_ssh_connection(domain.get());
}

void Libvirt_hypervisor::stop(const Stop &task, Time_measurement &time_measurement)
{
	(void) time_measurement;
	// Connect to libvirt to libvirt
	auto driver = task.driver.is_valid() ? task.driver.get() : default_driver;
	auto conn = connect("", driver);
	// Get domain by name
	std::shared_ptr<virDomain> domain(
		find_by_name(conn.get(), task.vm_name)
	);
	// Get domain info + check if in running state
	check_state(domain.get(), VIR_DOMAIN_RUNNING);
	// Detach PCI devices
	pci_device_handler->detach(domain.get());
	// Destroy or shutdown domain
	if (task.force.is_valid()) {
		if (virDomainDestroy(domain.get()) == -1)
			throw std::runtime_error("Error destroying domain.");
	} else {
		if (virDomainShutdown(domain.get()) == -1)
			throw std::runtime_error("Error shutting domain down.");
	}
	// Wait until domain is shut down
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Wait until domain is shut down.";
	wait_for_state(domain.get(), VIR_DOMAIN_SHUTOFF);
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Domain is shut down.";
}

void Libvirt_hypervisor::migrate(const Migrate &task, Time_measurement &time_measurement)
{
	const std::string &dest_hostname = task.dest_hostname;
	auto migration_type = task.migration_type.is_valid() ? task.migration_type.get() : "warm";
	bool live_migration = migration_type == "live";
	bool rdma_migration = task.rdma_migration.is_valid() ? task.rdma_migration.get() : false;;
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Migrate " << task.vm_name << " to " << task.dest_hostname << ".";
	FASTLIB_LOG(libvirt_hyp_log, trace) << "migration-type=" << migration_type;
	FASTLIB_LOG(libvirt_hyp_log, trace) << "live-migration=" << live_migration;
	FASTLIB_LOG(libvirt_hyp_log, trace) << "rdma-migration=" << rdma_migration;
	// Connect to libvirt to libvirt
	auto driver = task.driver.is_valid() ? task.driver.get() : default_driver;
	FASTLIB_LOG(libvirt_hyp_log, trace) << "driver=" << driver;
	auto conn = connect("", driver);
	// Get domain by name
	auto domain = find_by_name(conn.get(), task.vm_name);
	// Check if domain is in running state
	check_state(domain.get(), VIR_DOMAIN_RUNNING);
	// Guard migration of PCI devices.
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Create guard for device migration.";
	Migrate_devices_guard dev_guard(pci_device_handler, domain, time_measurement);
	// Connect to destination
	auto transport = task.transport.is_valid() ? task.transport.get() : default_transport;
	FASTLIB_LOG(libvirt_hyp_log, trace) << "transport=" << transport;
	auto dest_connection = connect(dest_hostname, driver, transport);
	// Set migration flags
	unsigned long flags = 0;
	flags |= live_migration ? VIR_MIGRATE_LIVE : 0;
	// create migrateuri
	std::string migrate_uri = rdma_migration ? "rdma://" + dest_hostname + "-ib" : "";
	FASTLIB_LOG(libvirt_hyp_log, trace) << (rdma_migration ? "Use migrate uri: " + migrate_uri + "." : "Use default migrate uri.");
	// Migrate domain
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Migrate domain.";
	time_measurement.tick("migrate");
	std::shared_ptr<virDomain> dest_domain(
		virDomainMigrate(domain.get(), dest_connection.get(), flags, 0, rdma_migration ? migrate_uri.c_str() : nullptr, 0),
		Deleter_virDomain()
	);
	time_measurement.tock("migrate");
	if (!dest_domain)
		throw std::runtime_error(std::string("Migration failed: ") + virGetLastErrorMessage());
	// Set destination domain for guards
	FASTLIB_LOG(libvirt_hyp_log, trace) << "Set destination domain for guards.";
	dev_guard.set_destination_domain(dest_domain);
}
