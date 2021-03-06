#pragma once

#include <string>
#include <vector>
#include <memory>

#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "env.hpp"

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;
class TCgroup;
class TSubsystem;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

struct TBindMount {
    TPath Source;
    TPath Dest;
    bool ReadOnly;
    bool ReadWrite;
};

struct TTaskEnv : public TNonCopyable {
    std::string Container;
    std::string Command;
    TScopedFd PortoInitFd;
    TPath Cwd;
    TPath ParentCwd;
    TPath Root; /* path in ParentNs.Mnt */
    bool RootRdOnly;
    TEnv Env;
    bool Isolate = false;
    bool TripleFork;
    bool QuadroFork;
    TStdStream Stdin, Stdout, Stderr;
    TNamespaceSnapshot ParentNs;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool SetEtcHostname;
    bool BindDns;
    std::string ResolvConf;
    std::vector<TBindMount> BindMounts;
    std::vector<TDevice> Devices;
    std::vector<std::string> Autoconf;
    int LoopDev;
    TCapabilities CapLimit;
    TCapabilities CapAmbient;
    bool NewMountNs;
    std::vector<TCgroup> Cgroups;
    TCred OwnerCred, Cred;

    TUnixSocket Sock, MasterSock;
    TUnixSocket Sock2,  MasterSock2;
    int ReportStage = 0;
};

class TTask: public TNonCopyable {
    std::unique_ptr<TTaskEnv> Env;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid, VPid, WPid;

    void ReportPid(pid_t pid) const;

    TError ChildApplyCapabilities();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildMountBinds();
    TError ChildMountRun();
    TError ChildMountRootFs();
    TError ChildRemountRootRo();
    TError ChildIsolateFs();

public:
    TTask(std::unique_ptr<TTaskEnv> &env);
    TTask(pid_t pid);
    ~TTask();

    TError Start();
    pid_t GetPid() const;
    pid_t GetWPid() const;
    pid_t GetPidFor(pid_t pid) const;
    std::vector<int> GetPids() const;
    bool IsRunning() const;
    int GetExitStatus() const;
    TError Kill(int signal) const;
    void Exit(int status);
    void ClearEnv();

    TError ChildApplyLimits();
    TError ChildSetHostname();
    TError ConfigureChild();
    TError WaitAutoconf();
    void StartChild();
    void Restore(std::vector<int> pids);
    void Abort(const TError &error) const;

    bool IsZombie() const;

    bool HasCorrectParent();
};
