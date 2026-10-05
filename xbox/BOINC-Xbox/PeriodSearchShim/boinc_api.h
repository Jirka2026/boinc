#pragma once

int boinc_init();
int boinc_is_standalone();
int boinc_time_to_checkpoint();
void boinc_checkpoint_completed();
void boinc_fraction_done(double fraction);
double boinc_get_fraction_done();
double boinc_worker_thread_cpu_time();
void boinc_finish(int status);
char* boinc_msg_prefix(char* buffer, int length);
