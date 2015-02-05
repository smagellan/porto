#include "property.hpp"
#include "config.hpp"
#include "subsystem.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "qdisc.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <linux/capability.h>
}

bool TPropertySet::ParentDefault(std::shared_ptr<TContainer> &c,
                                 const std::string &property) {
    TError error = GetSharedContainer(c);
    if (error) {
        L_ERR() << "Can't get default for " << property << ": " << error << std::endl;
        return "";
    }

    return c->UseParentNamespace() && HasFlags(property, PARENT_DEF_PROPERTY);
}

bool TPropertySet::IsDefault(const std::string &property) {
    return VariantSet.IsDefault(property);
}

bool TPropertySet::HasFlags(const std::string &property, int flags) {
    TError error = Valid(property);
    if (error) {
        L_ERR() << error << std::endl;
        return false;
    }

    auto av = VariantSet[property];
    return av->GetFlags() & flags;
}
bool TPropertySet::HasState(const std::string &property, EContainerState state) {
    TError error = Valid(property);
    if (error) {
        L_ERR() << error << std::endl;
        return false;
    }

    auto cv = ToContainerValue(VariantSet[property]);
    auto valueState = cv->GetState();

    return valueState.find(state) != valueState.end();
}

TError TPropertySet::Valid(const std::string &property) {
    if (!VariantSet.IsValid(property))
        return TError(EError::Unknown, "Invalid property " + property);

    return TError::Success();
}

TError TPropertySet::Create() {
    return VariantSet.Create();
}

TError TPropertySet::Restore(const kv::TNode &node) {
    return VariantSet.Restore(node);
}

void TPropertySet::Reset(const std::string &name) {
    VariantSet.Reset(name);
}

bool TPropertySet::HasValue(const std::string &name) {
    return VariantSet.HasValue(name);
}

TError TPropertySet::Flush() {
    return VariantSet.Flush();
}

TError TPropertySet::Sync() {
    return VariantSet.Sync();
}

TError TPropertySet::PrepareTaskEnv(const std::string &property,
                                    std::shared_ptr<TTaskEnv> taskEnv) {
    auto av = VariantSet[property];

    if (VariantSet.IsDefault(property)) {
        // if the value is default we still need PrepareTaskEnv method
        // to be called, so set value to default and then reset it
        TError error = av->FromString(av->DefaultString());
        if (error)
            return error;

        av->Reset();
    }

    return ToContainerValue(av)->PrepareTaskEnv(taskEnv);
}

TError TPropertySet::GetSharedContainer(std::shared_ptr<TContainer> &c) {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    return TError::Success();
}

static TError ValidPath(std::shared_ptr<TContainer> c, const std::string &str) {
    if (!str.length() || str[0] != '/')
        return TError(EError::InvalidValue, "invalid directory");
    return TError::Success();
}

static TError ExistingFile(std::shared_ptr<TContainer> c, const std::string &str) {
    TFile f(str);
    if (!f.Exists())
        return TError(EError::InvalidValue, "file doesn't exist");
    return TError::Success();
}

static std::string DefaultStdFile(std::shared_ptr<TContainer> c,
                                  const std::string &name) {
    std::string cwd, root;
    TError error = c->GetProperty("cwd", cwd);
    if (error) {
        L_ERR() << "Can't get cwd for std file: " << error << std::endl;
        return "";
    }

    error = c->GetProperty("root", root);
    if (error) {
        L_ERR() << "Can't get root for std file: " << error << std::endl;
        return "";
    }

    std::string prefix;
    if (c->UseParentNamespace())
        prefix = c->GetName(false) + ".";

    TPath path = root;
    if (!path.Exists() || path.GetType() == EFileType::Directory) {
        path = path.AddComponent(cwd);
    } else {
        path = c->GetTmpDir();
    }

    return path.AddComponent(prefix + name).ToString();
}

static std::set<EContainerState> staticProperty = {
    EContainerState::Stopped,
};

static std::set<EContainerState> dynamicProperty = {
    EContainerState::Stopped,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta,
};

static std::set<EContainerState> anyState = {
    EContainerState::Stopped,
    EContainerState::Dead,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta
};

