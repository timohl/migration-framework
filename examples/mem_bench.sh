#!/bin/bash

echo "Starting mem bench."

server_a=pandora1
server_b=pandora2
vm_name=centos7113
host=devon
vcpus=1
mem_values_GiB="30"
#mem_values_GiB="30 28 26 24 22"
iterations=1
rdma_migration='false'
live_migration='false'
memory_ballooning='true'
start_template="$(cat start_template.yaml | sed s/\$vcpus/$vcpus/ | sed s/\$vm_name/$vm_name/)"
migrate_template="$(cat migrate_template.yaml | sed s/\$rdma_migration/$rdma_migration/ | sed s/\$live_migration/$live_migration/ | sed s/\$vm_name/$vm_name/ | sed s/\$memory_ballooning/$memory_ballooning/)"
stop_task="$(cat stop_template.yaml | sed s/\$vm_name/$vm_name/)"

parse_result_gawk='
BEGIN {
	vm_index = 0
	if (result_type == "") {
		print "No result_type variable found."
		exit 1
	}
}
$1 == "result:" {
	if ($0 != ("result: " result_type)) {
		print "No result of correct type.", "First line: " $0
		exit 1
	}
}
function save(first_column) {
	if ($first_column ~ /:$/) {
		value = ""
		for (i = first_column + 1 ; i <= NF ; ++i) {
			if (value != "") {
				value = value " "
			}
			value = value $i
		}
		vms[vm_index][$first_column] = value
	}
}
function save_time_measurement(first_column) {
	cnt = 2
	if ("'"$memory_ballooning"'" == "true") {
		cnt =  4
	}
	for (i = 0 ; i != cnt ; ++i) {
		getline
		time_measurement[$1] = $2
	}
	next
}
function process(first_column) {
	if ($first_column == "time-measurement:") {
		save_time_measurement(first_column)
	} else {
		save(first_column)
	}
}
$0 !~ /^(result:|\.\.\.|---)|list:$/ {
	if ($1 == "-") {
		++vm_index
		process(2)
	} else {
		process(1)
	}
}
END {
	return_value = 0
	for (vm in vms) {
		if (verbose) {
			print vms[vm]["vm-name:"] ":" vms[vm]["status:"]
		}
		if (vms[vm]["status:"] != "success") {
			return_value = 1
		}
	}
	if (verbose) {
		for (key in time_measurement) {
			print key " " time_measurement[key]
		}
	}
	exit return_value
}
'
parse_iterations_output='
BEGIN {
	iteration=0
}
$0 ~ /success/ {
	++iteration
}
$0 !~ /success/ {
	values[$1][iteration] = $2
}
END {
	PROCINFO["sorted_in"] = "@ind_str_asc"
	for (key in values) {
		for (it in values[key]) {
			average[key] += values[key][it]
		}
		average[key] /= iteration
		summary_head = summary_head " " key
		summary = summary " " average[key]
	}
	print summary_head
	print summary
}
'

parse_mem_values_output='
$1 == "mem:" {
	mem = $2
}
NR == 2 {
	str = $1
	for (i = 2 ; i <= NF ; ++i) {
		str = str "\t" $i
	}
	print "mem" "\t" str
}
$1 ~ /^[[:digit:]]+\.?[[:digit:]]*$/ {
	str = $1
	for (i = 2 ; i <= NF ; ++i) {
		str = str "\t" $i
	}
	print mem "\t" str
}
'

for mem in $mem_values_GiB; do
	echo "mem:" $mem
	# Start vm
	ssh "$server_a" ". /global/cluster/libvirt-1.2.16/bin/set_environment.sh; virsh define ~/centos7113-configs/centos7113_${mem}G.xml" > /dev/null
	ssh "$server_b" ". /global/cluster/libvirt-1.2.16/bin/set_environment.sh; virsh define ~/centos7113-configs/centos7113_${mem}G.xml" > /dev/null
	memory=$(( mem * 1024 * 1024 ))
	start_task=$(echo "$start_template" | sed s/\$memory/$memory/)
	mosquitto_sub -q 2 -h "$host" -t fast/migfra/"$server_a"/result -C 1 | gawk -v result_type="vm started" "$parse_result_gawk" || { echo "Failed starting vm."; exit 1; } &
	mosquitto_pub -q 2 -h "$host" -t fast/migfra/"$server_a"/task -m "$start_task"
	wait $!
	sleep 5

	# Migrate forth and back	
	for (( n=0 ; n!=$iterations ; ++n )); do
		# Allocate, write and free memory.
		ssh centos7113 "~/bachelor-thesis/mem_balloon_bench/build/guest/mem_balloon_bench_guest --mode alloc --memory $((mem - 1)) --sleep 1" > /dev/null
		sleep 5

		# Migrate from a to b and save time
		destination="$server_b"
		time_measurement='true'
		migrate_task=$(echo "$migrate_template" | sed s/\$time_measurement/$time_measurement/ | sed s/\$destination/$destination/)
		mosquitto_sub -q 2 -h "$host" -t fast/migfra/"$server_a"/result -C 1 | gawk -v result_type="vm migrated" -v verbose=1 "$parse_result_gawk" || { echo "Failed migrating."; exit 1; } &
		mosquitto_pub -q 2 -h "$host" -t fast/migfra/"$server_a"/task -m "$migrate_task"
		wait $!

		# Migrate back from b to a
		destination="$server_a"
		memory_ballooning='false'
		time_measurement='false'
		migrate_task=$(echo "$migrate_template" | sed s/\$memory_ballooning/$memory_ballooning/ | sed s/\$time_measurement/$time_measurement/ | sed s/\$destination/$destination/)
		mosquitto_sub -q 2 -h "$host" -t fast/migfra/"$server_b"/result -C 1 | gawk -v result_type="vm migrated" "$parse_result_gawk" || { echo "Failed migrating."; exit 1; } &
		mosquitto_pub -q 2 -h "$host" -t fast/migfra/"$server_b"/task -m "$migrate_task"
		wait $!
	done | tee -a mem_bench_internal.log | gawk "$parse_iterations_output"
	# stop
	mosquitto_sub -q 2 -h "$host" -t fast/migfra/"$server_a"/result -C 1 | gawk -v result_type="vm stopped" "$parse_result_gawk" || { echo "Failed stopping vm."; exit 1; } &
	mosquitto_pub -q 2 -h "$host" -t fast/migfra/"$server_a"/task -m "$stop_task"
	wait $!
done | tee mem_bench_external.log | gawk "$parse_mem_values_output"
