#ifndef __PORTO_HPP__
#define __PORTO_HPP__

const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

// TODO: rework this into some kind of notify interface
extern void AckExitStatus(int pid);

#endif