class TCommandProperty : public TStringValue, public TContainerValue {
public:
    TCommandProperty() :
        TStringValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_COMMAND,
                        "Command executed upon container start",
                        staticProperty) {}

    std::string GetDefault() const override {
        if (GetContainer()->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/sbin/init";

        return "";
    }

};

class TUserProperty : public TStringValue, public TContainerValue {
public:
    TUserProperty() :
        TStringValue(SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_USER,
                        "Start command with given user",
                        staticProperty) {}

    TError CheckValue(const std::string &value) override {
        TUser u(value);
        TError error = u.Load();
        if (error)
            return error;

        GetContainer()->Cred.Uid = u.GetId();

        return TError::Success();
    }
};

class TGroupProperty : public TStringValue, public TContainerValue {
public:
    TGroupProperty() :
        TStringValue(SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_GROUP,
                        "Start command with given group",
                        staticProperty) {}

    TError CheckValue(const std::string &value) override {
        TGroup g(value);
        TError error = g.Load();
        if (error)
            return error;

        GetContainer()->Cred.Gid = g.GetId();

        return TError::Success();
    }
};

class TEnvProperty : public TListValue, public TContainerValue {
public:
    TEnvProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ENV,
                        "Container environment variables",
                        staticProperty) {}
};

class TRootProperty : public TStringValue, public TContainerValue {
public:
    TRootProperty() :
        TStringValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ROOT,
                     "Container root directory",
                     staticProperty) {}

    std::string GetDefault() const override {
        return "/";
    }

    TError CheckValue(const std::string &value) override {
        return ValidPath(GetContainer(), value);
    }
};

class TRootRdOnlyProperty : public TBoolValue, public TContainerValue {
public:
    TRootRdOnlyProperty() :
        TBoolValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ROOT_RDONLY,
                        "Mount root directory in read-only mode",
                        staticProperty) {}

    bool GetDefault() const override {
        return false;
    }
};

class TCwdProperty : public TStringValue, public TContainerValue {
public:
    TCwdProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_CWD,
                        "Container working directory",
                        staticProperty) {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/";

        if (!c->Prop->IsDefault("root"))
            return "/";

        return config().container().tmp_dir() + "/" + c->GetName();
    }

    TError CheckValue(const std::string &value) override {
        return ValidPath(GetContainer(), value);
    }
};

class TStdinPathProperty : public TStringValue, public TContainerValue {
public:
    TStdinPathProperty() :
        TStringValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_STDIN_PATH,
                        "Container standard input path",
                        staticProperty) {}

    std::string GetDefault() const override {
        return "/dev/null";
    }

    TError CheckValue(const std::string &value) override {
        return ExistingFile(GetContainer(), value);
    }
};

class TStdoutPathProperty : public TStringValue, public TContainerValue {
public:
    TStdoutPathProperty() :
        TStringValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_STDOUT_PATH,
                        "Container standard input path",
                        staticProperty) {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/dev/null";

        return DefaultStdFile(c, "stdout");
    }

    TError CheckValue(const std::string &value) override {
        return ValidPath(GetContainer(), value);
    }
};

class TStderrPathProperty : public TStringValue, public TContainerValue {
public:
    TStderrPathProperty() :
        TStringValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_STDERR_PATH,
                        "Container standard error path",
                        staticProperty) {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/dev/null";

        return DefaultStdFile(c, "stderr");
    }

    TError CheckValue(const std::string &value) override {
        return ValidPath(GetContainer(), value);
    }
};

class TStdoutLimitProperty : public TUintValue, public TContainerValue {
public:
    TStdoutLimitProperty() :
        TUintValue(PERSISTENT_VALUE),
        TContainerValue(P_STDOUT_LIMIT,
                        "Return no more than given number of bytes from standard output/error",
                        staticProperty) {}

    uint64_t GetDefault() const override {
        return config().container().stdout_limit();
    }

    TError CheckValue(const uint64_t &value) override {
        uint32_t max = config().container().stdout_limit();

        if (value > max)
            return TError(EError::InvalidValue,
                          "Maximum number of bytes: " +
                          std::to_string(max));

        return TError::Success();
    }
};

