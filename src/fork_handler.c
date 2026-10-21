#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define __USE_GNU
#include <dlfcn.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

void __attribute__((constructor(1000))) scorep_constructor(int argc, char* argv[]);
void scorep_atfork_child();
void scorep_exit_handler();

#ifndef SCOREP_RELOADABLE_SO
#define SCOREP_RELOADABLE_SO "./init.so"
#endif

void* orig_dlopen = NULL;
void* orig_dlsym = NULL;
void (*SCOREP_InitMeasurement)(void) = NULL;
void (*SCOREP_FinalizeMeasurement)(void) = NULL;
void (*libwrap_subsystem_restore_original_bindings)(void*, void*) = NULL;
void* handle = NULL;

void scorep_constructor(int argc, char* argv[]) {
	atexit(scorep_exit_handler);
	pthread_atfork(NULL, NULL, scorep_atfork_child);

	handle = dlopen(SCOREP_RELOADABLE_SO, RTLD_NOW);
	// handle errors...

	orig_dlopen = dlsym(RTLD_NEXT, "dlopen");
	orig_dlsym = dlsym(RTLD_NEXT, "dlsym");

	SCOREP_InitMeasurement = dlsym(handle, "SCOREP_InitMeasurement");
	SCOREP_FinalizeMeasurement = dlsym(handle, "SCOREP_FinalizeMeasurement");
	libwrap_subsystem_restore_original_bindings = dlsym(handle, "libwrap_subsystem_restore_original_bindings");
	// handle errors after each dlsym...

	SCOREP_InitMeasurement();
}

void scorep_atfork_child() {
	libwrap_subsystem_restore_original_bindings(orig_dlopen, orig_dlsym);

	char dir_name[100];
	snprintf(dir_name, 100, "scorep-experiment-child_%d", getpid());
	setenv("SCOREP_EXPERIMENT_DIRECTORY", dir_name, 1);

	dlclose(handle);
	// handle errors...

	SCOREP_InitMeasurement = NULL;
	SCOREP_FinalizeMeasurement = NULL;
	libwrap_subsystem_restore_original_bindings = NULL;

	handle = dlopen(SCOREP_RELOADABLE_SO, RTLD_NOW);
	// handle errors...

	SCOREP_InitMeasurement = dlsym(handle, "SCOREP_InitMeasurement");
	SCOREP_FinalizeMeasurement = dlsym(handle, "SCOREP_FinalizeMeasurement");
	libwrap_subsystem_restore_original_bindings = dlsym(handle, "libwrap_subsystem_restore_original_bindings");
	// handle errors after each dlsym...

	SCOREP_InitMeasurement();
}

void scorep_exit_handler() {
	SCOREP_FinalizeMeasurement();
}

__attribute__((noreturn)) void _exit(int status) {
	struct sigaction action;
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = SIG_IGN;
	sigaction(SIGTERM, &action, NULL);

	scorep_exit_handler();

	__attribute__((noreturn)) void (*orig__exit)(int) = dlsym(RTLD_NEXT, "_exit");
	// handle errors...
	orig__exit(status);
}