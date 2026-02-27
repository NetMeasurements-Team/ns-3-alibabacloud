// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-utils.h"
#include "ns3/command-line.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <string>

#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

using namespace utils;

std::string FormatTime(double time_us)
{
    double val = time_us;
    const char* unit = " us";
    int precision = 0;
    if (time_us >= 1e6) {
        val = time_us / 1e6;
        unit = " s";
        precision = 6;
    } else if (time_us >= 1e3) {
        val = time_us / 1e3;
        unit = " ms";
        precision = 3;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val << unit;
    return oss.str();
}

void CheckNoProgress(double sim_time_us, std::ostringstream& oss)
{
    static uint32_t last_completed_tasks = 0;
    static double last_progress_time_us = 0;
    uint32_t completed_tasks = 0;
    
    // Count completed tasks
    for (auto& task : UbTrafficGen::Get()->m_taskStates) {
        if (task.second == UbTrafficGen::TaskState::COMPLETED) {
            completed_tasks++;
        }
    }

    // If new tasks are completed, update status
    if (completed_tasks > last_completed_tasks) {
        last_completed_tasks = completed_tasks;
        last_progress_time_us = sim_time_us;
    }

    // If no new task is completed for more than 10ms, and total time exceeds 10ms, warn about possible deadlock or congestion
    if (sim_time_us - last_progress_time_us > 10000 && sim_time_us > 10000) {
         oss << " [WARNING: No task completed for " << FormatTime(sim_time_us - last_progress_time_us) << "]";
    }
}

void CheckExampleProcess()
{
    double sim_time_us = Simulator::Now().GetMicroSeconds();
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "] "
        << "Simulation time progress: " << FormatTime(sim_time_us);

    CheckNoProgress(sim_time_us, oss);

    std::cout << "\r" << oss.str() << std::flush;
    if (!UbTrafficGen::Get()->IsCompleted()) {
            Simulator::Schedule(MicroSeconds(100), &CheckExampleProcess);
            return;
    }
    std::cout << std::endl;
    Simulator::Stop();
    return;
}

// Execute test case based on configuration file path
void ConfigCase(const string& configPath)
{
    RngSeedManager::SetSeed(10);
    string LoadConfigFilePath = configPath + "/network_attribute.txt";
    UbUtils::Get()->SetComponentsAttribute(LoadConfigFilePath);
    UbUtils::Get()->CreateTraceDir();
    string NodeConfigFile = configPath + "/node.csv";
    UbUtils::Get()->CreateNode(NodeConfigFile);
    string TopoConfigFile = configPath + "/topology.csv";
    UbUtils::Get()->CreateTopo(TopoConfigFile);
    string RouterConfigFile = configPath + "/routing_table.csv";
    UbUtils::Get()->AddRoutingTable(RouterConfigFile);
    string TpConfigFile = configPath + "/transport_channel.csv";
    UbUtils::Get()->CreateTp(TpConfigFile);
    UbUtils::Get()->TopoTraceConnect();
}

void InitiateTasks(const string& configPath)
{
    string TrafficConfigFile = configPath + "/traffic.csv";
    auto trafficData = UbUtils::Get()->ReadTrafficCSV(TrafficConfigFile);
    BooleanValue gFaultEnable;
    UbUtils::Get()->g_fault_enable.GetValue(gFaultEnable);
    if (gFaultEnable.Get()) {
        string FaultConfigFile = configPath + "/fault.csv";
        UbUtils::Get()->InitFaultMoudle(FaultConfigFile);
    }
    // Iterate through Traffic data and start client
    UbUtils::Get()->PrintTimestamp ("Start Client.");
    for (auto& record : trafficData) {
        auto node = NodeList::GetNode (record.sourceNode);
        if (node->GetNApplications()==0) {
            Ptr<UbApp> client = CreateObject<UbApp>();
            node->AddApplication (client);
            UbUtils::Get()->ClientTraceConnect(record.sourceNode);
        }
        UbTrafficGen::Get()->AddTask(record);
    }
    UbTrafficGen::Get()->ScheduleNextTasks();
    CheckExampleProcess();
}

// Execute test case based on configuration file path
int main(int argc, char* argv[])
{
    // First check if querying attribute info (use independent argument check, do not trigger strict parsing)
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg.find("--ClassName") == 0) {
            if (UbUtils::Get()->QueryAttributeInfo(argc, argv))
                return 0;
            break;
        }
    }

    // Multi-thread acceleration configuration (needs to be enabled at compile time: ./ns3 configure --enable-mtp)
    // Specify number of threads at runtime via --mtp-threads=N (0-1=disable, >=2 enable)
    uint32_t mtpThreads = 0;
    CommandLine cmd;
    cmd.AddValue("mtp-threads", "Number of MTP threads (0-1 to disable, >=2 to enable)", mtpThreads);
    cmd.Parse(argc, argv);

