/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/pch.h"

#include "mongo/db/repl/rs_sync.h"

#include <vector>

#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_sync.h"

namespace mongo {

    using namespace bson;
   
    /* should be in RECOVERING state on arrival here.
       readlocks
       On input, should have repl lock and global write lock held
       @return true if transitioned to SECONDARY
    */
    void ReplSetImpl::tryToGoLiveAsASecondary() {
        // make sure we're not primary, secondary, or fatal already
        verify(lockedByMe());
        
        // Not sure if this if clause is necessary anymore. Hard to prove
        // either way for now, so leaving it in
        if (box.getState().primary() || box.getState().secondary() || box.getState().fatal()) {
            return;
        }

        if (_maintenanceMode > 0 || _blockSync) {
            // we're not actually going live
            return;
        }

        sethbmsg("");
        changeState(MemberState::RS_SECONDARY);
        BackgroundSync::get()->startOpSyncThread();
        return;
    }


    bool ReplSetImpl::forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result) {
        lock lk(this);

        // initial sanity check
        if (iAmArbiterOnly()) {
            errmsg = "arbiters don't sync";
            return false;
        }
        if (box.getState().primary()) {
            errmsg = "primaries don't sync";
            return false;
        }
        if (_self != NULL && host == _self->fullName()) {
            errmsg = "I cannot sync from myself";
            return false;
        }

        // find the member we want to sync from
        Member *newTarget = 0;
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->fullName() == host) {
                newTarget = m;
                break;
            }
        }

        // do some more sanity checks
        if (!newTarget) {
            // this will also catch if someone tries to sync a member from itself, as _self is not
            // included in the _members list.
            errmsg = "could not find member in replica set";
            return false;
        }
        if (newTarget->config().arbiterOnly) {
            errmsg = "I cannot sync from an arbiter";
            return false;
        }
        if (!newTarget->config().buildIndexes && myConfig().buildIndexes) {
            errmsg = "I cannot sync from a member who does not build indexes";
            return false;
        }
        if (newTarget->hbinfo().authIssue) {
            errmsg = "I cannot authenticate against the requested member";
            return false;
        }
        if (newTarget->hbinfo().health == 0) {
            errmsg = "I cannot reach the requested member";
            return false;
        }
        if (newTarget->hbinfo().opTime + 10000 < gtidManager->getCurrTimestamp()) {
            log() << "attempting to sync from " << newTarget->fullName()
                  << ", but its latest opTime is " << newTarget->hbinfo().opTime / 1000
                  << " and ours is " << gtidManager->getCurrTimestamp() / 1000 << " so this may not work"
                  << rsLog;
            result.append("warning", "requested member is more than 10 seconds behind us");
            // not returning false, just warning
        }

        // record the previous member we were syncing from
        Member *prev = BackgroundSync::get()->getSyncTarget();
        if (prev) {
            result.append("prevSyncTarget", prev->fullName());
        }

        // finally, set the new target
        _forceSyncTarget = newTarget;
        return true;
    }

    bool ReplSetImpl::gotForceSync() {
        lock lk(this);
        return _forceSyncTarget != 0;
    }

    // on input, _stateChangeMutex is held,
    // see manager.cpp, msgCheckNewState
    // that calls checkAuth
    void ReplSetImpl::blockSync(bool block) {
        // if told to block, and currently not blocking,
        // stop opsync thread and go into recovering state
        if (block && !_blockSync) {
            BackgroundSync::get()->stopOpSyncThread();
            RSBase::lock lk(this);
            changeState(MemberState::RS_RECOVERING);
        }
        else if (!block && _blockSync) {
            // this is messy
            // replLock is already locked on input here
            // see usage of this function in manager.cpp
            Lock::GlobalWrite writeLock;
            tryToGoLiveAsASecondary();
        }
        _blockSync = block;
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void GhostSync::associateSlave(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        rwlock lk( _lock , true );
        shared_ptr<GhostSlave> &g = _ghostCache[rid];
        if( g.get() == 0 ) {
            g.reset( new GhostSlave() );
            wassert( _ghostCache.size() < 10000 );
        }
        GhostSlave &slave = *g;
        if (slave.init) {
            LOG(1) << "tracking " << slave.slave->h().toString() << " as " << rid << rsLog;
            return;
        }

        slave.slave = (Member*)rs->findById(memberId);
        if (slave.slave != 0) {
            slave.init = true;
        }
        else {
            log() << "replset couldn't find a slave with id " << memberId
                  << ", not tracking " << rid << rsLog;
        }
    }

    void GhostSync::updateSlave(const mongo::OID& rid, const GTID& lastGTID) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update slave " << rid << " no entry" << rsLog;
            return;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update slave " << rid << " not init" << rsLog;
            return;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(lastGTID);
    }

    void GhostSync::percolate(const BSONObj& id, const GTID& lastGTID) {
        const OID rid = id["_id"].OID();
        GhostSlave* slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second.get();
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }

        verify(slave->slave);

        const Member *target = BackgroundSync::get()->getSyncTarget();
        if (!target || rs->box.getState().primary()
            // we are currently syncing from someone who's syncing from us
            // the target might end up with a new Member, but s.slave never
            // changes so we'll compare the names
            || target == slave->slave || target->fullName() == slave->slave->fullName()) {
            LOG(1) << "replica set ghost target no good" << endl;
            return;
        }

        try {
            if (!slave->reader.haveCursor()) {
                if (!slave->reader.connect(id, slave->slave->id(), target->fullName())) {
                    // error message logged in OplogReader::connect
                    return;
                }
                slave->reader.ghostQueryGTE(rsoplog, lastGTID);
            }

            LOG(1) << "replSet last: " << slave->lastGTID.toString() << " to " << lastGTID.toString() << rsLog;
            if ( GTID::cmp(slave->lastGTID, lastGTID) > 0 ) {
                return;
            }

            while ( GTID::cmp(slave->lastGTID, lastGTID) <= 0 ) {
                if (!slave->reader.more()) {
                    // we'll be back
                    return;
                }

                BSONObj o = slave->reader.nextSafe();
                slave->lastGTID = getGTIDFromBSON("_id", o);
            }
            LOG(2) << "now last is " << slave->lastGTID.toString() << rsLog;
        }
        catch (DBException& e) {
            // we'll be back
            LOG(2) << "replSet ghost sync error: " << e.what() << " for "
                   << slave->slave->fullName() << rsLog;
            slave->reader.resetConnection();
        }
    }
}
