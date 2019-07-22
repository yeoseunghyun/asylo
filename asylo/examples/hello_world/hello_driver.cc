/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <iostream>
#include <string>
#include <vector>
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_split.h"
#include "asylo/client.h"
#include "asylo/examples/hello_world/hello.pb.h"
#include "asylo/util/logging.h"


#include <pthread.h>
#include <stdlib.h>
#include <sys/epoll.h>


ABSL_FLAG(std::string, enclave_path, "", "Path to enclave to load");
ABSL_FLAG(std::string, names, "",
		"A comma-separated list of names to pass to the enclave");


void * testing(void * args){
	int epoll_fd;
	int watching_fd=0;//stdin for test;
	int nfds;
	struct epoll_event ev, events;
	epoll_fd = epoll_create1(0);
	if(epoll_fd == -1){
		perror("epoll_create1");
		return nullptr;
	}

	ev.events = EPOLLIN;//EPOLLIN, EPOLLOUT, EPOLLERR
	ev.data.fd = watching_fd;
	
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, watching_fd, &ev) == -1) {
		perror("epoll_ctl: watching_fd");
		return nullptr;
	}
	
	std::cout << std::endl << "waiting for epoll" << std::endl;
	nfds = epoll_wait(epoll_fd,&events,1,-1);//-1 for forever
	
	if(nfds == -1){
		perror("epoll_wait");
		return nullptr;
	}
	
	std::cout << "EnterAndTakesnapshot" << std::endl;
	return nullptr;
}


int main(int argc, char *argv[]) {
	// Part 0: Setup
	pthread_t thread;
	pthread_create(&thread, nullptr, testing, nullptr);//create epoll_thread
	
	
	absl::ParseCommandLine(argc, argv);
	if (absl::GetFlag(FLAGS_names).empty()) {
		LOG(QFATAL) << "Must supply a non-empty list of names with --names";
	}
	std::vector<std::string> names =
		absl::StrSplit(absl::GetFlag(FLAGS_names), ',');
	// Part 1: Initialization
	asylo::EnclaveManager::Configure(asylo::EnclaveManagerOptions());
	auto manager_result = asylo::EnclaveManager::Instance();
	if (!manager_result.ok()) {
		LOG(QFATAL) << "EnclaveManager unavailable: " << manager_result.status();
	}
	asylo::EnclaveManager *manager = manager_result.ValueOrDie();
	std::cout << "Loading " << absl::GetFlag(FLAGS_enclave_path) << std::endl;
	asylo::SimLoader loader(absl::GetFlag(FLAGS_enclave_path), /*debug=*/true);
	asylo::Status status = manager->LoadEnclave("hello_enclave", loader);
	if (!status.ok()) {
		LOG(QFATAL) << "Load " << absl::GetFlag(FLAGS_enclave_path)
			<< " failed: " << status;
	}
	// Part 2: Secure execution
	asylo::EnclaveClient *client = manager->GetClient("hello_enclave");
	for (const auto &name : names) {
		asylo::EnclaveInput input;
		input.MutableExtension(hello_world::enclave_input_hello)
			->set_to_greet(name);
		asylo::EnclaveOutput output;
		status = client->EnterAndRun(input, &output);
		if (!status.ok()) {
			LOG(QFATAL) << "EnterAndRun failed: " << status;
		}
		if (!output.HasExtension(hello_world::enclave_output_hello)) {
			LOG(QFATAL) << "Enclave did not assign an ID for " << name;
		}
		std::cout << "Message from enclave: "
			<< output.GetExtension(hello_world::enclave_output_hello)
			.greeting_message()
			<< std::endl;
	}
	// Part 3: Finalization
	asylo::EnclaveFinal final_input;
	status = manager->DestroyEnclave(client, final_input);
	if (!status.ok()) {
		LOG(QFATAL) << "Destroy " << absl::GetFlag(FLAGS_enclave_path)
			<< " failed: " << status;
	}
	
	
	pthread_join(thread,nullptr);//join epoll_thread
	return 0;
}
