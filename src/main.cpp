/*
 * This source file is part of Sidewinder daemon.
 *
 * Copyright (c) 2014 Tolga Cakir <tolga@cevel.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <iostream>

#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <unistd.h>

#include <libconfig.h++>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <systemd/sd-daemon.h>

#include "keyboard.hpp"

/* global variables */
namespace sidewinderd {
	std::atomic<bool> state;
};

void sig_handler(int sig) {
	switch (sig) {
		case SIGINT:
			sidewinderd::state = 0;
			break;
		case SIGTERM:
			sidewinderd::state = 0;
			break;
		default:
			std::cout << "Unknown signal received." << std::endl;
	}
}

int create_pid(std::string pid_file) {
	int pid_fd = open(pid_file.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	if (pid_fd < 0) {
		std::cout << "PID file could not be created." << std::endl;
		return -1;
	}

	if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0) {
		std::cout << "Could not lock PID file, another instance is already running. Terminating." << std::endl;
		close(pid_fd);
		return -1;
	}

	return pid_fd;
}

void close_pid(int pid_fd, std::string pid_file) {
	flock(pid_fd, LOCK_UN);
	close(pid_fd);
	unlink(pid_file.c_str());
}

void setup_config(libconfig::Config *config, std::string config_file = "/etc/sidewinderd.conf") {
	try {
		config->readFile(config_file.c_str());
	} catch (const libconfig::FileIOException &fioex) {
		std::cerr << "I/O error while reading file." << std::endl;
	} catch (const libconfig::ParseException &pex) {
		std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine() << " - " << pex.getError() << std::endl;
	}

	libconfig::Setting &root = config->getRoot();

	/* TODO: check values for validity and throw exceptions, if invalid */
	if (!root.exists("user")) {
		root.add("user", libconfig::Setting::TypeString) = "root";
	}

	if (!root.exists("profile")) {
		root.add("profile", libconfig::Setting::TypeInt) = 1;
	}

	if (!root.exists("capture_delays")) {
		root.add("capture_delays", libconfig::Setting::TypeBoolean) = true;
	}

	if (!root.exists("pid-file")) {
		root.add("pid-file", libconfig::Setting::TypeString) = "/var/run/sidewinderd.pid";
	}

	try {
		config->writeFile(config_file.c_str());
	} catch (const libconfig::FileIOException &fioex) {
		std::cerr << "I/O error while writing file." << std::endl;
	}
}

int main(int argc, char *argv[]) {
	/* signal handling */
	struct sigaction action;

	action.sa_handler = sig_handler;

	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	sidewinderd::state = 1;

	/* handling command-line options */
	static struct option long_options[] = {
		{"config", required_argument, 0, 'c'},
		{"daemon", no_argument, 0, 'd'},
		{"verbose", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};

	int opt, index;
	index = 0;

	std::string config_file;

	while ((opt = getopt_long(argc, argv, ":c:dv", long_options, &index)) != -1) {
		switch (opt) {
			case 'c':
				std::cout << "Option --config" << std::endl;
				config_file = optarg;
				break;
			case 'd':
				/* TODO: add optional old fashioned fork() daemon */
				std::cout << "Option --daemon" << std::endl;
				break;
			case 'v':
				std::cout << "Option --verbose" << std::endl;
				break;
			case ':':
				std::cout << "Missing argument." << std::endl;
				break;
			case '?':
				std::cout << "Unrecognized option." << std::endl;
				break;
			default:
				std::cout << "Unexpected error." << std::endl;
				return EXIT_FAILURE;
		}
	}

	/* reading config file */
	libconfig::Config config;

	if (config_file.empty()) {
		setup_config(&config);
	} else {
		setup_config(&config, config_file);
	}

	/* creating pid file for single instance mechanism */
	std::string pid_file = config.lookup("pid-file");
	int pid_fd = create_pid(pid_file);

	if (pid_fd < 0) {
		return EXIT_FAILURE;
	}

	/* get user's home directory */
	std::string user = config.lookup("user");
	struct passwd *pw = getpwnam(user.c_str());

	/* setting gid and uid to configured user */
	setegid(pw->pw_gid);
	seteuid(pw->pw_uid);

	/* creating sidewinderd directory in user's home directory */
	std::string workdir = pw->pw_dir;
	workdir.append("/.sidewinderd");
	mkdir(workdir.c_str(), S_IRWXU);

	if (chdir(workdir.c_str())) {
		std::cout << "Error chdir" << std::endl;
	}

	Keyboard kbd(&config, pw);

	/* main loop */
	/* TODO: exit loop, if keyboards gets unplugged */
	while (sidewinderd::state) {
		kbd.listen_key();
	}

	close_pid(pid_fd, pid_file);

	return EXIT_SUCCESS;
}