class TMemoryGuaranteeProperty : public TUintValue, public TContainerValue {
public:
    TMemoryGuaranteeProperty() :
        TUintValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | UINT_UNIT_VALUE),
        TContainerValue(P_MEM_GUARANTEE,
                        "Guaranteed amount of memory",
                        dynamicProperty) {}

    TError CheckValue(const uint64_t &value) override {
        auto c = GetContainer();

        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.low_limit_in_bytes"))
            return TError(EError::NotSupported, "invalid kernel");

        if (!c->ValidHierarchicalProperty(P_MEM_GUARANTEE, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        uint64_t total = c->GetRoot()->GetChildrenSum(P_MEM_GUARANTEE, c, value);
        if (total + config().daemon().memory_guarantee_reserve() >
            GetTotalMemory())
            return TError(EError::ResourceNotAvailable,
                          "can't guarantee all available memory");

        return TError::Success();
    }
};

class TMemoryLimitProperty : public TUintValue, public TContainerValue {
public:
    TMemoryLimitProperty() :
        TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE),
        TContainerValue(P_MEM_LIMIT,
                        "Memory hard limit",
                        dynamicProperty) {}

    TError CheckValue(const uint64_t &value) override {
        if (!GetContainer()->ValidHierarchicalProperty(P_MEM_LIMIT, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        return TError::Success();
    }
};

class TRechargeOnPgfaultProperty : public TBoolValue, public TContainerValue {
public:
    TRechargeOnPgfaultProperty() :
        TBoolValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_RECHARGE_ON_PGFAULT,
                        "Recharge memory on page fault",
                        dynamicProperty) {}

    bool GetDefault() const override {
        return false;
    }

    TError CheckValue(const bool &value) override {
        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.recharge_on_pgfault"))
            return TError(EError::NotSupported, "invalid kernel");

        return TError::Success();
    }
};

class TCpuPolicyProperty : public TStringValue, public TContainerValue {
public:
    TCpuPolicyProperty() :
        TStringValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CPU_POLICY,
                        "CPU policy: rt, normal, idle",
                        dynamicProperty) {}

    std::string GetDefault() const override {
        return "normal";
    }

    TError CheckValue(const std::string &value) override {
        if (value != "normal" && value != "rt" && value != "idle")
            return TError(EError::InvalidValue, "invalid policy");

        if (value == "rt") {
            auto cpuroot = cpuSubsystem->GetRootCgroup();
            if (!cpuroot->HasKnob("cpu.smart"))
                return TError(EError::NotSupported, "invalid kernel");
        }

        if (value == "idle")
            return TError(EError::NotSupported, "not implemented");

        return TError::Success();
    }
};

class TCpuPriorityProperty : public TUintValue, public TContainerValue {
public:
    TCpuPriorityProperty() :
        TUintValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CPU_PRIO,
                        "CPU priority: 0-99",
                        dynamicProperty) {}

    uint64_t GetDefault() const override {
        return config().container().default_cpu_prio();
    }

    TError CheckValue(const uint64_t &value) override {
        if (value < 0 || value > 99)
            return TError(EError::InvalidValue, "invalid value");

        return TError::Success();
    }
};

class TNetMapValue : public TMapValue, public TContainerValue {
    uint64_t Def, RootDef;

public:
    virtual uint32_t GetDef() const { return 0; }
    virtual uint32_t GetRootDef() const { return 0; }

    TNetMapValue(
           const char *name,
           const char *desc,
           const int flags,
           const std::set<EContainerState> &state) :
        TMapValue(flags),
        TContainerValue(name, desc, state) {}

    TUintMap GetDefault() const override {
        auto c = GetContainer();

        uint64_t def =  c->IsRoot() ? GetRootDef() : GetDef();

        TUintMap m;
        for (auto &link : c->Net->GetLinks())
            m[link->GetAlias()] = def;
        return m;
    }

    TError CheckValue(const TUintMap &value) {
        std::set<std::string> validKey;

        for (auto &link : GetContainer()->Net->GetLinks())
            validKey.insert(link->GetAlias());

        for (auto &kv : value)
            if (validKey.find(kv.first) == validKey.end())
                return TError(EError::InvalidValue,
                              "invalid interface " + kv.first);

        // TODO: check that all interfaces are specified!!!

        /*
        TUintMap m = Get();
        for (auto &kv : value)
            m[kv.first] = kv.second;
            */

        return TError::Success();
    }
};

