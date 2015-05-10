/*
 * Copyright (C) 2015 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#ifndef UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_SERVER_DISTRIBUTED_SERVER_HPP_INCLUDED
#define UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_SERVER_DISTRIBUTED_SERVER_HPP_INCLUDED

#include <uavcan/build_config.hpp>
#include <uavcan/debug.hpp>
#include <uavcan/node/timer.hpp>
#include <uavcan/protocol/dynamic_node_id_server/distributed/types.hpp>
#include <uavcan/protocol/dynamic_node_id_server/distributed/event.hpp>
#include <uavcan/protocol/dynamic_node_id_server/distributed/raft_core.hpp>
#include <uavcan/protocol/dynamic_node_id_server/allocation_request_manager.hpp>
#include <uavcan/protocol/dynamic_node_id_server/node_id_selector.hpp>

namespace uavcan
{
namespace dynamic_node_id_server
{
namespace distributed
{
/**
 * This class implements the top-level allocation logic and server API.
 */
class Server : private IAllocationRequestHandler
             , private IRaftLeaderMonitor
{
    struct UniqueIDLogPredicate
    {
        const UniqueID unique_id;

        UniqueIDLogPredicate(const UniqueID& uid)
            : unique_id(uid)
        { }

        bool operator()(const RaftCore::LogEntryInfo& info) const
        {
            return info.entry.unique_id == unique_id;
        }
    };

    struct NodeIDLogPredicate
    {
        const NodeID node_id;

        NodeIDLogPredicate(const NodeID& nid)
            : node_id(nid)
        { }

        bool operator()(const RaftCore::LogEntryInfo& info) const
        {
            return info.entry.node_id == node_id.get();
        }
    };

    /*
     * States
     */
    INode& node_;
    RaftCore raft_core_;
    AllocationRequestManager allocation_request_manager_;

    /*
     * Methods of IAllocationRequestHandler
     */
    virtual bool canPublishFollowupAllocationResponse() const
    {
        /*
         * The server is allowed to publish follow-up allocation responses only if both conditions are met:
         * - The server is leader.
         * - The last allocation request has been completed successfully.
         *
         * Why second condition? Imagine a case when there's two Raft nodes that don't hear each other - A and B,
         * both of them are leaders (but only A can commit to the log, B is in a minor partition); then there's a
         * client X that can exchange with both leaders, and a client Y that can exchange only with A. Such a
         * situation can occur in case of a very unlikely failure of redundant interfaces.
         *
         * Both clients X and Y initially send a first-stage Allocation request; A responds to Y with a first-stage
         * response, whereas B responds to X. Both X and Y will issue a follow-up second-stage requests, which may
         * cause A to mix second-stage Allocation requests from different nodes, leading to reception of an invalid
         * unique ID. When both leaders receive full unique IDs (A will receive an invalid one, B will receive a valid
         * unique ID of X), only A will be able to make a commit, because B is in a minority. Since both clients were
         * unable to receive node ID values in this round, they will try again later.
         *
         * Now, in order to prevent B from disrupting client-server communication second time around, we introduce this
         * second restriction: the server cannot exchange with clients as long as its log contains uncommitted entries.
         *
         * Note that this restriction does not apply to allocation requests sent via CAN FD frames, as in this case
         * no follow-up responses are necessary. So only CAN FD can offer reliable Allocation exchange.
         */
        return raft_core_.isLeader() && raft_core_.areAllLogEntriesCommitted();
    }

    virtual void handleAllocationRequest(const UniqueID& unique_id, const NodeID preferred_node_id)
    {
        /*
         * Note that it is possible that the local node is not leader. We will still perform the log search
         * and try to find the node that requested allocation. If the node is found, response will be sent;
         * otherwise the request will be ignored because only leader can add new allocations.
         */
        const LazyConstructor<RaftCore::LogEntryInfo> result =
            raft_core_.traverseLogFromEndUntil(UniqueIDLogPredicate(unique_id));

         if (result.isConstructed())
         {
             if (result->committed)
             {
                 tryPublishAllocationResult(result->entry);
                 UAVCAN_TRACE("dynamic_node_id_server::distributed::Server",
                              "Allocation request served with existing allocation; node ID %d",
                              int(result->entry.node_id));
             }
             else
             {
                 UAVCAN_TRACE("dynamic_node_id_server::distributed::Server",
                              "Allocation request ignored - allocation exists but not committed yet; node ID %d",
                              int(result->entry.node_id));
             }
         }
         else
         {
             // TODO: new allocations can be added only if the list of unidentified nodes is not empty
             if (raft_core_.isLeader())
             {
                 allocateNewNode(unique_id, preferred_node_id);
             }
         }
    }

    /*
     * Methods of IRaftLeaderMonitor
     */
    virtual void handleLogCommitOnLeader(const protocol::dynamic_node_id::server::Entry& entry)
    {
        /*
         * Maybe this node did not request allocation at all, we don't care, we publish anyway.
         */
        tryPublishAllocationResult(entry);
    }

    /*
     * Private methods
     */
    bool isNodeIDTaken(const NodeID node_id) const
    {
        UAVCAN_TRACE("dynamic_node_id_server::distributed::Server",
                     "Testing if node ID %d is taken", int(node_id.get()));
        return raft_core_.traverseLogFromEndUntil(NodeIDLogPredicate(node_id));
    }

    void allocateNewNode(const UniqueID& unique_id, const NodeID preferred_node_id)
    {
        const NodeID allocated_node_id =
            NodeIDSelector<Server>(this, &Server::isNodeIDTaken).findFreeNodeID(preferred_node_id);
        if (!allocated_node_id.isUnicast())
        {
            UAVCAN_TRACE("dynamic_node_id_server::distributed::Server", "Request ignored - no free node ID left");
            return;
        }

        UAVCAN_TRACE("dynamic_node_id_server::distributed::Server", "New node ID allocated: %d",
                     int(allocated_node_id.get()));
        const int res = raft_core_.appendLog(unique_id, allocated_node_id);
        if (res < 0)
        {
            node_.registerInternalFailure("Raft log append new allocation");
        }
    }

    void tryPublishAllocationResult(const protocol::dynamic_node_id::server::Entry& entry)
    {
        const int res = allocation_request_manager_.broadcastAllocationResponse(entry.unique_id, entry.node_id);
        if (res < 0)
        {
            node_.registerInternalFailure("Dynamic allocation final broadcast");
        }
    }

public:
    Server(INode& node,
           IStorageBackend& storage,
           IEventTracer& tracer)
        : node_(node)
        , raft_core_(node, storage, tracer, *this)
        , allocation_request_manager_(node, *this)
    { }

    int init(uint8_t cluster_size = ClusterManager::ClusterSizeUnknown)
    {
        int res = raft_core_.init(cluster_size);
        if (res < 0)
        {
            return res;
        }

        res = allocation_request_manager_.init();
        if (res < 0)
        {
            return res;
        }

        // TODO Initialize the node info transport

        return 0;
    }
};

}
}
}

#endif // Include guard
