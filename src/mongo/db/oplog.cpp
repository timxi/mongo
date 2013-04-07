// @file oplog.cpp

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

#include "pch.h"
#include "oplog.h"
#include "repl_block.h"
#include "repl.h"
#include "commands.h"
#include "repl/rs.h"
#include "stats/counters.h"
#include "../util/file.h"
#include "../util/startup_test.h"
#include "queryoptimizer.h"
#include "ops/update.h"
#include "ops/delete.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/db_flags.h"

namespace mongo {

    // from d_migrate.cpp
    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );

    int __findingStartInitialTimeout = 5; // configurable for testing

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static Database *localDB = 0;
    static NamespaceDetails *rsOplogDetails = NULL;
    static NamespaceDetails *replInfoDetails = NULL;
    
    void oplogCheckCloseDatabase( Database * db ) {
        verify( Lock::isW() );
        localDB = 0;
        rsOplogDetails = NULL;
        replInfoDetails = NULL;
        resetSlaveCache();
    }

    static void _logOpUninitialized(BSONObj id, BSONArray& opInfo) {
        log() << "WHAT IS GOING ON???????? " << endl;
    }

    void deleteOplogFiles() {
        Lock::DBWrite lk1("local");
        localDB = NULL;
        rsOplogDetails = NULL;
        replInfoDetails = NULL;
        
        Client::Context ctx( rsoplog, dbpath, false);
        // TODO: code review this for possible error cases
        // although, I don't think we care about error cases,
        // just that after we exit, oplog files don't exist
        BSONObjBuilder out;
        string errmsg;
        dropCollection(rsoplog, errmsg, out);
        BSONObjBuilder out2;
        string errmsg2;
        dropCollection(rsReplInfo, errmsg, out);
    }

    void openOplogFiles() {
        Lock::DBWrite lk1("local");
        const char *logns = rsoplog;
        if ( rsOplogDetails == 0 ) {
            Client::Context ctx( logns , dbpath, false);
            localDB = ctx.db();
            verify( localDB );
            rsOplogDetails = nsdetails(logns);
            massert(13347, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
        }
        if (replInfoDetails == NULL) {
            Client::Context ctx( rsReplInfo , dbpath, false);
            replInfoDetails = nsdetails(rsReplInfo);
            massert(16472, "local.replInfo missing. did you drop it? if so restart server", replInfoDetails);
        }
    }
    
    static void _logTransactionOps(BSONObj id, BSONArray& opInfo) {
        Lock::DBRead lk1("local");
        mutex::scoped_lock lk2(OpTime::m);

        const OpTime ts = OpTime::now(lk2);
        long long hashNew;
        if( theReplSet ) {
            //massert(13312, "replSet error : logOp() but not primary?", theReplSet->box.getState().primary());
            hashNew = (theReplSet->lastH * 131 + ts.asLL()) * 17 + theReplSet->selfId();
            theReplSet->lastOpTimeWritten = ts;
            theReplSet->lastH = hashNew;
        }
        else {
            // must be initiation
            hashNew = 0;
        }

        BSONObjBuilder b;
        b.append("_id", id);
        b.appendTimestamp("ts", ts.asDate());
        b.append("h", hashNew);
        b.append("a", true);
        b.append("ops", opInfo);

        verify(rsOplogDetails);
        BSONObj bb = b.done();
        uint64_t flags = (ND_UNIQUE_CHECKS_OFF | ND_LOCK_TREE_OFF);
        rsOplogDetails->insertObject(bb, flags);
    }

    void logToReplInfo(GTID minLiveGTID, GTID minUnappliedGTID) {
        Lock::DBRead lk("local");
        BufBuilder bufbuilder(256);
        BSONObjBuilder b(bufbuilder);
        b.append("_id", "minLive");
        b.append("GTID", minLiveGTID.getBSON());
        BSONObj bb = b.done();
        uint64_t flags = (ND_UNIQUE_CHECKS_OFF | ND_LOCK_TREE_OFF);
        replInfoDetails->insertObject(bb, flags);

        bufbuilder.reset();
        BSONObjBuilder b2(bufbuilder);
        b2.append("_id", "minUnapplied");
        b2.append("GTID", minUnappliedGTID.getBSON());
        BSONObj bb2 = b2.done();
        replInfoDetails->insertObject(bb2, flags);
    }
    
    static void (*_logTransactionOp)(BSONObj id, BSONArray& opInfo) = _logOpUninitialized;
    // TODO: (Zardosht) hopefully remove these two phases
    void newReplUp() {
        _logTransactionOp = _logTransactionOps;
    }
    void newRepl() {
        _logTransactionOp = _logTransactionOps;
    }

    void logTransactionOps(BSONObj id, BSONArray& opInfo) {
        _logTransactionOp(id, opInfo);
        // TODO: Figure out for sharding
        //logOpForSharding( opstr , ns , obj , patt );
    }

    void createOplog() {
        Lock::GlobalWrite lk;
        bool rs = !cmdLine._replSet.empty();
        verify(rs);
        
        const char * oplogNS = rsoplog;
        const char * replInfoNS = rsReplInfo;
        Client::Context ctx(oplogNS);
        NamespaceDetails * oplogNSD = nsdetails(oplogNS);
        NamespaceDetails * replInfoNSD = nsdetails(replInfoNS);
        if (oplogNSD || replInfoNSD) {
            // TODO: (Zardosht), figure out if there are any checks to do here
            // not sure under what scenarios we can be here, so
            // making a printf to catch this so we can investigate
            tokulog() << "createOplog called with existing collections, investigate why.\n" << endl;
            return;
        }

        /* create an oplog collection, if it doesn't yet exist. */
        BSONObjBuilder b;

        log() << "******" << endl;
        log() << "creating replication oplog." << endl;
        log() << "TODO: FIGURE OUT SIZE!!!." << endl;
        // create the namespace
        string err;
        BSONObj o = b.done();
        bool ret = userCreateNS(oplogNS, o, err, false);
        verify(ret);
        ret = userCreateNS(replInfoNS, o, err, false);
        verify(ret);
        log() << "******" << endl;
    }

    // -------------------------------------

    struct TestOpTime : public StartupTest {
        void run() {
            OpTime t;
            for ( int i = 0; i < 10; i++ ) {
                OpTime s = OpTime::_now();
                verify( s != t );
                t = s;
            }
            OpTime q = t;
            verify( q == t );
            verify( !(q != t) );
        }
    } testoptime;

    int _dummy_z;

    void Sync::setHostname(const string& hostname) {
        hn = hostname;
    }

    BSONObj Sync::getMissingDoc(const BSONObj& o) {
        OplogReader missingObjReader;
        const char *ns = o.getStringField("ns");

        // capped collections
        NamespaceDetails *nsd = nsdetails(ns);
        if ( nsd && nsd->isCapped() ) {
            log() << "replication missing doc, but this is okay for a capped collection (" << ns << ")" << endl;
            return BSONObj();
        }

        uassert(15916, str::stream() << "Can no longer connect to initial sync source: " << hn, missingObjReader.connect(hn));

        // might be more than just _id in the update criteria
        BSONObj query = BSONObjBuilder().append(o.getObjectField("o2")["_id"]).obj();
        BSONObj missingObj;
        try {
            missingObj = missingObjReader.findOne(ns, query);
        } catch(DBException& e) {
            log() << "replication assertion fetching missing object: " << e.what() << endl;
            throw;
        }

        return missingObj;
    }

    bool Sync::shouldRetry(const BSONObj& o) {
        // should already have write lock
        const char *ns = o.getStringField("ns");
        Client::Context ctx(ns);

        // we don't have the object yet, which is possible on initial sync.  get it.
        log() << "replication info adding missing object" << endl; // rare enough we can log

        BSONObj missingObj = getMissingDoc(o);

        if( missingObj.isEmpty() ) {
            log() << "replication missing object not found on source. presumably deleted later in oplog" << endl;
            log() << "replication o2: " << o.getObjectField("o2").toString() << endl;
            log() << "replication o firstfield: " << o.getObjectField("o").firstElementFieldName() << endl;

            return false;
        }
        else {
            ::abort(); //DiskLoc d = //theDataFileMgr.insert(ns, (void*) missingObj.objdata(), missingObj.objsize());
            //uassert(15917, "Got bad disk location when attempting to insert", !d.isNull());

            LOG(1) << "replication inserted missing doc: " << missingObj.toString() << endl;
            return true;
        }
    }

    /** @param fromRepl false if from ApplyOpsCmd
        @return true if was and update should have happened and the document DNE.  see replset initial sync code.
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl, bool convertUpdateToUpsert) {
        LOG(6) << "applying op: " << op << endl;
        bool failedUpdate = false;

        OpCounters * opCounters = fromRepl ? &replOpCounters : &globalOpCounters;

        const char *names[] = { "o", "ns", "op", "b" };
        BSONElement fields[4];
        op.getFields(4, names, fields);

        BSONObj o;
        if( fields[0].isABSONObj() )
            o = fields[0].embeddedObject();
            
        const char *ns = fields[1].valuestrsafe();

        Lock::assertWriteLocked(ns);

        NamespaceDetails *nsd = nsdetails(ns);
        (void) nsd; // TODO: Suppress unused warning

        const char *opType = fields[2].valuestrsafe();

        if ( *opType == 'i' ) {
            opCounters->gotInsert();

            const char *p = strchr(ns, '.');
            if ( p && strcmp(p, ".system.indexes") == 0 ) {
                // updates aren't allowed for indexes -- so we will do a regular insert. if index already
                // exists, that is ok.
                //theDataFileMgr.insert(ns, (void*) o.objdata(), o.objsize());
                ::abort();
            }
            else {
                // do upserts for inserts as we might get replayed more than once
                OpDebug debug;
                BSONElement _id;
                if( !o.getObjectID(_id) ) {
                    /* No _id.  This will be very slow. */
                    Timer t;
                    updateObjects(ns, o, o, true, false, false, debug, false,
                                  QueryPlanSelectionPolicy::idElseNatural() );
                    if( t.millis() >= 2 ) {
                        RARELY OCCASIONALLY log() << "warning, repl doing slow updates (no _id field) for " << ns << endl;
                    }
                }
                else {
                    // probably don't need this since all replicated colls have _id indexes now
                    // but keep it just in case
                    // TODO: Should we care about this period assert? Sounds like no.
                    //RARELY if ( nsd && !nsd->isCapped() ) { ensureHaveIdIndex(ns); }

                    /* todo : it may be better to do an insert here, and then catch the dup key exception and do update
                              then.  very few upserts will not be inserts...
                              */
                    BSONObjBuilder b;
                    b.append(_id);
                    updateObjects(ns, o, b.done(), true, false, false , debug, false,
                                  QueryPlanSelectionPolicy::idElseNatural() );
                }
            }
        }
        else if ( *opType == 'u' ) {
            opCounters->gotUpdate();

            // probably don't need this since all replicated colls have _id indexes now
            // but keep it just in case
            // TODO: Should we care about this period assert? Sounds like no.
            //RARELY if ( nsd && !nsd->isCapped() ) { ensureHaveIdIndex(ns); }

            OpDebug debug;
            BSONObj updateCriteria = op.getObjectField("o2");
            bool upsert = fields[3].booleanSafe() || convertUpdateToUpsert;
            UpdateResult ur = updateObjects(ns, o, updateCriteria, upsert, /*multi*/ false,
                                            /*logop*/ false , debug, /*fromMigrate*/ false,
                                            QueryPlanSelectionPolicy::idElseNatural() );
            if( ur.num == 0 ) { 
                if( ur.mod ) {
                    if( updateCriteria.nFields() == 1 ) {
                        // was a simple { _id : ... } update criteria
                        failedUpdate = true;
                        log() << "replication failed to apply update: " << op.toString() << endl;
                    }
                    // need to check to see if it isn't present so we can set failedUpdate correctly.
                    // note that adds some overhead for this extra check in some cases, such as an updateCriteria
                    // of the form
                    //   { _id:..., { x : {$size:...} }
                    // thus this is not ideal.
                    else {
#if 0
                        if (nsd == NULL ||
                            (nsd->findIdIndex() >= 0 && Helpers::findById(nsd, updateCriteria).isNull()) ||
                            // capped collections won't have an _id index
                            (nsd->findIdIndex() < 0 && Helpers::findOne(ns, updateCriteria, false).isNull())) {
                            failedUpdate = true;
                            log() << "replication couldn't find doc: " << op.toString() << endl;
                        }
#endif
                        ::abort();

                        // Otherwise, it's present; zero objects were updated because of additional specifiers
                        // in the query for idempotence
                    }
                }
                else { 
                    // this could happen benignly on an oplog duplicate replay of an upsert
                    // (because we are idempotent), 
                    // if an regular non-mod update fails the item is (presumably) missing.
                    if( !upsert ) {
                        failedUpdate = true;
                        log() << "replication update of non-mod failed: " << op.toString() << endl;
                    }
                }
            }
        }
        else if ( *opType == 'd' ) {
            opCounters->gotDelete();
            if ( opType[1] == 0 )
                deleteObjects(ns, o, /*justOne*/ fields[3].booleanSafe());
            else
                verify( opType[1] == 'b' ); // "db" advertisement
        }
        else if ( *opType == 'c' ) {
            opCounters->gotCommand();
            BufBuilder bb;
            BSONObjBuilder ob;
            _runCommands(ns, o, bb, ob, true, 0);
        }
        else if ( *opType == 'n' ) {
            // no op
        }
        else {
            throw MsgAssertionException( 14825 , ErrorMsg("error in applyOperation : unknown opType ", *opType) );
        }
        return failedUpdate;
    }

    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool lockGlobally() const { return true; } // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple ns used so locking individually requires more analysis
        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if ( cmdObj.firstElement().type() != Array ) {
                errmsg = "ops has to be an array";
                return false;
            }

            BSONObj ops = cmdObj.firstElement().Obj();

            {
                // check input
                BSONObjIterator i( ops );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() == Object )
                        continue;
                    errmsg = "op not an object: ";
                    errmsg += e.fieldName();
                    return false;
                }
            }

            if ( cmdObj["preCondition"].type() == Array ) {
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ) {
                    BSONObj f = i.next().Obj();

                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );

                    Matcher m( f["res"].Obj() );
                    if ( ! m.matches( realres ) ) {
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }

            // apply
            int num = 0;
            int errors = 0;
            
            BSONObjIterator i( ops );
            BSONArrayBuilder ab;
            
            while ( i.more() ) {
                BSONElement e = i.next();
                const BSONObj& temp = e.Obj();
                
                Client::Context ctx( temp["ns"].String() ); // this handles security
                bool failed = applyOperation_inlock( temp , false );
                ab.append(!failed);
                if ( failed )
                    errors++;

                num++;
            }

            result.append( "applied" , num );
            result.append( "results" , ab.arr() );

            if ( ! fromRepl ) {
                // We want this applied atomically on slaves
                // so we re-wrap without the pre-condition for speed

                string tempNS = str::stream() << dbname << ".$cmd";
                // TODO: Zardosht, figure out what this does
                ::abort();
                //logOp( "c" , tempNS.c_str() , cmdObj.firstElement().wrap() );
            }

            return errors == 0;
        }

        DBDirectClient db;

    } applyOpsCmd;

}