#ifdef NS3_MTP
    if (mtpThreads > 1) {
        MtpInterface::Enable(mtpThreads);
        std::cout << "[INFO] MTP enabled with " << mtpThreads << " threads." << std::endl;
    }
#else
    if (mtpThreads > 1) {
        std::cerr << "[WARNING] MTP requested but not compiled. Reconfigure with --enable-mtp" << std::endl;
    }
#endif

    // Start timing
    auto start = std::chrono::high_resolution_clock::now();
    Time::SetResolution(Time::PS);

    // Add time prefix to logs
    ns3::LogComponentEnableAll(LOG_PREFIX_TIME);

    // Example: Set log level for specific component, set time prefix for specific component
    // LogComponentEnable("UbApp", LOG_LEVEL_INFO);
    // LogComponentEnable("UbApp", LOG_PREFIX_TIME);

    // Enable logs
    LogComponentEnable("UbSwitchAllocator", LOG_LEVEL_WARN);
    LogComponentEnable("UbQueueManager", LOG_LEVEL_WARN);
    LogComponentEnable("UbCaqm", LOG_LEVEL_WARN);
    LogComponentEnable("UbTrafficGen", LOG_LEVEL_WARN);
    LogComponentEnable("UbApp", LOG_LEVEL_WARN);
    LogComponentEnable("UbCongestionControl", LOG_LEVEL_WARN);
    LogComponentEnable("UbController", LOG_LEVEL_WARN);
    LogComponentEnable("UbDataLink", LOG_LEVEL_WARN);
    LogComponentEnable("UbFlowControl", LOG_LEVEL_WARN);
    LogComponentEnable("UbHeader", LOG_LEVEL_WARN);
    LogComponentEnable("UbLink", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstInstance", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstThread", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstApi", LOG_LEVEL_WARN);
    LogComponentEnable("UbPort", LOG_LEVEL_WARN);
    LogComponentEnable("UbRoutingProcess", LOG_LEVEL_WARN);
    LogComponentEnable("UbSwitch", LOG_LEVEL_WARN);
    LogComponentEnable("UbFunction", LOG_LEVEL_WARN);
    LogComponentEnable("UbTransportChannel", LOG_LEVEL_WARN);
    LogComponentEnable("UbFault", LOG_LEVEL_WARN);
    LogComponentEnable("UbTransaction", LOG_LEVEL_WARN);
    LogComponentEnable("TpConnectionManager", LOG_LEVEL_WARN);

    // Configuration file path
    string configPath = "scratch/2nodes_single-tp";
    if (argc > 1) {
        configPath = string(argv[1]);
    }

    // Read configuration file and execute test case
    string runCase = "Run case: " + configPath;
    UbUtils::Get()->PrintTimestamp(runCase);
    ConfigCase(configPath);
    Simulator::Schedule(Time(0), InitiateTasks, configPath);
    auto sim_wall_start = std::chrono::high_resolution_clock::now();
    Simulator::Run();
    auto sim_wall_end = std::chrono::high_resolution_clock::now();
    UbUtils::Get()->Destroy();
    Simulator::Destroy();
    UbUtils::Get()->PrintTimestamp("Simulator finished!");
    auto trace_wall_start = std::chrono::high_resolution_clock::now();
    UbUtils::Get()->ParseTrace();

    auto end = std::chrono::high_resolution_clock::now();
    UbUtils::Get()->PrintTimestamp("Program finished.");
    // Phased wall-clock time statistics (unit: seconds)
    double config_wall_s = std::chrono::duration_cast<std::chrono::microseconds>(sim_wall_start - start).count() / 1e6;
    double run_wall_s    = std::chrono::duration_cast<std::chrono::microseconds>(sim_wall_end - sim_wall_start).count() / 1e6;
    double trace_wall_s  = std::chrono::duration_cast<std::chrono::microseconds>(end - trace_wall_start).count() / 1e6;
    double total_wall_s  = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1e6;
    UbUtils::Get()->PrintTimestamp("Wall-clock (config phase): " + std::to_string(config_wall_s) + " s");
    UbUtils::Get()->PrintTimestamp("Wall-clock (run phase): " + std::to_string(run_wall_s) + " s");
    UbUtils::Get()->PrintTimestamp("Wall-clock (trace phase): " + std::to_string(trace_wall_s) + " s");
    UbUtils::Get()->PrintTimestamp("Wall-clock (total): " + std::to_string(total_wall_s) + " s");
    return 0;
}
