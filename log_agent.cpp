/**
 *    Copyright (C) 2025 EloqData Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under either of the following two licenses:
 *    1. GNU Affero General Public License, version 3, as published by the Free
 *    Software Foundation.
 *    2. GNU General Public License as published by the Free Software
 *    Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License or GNU General Public License for more
 *    details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    and GNU General Public License V2 along with this program.  If not, see
 *    <http://www.gnu.org/licenses/>.
 *
 */
#include "log_agent.h"

#include <braft/raft.h>
#include <braft/route_table.h>
#include <braft/util.h>
#include <brpc/channel.h>
#include <brpc/controller.h>
#include <bthread/bthread.h>

#include <thread>

#include "log_util.h"
#include "log.pb.h"

namespace txlog
{
static const uint32_t RefreshThreshold = 5;

/**
 * @brief Initialize all the log service stub and store them in log_stub_map_.
 * For each log request, we use the current cached log_group_leader to search
 * the stub directly.
 *
 * Note that stub CallMethods are thread safe, which can be shared by
 * Txprocessors and replay/recover threads.
 *
 */
void LogAgent::Init(std::vector<std::string> &ip_list,
                    std::vector<uint16_t> &port_list,
                    const uint32_t start_log_group_id,
                    const uint32_t log_group_replica_num)
{
    ips_ = ip_list;
    ports_ = port_list;
    std::pair<std::unordered_map<uint32_t, LogUtil::RaftGroupConfig>, uint32_t>
        log_raft_group_config = LogUtil::GenerateLogRaftGroupConfig(
            ips_, ports_, start_log_group_id, log_group_replica_num);

    LogUtil::DumpLogRaftGroupConfig(log_raft_group_config);

    log_group_replica_num_ = log_raft_group_config.second;
    log_group_cnt_ = log_raft_group_config.first.size();

    for (auto &[log_group_id, raft_group_config] : log_raft_group_config.first)
    {
        std::string log_group_name = raft_group_config.group_name_;
        std::string log_group_conf = raft_group_config.group_conf_;
        if (braft::rtb::update_configuration(log_group_name, log_group_conf) !=
            0)
        {
            LOG(ERROR) << "Fail to register in the routing table the log group "
                       << log_group_conf;
        }

        log_group_ids_.push_back(log_group_id);
        lg_leader_cache_.try_emplace(log_group_id, 0);
        std::vector<uint32_t> group_nodes = raft_group_config.group_nodes_;
        for (uint32_t node_id : group_nodes)
        {
            auto it = log_channel_map_.find(node_id);
            if (it != log_channel_map_.end())
            {
                continue;
            }

            // initialize the channel and stub map.
            auto channel_it = log_channel_map_.try_emplace(
                node_id, std::make_shared<brpc::Channel>());
            brpc::Channel &channel = *channel_it.first->second;

            brpc::ChannelOptions options;
            // The original timeout is 500ms, which is too short to finish the
            // raft log request. Increase it to 10 seconds.
            options.timeout_ms = 10000;
            options.max_retry = 3;
            butil::ip_t ip_t;
            if (0 != butil::str2ip(ips_[node_id].c_str(), &ip_t))
            {
                // for case `ips_[node_id]` is hostname format.
                std::string naming_service_url;
                braft::HostNameAddr hostname_addr(ips_[node_id],
                                                  ports_[node_id]);
                braft::HostNameAddr2NSUrl(hostname_addr, naming_service_url);
                if (channel.Init(naming_service_url.c_str(),
                                 braft::LOAD_BALANCER_NAME,
                                 &options) != 0)
                {
                    LOG(ERROR)
                        << "Fail to init channel to " << ips_[node_id].c_str()
                        << ":"
                        << ports_[node_id];  // convert to log service port
                    return;
                }
            }
            else
            {
                if (channel.Init(
                        ips_[node_id].c_str(),
                        static_cast<int>(
                            ports_[node_id]),  // convert to log service port
                        &options) != 0)
                {
                    LOG(ERROR)
                        << "Fail to init channel to " << ips_[node_id].c_str()
                        << ":"
                        << ports_[node_id];  // convert to log service port
                    return;
                }
            }
        }
    }
}

std::shared_ptr<brpc::Channel> LogAgent::GetChannel(uint32_t log_group_id)
{
    std::shared_lock<std::shared_mutex> lock(config_map_mutex_);
    uint32_t node_id =
        lg_leader_cache_.at(log_group_id).load(std::memory_order_acquire);

    auto log_channel_it = log_channel_map_.find(node_id);
    if (log_channel_it == log_channel_map_.end())
    {
        return nullptr;
    }
    return log_channel_it->second;
}

int LogAgent::RefreshLeader(uint32_t log_group_id, int timeout_ms)
{
    return 0;
}

void LogAgent::WriteLog(uint32_t log_group_id,
                        brpc::Controller *controller,
                        const LogRequest *request,
                        LogResponse *response,
                        ::google::protobuf::Closure *done)
{
    auto channel = GetChannel(log_group_id);
    if (channel == nullptr)
    {
        controller->SetFailed("Log channel not found for group " +
                              std::to_string(log_group_id));
        return;
    }
    LogService_Stub stub(channel.get());
    stub.WriteLog(controller, request, response, done);
}

void LogAgent::CheckMigrationIsFinished(
    uint32_t log_group_id,
    brpc::Controller *controller,
    const CheckMigrationIsFinishedRequest *request,
    CheckMigrationIsFinishedResponse *response,
    ::google::protobuf::Closure *done)
{
    auto channel = GetChannel(log_group_id);
    if (channel == nullptr)
    {
        controller->SetFailed("Log channel not found for group " +
                              std::to_string(log_group_id));
        return;
    }
    LogService_Stub stub(channel.get());
    stub.CheckMigrationIsFinished(controller, request, response, done);
}

CheckClusterScaleStatusResponse::Status LogAgent::CheckClusterScaleStatus(
    uint32_t log_group_id, const std::string &id)
{
    assert(log_group_id == 0);

    brpc::Controller cntl;
    LogRequest request;
    CheckClusterScaleStatusRequest *check_cluster_scale_status_req =
        request.mutable_check_scale_status_request();
    check_cluster_scale_status_req->set_id(id);
    check_cluster_scale_status_req->set_log_group_id(log_group_id);
    LogResponse response;

    auto channel = GetChannel(log_group_id);
    if (channel == nullptr)
    {
        LOG(ERROR)
            << "CheckClusterScaleStatus: Log channel not found for group "
            << log_group_id;
        return CheckClusterScaleStatusResponse::Status::
            CheckClusterScaleStatusResponse_Status_UNKNOWN;
    }
    LogService_Stub stub(channel.get());

    cntl.Reset();
    cntl.set_timeout_ms(300);

    stub.CheckClusterScaleStatus(&cntl, &request, &response, nullptr);
    if (cntl.Failed())
    {
        LOG(INFO) << "Failed to send RPC#CheckClusterScaleStatus, error text:"
                  << cntl.ErrorText();
        return CheckClusterScaleStatusResponse::Status::
            CheckClusterScaleStatusResponse_Status_UNKNOWN;
    }

    if (response.response_status() !=
        LogResponse::ResponseStatus::LogResponse_ResponseStatus_Success)
    {
        return CheckClusterScaleStatusResponse::Status::
            CheckClusterScaleStatusResponse_Status_UNKNOWN;
    }

    return response.check_scale_status_response().status();
}

void LogAgent::UpdateCheckpointTs(uint32_t cc_node_group_id,
                                  int64_t term,
                                  uint64_t checkpoint_timestamp)
{
    brpc::Controller cntl;
    // prepare request
    LogRequest req;
    UpdateCheckpointTsRequest *update_ckpt_ts_req =
        req.mutable_update_ckpt_ts_request();
    update_ckpt_ts_req->set_cc_node_group_id(cc_node_group_id);
    update_ckpt_ts_req->set_cc_ng_term(term);
    update_ckpt_ts_req->set_ckpt_timestamp(checkpoint_timestamp);
    LogResponse resp;
    std::shared_lock<std::shared_mutex> lock(config_map_mutex_);
    for (const auto &[lg_id, node_id] : lg_leader_cache_)
    {
        DLOG(INFO) << "UpdateCheckpointTs lg_id:" << lg_id
                   << " node_id:" << node_id.load(std::memory_order_acquire)
                   << " ckpt_ts:" << checkpoint_timestamp;
        cntl.Reset();
        cntl.set_timeout_ms(100);
        update_ckpt_ts_req->set_log_group_id(lg_id);
        auto channel = GetChannel(lg_id);
        if (channel == nullptr)
        {
            LOG(ERROR) << "UpdateCheckpointTs: Log channel not found for group "
                       << lg_id;
            continue;
        }
        LogService_Stub stub(channel.get());
        stub.UpdateCheckpointTs(&cntl, &req, &resp, nullptr);
    }
}

void LogAgent::ReplayLog(uint32_t cc_node_group_id,
                         int64_t term,
                         const std::string &source_ip,
                         uint16_t source_port,
                         int log_group,
                         uint64_t start_ts,
                         std::atomic<bool> &interrupt,
                         bool no_replay)
{
    LogRequest req;
    ReplayLogRequest *replay_log_req = req.mutable_replay_log_request();
    replay_log_req->set_cc_node_group_id(cc_node_group_id);
    replay_log_req->set_term(term);
    replay_log_req->set_source_ip(source_ip);
    replay_log_req->set_source_port(source_port);
    replay_log_req->set_no_replay(no_replay);
    replay_log_req->set_start_ts(start_ts);
    LogResponse resp;
    resp.set_response_status(
        LogResponse::ResponseStatus::LogResponse_ResponseStatus_Fail);

    std::vector<uint32_t> dest_log_groups;
    if (log_group < 0)  // send ReplayLogRequest to all log groups
    {
        dest_log_groups = log_group_ids_;
    }
    else
    {
        dest_log_groups.push_back(log_group);
    }

    for (auto log_group_id : dest_log_groups)
    {
        // While loop to make sure recover is finished. If not re-sending the
        // message until it succeeds. (Or, the cc node is considered not
        // finishing failover and not being able to serve anyway.)
        while (true)
        {
            auto channel = GetChannel(log_group_id);
            if (channel == nullptr)
            {
                LOG(ERROR)
                    << "Failed to replay log, log channel not found for group "
                    << log_group_id;
                continue;
            }
            LogService_Stub stub(channel.get());

            replay_log_req->set_log_group_id(log_group_id);
            resp.clear_replay_log_response();
            brpc::Controller cntl;
            cntl.Reset();
            stub.ReplayLog(&cntl, &req, &resp, nullptr);

            auto res_status = resp.response_status();
            if (!cntl.Failed() && res_status ==
                                      LogResponse::ResponseStatus::
                                          LogResponse_ResponseStatus_Success)
            {
                break;
            }

            if (cntl.Failed())
            {
                LOG(ERROR) << "Failed to ReplayLog, cntl failed";
            }

            if (res_status !=
                LogResponse::ResponseStatus::LogResponse_ResponseStatus_Success)
            {
                LOG(ERROR) << "Failed to ReplayLog, status code: "
                           << res_status;
            }

            using namespace std::chrono_literals;
            std::this_thread::sleep_for(2s);

            if (interrupt.load(std::memory_order_acquire))
            {
                return;
            }
        }
    }
}

void LogAgent::RemoveCcNodeGroup(uint32_t cc_node_group_id, int64_t term)
{
    brpc::Controller cntl;
    // prepare request
    LogRequest req;
    RemoveCcNodeGroupRequest *remove_cc_node_group_req =
        req.mutable_remove_cc_node_group_request();
    remove_cc_node_group_req->set_cc_node_group_id(cc_node_group_id);
    remove_cc_node_group_req->set_cc_ng_term(term);
    LogResponse resp;
    std::shared_lock<std::shared_mutex> lock(config_map_mutex_);
    for (const auto &[lg_id, node_id] : lg_leader_cache_)
    {
        DLOG(INFO) << "RemoveCcNodeGroup lg_id:" << lg_id
                   << " node_id:" << node_id.load(std::memory_order_acquire)
                   << ", cc_node_group_id:" << cc_node_group_id
                   << ", term:" << term;
        cntl.Reset();
        cntl.set_timeout_ms(100);
        remove_cc_node_group_req->set_log_group_id(lg_id);
        auto channel = GetChannel(lg_id);
        if (channel == nullptr)
        {
            LOG(ERROR) << "RemoveCcNodeGroup: Log channel not found for group "
                       << lg_id;
            continue;
        }
        LogService_Stub stub(channel.get());
        stub.RemoveCcNodeGroup(&cntl, &req, &resp, nullptr);
    }
}

RecoverTxResponse_TxStatus LogAgent::RecoverTx(uint64_t lock_tx_number,
                                               int64_t lock_tx_coord_term,
                                               uint64_t write_lock_ts,
                                               uint32_t cc_ng_id,
                                               int64_t cc_ng_term,
                                               const std::string &source_ip,
                                               uint16_t source_port,
                                               uint32_t log_group_id)
{
    RecoverTxRequest request;
    RecoverTxResponse response;

    // Create stub on the fly using channel
    auto channel = GetChannel(log_group_id);
    if (channel == nullptr)
    {
        LOG(ERROR) << "RecoverTx: Log channel not found for group "
                   << log_group_id;
        return RecoverTxResponse_TxStatus_RecoverError;
    }
    LogService_Stub stub(channel.get());

    request.set_lock_tx_number(lock_tx_number);
    request.set_lock_tx_coord_term(lock_tx_coord_term);
    request.set_cc_node_group_id(cc_ng_id);
    request.set_cc_ng_term(cc_ng_term);
    request.set_source_ip(source_ip);
    request.set_source_port(source_port);
    request.set_log_group_id(log_group_id);
    request.set_write_lock_ts(write_lock_ts);

    brpc::Controller cntl;
    stub.RecoverTx(&cntl, &request, &response, nullptr);

    // If there is an error, does nothing. The next conflicting tx will try a
    // new recovery.
    return cntl.Failed() ? RecoverTxResponse_TxStatus_RecoverError
                         : response.tx_status();
}

/**
 * @brief TransferLeader is used by test case only.
 */
void LogAgent::TransferLeader(uint32_t log_group_id, uint32_t leader_idx)
{
    TransferRequest req;
    TransferResponse res;
    req.set_lg_id(log_group_id);
    req.set_leader_idx(leader_idx);

    auto channel = GetChannel(log_group_id);
    if (channel == nullptr)
    {
        LOG(ERROR) << "TransferLeader: Log channel not found for group "
                   << log_group_id;
        return;
    }
    LogService_Stub stub(channel.get());
    brpc::Controller cntl;
    cntl.set_timeout_ms(-1);
    stub.TransferLeader(&cntl, &req, &res, nullptr);
    if (cntl.Failed())
    {
        LOG(ERROR) << "Fail the TransferLeader RPC of log group "
                   << log_group_id << ". Error code: " << cntl.ErrorCode()
                   << ". Msg: " << cntl.ErrorText();
    }
    else if (res.error())
    {
        // TODO: consider retry logic.
        LOG(ERROR) << "Fail to transfer the leader of log group "
                   << log_group_id;
    }
}

void LogAgent::UpdateLeaderCache(uint32_t lg_id, uint32_t node_id)
{
    std::shared_lock<std::shared_mutex> lock(config_map_mutex_);
    lg_leader_cache_.at(lg_id).store(node_id, std::memory_order_release);
}

bool LogAgent::UpdateLogGroupConfig(std::vector<std::string> &ips,
                                    std::vector<uint16_t> &ports,
                                    uint32_t log_group_id)
{
    std::shared_lock<std::shared_mutex> share_lock(config_map_mutex_);

    // Check if the log group id is valid
    bool found = false;
    for (uint32_t i = 0; i < log_group_ids_.size(); ++i)
    {
        if (log_group_ids_[i] == log_group_id)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        LOG(ERROR) << "Log group id " << log_group_id << " not found";
        return false;
    }

    // Create channels to new nodes
    std::unordered_map<uint32_t, std::shared_ptr<brpc::Channel>> new_channels;
    std::vector<std::string> new_ips;
    std::vector<uint16_t> new_ports;
    uint32_t next_node_id = ips_.size();
    for (uint32_t i = 0; i < ips.size(); ++i)
    {
        bool found = false;
        for (uint32_t j = 0; j < ips_.size(); ++j)
        {
            if (ips_[j] == ips[i] && ports_[j] == ports[i])
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            new_ips.push_back(ips[i]);
            new_ports.push_back(ports[i]);
            // Create a new channel
            std::shared_ptr<brpc::Channel> channel =
                std::make_shared<brpc::Channel>();
            brpc::ChannelOptions options;
            // The original timeout is 500ms, which is too short to finish the
            // raft log request. Increase it to 10 seconds.
            options.timeout_ms = 10000;
            options.max_retry = 3;
            butil::ip_t ip_t;
            if (0 != butil::str2ip(ips[i].c_str(), &ip_t))
            {
                // for case `ips_[node_id]` is hostname format.
                std::string naming_service_url;
                braft::HostNameAddr hostname_addr(ips[i], ports[i]);
                braft::HostNameAddr2NSUrl(hostname_addr, naming_service_url);
                if (channel->Init(naming_service_url.c_str(),
                                  braft::LOAD_BALANCER_NAME,
                                  &options) != 0)
                {
                    LOG(ERROR) << "Fail to init channel to " << ips[i].c_str()
                               << ":" << ports[i];
                    return false;
                }
            }
            else
            {
                if (channel->Init(ips[i].c_str(),
                                  static_cast<int>(ports[i]),
                                  &options) != 0)
                {
                    LOG(ERROR) << "Fail to init channel to " << ips[i].c_str()
                               << ":" << ports[i];
                    return false;
                }
            }
            new_channels[next_node_id++] = std::move(channel);
        }
    }
    share_lock.unlock();

    {
        std::lock_guard<std::shared_mutex> lock(config_map_mutex_);
        for (uint32_t i = 0; i < new_ips.size(); ++i)
        {
            ips_.push_back(new_ips[i]);
            ports_.push_back(new_ports[i]);
        }
        log_group_replica_num_ = ips.size() > log_group_replica_num_
                                     ? ips.size()
                                     : log_group_replica_num_;
        for (auto &channel_pair : new_channels)
        {
            log_channel_map_[channel_pair.first] =
                std::move(channel_pair.second);
        }
        lg_leader_cache_.try_emplace(log_group_id, log_group_id);

        std::string log_group_name = "lg" + std::to_string(log_group_id);
        std::string log_group_conf = "";
        for (uint32_t i = 0; i < ips.size(); ++i)
        {
            log_group_conf += ips[i] + ":" + std::to_string(ports[i]) + ":0,";
        }
        log_group_conf.pop_back();
        if (braft::rtb::update_configuration(log_group_name, log_group_conf) !=
            0)
        {
            // update conf should never fail unless log group is empty.
            assert(false);
            LOG(ERROR) << "Fail to register in the routing table the log group "
                       << log_group_conf;
        }
    }

    return true;
}
}  // namespace txlog