class TNetGuaranteeProperty : public TNetMapValue {
public:
    TNetGuaranteeProperty() :
        TNetMapValue(P_NET_GUARANTEE,
                     "Guaranteed container network bandwidth [bytes/s] (max 32Gbps)",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_guarantee(); }
    uint32_t GetRootDef() const override { return config().network().default_max_guarantee(); }
};

class TNetCeilProperty : public TNetMapValue {
public:
    TNetCeilProperty() :
        TNetMapValue(P_NET_CEIL,
                     "Maximum container network bandwidth [bytes/s] (max 32Gbps)",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_limit(); }
    uint32_t GetRootDef() const override { return config().network().default_max_guarantee(); }
};

class TNetPriorityProperty : public TNetMapValue {
public:
    TNetPriorityProperty() :
        TNetMapValue(P_NET_PRIO,
                     "Container network priority: 0-7",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_prio(); }
    uint32_t GetRootDef() const override { return config().network().default_prio(); }

    TError CheckValue(const TUintMap &value) {
        TError error = TNetMapValue::CheckValue(value);
        if (error)
            return error;

        for (auto &kv : value) {
            if (kv.second > 7)
                return TError(EError::InvalidValue, "invalid value");
        }

        return TError::Success();
    }
};

class TRespawnProperty : public TBoolValue, public TContainerValue {
public:
    TRespawnProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_RESPAWN,
                        "Automatically respawn dead container",
                        staticProperty) {}

    bool GetDefault() const override {
        return false;
    }
};

class TMaxRespawnsProperty : public TIntValue, public TContainerValue {
public:
    TMaxRespawnsProperty() :
        TIntValue(PERSISTENT_VALUE),
        TContainerValue(P_MAX_RESPAWNS,
                        "Limit respawn count for specific container",
                        staticProperty) {}

    int GetDefault() const override {
        return -1;
    }
};

class TIsolateProperty : public TBoolValue, public TContainerValue {
public:
    TIsolateProperty() :
        TBoolValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_ISOLATE,
                        "Isolate container from parent",
                        staticProperty) {}

    bool GetDefault() const override {
        return true;
    }
};

class TPrivateProperty : public TStringValue, public TContainerValue {
public:
    TPrivateProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_PRIVATE,
                        "User-defined property",
                        dynamicProperty) {}

    std::string GetDefault() const override {
        return "";
    }

    TError CheckValue(const std::string &value) override {
        uint32_t max = config().container().private_max();

        if (value.length() > max)
            return TError(EError::InvalidValue, "Value is too long");

        return TError::Success();
    }
};

class TUlimitProperty : public TListValue, public TContainerValue {
    std::map<int,struct rlimit> Rlimit;

public:
    TUlimitProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ULIMIT,
                        "Container resource limits",
                        staticProperty) {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        Rlimit.clear();

        static const std::map<std::string,int> nameToIdx = {
            { "as", RLIMIT_AS },
            { "core", RLIMIT_CORE },
            { "cpu", RLIMIT_CPU },
            { "data", RLIMIT_DATA },
            { "fsize", RLIMIT_FSIZE },
            { "locks", RLIMIT_LOCKS },
            { "memlock", RLIMIT_MEMLOCK },
            { "msgqueue", RLIMIT_MSGQUEUE },
            { "nice", RLIMIT_NICE },
            { "nofile", RLIMIT_NOFILE },
            { "nproc", RLIMIT_NPROC },
            { "rss", RLIMIT_RSS },
            { "rtprio", RLIMIT_RTPRIO },
            { "rttime", RLIMIT_RTTIME },
            { "sigpending", RLIMIT_SIGPENDING },
            { "stask", RLIMIT_STACK },
        };

        for (auto &limit : lines) {
            std::vector<std::string> nameval;

            (void)SplitString(limit, ':', nameval);
            if (nameval.size() != 2)
                return TError(EError::InvalidValue, "Invalid limits format");

            std::string name = StringTrim(nameval[0]);
            if (nameToIdx.find(name) == nameToIdx.end())
                return TError(EError::InvalidValue, "Invalid limit " + name);
            int idx = nameToIdx.at(name);

            std::vector<std::string> softhard;
            (void)SplitString(StringTrim(nameval[1]), ' ', softhard);
            if (softhard.size() != 2)
                return TError(EError::InvalidValue, "Invalid limits number for " + name);

            rlim_t soft, hard;
            if (softhard[0] == "unlim" || softhard[0] == "unliminted") {
                soft = RLIM_INFINITY;
            } else {
                TError error = StringToUint64(softhard[0], soft);
                if (error)
                    return TError(EError::InvalidValue, "Invalid soft limit for " + name);
            }

            if (softhard[1] == "unlim" || softhard[1] == "unliminted") {
                hard = RLIM_INFINITY;
            } else {
                TError error = StringToUint64(softhard[1], hard);
                if (error)
                    return TError(EError::InvalidValue, "Invalid hard limit for " + name);
            }

            Rlimit[idx].rlim_cur = soft;
            Rlimit[idx].rlim_max = hard;
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->Rlimit = Rlimit;
        return TError::Success();
    }
};

