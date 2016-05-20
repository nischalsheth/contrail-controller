/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_peer_close.h"

#include <boost/foreach.hpp>

#include "bgp/bgp_log.h"
#include "bgp/bgp_membership.h"
#include "bgp/bgp_peer_types.h"
#include "bgp/bgp_route.h"
#include "bgp/bgp_server.h"
#include "net/community_type.h"

#define PEER_CLOSE_MANAGER_LOG(msg) \
    BGP_LOG_PEER(Event, peer_close_->peer(), SandeshLevel::SYS_INFO,           \
        BGP_LOG_FLAG_ALL, BGP_PEER_DIR_NA,                                     \
        "PeerCloseManager: State " << GetStateName(state_) <<                  \
        ", CloseAgain? " << (close_again_ ? "Yes" : "No") << ": " << msg);

#define MOVE_TO_STATE(state)                                                   \
    do {                                                                       \
        assert(state_ != state);                                               \
        PEER_CLOSE_MANAGER_LOG("Move to state " << GetStateName(state));       \
        state_ = state;                                                        \
    } while (false)

// Create an instance of PeerCloseManager with back reference to parent IPeer
PeerCloseManager::PeerCloseManager(IPeerClose *peer_close,
                                   boost::asio::io_service &io_service) :
        peer_close_(peer_close), stale_timer_(NULL), sweep_timer_(NULL),
        state_(NONE), close_again_(false), non_graceful_(false), gr_elapsed_(0),
        llgr_elapsed_(0), membership_state_(MEMBERSHIP_NONE) {
    stats_.init++;
    stale_timer_ = TimerManager::CreateTimer(io_service,
        "Graceful Restart StaleTimer",
        TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0);
    sweep_timer_ = TimerManager::CreateTimer(io_service,
        "Graceful Restart SweepTimer",
        TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0);
}

// Create an instance of PeerCloseManager with back reference to parent IPeer
PeerCloseManager::PeerCloseManager(IPeerClose *peer_close) :
        peer_close_(peer_close), stale_timer_(NULL), sweep_timer_(NULL),
        state_(NONE), close_again_(false), non_graceful_(false), gr_elapsed_(0),
        llgr_elapsed_(0), membership_state_(MEMBERSHIP_NONE) {
    stats_.init++;
    if (peer_close->peer() && peer_close->peer()->server()) {
        stale_timer_ = TimerManager::CreateTimer(
            *peer_close->peer()->server()->ioservice(),
            "Graceful Restart StaleTimer",
            TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0);
        sweep_timer_ = TimerManager::CreateTimer(
            *peer_close->peer()->server()->ioservice(),
            "Graceful Restart SweepTimer",
            TaskScheduler::GetInstance()->GetTaskId("bgp::Config"), 0);
    }
}

PeerCloseManager::~PeerCloseManager() {
    TimerManager::DeleteTimer(stale_timer_);
    TimerManager::DeleteTimer(sweep_timer_);
}

const std::string PeerCloseManager::GetStateName(State state) const {
    switch (state) {
    case NONE:
        return "NONE";
    case GR_TIMER:
        return "GR_TIMER";
    case STALE:
        return "STALE";
    case LLGR_STALE:
        return "LLGR_STALE";
    case LLGR_TIMER:
        return "LLGR_TIMER";
    case SWEEP:
        return "SWEEP";
    case DELETE:
        return "DELETE";
    }
    assert(false);
    return "";
}

// Trigger closure of an IPeer
//
// Graceful                                 close_state_: NONE
// RibIn Stale Marking and Ribout deletion  close_state_: STALE
// StateMachine restart and GR timer start  close_state_: GR_TIMER
//
// Peer IsReady() in GR timer callback (or via reception of all EoRs)
// RibIn Sweep and Ribout Generation        close_state_: SWEEP
//   MembershipRequestCallback                 close_state_: NONE
//
// Peer not IsReady() in GR timer callback
// If LLGR supported                     close_state_: LLGR_STALE
//   RibIn Stale marking with LLGR_STALE community close_state_: LLGR_TIMER
//
//     Peer not IsReady() in LLGR timer callback
//       RibIn Delete                           close_state_: DELETE
//       MembershipRequestCallback                 close_state_: NONE
//
//     Peer IsReady() in LLGR timer callback (or via reception of all EoRs)
//     RibIn Sweep                              close_state_: SWEEP
//       MembershipRequestCallback                 close_state_: NONE
//
// If LLGR is not supported
//     RibIn Delete                           close_state_: DELETE
//     MembershipRequestCallback                 close_state_: NONE
//
// Close() call during any state other than NONE and DELETE
//     Cancel GR timer and restart GR Closure all over again
//
// NonGraceful                              close_state_ = * (except DELETE)
// A. RibIn deletion and Ribout deletion    close_state_ = DELETE
// B. MembershipRequestCallback => Peers delete/StateMachine restart
//                                          close_state_ = NONE
//
// If Close is restarted, account for GR timer's elapsed time.
//
// Use non_graceful as true for non-graceful closure
void PeerCloseManager::Close(bool non_graceful) {
    tbb::mutex::scoped_lock lock(mutex_);

    // Note down non-graceful close trigger. Once non-graceful closure is
    // triggered, it should remain so until close process is complete. Further
    // graceful closure calls until then should remain non-graceful.
    non_graceful_ |= non_graceful;
    CloseInternal();
}

