// Copyright (c) 2017-present, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#include "dist/replication/lib/replica_stub.h"
#include "dist/replication/lib/replica.h"

#include "duplication_sync_timer.h"
#include "replica_duplicator_manager.h"

#include <dsn/dist/fmt_logging.h>
#include <dsn/tool-api/command_manager.h>
#include <dsn/utility/output_utils.h>
#include <dsn/utility/string_conv.h>

namespace dsn {
namespace replication {

DEFINE_TASK_CODE(LPC_DUPLICATION_SYNC_TIMER, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)

void duplication_sync_timer::run()
{
    // ensure duplication sync never be concurrent
    if (_rpc_task) {
        ddebug_f("a duplication sync is already ongoing");
        return;
    }

    {
        zauto_lock l(_stub->_state_lock);
        if (_stub->_state == replica_stub::NS_Disconnected) {
            ddebug_f("stop this round of duplication sync because this server is disconnected from "
                     "meta server");
            return;
        }
    }

    auto req = make_unique<duplication_sync_request>();
    req->node = _stub->primary_address();

    // collects confirm points from all primaries on this server
    uint64_t pending_muts_cnt = 0;
    for (const replica_ptr &r : get_all_primaries()) {
        auto confirmed = r->get_duplication_manager()->get_duplication_confirms_to_update();
        if (!confirmed.empty()) {
            req->confirm_list[r->get_gpid()] = std::move(confirmed);
        }
        pending_muts_cnt += r->get_duplication_manager()->get_pending_mutations_count();
    }
    _stub->_counter_dup_pending_mutations_count->set(pending_muts_cnt);

    duplication_sync_rpc rpc(std::move(req), RPC_CM_DUPLICATION_SYNC, 3_s);
    rpc_address meta_server_address(_stub->get_meta_server_address());
    ddebug_f("duplication_sync to meta({})", meta_server_address.to_string());

    zauto_lock l(_lock);
    _rpc_task =
        rpc.call(meta_server_address, &_stub->_tracker, [this, rpc](error_code err) mutable {
            on_duplication_sync_reply(err, rpc.response());
        });
}

void duplication_sync_timer::on_duplication_sync_reply(error_code err,
                                                       const duplication_sync_response &resp)
{
    if (err == ERR_OK && resp.err != ERR_OK) {
        err = resp.err;
    }
    if (err != ERR_OK) {
        derror_f("on_duplication_sync_reply: err({})", err.to_string());
    } else {
        update_duplication_map(resp.dup_map);
    }

    zauto_lock l(_lock);
    _rpc_task = nullptr;
}

void duplication_sync_timer::update_duplication_map(
    const std::map<int32_t, std::map<int32_t, duplication_entry>> &dup_map)
{
    for (replica_ptr &r : get_all_replicas()) {
        auto it = dup_map.find(r->get_gpid().get_app_id());
        if (it == dup_map.end()) {
            // no duplication is assigned to this app
            r->get_duplication_manager()->update_duplication_map({});
        } else {
            r->get_duplication_manager()->update_duplication_map(it->second);
        }
    }
}

duplication_sync_timer::duplication_sync_timer(replica_stub *stub) : _stub(stub) {}

duplication_sync_timer::~duplication_sync_timer() {}

std::vector<replica_ptr> duplication_sync_timer::get_all_primaries()
{
    std::vector<replica_ptr> replica_vec;
    {
        zauto_read_lock l(_stub->_replicas_lock);
        for (auto &kv : _stub->_replicas) {
            replica_ptr r = kv.second;
            if (r->status() != partition_status::PS_PRIMARY) {
                continue;
            }
            replica_vec.emplace_back(std::move(r));
        }
    }
    return replica_vec;
}

std::vector<replica_ptr> duplication_sync_timer::get_all_replicas()
{
    std::vector<replica_ptr> replica_vec;
    {
        zauto_read_lock l(_stub->_replicas_lock);
        for (auto &kv : _stub->_replicas) {
            replica_ptr r = kv.second;
            replica_vec.emplace_back(std::move(r));
        }
    }
    return replica_vec;
}

void duplication_sync_timer::close()
{
    ddebug("stop duplication sync");

    {
        zauto_lock l(_lock);
        if (_rpc_task) {
            _rpc_task->cancel(true);
            _rpc_task = nullptr;
        }
    }

    if (_timer_task) {
        _timer_task->cancel(true);
        _timer_task = nullptr;
    }
}

void duplication_sync_timer::start()
{
    ddebug_f("run duplication sync periodically in {}s", DUPLICATION_SYNC_PERIOD_SECOND);

    _timer_task = tasking::enqueue_timer(LPC_DUPLICATION_SYNC_TIMER,
                                         &_stub->_tracker,
                                         [this]() { run(); },
                                         DUPLICATION_SYNC_PERIOD_SECOND * 1_s,
                                         0,
                                         DUPLICATION_SYNC_PERIOD_SECOND * 1_s);
}

} // namespace replication
} // namespace dsn