class THostnameProperty : public TStringValue, public TContainerValue {
public:
    THostnameProperty() :
        TStringValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_HOSTNAME,
                        "Container hostname",
                        staticProperty) {}
};

class TBindDnsProperty : public TBoolValue, public TContainerValue {
public:
    TBindDnsProperty() :
        TBoolValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_BIND_DNS,
                        "Bind /etc/resolv.conf and /etc/hosts of host to container",
                        staticProperty) {}

    bool GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return false;

        if (!c->Prop->GetBool("isolate"))
            return false;
        else if (c->Prop->IsDefault("root"))
            return false;
        else
            return true;
    }
};

class TBindProperty : public TListValue, public TContainerValue {
    std::vector<TBindMap> BindMap;

public:
    TBindProperty() :
        TListValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_BIND,
                        "Share host directories with container",
                        staticProperty) {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        if (lines.size())

        BindMap.clear();

        for (auto &line : lines) {
            std::vector<std::string> tok;
            TBindMap m;

            TError error = SplitEscapedString(line, ' ', tok);
            if (error)
                return error;

            if (tok.size() != 2 && tok.size() != 3)
                return TError(EError::InvalidValue, "Invalid bind in: " + line);

            m.Source = tok[0];
            m.Dest = tok[1];
            m.Rdonly = false;

            if (tok.size() == 3) {
                if (tok[2] == "ro")
                    m.Rdonly = true;
                else if (tok[2] == "rw")
                    m.Rdonly = false;
                else
                    return TError(EError::InvalidValue, "Invalid bind type in: " + line);
            }

            if (!m.Source.Exists())
                return TError(EError::InvalidValue, "Source bind " + m.Source.ToString() + " doesn't exist");

            BindMap.push_back(m);
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->BindMap = BindMap;
        return TError::Success();
    }
};

class TDefaultGwProperty : public TStringValue, public TContainerValue {
    struct TNlAddr Addr;
public:
    TDefaultGwProperty() :
        TStringValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | HIDDEN_VALUE),
        TContainerValue(P_DEFAULT_GW,
                        "Default gateway",
                        staticProperty) {}

    std::string GetDefault() const override {
        return "0.0.0.0";
    }

    TError CheckValue(const std::string &value) override {
        return Addr.Parse(value);
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->DefaultGw = Addr;
        return TError::Success();
    }
};

class TIpProperty : public TListValue, public TContainerValue {
    std::map<std::string, TIpMap> IpMap;

public:
    TIpProperty() :
        TListValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | HIDDEN_VALUE),
        TContainerValue(P_IP,
                        "IP configuration",
                        staticProperty) {}

    TStrList GetDefault() const override {
        return TStrList{ "- 0.0.0.0/0" };
    }

    TError CheckValue(const std::vector<std::string> &lines) override {
        IpMap.clear();
        for (auto &line : lines) {
            std::vector<std::string> settings;
            TError error = SplitEscapedString(line, ' ', settings);
            if (error)
                return error;

            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid address/prefix in: " + line);

            TIpMap ip;
            error = ParseIpPrefix(settings[1], ip.Addr, ip.Prefix);
            if (error)
                return error;

            IpMap[settings[0]] = ip;
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->IpMap = IpMap;
        return TError::Success();
    }
};