void PeerCloseManager::CloseInternal() {
    stats_.close++;

    // Ignore nested closures
    if (close_again_) {
        PEER_CLOSE_MANAGER_LOG("Nested close calls ignored");
        return;
    }

    switch (state_) {
    case NONE:
        ProcessClosure();
        break;

    case GR_TIMER:
        PEER_CLOSE_MANAGER_LOG("Nested close: Restart GR");
        close_again_ = true;
        stats_.nested++;
        gr_elapsed_ += stale_timer_->GetElapsedTime();
        CloseComplete();
        break;

    case LLGR_TIMER:
        PEER_CLOSE_MANAGER_LOG("Nested close: Restart LLGR");
        close_again_ = true;
        stats_.nested++;
        llgr_elapsed_ += stale_timer_->GetElapsedTime();
        CloseComplete();
        break;

    case STALE:
    case LLGR_STALE:
    case SWEEP:
    case DELETE:
        PEER_CLOSE_MANAGER_LOG("Nested close");
        close_again_ = true;
        stats_.nested++;
        break;
    }
}

void PeerCloseManager::ProcessEORMarkerReceived(Address::Family family) {
    tbb::mutex::scoped_lock lock(mutex_);
    if ((state_ == GR_TIMER || state_ == LLGR_TIMER) && !families_.empty()) {
        if (family == Address::UNSPEC) {
            families_.clear();
        } else {
            families_.erase(family);
        }
        if (families_.empty())
            StartRestartTimer(0);
    }
}

// Process RibIn staling related activities during peer closure
// Return true if at least ome time is started, false otherwise
void PeerCloseManager::StartRestartTimer(int time) {
    stale_timer_->Cancel();
    PEER_CLOSE_MANAGER_LOG("GR Timer started to fire after " << time <<
                           " seconds");
    stale_timer_->Start(time,
        boost::bind(&PeerCloseManager::RestartTimerCallback, this));
}

bool PeerCloseManager::RestartTimerCallback() {
    tbb::mutex::scoped_lock lock(mutex_);

    PEER_CLOSE_MANAGER_LOG("GR Timer callback started");
    if (state_ == GR_TIMER || state_ == LLGR_TIMER)
        ProcessClosure();
    return false;
}

// Route stale timer callback. If the peer has come back up, sweep routes for
// those address families that are still active. Delete the rest
void PeerCloseManager::ProcessClosure() {

    // If the peer is back up and this address family is still supported,
    // sweep old paths which may not have come back in the new session
    switch (state_) {
        case NONE:
            if (non_graceful_ || !peer_close_->IsCloseGraceful()) {
                MOVE_TO_STATE(DELETE);
                stats_.deletes++;
            } else {
                MOVE_TO_STATE(STALE);
                stats_.stale++;
                peer_close_->GracefulRestartStale();
            }
            break;
        case GR_TIMER:
            if (peer_close_->IsReady()) {
                MOVE_TO_STATE(SWEEP);
                gr_elapsed_ = 0;
                llgr_elapsed_ = 0;
                stats_.sweep++;
                break;
            }
            if (peer_close_->IsCloseLongLivedGraceful()) {
                MOVE_TO_STATE(LLGR_STALE);
                stats_.llgr_stale++;
                break;
            }
            MOVE_TO_STATE(DELETE);
            stats_.deletes++;
            break;

        case LLGR_TIMER:
            if (peer_close_->IsReady()) {
                MOVE_TO_STATE(SWEEP);
                gr_elapsed_ = 0;
                llgr_elapsed_ = 0;
                stats_.sweep++;
                break;
            }
            MOVE_TO_STATE(DELETE);
            stats_.deletes++;
            break;

        case STALE:
        case LLGR_STALE:
        case SWEEP:
        case DELETE:
            assert(false);
            return;
    }

    if (state_ == DELETE)
        peer_close_->CustomClose();
    MembershipRequestInternal();
}

