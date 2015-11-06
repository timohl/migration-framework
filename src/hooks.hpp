#ifndef HOOKS_HPP
#define HOOKS_HPP

#include "time_measurement.hpp"

#include <fast-lib/communication/mqtt_communicator.hpp>

#include <memory>
#include <string>

class Suspend_pscom
{	
public:
	Suspend_pscom(const std::string &vm_name,
		      unsigned int messages_expected,
		      std::shared_ptr<fast::Communicator> comm,
		      Time_measurement &time_measurement);
	~Suspend_pscom();
private:
	void suspend();
	void resume();

	const std::string vm_name;
	std::string request_topic;
	std::string response_topic;
	const unsigned int messages_expected;
	std::shared_ptr<fast::MQTT_communicator> comm;
	unsigned int answers;
	int qos;
	Time_measurement &time_measurement;
};

#endif