class TNetProperty : public TListValue, public TContainerValue {
    TNetCfg NetCfg;

public:
    TNetProperty() :
        TListValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_NET,
                        "Container network settings",
                        staticProperty) {}

    TStrList GetDefault() const override {
        return TStrList{ "host" };
    }

    TError CheckValue(const std::vector<std::string> &lines) override {
        if (!config().network().enabled())
            return TError(EError::Unknown, "Network support is disabled");

        bool none = false;
        NetCfg.Share = false;
        NetCfg.Host.clear();
        NetCfg.MacVlan.clear();
        NetCfg.Veth.clear();
        int idx = 0;

        if (lines.size() == 0)
            return TError(EError::InvalidValue, "Configuration is not specified");

        auto c = GetContainer();

        for (auto &line : lines) {
            if (none)
                return TError(EError::InvalidValue,
                              "none can't be mixed with other types");

            std::vector<std::string> settings;

            TError error = SplitEscapedString(line, ' ', settings);
            if (error)
                return error;

            if (settings.size() == 0)
                return TError(EError::InvalidValue, "Invalid net in: " + line);

            std::string type = StringTrim(settings[0]);

            if (NetCfg.Share)
                return TError(EError::InvalidValue,
                              "host can't be mixed with other settings");

            if (type == "none") {
                none = true;
            } else if (type == "host") {
                THostNetCfg hnet;

                if (settings.size() > 2)
                    return TError(EError::InvalidValue, "Invalid net in: " + line);

                if (settings.size() == 1) {
                    NetCfg.Share = true;
                } else {
                    hnet.Dev = StringTrim(settings[1]);

                    auto link = c->ValidLink(hnet.Dev);
                    if (!link)
                        return TError(EError::InvalidValue,
                                      "Invalid host interface " + hnet.Dev);

                    NetCfg.Host.push_back(hnet);
                }
            } else if (type == "macvlan") {
                // macvlan <master> <name> [type] [mtu] [hw]

                if (settings.size() < 3)
                    return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

                std::string master = StringTrim(settings[1]);
                std::string name = StringTrim(settings[2]);
                std::string type = "bridge";
                std::string hw = "";
                int mtu = -1;

                auto link = c->GetLink(master);
                if (!link)
                    return TError(EError::InvalidValue,
                                  "Invalid macvlan master " + master);

                if (settings.size() > 3) {
                    type = StringTrim(settings[3]);
                    if (!TNlLink::ValidMacVlanType(type))
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan type " + type);
                }

                if (settings.size() > 4) {
                    TError error = StringToInt(settings[4], mtu);
                    if (error)
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan mtu " + settings[4]);
                }

                if (settings.size() > 5) {
                    hw = StringTrim(settings[5]);
                    if (!TNlLink::ValidMacAddr(hw))
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan address " + hw);
                }

                int idx = link->FindIndex(master);
                if (idx < 0)
                    return TError(EError::InvalidValue, "Interface " + master + " doesn't exist or not in running state");

                TMacVlanNetCfg mvlan;
                mvlan.Master = master;
                mvlan.Name = name;
                mvlan.Type = type;
                mvlan.Hw = hw;
                mvlan.Mtu = mtu;

                NetCfg.MacVlan.push_back(mvlan);
            } else if (type == "veth") {
                // veth <name> <bridge> [mtu] [hw]

                if (settings.size() < 3)
                    return TError(EError::InvalidValue, "Invalid veth in: " + line);
                std::string name = StringTrim(settings[1]);
                std::string bridge = StringTrim(settings[2]);
                std::string hw = "";
                int mtu = -1;

                if (settings.size() > 3) {
                    TError error = StringToInt(settings[3], mtu);
                    if (error)
                        return TError(EError::InvalidValue,
                                      "Invalid veth mtu " + settings[3]);
                }

                if (settings.size() > 4) {
                    hw = StringTrim(settings[4]);
                    if (!TNlLink::ValidMacAddr(hw))
                        return TError(EError::InvalidValue,
                                      "Invalid veth address " + hw);
                }

                if (!c->ValidLink(bridge))
                    return TError(EError::InvalidValue, "Interface " + bridge + " doesn't exist or not in running state");

                TVethNetCfg veth;
                veth.Bridge = bridge;
                veth.Name = name;
                veth.Hw = hw;
                veth.Mtu = mtu;
                veth.Peer = "portove-" + std::to_string(c->GetId()) + "-" + std::to_string(idx++);

                NetCfg.Veth.push_back(veth);
            } else {
                return TError(EError::InvalidValue, "Configuration is not specified");
            }
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->NetCfg = NetCfg;
        return TError::Success();
    }
};