void PeerCloseManager::CloseComplete() {
    MOVE_TO_STATE(NONE);
    stale_timer_->Cancel();
    sweep_timer_->Cancel();
    families_.clear();
    stats_.init++;


    // Nested closures trigger fresh GR
    if (close_again_) {
        close_again_ = false;
        CloseInternal();
    }
}

bool PeerCloseManager::ProcessSweepStateActions() {
    assert(state_ == SWEEP);

    // Notify clients to trigger sweep as appropriate.
    peer_close_->GracefulRestartSweep();
    CloseComplete();
    return false;
}

void PeerCloseManager::TriggerSweepStateActions() {
    PEER_CLOSE_MANAGER_LOG("Sweep Timer started to fire right away");
    sweep_timer_->Cancel();
    sweep_timer_->Start(0,
        boost::bind(&PeerCloseManager::ProcessSweepStateActions, this));
}

void PeerCloseManager::MembershipRequest() {
    tbb::mutex::scoped_lock lock(mutex_);
    MembershipRequestInternal();
}

bool PeerCloseManager::CanUseMembershipManager() const {
    return peer_close_->peer()->CanUseMembershipManager();
}

bool PeerCloseManager::IsMembershipPending() const {
    BgpMembershipManager *mgr = peer_close_->peer()->server()->membership_mgr();
    return mgr->IsPending(peer_close_->peer());
}

BgpMembershipManager *PeerCloseManager::membership_mgr() const {
    return peer_close_->peer()->server()->membership_mgr();
}

void PeerCloseManager::MembershipRequestInternal() {
    assert(membership_state() != MEMBERSHIP_IN_USE);

    // Pause if membership manager is not ready for usage.
    if (!CanUseMembershipManager()) {
        set_membership_state(MEMBERSHIP_IN_WAIT);
        return;
    }
    set_membership_state(MEMBERSHIP_IN_USE);
    BgpMembershipManager *mgr = membership_mgr();
    if (!mgr)
        return;

    std::list<BgpTable *> tables;
    mgr->GetRegisteredRibs(peer_close_->peer(), &tables);

    if (tables.empty()) {
        MembershipRequestCallbackInternal();
        return;
    }

    BOOST_FOREACH(BgpTable *table, tables) {
        if (mgr->IsRegistered(peer_close_->peer(), table)) {
            if (state_ == PeerCloseManager::DELETE) {
                mgr->Unregister(peer_close_->peer(), table);
            } else {
                mgr->UnregisterRibOut(peer_close_->peer(), table);
            }
        } else {
            assert(mgr->IsRibInRegistered(peer_close_->peer(), table));
            if (state_ == PeerCloseManager::DELETE) {
                mgr->UnregisterRibIn(peer_close_->peer(), table);
            } else {
                mgr->WalkRibIn(peer_close_->peer(), table);
            }
        }
    }
}

// Concurrency: Runs in the context of the BGP peer rib membership task.
//
// Close process for this peer in terms of walking RibIns and RibOuts are
// complete. Do the final cleanups necessary and notify interested party
bool PeerCloseManager::MembershipRequestCallback() {
    tbb::mutex::scoped_lock lock(mutex_);
    return MembershipRequestCallbackInternal();
}