class TAllowedDevicesProperty : public TListValue, public TContainerValue {
public:
    TAllowedDevicesProperty() :
        TListValue(PARENT_RO_PROPERTY | PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_ALLOWED_DEVICES,
                        "Devices that container can create/read/write",
                        staticProperty) {}

    TStrList GetDefault() const override {
        if (GetContainer()->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS)
            return TStrList{
                "c 1:3 rwm", "c 1:5 rwm", "c 1:7 rwm", "c 1:9 rwm",
                "c 1:8 rwm", "c 136:* rw", "c 5:2 rwm", "c 254:0 rm",
                "c 254:0 rm", "c 10:237 rmw", "b 7:* rmw"
            };

        return TStrList{ "a *:* rwm" };
    }
};

struct TCapDesc {
    uint64_t id;
    int flags;
};

#define RESTRICTED_CAP 1

class TCapabilitiesProperty : public TListValue, public TContainerValue {
    uint64_t Caps;
    const std::map<std::string, TCapDesc> supported = {
        { "CHOWN",              { CAP_CHOWN, RESTRICTED_CAP } },
        { "DAC_OVERRIDE",       { CAP_DAC_OVERRIDE, RESTRICTED_CAP } },
        { "DAC_READ_SEARCH",    { CAP_DAC_READ_SEARCH, 0 } },
        { "FOWNER",             { CAP_FOWNER, RESTRICTED_CAP } },
        { "FSETID",             { CAP_FSETID, RESTRICTED_CAP } },
        { "KILL",               { CAP_KILL, RESTRICTED_CAP } },
        { "SETGID",             { CAP_SETGID, RESTRICTED_CAP } },
        { "SETUID",             { CAP_SETUID, RESTRICTED_CAP } },
        { "SETPCAP",            { CAP_SETPCAP, 0 } },
        { "LINUX_IMMUTABLE",    { CAP_LINUX_IMMUTABLE, 0 } },
        { "NET_BIND_SERVICE",   { CAP_NET_BIND_SERVICE, RESTRICTED_CAP } },
        { "NET_BROADCAST",      { CAP_NET_BROADCAST, 0 } },
        { "NET_ADMIN",          { CAP_NET_ADMIN, RESTRICTED_CAP } },
        { "NET_RAW",            { CAP_NET_RAW, RESTRICTED_CAP } },
        { "IPC_LOCK",           { CAP_IPC_LOCK, RESTRICTED_CAP } },
        { "IPC_OWNER",          { CAP_IPC_OWNER, 0 } },
        { "SYS_MODULE",         { CAP_SYS_MODULE, 0 } },
        { "SYS_RAWIO",          { CAP_SYS_RAWIO, 0 } },
        { "SYS_CHROOT",         { CAP_SYS_CHROOT, RESTRICTED_CAP } },
        { "SYS_PTRACE",         { CAP_SYS_PTRACE, 0 } },
        { "SYS_PACCT",          { CAP_SYS_PACCT, 0 } },
        { "SYS_ADMIN",          { CAP_SYS_ADMIN, 0 } },
        { "SYS_BOOT",           { CAP_SYS_BOOT, 0 } },
        { "SYS_NICE",           { CAP_SYS_NICE, 0 } },
        { "SYS_RESOURCE",       { CAP_SYS_RESOURCE, RESTRICTED_CAP } },
        { "SYS_TIME",           { CAP_SYS_TIME, 0 } },
        { "SYS_TTY_CONFIG",     { CAP_SYS_TTY_CONFIG, 0 } },
        { "MKNOD",              { CAP_MKNOD, 0 } },
        { "LEASE",              { CAP_LEASE, 0 } },
        { "AUDIT_WRITE",        { CAP_AUDIT_WRITE, 0 } },
        { "AUDIT_CONTROL",      { CAP_AUDIT_CONTROL, 0 } },
        { "SETFCAP",            { CAP_SETFCAP, 0 } },
        { "MAC_OVERRIDE",       { CAP_MAC_OVERRIDE, 0 } },
        { "MAC_ADMIN",          { CAP_MAC_ADMIN, 0 } },
        { "SYSLOG",             { CAP_SYSLOG, 0 } },
        { "WAKE_ALARM",         { CAP_WAKE_ALARM, 0 } },
        { "BLOCK_SUSPEND",      { CAP_BLOCK_SUSPEND, 0 } },
    };

public:
    TCapabilitiesProperty() :
        TListValue(PERSISTENT_VALUE | OS_MODE_PROPERTY | SUPERUSER_PROPERTY),
        TContainerValue(P_CAPABILITIES,
                        "Limit container capabilities",
                        staticProperty) {}

    TStrList GetDefault() const override {
        TStrList v;
        auto c = GetContainer();

        bool root = c->Cred.IsRoot();
        bool restricted = c->Prop->GetInt(P_VIRT_MODE) == VIRT_MODE_OS;

        for (auto kv : supported)
            if (root || (restricted && kv.second.flags & RESTRICTED_CAP))
                v.push_back(kv.first);
        return v;
    }


    TError CheckValue(const std::vector<std::string> &lines) override {
        uint64_t allowed = 0;

        for (auto &line: lines) {
            if (supported.find(line) == supported.end())
                return TError(EError::InvalidValue,
                              "Unsupported capability " + line);

            allowed |= (1ULL << supported.at(line).id);
        }

        Caps = allowed;

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->Caps = Caps;
        return TError::Success();
    }
};

class TVirtModeProperty : public TIntValue, public TContainerValue {
public:
    TVirtModeProperty() :
        TIntValue(PERSISTENT_VALUE | RESTROOT_PROPERTY),
        TContainerValue(P_VIRT_MODE,
                        "Virtualization mode: os or app",
                        staticProperty) {}

    TError CheckValue(const int &value) override {
        if (value != VIRT_MODE_APP && value != VIRT_MODE_OS)
            return TError(EError::InvalidValue, std::string("Unsupported ") + P_VIRT_MODE);




        return TError::Success();
    }

    TError FromString(const std::string &value) override {
        if (value == "os")
            return Set(VIRT_MODE_OS);
        else if (value == "app")
            return Set(VIRT_MODE_APP);
        else
            return TError(EError::InvalidValue, std::string("Unsupported ") + P_VIRT_MODE + ": " + value);

    }
};

class TRawIdProperty : public TIntValue, public TContainerValue {
public:
    TRawIdProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ID, "", anyState) {}
};

class TRawRootPidProperty : public TIntValue, public TContainerValue {
public:
    TRawRootPidProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ROOT_PID, "", anyState) {}
};

class TRawLoopDevProperty : public TIntValue, public TContainerValue {
public:
    TRawLoopDevProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_LOOP_DEV, "", anyState) {}
    int GetDefault() const override { return -1; }
};

class TRawNameProperty : public TStringValue, public TContainerValue {
public:
    TRawNameProperty() :
        TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_NAME, "", anyState) {}
};

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c) {
    std::vector<TAbstractValue *> properties = {
        new TCommandProperty,
        new TUserProperty,
        new TGroupProperty,
        new TEnvProperty,
        new TRootProperty,
        new TRootRdOnlyProperty,
        new TCwdProperty,
        new TStdinPathProperty,
        new TStdoutPathProperty,
        new TStderrPathProperty,
        new TStdoutLimitProperty,
        new TMemoryGuaranteeProperty,
        new TMemoryLimitProperty,
        new TRechargeOnPgfaultProperty,
        new TCpuPolicyProperty,
        new TCpuPriorityProperty,
        new TNetGuaranteeProperty,
        new TNetCeilProperty,
        new TNetPriorityProperty,
        new TRespawnProperty,
        new TMaxRespawnsProperty,
        new TIsolateProperty,
        new TPrivateProperty,
        new TUlimitProperty,
        new THostnameProperty,
        new TBindDnsProperty,
        new TBindProperty,
        new TNetProperty,
        new TAllowedDevicesProperty,
        new TCapabilitiesProperty,
        new TIpProperty,
        new TDefaultGwProperty,
        new TVirtModeProperty,

        new TRawIdProperty,
        new TRawRootPidProperty,
        new TRawLoopDevProperty,
        new TRawNameProperty,
    };

    for (auto p : properties)
        AddContainerValue(m, c, p);
}