bool PeerCloseManager::MembershipRequestCallbackInternal() {
    assert(state_ == STALE || LLGR_STALE || state_ == SWEEP ||
           state_ == DELETE);
    assert(membership_state() == MEMBERSHIP_IN_USE);

    if (IsMembershipPending())
        return false;

    set_membership_state(MEMBERSHIP_NONE);
    PEER_CLOSE_MANAGER_LOG("RibWalk completed");

    if (state_ == DELETE) {
        MOVE_TO_STATE(NONE);
        peer_close_->Delete();
        gr_elapsed_ = 0;
        llgr_elapsed_ = 0;
        stats_.init++;
        close_again_ = false;
        non_graceful_ = false;
        return true;
    }

    // Process nested closures.
    if (close_again_) {
        CloseComplete();
        return true;
    }

    // If any GR stale timer has to be launched, then to wait for some time
    // hoping for the peer (and the paths) to come back up.
    if (state_ == STALE) {
        peer_close_->CloseComplete();
        MOVE_TO_STATE(GR_TIMER);
        peer_close_->GetGracefulRestartFamilies(&families_);

        // Offset restart time with elapsed time during nested closures.
        int time = peer_close_->GetGracefulRestartTime() * 1000;
        time -= gr_elapsed_;
        if (time < 0)
            time = 0;
        StartRestartTimer(time);
        stats_.gr_timer++;
        return true;
    }

    // From LLGR_STALE state, switch to LLGR_TIMER state. Typically this would
    // be a very long timer, and we expect to receive EORs before this timer
    // expires.
    if (state_ == LLGR_STALE) {
        MOVE_TO_STATE(LLGR_TIMER);
        peer_close_->GetGracefulRestartFamilies(&families_);
        StartRestartTimer(1000 *
                peer_close_->GetLongLivedGracefulRestartTime());

        // Offset restart time with elapsed time during nested closures.
        int time = peer_close_->GetLongLivedGracefulRestartTime() *1000;
        time -= llgr_elapsed_;
        if (time < 0)
            time = 0;
        StartRestartTimer(time);
        stats_.llgr_timer++;
        return true;
    }

    TriggerSweepStateActions();
    return true;
}

void PeerCloseManager::FillCloseInfo(BgpNeighborResp *resp) const {
    tbb::mutex::scoped_lock lock(mutex_);

    PeerCloseInfo peer_close_info;
    peer_close_info.state = GetStateName(state_);
    peer_close_info.close_again = close_again_;
    peer_close_info.non_graceful = non_graceful_;
    peer_close_info.init = stats_.init;
    peer_close_info.close = stats_.close;
    peer_close_info.nested = stats_.nested;
    peer_close_info.deletes = stats_.deletes;
    peer_close_info.stale = stats_.stale;
    peer_close_info.sweep = stats_.sweep;
    peer_close_info.gr_timer = stats_.gr_timer;

    resp->set_peer_close_info(peer_close_info);
}

bool PeerCloseManager::MembershipPathCallback(DBTablePartBase *root,
                                              BgpRoute *rt, BgpPath *path) {
    DBRequest::DBOperation oper;
    BgpAttrPtr attrs;

    BgpTable *table = static_cast<BgpTable *>(root->parent());
    assert(table);

    uint32_t stale = 0;

    tbb::mutex::scoped_lock lock(mutex_);
    switch (state_) {
        case NONE:
        case GR_TIMER:
        case LLGR_TIMER:
            return false;

        case SWEEP:

            // Stale paths must be deleted.
            if (!path->IsStale() && !path->IsLlgrStale())
                return false;
            path->ResetStale();
            path->ResetLlgrStale();
            oper = DBRequest::DB_ENTRY_DELETE;
            attrs = NULL;
            break;

        case DELETE:

            // This path must be deleted. Hence attr is not required.
            oper = DBRequest::DB_ENTRY_DELETE;
            attrs = NULL;
            break;

        case STALE:

            // If path is already marked as stale, then there is no need to
            // process again. This can happen if the session flips while in
            // GR_TIMER state.
            if (path->IsStale())
                return false;

            // This path must be marked for staling. Update the local
            // preference and update the route accordingly.
            oper = DBRequest::DB_ENTRY_ADD_CHANGE;
            attrs = path->GetAttr();
            stale = BgpPath::Stale;
            break;

        case LLGR_STALE:

            // If the path has NO_LLGR community, DELETE it.
            if (path->GetAttr()->community() &&
                path->GetAttr()->community()->ContainsValue(
                    CommunityType::NoLlgr)) {
                oper = DBRequest::DB_ENTRY_DELETE;
                attrs = NULL;
                break;
            }

            // If path is already marked as llgr_stale, then there is no
            // need to process again. This can happen if the session flips
            // while in LLGR_TIMER state.
            if (path->IsLlgrStale())
                return false;

            attrs = path->GetAttr();
            stale = BgpPath::LlgrStale;
            oper = DBRequest::DB_ENTRY_ADD_CHANGE;
            break;
    }

    // Feed the route modify/delete request to the table input process.
    return table->InputCommon(root, rt, path, peer_close_->peer(), NULL, oper,
                              attrs, path->GetPathId(),
                              path->GetFlags() | stale, path->GetLabel());
}
