/**
*    Copyright (C) 2008 10gen Inc.
*    Copyright (C) 2013 Tokutek Inc.
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

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespacestring.h"
#include "mongo/db/stats/counters.h"

#include "mongo/s/chunk.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/field_parser.h"
#include "mongo/s/grid.h"
#include "mongo/s/strategy.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_shard.h"
#include "mongo/s/writeback_listener.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

    namespace dbgrid_cmds {

        class GridAdminCmd : public InformationCommand {
        public:
            GridAdminCmd( const char * n ) : InformationCommand( n , false, tolowerString(n).c_str() ) {
            }
            virtual bool adminOnly() const {
                return true;
            }

            bool okForConfigChanges( string& errmsg ) {
                string e;
                if ( ! configServer.allUp(e) ) {
                    errmsg = str::stream() << "not all config servers are up: " << e;
                    return false;
                }
                return true;
            }
        };

        // --------------- misc commands ----------------------

        class NetStatCmd : public GridAdminCmd {
        public:
            NetStatCmd() : GridAdminCmd("netstat") { }
            virtual void help( stringstream& help ) const {
                help << " shows status/reachability of servers in the cluster";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::netstat);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("configserver", configServer.getPrimary().getConnString() );
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;

        class FlushRouterConfigCmd : public GridAdminCmd {
        public:
            FlushRouterConfigCmd() : GridAdminCmd("flushRouterConfig") { }
            virtual void help( stringstream& help ) const {
                help << "flush all router config";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::flushRouterConfig);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                grid.flushConfig();
                result.appendBool( "flushed" , true );
                return true;
            }
        } flushRouterConfigCmd;

        class FsyncCommand : public GridAdminCmd {
        public:
            FsyncCommand() : GridAdminCmd( "fsync" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::fsync);
                out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                if ( cmdObj["lock"].trueValue() ) {
                    errmsg = "can't do lock through mongos";
                    return false;
                }

                BSONObjBuilder sub;

                bool ok = true;
                int numFiles = 0;

                vector<Shard> shards;
                Shard::getAllShards( shards );
                for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ) {
                    Shard s = *i;

                    BSONObj x = s.runCommand( "admin" , "fsync" );
                    sub.append( s.getName() , x );

                    if ( ! x["ok"].trueValue() ) {
                        ok = false;
                        errmsg = x["errmsg"].String();
                    }

                    numFiles += x["numFiles"].numberInt();
                }

                result.append( "numFiles" , numFiles );
                result.append( "all" , sub.obj() );
                return ok;
            }
        } fsyncCmd;

        // ------------ database level commands -------------

        class MoveDatabasePrimaryCommand : public GridAdminCmd {
        public:
            MoveDatabasePrimaryCommand() : GridAdminCmd("movePrimary") { }
            virtual void help( stringstream& help ) const {
                help << " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::movePrimary);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string dbname = cmdObj.firstElement().valuestrsafe();

                if ( dbname.size() == 0 ) {
                    errmsg = "no db";
                    return false;
                }

                if ( dbname == "config" ) {
                    errmsg = "can't move config db";
                    return false;
                }

                // Flush the configuration
                // This can't be perfect, but it's better than nothing.
                grid.flushConfig();

                DBConfigPtr config = grid.getDBConfig( dbname , false );
                if ( ! config ) {
                    errmsg = "can't find db!";
                    return false;
                }

                string to = cmdObj["to"].valuestrsafe();
                if ( ! to.size()  ) {
                    errmsg = "you have to specify where you want to move it";
                    return false;
                }
                Shard s = Shard::make( to );

                if ( config->getPrimary() == s.getConnString() ) {
                    errmsg = "it is already the primary";
                    return false;
                }

                if ( ! grid.knowAboutShard( s.getConnString() ) ) {
                    errmsg = "that server isn't known to me";
                    return false;
                }

                log() << "Moving " << dbname << " primary from: " << config->getPrimary().toString()
                      << " to: " << s.toString() << endl;

                // Locking enabled now...
                DistributedLock lockSetup( configServer.getConnectionString(), dbname + "-movePrimary" );
                dist_lock_try dlk;

                // Distributed locking added.
                try{
                    dlk = dist_lock_try( &lockSetup , string("Moving primary shard of ") + dbname );
                }
                catch( LockException& e ){
	                errmsg = str::stream() << "error locking distributed lock to move primary shard of " << dbname << causedBy( e );
	                warning() << errmsg << endl;
	                return false;
                }

                if ( ! dlk.got() ) {
	                errmsg = (string)"metadata lock is already taken for moving " + dbname;
	                return false;
                }

                set<string> shardedColls;
                config->getAllShardedCollections( shardedColls );

                BSONArrayBuilder barr;
                barr.append( shardedColls );

                scoped_ptr<ScopedDbConnection> toconn(
                        ScopedDbConnection::getScopedDbConnection( s.getConnString() ) );

                // TODO ERH - we need a clone command which replays operations from clone start to now
                //            can just use local.oplog.$main
                BSONObj cloneRes;
                bool worked = toconn->get()->runCommand(
                    dbname.c_str(),
                    BSON( "clone" << config->getPrimary().getConnString() <<
                          "collsToIgnore" << barr.arr() ),
                    cloneRes );
                toconn->done();

                if ( ! worked ) {
                    log() << "clone failed" << cloneRes << endl;
                    errmsg = "clone failed";
                    return false;
                }

                string oldPrimary = config->getPrimary().getConnString();

                scoped_ptr<ScopedDbConnection> fromconn(
                        ScopedDbConnection::getScopedDbConnection( config->getPrimary()
                                                                   .getConnString() ) );

                config->setPrimary( s.getConnString() );

                if( shardedColls.empty() ){

                    // TODO: Collections can be created in the meantime, and we should handle in the future.
                    log() << "movePrimary dropping database on " << oldPrimary << ", no sharded collections in " << dbname << endl;

                    try {
                        fromconn->get()->dropDatabase( dbname.c_str() );
                    }
                    catch( DBException& e ){
                        e.addContext( str::stream() << "movePrimary could not drop the database " << dbname << " on " << oldPrimary );
                        throw;
                    }

                }
                else if( cloneRes["clonedColls"].type() != Array ){

                    // Legacy behavior from old mongod with sharded collections, *do not* delete database,
                    // but inform user they can drop manually (or ignore).
                    warning() << "movePrimary legacy mongod behavior detected, user must manually remove unsharded collections in "
                          << "database " << dbname << " on " << oldPrimary << endl;

                }
                else {

                    // We moved some unsharded collections, but not all
                    BSONObjIterator it( cloneRes["clonedColls"].Obj() );

                    while( it.more() ){
                        BSONElement el = it.next();
                        if( el.type() == String ){
                            try {
                                log() << "movePrimary dropping cloned collection " << el.String() << " on " << oldPrimary << endl;
                                fromconn->get()->dropCollection( el.String() );
                            }
                            catch( DBException& e ){
                                e.addContext( str::stream() << "movePrimary could not drop the cloned collection " << el.String() << " on " << oldPrimary );
                                throw;
                            }
                        }
                    }
                }

                fromconn->done();

                result << "primary " << s.toString();

                return true;
            }
        } movePrimary;

        class EnableShardingCmd : public GridAdminCmd {
        public:
            EnableShardingCmd() : GridAdminCmd( "enableSharding" ) {}
            virtual void help( stringstream& help ) const {
                help
                        << "Enable sharding for a db. (Use 'shardcollection' command afterwards.)\n"
                        << "  { enablesharding : \"<dbname>\" }\n";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::enableSharding);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string dbname = cmdObj.firstElement().valuestrsafe();
                if ( dbname.size() == 0 ) {
                    errmsg = "no db";
                    return false;
                }
                
                if ( dbname == "admin" ) {
                    errmsg = "can't shard the admin db";
                    return false;
                }
                if ( dbname == "local" ) {
                    errmsg = "can't shard the local db";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( dbname );
                if ( config->isShardingEnabled() ) {
                    errmsg = "already enabled";
                    return false;
                }
                
                if ( ! okForConfigChanges( errmsg ) )
                    return false;
                
                log() << "enabling sharding on: " << dbname << endl;

                config->enableSharding();

                return true;
            }
        } enableShardingCmd;

        // ------------ collection level commands -------------

        class ShardCollectionCmd : public GridAdminCmd {
        public:
            ShardCollectionCmd() : GridAdminCmd( "shardCollection" ) {}

            virtual void help( stringstream& help ) const {
                help
                        << "Shard a collection.  Requires key.  Optional unique. Sharding must already be enabled for the database.\n"
                        << "  { enablesharding : \"<dbname>\" }\n";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::shardCollection);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                const string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                if ( !NamespaceString::isValid(ns) ){
                    errmsg = str::stream() << "bad ns[" << ns << "]";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isShardingEnabled() ) {
                    errmsg = "sharding not enabled for db";
                    return false;
                }

                if ( config->isSharded( ns ) ) {
                    errmsg = "already sharded";
                    return false;
                }

                BSONObj proposedKey = cmdObj.getObjectField( "key" );
                if ( proposedKey.isEmpty() ) {
                    errmsg = "no shard key";
                    return false;
                }

                // Currently the allowable shard keys are either
                // i) a hashed single field, e.g. { a : "hashed" }, or
                // ii) a compound list of ascending fields, e.g. { a : 1 , b : 1 }
                if ( proposedKey.firstElementType() == mongo::String ) {
                    // case i)
                    if ( !str::equals( proposedKey.firstElement().valuestrsafe() , "hashed" ) ) {
                        errmsg = "unrecognized string: " + proposedKey.firstElement().str();
                        return false;
                    }
                    if ( proposedKey.nFields() > 1 ) {
                        errmsg = "hashed shard keys currently only support single field keys";
                        return false;
                    }
                    if ( cmdObj["unique"].trueValue() ) {
                        // it's possible to ensure uniqueness on the hashed field by
                        // declaring an additional (non-hashed) unique index on the field,
                        // but the hashed shard key itself should not be declared unique
                        errmsg = "hashed shard keys cannot be declared unique.";
                        return false;
                    }
                } else {
                    // case ii)
                    BSONForEach(e, proposedKey) {
                        if (!e.isNumber() || e.number() != 1.0) {
                            errmsg = str::stream() << "Unsupported shard key pattern.  Pattern must"
                                                   << " either be a single hashed field, or a list"
                                                   << " of ascending fields.";
                            return false;
                        }
                    }
                }

                if ( NamespaceString::isSystem(ns) ) {
                    errmsg = "can't shard system namespaces";
                    return false;
                }

                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                //the rest of the checks require a connection to the primary db
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getScopedDbConnection(
                                        config->getPrimary().getConnString() ) );

                //check that collection is not capped
                BSONObj res = conn->get()->findOne( config->getName() + ".system.namespaces",
                                                    BSON( "name" << ns ) );
                if ( res["options"].type() == Object &&
                     res["options"].embeddedObject()["capped"].trueValue() ) {
                    errmsg = "can't shard capped collection";
                    conn->done();
                    return false;
                }
                if ( res["options"].type() == Object &&
                     res["options"].embeddedObject()["partitioned"].trueValue() ) {
                    errmsg = "can't shard partitioned collection";
                    conn->done();
                    return false;
                }

                // The proposed shard key must be validated against the set of existing indexes.
                // In particular, we must ensure the following constraints
                //
                // 1. All existing unique indexes, except those which start with the _id index,
                //    must contain the proposed key as a prefix (uniqueness of the _id index is
                //    ensured by the _id generation process or guaranteed by the user).
                //
                // 2. If the collection is not empty, there must exist at least one index that
                //    is "useful" for the proposed key.  A "useful" index is defined as follows
                //    Useful Index:
                //         i. contains proposedKey as a prefix
                //         ii. is not sparse
                //         iii. contains no null values
                //         iv. is not multikey (maybe lift this restriction later)
                //
                // 3. If the proposed shard key is specified as unique, there must exist a useful,
                //    unique index exactly equal to the proposedKey (not just a prefix).
                //
                // After validating these constraint:
                //
                // 4. If there is no useful index, and the collection is non-empty, we
                //    must fail.
                //
                // 5. If the collection is empty, and it's still possible to create an index
                //    on the proposed key, we go ahead and do so.

                string indexNS = config->getName() + ".system.indexes";

                // 1.  Verify consistency with existing unique indexes
                BSONObj uniqueQuery = BSON( "ns" << ns << "unique" << true );
                auto_ptr<DBClientCursor> uniqueQueryResult =
                                conn->get()->query( indexNS , uniqueQuery );

                ShardKeyPattern proposedShardKey( proposedKey );
                while ( uniqueQueryResult->more() ) {
                    BSONObj idx = uniqueQueryResult->next();
                    BSONObj currentKey = idx["key"].embeddedObject();
                    if( ! proposedShardKey.isUniqueIndexCompatible( currentKey ) ) {
                        errmsg = str::stream() << "can't shard collection '" << ns << "' "
                                               << "with unique index on " << currentKey << " "
                                               << "and proposed shard key " << proposedKey << ". "
                                               << "Uniqueness can't be maintained unless "
                                               << "shard key is a prefix";
                        conn->done();
                        return false;
                    }
                }

                // 2. Check for a useful index
                bool hasUsefulIndexForKey = false;

                BSONObj allQuery = BSON( "ns" << ns );
                auto_ptr<DBClientCursor> allQueryResult =
                                conn->get()->query( indexNS , allQuery );

                BSONArrayBuilder allIndexes;
                while ( allQueryResult->more() ) {
                    BSONObj idx = allQueryResult->next();
                    allIndexes.append( idx );
                    BSONObj currentKey = idx["key"].embeddedObject();
                    // Check 2.i. and 2.ii.
                    if ( ! idx["sparse"].trueValue() && proposedKey.isPrefixOf( currentKey ) ) {
                        BSONElement ce = cmdObj["clustering"];
                        if (idx["clustering"].trueValue()) {
                            if (ce.ok() && !ce.trueValue()) {
                                stringstream ss;
                                ss << "key " << currentKey << " is clustering, but a non-clustering key was requested.";
                                errmsg = ss.str();
                                conn->done();
                                return false;
                            }
                        }
                        else {
                            if (ce.ok() && !ce.trueValue()) {
                                LOG(0) << "WARNING: Sharding " << ns << " on non-clustering key " << currentKey
                                       << ". This may lead to poor performance of migrations and range queries." << endl;
                            }
                            else {
                                stringstream ss;
                                ss << "Sharding " << ns << " on the non-clustering key " << currentKey
                                   << " may lead to poor performance of migrations and range queries."
                                   << " Either consider sharding on a different key (with a clustering index)"
                                   << " or drop the index " << currentKey << " and re-create it as a"
                                   << " clustering index. If you are sure you want to shard on a non-clustering"
                                   << " index, try again with sh.shardCollection(\"" << ns << "\", {key: "
                                   << currentKey << ", clustering: false}).";
                                errmsg = ss.str();
                                LOG(0) << ss.str() << endl;
                                conn->done();
                                return false;
                            }
                        }
                        hasUsefulIndexForKey = true;
                    }
                }

                // 3. If proposed key is required to be unique, additionally check for exact match.
                bool careAboutUnique = cmdObj["unique"].trueValue();
                if ( hasUsefulIndexForKey && careAboutUnique ) {
                    BSONObj eqQuery = BSON( "ns" << ns << "key" << proposedKey );
                    BSONObj eqQueryResult = conn->get()->findOne( indexNS, eqQuery );
                    if ( eqQueryResult.isEmpty() ) {
                        hasUsefulIndexForKey = false;  // if no exact match, index not useful,
                                                       // but still possible to create one later
                    }
                    else {
                        bool isExplicitlyUnique = eqQueryResult["unique"].trueValue();
                        BSONObj currKey = eqQueryResult["key"].embeddedObject();
                        bool isCurrentID = str::equals( currKey.firstElementFieldName() , "_id" );
                        if ( ! isExplicitlyUnique && ! isCurrentID ) {
                            errmsg = str::stream() << "can't shard collection " << ns << ", "
                                                   << proposedKey << " index not unique, "
                                                   << "and unique index explicitly specified";
                            conn->done();
                            return false;
                        }
                    }
                }

                if ( hasUsefulIndexForKey ) {
                    // Check 2.iii and 2.iv. Make sure no null entries in the sharding index
                    // and that there is a useful, non-multikey index available
                    BSONObjBuilder cmd;
                    cmd.append( "checkShardingIndex" , ns );
                    cmd.append( "keyPattern" , proposedKey );
                    BSONObj cmdObj = cmd.obj();
                    if ( ! conn->get()->runCommand( nsToDatabase(ns) , cmdObj , res ) ) {
                        errmsg = res["errmsg"].str();
                        conn->done();
                        return false;
                    }
                }
                // 4. if no useful index, and collection is non-empty, fail
                else if ( conn->get()->count( ns ) != 0 ) {
                    errmsg = str::stream() << "please create a clustering index that starts with the "
                                           << "shard key before sharding.";
                    result.append( "proposedKey" , proposedKey );
                    result.appendArray( "curIndexes" , allIndexes.done() );
                    conn->done();
                    return false;
                }
                // 5. If no useful index exists, and collection empty, create one on proposedKey.
                //    Only need to call ensureIndex on primary shard, since indexes get copied to
                //    receiving shard whenever a migrate occurs.
                else {
                    bool collectionExists;
                    {
                        BSONObj res;
                        BSONObjBuilder cmd;
                        BSONArrayBuilder b(cmd.subarrayStart("_collectionsExist"));
                        b.append(ns);
                        b.doneFast();
                        collectionExists = conn->get()->runCommand(config->getName(), cmd.done(), res);
                    }
                    bool onlyId = proposedKey.nFields() == 1 && proposedKey["_id"].ok();
                    bool onlyHashed = proposedKey.nFields() == 1 && StringData(proposedKey.firstElement().valuestrsafe()) == "hashed";
                    if (!collectionExists && !onlyId && !onlyHashed && !careAboutUnique) {
                        BSONObjBuilder cmd;
                        cmd.append("create", NamespaceString(ns).coll);
                        BSONObjBuilder pk(cmd.subobjStart("primaryKey"));
                        pk.appendElements(proposedKey);
                        bool containsId = false;
                        for (BSONObjIterator it(proposedKey); it.more(); ) {
                            BSONElement e = it.next();
                            if (StringData(e.fieldName()) == "_id") {
                                uassert(17212, "_id must be ascending if present in shard key", e.numberLong() == 1);
                                uassert(17213, "_id:1 must be last field in shard key, if present", !it.more());
                                containsId = true;
                            }
                        }
                        if (!containsId) {
                            pk.append("_id", 1);
                        }
                        BSONObj pkObj = pk.done();

                        LOG(0) << "sharding non-existent collection, creating " << ns << " with a primary key on " << pkObj << endl;

                        BSONObj res;
                        bool createSuccess = conn->get()->runCommand(config->getName(), cmd.done(), res);
                        if (!createSuccess) {
                            errmsg = "create failed to create collection on primary shard: " + res["errmsg"].String();
                            conn->done();
                            return false;
                        }
                    } else {
                        BSONElement ce = cmdObj["clustering"];
                        bool clustering = (ce.ok() ? ce.trueValue() : true);
                        // call ensureIndex with cache=false, see SERVER-1691
                        bool ensureSuccess = conn->get()->ensureIndex(ns,
                                                                      proposedKey,
                                                                      careAboutUnique,
                                                                      clustering,
                                                                      "",
                                                                      false);
                        if ( ! ensureSuccess ) {
                            errmsg = "ensureIndex failed to create index on primary shard";
                            conn->done();
                            return false;
                        }
                    }
                }

                bool isEmpty = ( conn->get()->count( ns ) == 0 );

                conn->done();

                // Pre-splitting:
                // For new collections which use hashed shard keys, we can can pre-split the
                // range of possible hashes into a large number of chunks, and distribute them
                // evenly at creation time. Until we design a better initialization scheme, the
                // safest way to pre-split is to
                // 1. make one big chunk for each shard
                // 2. move them one at a time
                // 3. split the big chunks to achieve the desired total number of initial chunks

                vector<Shard> shards;
                Shard primary = config->getPrimary();
                primary.getAllShards( shards );
                int numShards = shards.size();

                vector<BSONObj> initSplits;  // there will be at most numShards-1 of these
                vector<BSONObj> allSplits;   // all of the initial desired split points

                bool isHashedShardKey =
                        str::equals(proposedKey.firstElement().valuestrsafe(), "hashed");

                // only pre-split when using a hashed shard key and collection is still empty
                if ( isHashedShardKey && isEmpty ){

                    int numChunks = cmdObj["numInitialChunks"].numberInt();
                    if ( numChunks <= 0 ) {
                        // In vanilla mongodb, this is 2*numShards.  This causes a lot of migrations
                        // during inital loading of data because it's easy for things to get out of
                        // whack with only a few chunks.  It's unclear why the default is set so
                        // low, but on vanilla it seems to cause a long startup time so that might
                        // be it (not sure where that comes from?).  With this setting, 256 chunks
                        // gives you a good runway of around 8GB per shard before you should expect
                        // to start having to split much.
                        numChunks = 256 * numShards;  // default number of initial chunks
                        // multiSplit has a max of (non-inclusive) 8192 split points.
                        if (numChunks > 4096) {
                            numChunks = 4096;
                        }
                    }

                    // hashes are signed, 64-bit ints. So we divide the range (-MIN long, +MAX long)
                    // into intervals of size (2^64/numChunks) and create split points at the
                    // boundaries.  The logic below ensures that initial chunks are all
                    // symmetric around 0.
                    long long intervalSize = ( std::numeric_limits<long long>::max()/ numChunks )*2;
                    long long current = 0;
                    if( numChunks % 2 == 0 ){
                        allSplits.push_back( BSON(proposedKey.firstElementFieldName() << current) );
                        current += intervalSize;
                    } else {
                        current += intervalSize/2;
                    }
                    for( int i=0; i < (numChunks-1)/2; i++ ){
                        allSplits.push_back( BSON(proposedKey.firstElementFieldName() << current) );
                        allSplits.push_back( BSON(proposedKey.firstElementFieldName() << -current));
                        current += intervalSize;
                    }
                    sort( allSplits.begin() , allSplits.end() );

                    // 1. the initial splits define the "big chunks" that we will subdivide later
                    int lastIndex = -1;
                    for ( int i = 1; i < numShards; i++ ){
                        if ( lastIndex < (i*numChunks)/numShards - 1 ){
                            lastIndex = (i*numChunks)/numShards - 1;
                            initSplits.push_back( allSplits[ lastIndex ] );
                        }
                    }
                }

                tlog() << "CMD: shardcollection: " << cmdObj << endl;

                config->shardCollection( ns , proposedKey , careAboutUnique , &initSplits );

                result << "collectionsharded" << ns;

                // only initially move chunks when using a hashed shard key
                if (isHashedShardKey) {

                    // Reload the new config info.  If we created more than one initial chunk, then
                    // we need to move them around to balance.
                    ChunkManagerPtr chunkManager = config->getChunkManager( ns , true );
                    ChunkMap chunkMap = chunkManager->getChunkMap();
                    // 2. Move and commit each "big chunk" to a different shard.
                    int i = 0;
                    for ( ChunkMap::const_iterator c = chunkMap.begin(); c != chunkMap.end(); ++c,++i ){
                        Shard to = shards[ i % numShards ];
                        ChunkPtr chunk = c->second;

                        // can't move chunk to shard it's already on
                        if ( to == chunk->getShard() )
                            continue;

                        BSONObj moveResult;
                        if ( ! chunk->moveAndCommit( to , moveResult ) ) {
                            warning() << "Couldn't move chunk " << chunk << " to shard "  << to
                                      << " while sharding collection " << ns << ". Reason: "
                                      <<  moveResult << endl;
                        }
                    }

                    if (allSplits.empty()) {
                        return true;
                    }

                    // Reload the config info, after all the migrations
                    chunkManager = config->getChunkManager( ns , true );

                    // 3. Subdivide the big chunks by splitting at each of the points in "allSplits"
                    //    that we haven't already split by.
                    ChunkPtr currentChunk = chunkManager->findIntersectingChunk( allSplits[0] );
                    vector<BSONObj> subSplits;
                    for ( unsigned i = 0 ; i <= allSplits.size(); i++){
                        if ( i == allSplits.size() || ! currentChunk->containsPoint( allSplits[i] ) ) {
                            if ( ! subSplits.empty() ){
                                BSONObj splitResult;
                                if ( ! currentChunk->multiSplit( subSplits , splitResult ) ){
                                    warning() << "Couldn't split chunk " << currentChunk
                                              << " while sharding collection " << ns << ". Reason: "
                                              << splitResult << endl;
                                }
                                subSplits.clear();
                            }
                            if ( i < allSplits.size() )
                                currentChunk = chunkManager->findIntersectingChunk( allSplits[i] );
                        } else {
                            subSplits.push_back( allSplits[i] );
                        }
                    }

                    // Proactively refresh the chunk manager. Not really necessary, but this way it's
                    // immediately up-to-date the next time it's used.
                    config->getChunkManager( ns , true );
                }

                return true;
            }
        } shardCollectionCmd;

        class GetShardVersion : public GridAdminCmd {
        public:
            GetShardVersion() : GridAdminCmd( "getShardVersion" ) {}
            virtual void help( stringstream& help ) const {
                help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::getShardVersion);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "need to specify fully namespace";
                    return false;
                }

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    errmsg = "ns not sharded.";
                    return false;
                }

                ChunkManagerPtr cm = config->getChunkManagerIfExists( ns );
                if ( ! cm ) {
                    errmsg = "no chunk manager?";
                    return false;
                }
                cm->_printChunks();
                cm->getVersion().addToBSON( result );

                return 1;
            }
        } getShardVersionCmd;

        class SplitCollectionCmd : public GridAdminCmd {
        public:
            SplitCollectionCmd() : GridAdminCmd( "split" ) {}
            virtual void help( stringstream& help ) const {
                help
                        << " example: - split the shard that contains give key \n"
                        << " { split : 'alleyinsider.blog.posts' , find : { ts : 1 } }\n"
                        << " example: - split the shard that contains the key with this as the middle \n"
                        << " { split : 'alleyinsider.blog.posts' , middle : { ts : 1 } }\n"
                        << " NOTE: this does not move move the chunks, it merely creates a logical separation \n"
                        ;
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::split);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                ShardConnection::sync( nsToDatabase(ns) );

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    config->reload();
                    if ( ! config->isSharded( ns ) ) {
                        errmsg = "ns not sharded.  have to shard before can split";
                        return false;
                    }
                }

                const BSONField<BSONObj> findField("find", BSONObj());
                const BSONField<BSONArray> boundsField("bounds", BSONArray());
                const BSONField<BSONObj> middleField("middle", BSONObj());

                BSONObj find;
                if (FieldParser::extract(cmdObj, findField, &find, &errmsg) ==
                        FieldParser::FIELD_INVALID) {
                    return false;
                }

                BSONArray bounds;
                if (FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg) ==
                        FieldParser::FIELD_INVALID) {
                    return false;
                }

                if (!bounds.isEmpty()) {
                    if (!bounds.hasField("0")) {
                        errmsg = "lower bound not specified";
                        return false;
                    }

                    if (!bounds.hasField("1")) {
                        errmsg = "upper bound not specified";
                        return false;
                    }
                }

                if (!find.isEmpty() && !bounds.isEmpty()) {
                    errmsg = "cannot specify bounds and find at the same time";
                    return false;
                }

                BSONObj middle;
                if (FieldParser::extract(cmdObj, middleField, &middle, &errmsg) ==
                        FieldParser::FIELD_INVALID) {
                    return false;
                }

                if (find.isEmpty() && bounds.isEmpty() && middle.isEmpty()) {
                    errmsg = "need to specify find/bounds or middle";
                    return false;
                }

                if (!find.isEmpty() && !middle.isEmpty()) {
                    errmsg = "cannot specify find and middle together";
                    return false;
                }

                if (!bounds.isEmpty() && !middle.isEmpty()) {
                    errmsg = "cannot specify bounds and middle together";
                    return false;
                }

                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr chunk;

                if (!find.isEmpty()) {
                    chunk = info->findChunkForDoc(find);
                }
                else if (!bounds.isEmpty()) {
                    chunk = info->findIntersectingChunk(bounds[0].Obj());
                    verify(chunk.get());

                    if (chunk->getMin() != bounds[0].Obj() ||
                        chunk->getMax() != bounds[1].Obj()) {
                        errmsg = "no chunk found from the given upper and lower bounds";
                        return false;
                    }
                }
                else { // middle
                    chunk = info->findIntersectingChunk(middle);
                }

                verify(chunk.get());
                log() << "splitting: " << ns << "  shard: " << chunk << endl;

                BSONObj res;
                bool worked;
                if ( middle.isEmpty() ) {
                    BSONObj ret = chunk->singleSplit( true /* force a split even if not enough data */ , res );
                    worked = !ret.isEmpty();
                }
                else {
                    // sanity check if the key provided is a valid split point
                    if ( ( middle == chunk->getMin() ) || ( middle == chunk->getMax() ) ) {
                        errmsg = "cannot split on initial or final chunk's key";
                        return false;
                    }

                    if (!fieldsMatch(middle, info->getShardKey().key())){
                        errmsg = "middle has different fields (or different order) than shard key";
                        return false;
                    }

                    vector<BSONObj> splitPoints;
                    splitPoints.push_back( middle );
                    worked = chunk->multiSplit( splitPoints , res );
                }

                if ( !worked ) {
                    errmsg = "split failed";
                    result.append( "cause" , res );
                    return false;
                }
                config->getChunkManager( ns , true );
                return true;
            }
        } splitCollectionCmd;

        class MoveChunkCmd : public GridAdminCmd {
        public:
            MoveChunkCmd() : GridAdminCmd( "moveChunk" ) {}
            virtual void help( stringstream& help ) const {
                help << "Example: move chunk that contains the doc {num : 7} to shard001\n"
                     << "  { movechunk : 'test.foo' , find : { num : 7 } , to : 'shard0001' }\n"
                     << "Example: move chunk with lower bound 0 and upper bound 10 to shard001\n"
                     << "  { movechunk : 'test.foo' , bounds : [ { num : 0 } , { num : 10 } ] "
                     << " , to : 'shard001' }\n";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::moveChunk);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                if ( ! okForConfigChanges( errmsg ) )
                    return false;

                string ns = cmdObj.firstElement().valuestrsafe();
                if ( ns.size() == 0 ) {
                    errmsg = "no ns";
                    return false;
                }

                ShardConnection::sync( nsToDatabase(ns) );

                Timer t;

                DBConfigPtr config = grid.getDBConfig( ns );
                if ( ! config->isSharded( ns ) ) {
                    config->reload();
                    if ( ! config->isSharded( ns ) ) {
                        errmsg = "ns not sharded.  have to shard before we can move a chunk";
                        return false;
                    }
                }

                string toString = cmdObj["to"].valuestrsafe();
                if ( ! toString.size()  ) {
                    errmsg = "you have to specify where you want to move the chunk";
                    return false;
                }

                Shard to = Shard::make( toString );

                BSONObj find = cmdObj.getObjectField( "find" );
                BSONObj bounds = cmdObj.getObjectField( "bounds" );

                // check that only one of the two chunk specification methods is used
                if ( find.isEmpty() == bounds.isEmpty() ) {
                    errmsg = "need to specify either a find query, or both lower and upper bounds.";
                    return false;
                }

                ChunkManagerPtr info = config->getChunkManager( ns );
                ChunkPtr c = find.isEmpty() ?
                                info->findIntersectingChunk( bounds[0].Obj() ) :
                                info->findChunkForDoc( find );

                if ( ! bounds.isEmpty() && ( c->getMin() != bounds[0].Obj() ||
                                             c->getMax() != bounds[1].Obj() ) ) {
                    errmsg = "no chunk found with those upper and lower bounds";
                    return false;
                }

                const Shard& from = c->getShard();

                if ( from == to ) {
                    errmsg = "that chunk is already on that shard";
                    return false;
                }

                tlog() << "CMD: movechunk: " << cmdObj << endl;

                BSONObj res;
                if ( ! c->moveAndCommit(to, res) ) {
                    errmsg = "move failed";
                    result.append( "cause" , res );
                    return false;
                }
                
                // preemptively reload the config to get new version info
                config->getChunkManager( ns , true );

                result.append( "millis" , t.millis() );
                return true;
            }
        } moveChunkCmd;

        // ------------ server level commands -------------

        class ListShardsCmd : public GridAdminCmd {
        public:
            ListShardsCmd() : GridAdminCmd("listShards") { }
            virtual void help( stringstream& help ) const {
                help << "list all shards of the system";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::listShards);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                configServer.getPrimary().getConnString(), 30));

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->get()->query( ShardType::ConfigNS , BSONObj() );
                while ( cursor->more() ) {
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }

                result.append("shards" , all );
                conn->done();

                return true;
            }
        } listShardsCmd;

        /* a shard is a single mongod server or a replica pair.  add it (them) to the cluster as a storage partition. */
        class AddShard : public GridAdminCmd {
        public:
            AddShard() : GridAdminCmd("addShard") { }
            virtual void help( stringstream& help ) const {
                help << "add a new shard to the system";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::addShard);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg.clear();

                // get replica set component hosts
                ConnectionString servers = ConnectionString::parse( cmdObj.firstElement().valuestrsafe() , errmsg );
                if ( ! errmsg.empty() ) {
                    log() << "addshard request " << cmdObj << " failed:" << errmsg << endl;
                    return false;
                }

                // using localhost in server names implies every other process must use localhost addresses too
                vector<HostAndPort> serverAddrs = servers.getServers();
                for ( size_t i = 0 ; i < serverAddrs.size() ; i++ ) {
                    if ( serverAddrs[i].isLocalHost() != grid.allowLocalHost() ) {
                        errmsg = str::stream() << 
                            "can't use localhost as a shard since all shards need to communicate. " <<
                            "either use all shards and configdbs in localhost or all in actual IPs " << 
                            " host: " << serverAddrs[i].toString() << " isLocalHost:" << serverAddrs[i].isLocalHost();
                        
                        log() << "addshard request " << cmdObj << " failed: attempt to mix localhosts and IPs" << endl;
                        return false;
                    }

                    // it's fine if mongods of a set all use default port
                    if ( ! serverAddrs[i].hasPort() ) {
                        serverAddrs[i].setPort( CmdLine::ShardServerPort );
                    }
                }

                // name is optional; addShard will provide one if needed
                string name = "";
                if ( cmdObj["name"].type() == String ) {
                    name = cmdObj["name"].valuestrsafe();
                }

                // maxSize is the space usage cap in a shard in MBs
                long long maxSize = 0;
                if ( cmdObj[ ShardType::maxSize() ].isNumber() ) {
                    maxSize = cmdObj[ ShardType::maxSize() ].numberLong();
                }

                if ( ! grid.addShard( &name , servers , maxSize , errmsg ) ) {
                    log() << "addshard request " << cmdObj << " failed: " << errmsg << endl;
                    return false;
                }

                result << "shardAdded" << name;
                return true;
            }

        } addServer;

        /* See usage docs at:
         * http://dochub.mongodb.org/core/configuringsharding#ConfiguringSharding-Removingashard
         */
        class RemoveShardCmd : public GridAdminCmd {
        public:
            RemoveShardCmd() : GridAdminCmd("removeShard") { }
            virtual void help( stringstream& help ) const {
                help << "remove a shard to the system.";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::removeShard);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string target = cmdObj.firstElement().valuestrsafe();
                Shard s = Shard::make( target );
                if ( ! grid.knowAboutShard( s.getConnString() ) ) {
                    errmsg = "unknown shard";
                    return false;
                }

                scoped_ptr<ScopedDbConnection> connPtr(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                configServer.getPrimary().getConnString(), 30));
                ScopedDbConnection& conn = *connPtr;

                if (conn->count(ShardType::ConfigNS,
                                BSON(ShardType::name() << NE << s.getName() <<
                                     ShardType::draining(true)))){
                    conn.done();
                    errmsg = "Can't have more than one draining shard at a time";
                    return false;
                }

                if (conn->count(ShardType::ConfigNS, BSON(ShardType::name() << NE << s.getName())) == 0){
                    conn.done();
                    errmsg = "Can't remove last shard";
                    return false;
                }

                BSONObj primaryDoc = BSON(DatabaseType::name.ne("local") <<
                                          DatabaseType::primary(s.getName()));
                BSONObj dbInfo; // appended at end of result on success
                {
                    boost::scoped_ptr<DBClientCursor> cursor (conn->query(DatabaseType::ConfigNS, primaryDoc));
                    if (cursor->more()) { // skip block and allocations if empty
                        BSONObjBuilder dbInfoBuilder;
                        dbInfoBuilder.append("note", "you need to drop or movePrimary these databases");
                        BSONArrayBuilder dbs(dbInfoBuilder.subarrayStart("dbsToMove"));

                        while (cursor->more()){
                            BSONObj db = cursor->nextSafe();
                            dbs.append(db[DatabaseType::name()]);
                        }
                        dbs.doneFast();

                        dbInfo = dbInfoBuilder.obj();
                    }
                }

                // If the server is not yet draining chunks, put it in draining mode.
                BSONObj searchDoc = BSON(ShardType::name() << s.getName());
                BSONObj drainingDoc = BSON(ShardType::name() << s.getName() << ShardType::draining(true));
                BSONObj shardDoc = conn->findOne(ShardType::ConfigNS, drainingDoc);
                if ( shardDoc.isEmpty() ) {

                    // TODO prevent move chunks to this shard.

                    log() << "going to start draining shard: " << s.getName() << endl;
                    BSONObj newStatus = BSON( "$set" << BSON( ShardType::draining(true) ) );
                    conn->update( ShardType::ConfigNS , searchDoc , newStatus, false /* do no upsert */);

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ) {
                        log() << "error starting remove shard: " << s.getName() << " err: " << errmsg << endl;
                        return false;
                    }

                    BSONObj primaryLocalDoc = BSON(DatabaseType::name("local") <<
                                                   DatabaseType::primary(s.getName()));
                    PRINT(primaryLocalDoc);
                    if (conn->count(DatabaseType::ConfigNS, primaryLocalDoc)) {
                        log() << "This shard is listed as primary of local db. Removing entry." << endl;
                        conn->remove(DatabaseType::ConfigNS, BSON(DatabaseType::name("local")));
                        errmsg = conn->getLastError();
                        if ( errmsg.size() ) {
                            log() << "error removing local db: " << errmsg << endl;
                            return false;
                        }
                    }

                    Shard::reloadShardInfo();

                    result.append( "msg"   , "draining started successfully" );
                    result.append( "state" , "started" );
                    result.append( "shard" , s.getName() );
                    result.appendElements(dbInfo);
                    conn.done();
                    return true;
                }

                // If the server has been completely drained, remove it from the ConfigDB.
                // Check not only for chunks but also databases.
                BSONObj shardIDDoc = BSON(ChunkType::shard(shardDoc[ShardType::name()].str()));
                long long chunkCount = conn->count(ChunkType::ConfigNS, shardIDDoc);
                long long dbCount = conn->count( DatabaseType::ConfigNS , primaryDoc );
                if ( ( chunkCount == 0 ) && ( dbCount == 0 ) ) {
                    log() << "going to remove shard: " << s.getName() << endl;
                    conn->remove( ShardType::ConfigNS , searchDoc );

                    errmsg = conn->getLastError();
                    if ( errmsg.size() ) {
                        log() << "error concluding remove shard: " << s.getName() << " err: " << errmsg << endl;
                        return false;
                    }

                    string shardName = shardDoc[ ShardType::name() ].str();
                    Shard::removeShard( shardName );
                    shardConnectionPool.removeHost( shardName );
                    ReplicaSetMonitor::remove( shardName, true );
                    Shard::reloadShardInfo();

                    result.append( "msg"   , "removeshard completed successfully" );
                    result.append( "state" , "completed" );
                    result.append( "shard" , s.getName() );
                    conn.done();
                    return true;
                }

                // If the server is already in draining mode, just report on its progress.
                // Report on databases (not just chunks) that are left too.
                result.append( "msg"  , "draining ongoing" );
                result.append( "state" , "ongoing" );
                BSONObjBuilder inner;
                inner.append( "chunks" , chunkCount );
                inner.append( "dbs" , dbCount );
                result.append( "remaining" , inner.obj() );
                result.appendElements(dbInfo);

                conn.done();
                return true;
            }
        } removeShardCmd;


        // --------------- public commands ----------------

        class IsDbGridCmd : public InformationCommand {
        public:
            virtual bool requiresAuth() { return false; }
            IsDbGridCmd() : InformationCommand("isdbgrid") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", getHostNameCached());
                return true;
            }
        } isdbgrid;

        class CmdIsMaster : public InformationCommand {
        public:
            virtual bool requiresAuth() { return false; }
            virtual void help( stringstream& help ) const {
                help << "test if this is master half of a replica pair";
            }
            CmdIsMaster() : InformationCommand("isMaster" , false , "ismaster") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result.appendBool("ismaster", true );
                result.append("msg", "isdbgrid");
                result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
                result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
                result.appendDate("localTime", jsTime());

                return true;
            }
        } ismaster;

        class CmdWhatsMyUri : public InformationCommand {
        public:
            CmdWhatsMyUri() : InformationCommand("whatsmyuri") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            virtual void help( stringstream &help ) const {
                help << "{whatsmyuri:1}";
            }
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                result << "you" << ClientInfo::get()->getRemote();
                return true;
            }
        } cmdWhatsMyUri;


        class CmdShardingGetPrevError : public InformationCommand {
        public:
            virtual bool requiresAuth() { return false; }
            virtual void help( stringstream& help ) const {
                help << "get previous error (since last reseterror command)";
            }
            CmdShardingGetPrevError() : InformationCommand( "getPrevError" , false , "getpreverror") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required
            virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg += "getpreverror not supported for sharded environments";
                return false;
            }
        } cmdGetPrevError;

        class CmdShardingGetLastError : public InformationCommand {
        public:
            virtual void help( stringstream& help ) const {
                help << "check for an error on the last command executed";
            }
            CmdShardingGetLastError() : InformationCommand("getLastError" , false , "getlasterror") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {} // No auth required

            virtual bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                LastError *le = lastError.disableForCommand();
                verify( le );
                {
                    if ( le->msg.size() && le->nPrev == 1 ) {
                        le->appendSelf( result );
                        return true;
                    }
                }
                ClientInfo * client = ClientInfo::get();
                bool res = client->getLastError( dbName, cmdObj , result, errmsg );
                client->disableForCommand();
                return res;
            }
        } cmdGetLastError;

    }

    class CmdShardingResetError : public InformationCommand {
    public:
        CmdShardingResetError() : InformationCommand( "resetError" , false , "reseterror" ) {}

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            LastError *le = lastError.get();
            if ( le )
                le->reset();

            ClientInfo * client = ClientInfo::get();
            set<string> * shards = client->getPrev();

            for ( set<string>::iterator i = shards->begin(); i != shards->end(); i++ ) {
                string theShard = *i;
                ShardConnection conn( theShard , "" );
                BSONObj res;
                conn->runCommand( dbName , cmdObj , res );
                conn.done();
            }

            return true;
        }
    } cmdShardingResetError;

    class CmdListDatabases : public WebInformationCommand {
    public:
        CmdListDatabases() : WebInformationCommand("listDatabases", true, "listdatabases") {}

        virtual bool logTheOp() { return false; }
        virtual bool adminOnly() const { return true; }
        virtual void help( stringstream& help ) const { help << "list databases on cluster"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::listDatabases);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }

        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            vector<Shard> shards;
            Shard::getAllShards( shards );

            map<string, long long> uncompressedSizes;
            map<string, long long> compressedSizes;
            map<string, shared_ptr<BSONObjBuilder> > dbShardInfo;

            for ( vector<Shard>::iterator i=shards.begin(); i!=shards.end(); i++ ) {
                Shard s = *i;
                BSONObj x = s.runCommand( "admin" , "listDatabases" );

                BSONObjIterator j( x["databases"].Obj() );
                while ( j.more() ) {
                    BSONObj theDB = j.next().Obj();

                    string name = theDB["name"].String();
                    size_t uncompressedSize = theDB["size"].numberLong();
                    size_t compressedSize = theDB["sizeOnDisk"].numberLong();

                    uncompressedSizes[name] += uncompressedSize;

                    long long& totalCompressedSize = compressedSizes[name];
                    if (compressedSize == 1) {
                        if (totalCompressedSize <= 1) {
                            totalCompressedSize = 1;
                        }
                    }
                    else {
                        totalCompressedSize += compressedSize;
                    }

                    shared_ptr<BSONObjBuilder>& bb = dbShardInfo[name];
                    if ( ! bb.get() )
                        bb.reset( new BSONObjBuilder() );
                    BSONObjBuilder shardb(bb->subobjStart(s.getName()));
                    shardb.appendNumber("size", uncompressedSize);
                    shardb.appendNumber("sizeOnDisk", compressedSize);
                    shardb.doneFast();
                }

            }

            size_t totalUncompressedSize = 0;
            size_t totalCompressedSize = 0;

            BSONArrayBuilder bb(result.subarrayStart("databases"));
            for (map<string, long long>::iterator i = uncompressedSizes.begin(); i != uncompressedSizes.end(); ++i) {
                const string &name = i->first;

                if (name == "local") {
                    // we don't return local
                    // since all shards have their own independent local
                    continue;
                }

                if (name == "config" || name == "admin") {
                    //always get this from the config servers
                    continue;
                }

                map<string, long long>::iterator ci = compressedSizes.find(name);

                long long uncompressedSize = i->second;
                totalUncompressedSize += uncompressedSize;
                long long compressedSize = ci->second;
                totalCompressedSize += compressedSize;

                BSONObjBuilder temp(bb.subobjStart());
                temp.append("name", name);
                temp.appendNumber("size", uncompressedSize);
                temp.appendNumber("sizeOnDisk", compressedSize);
                temp.appendBool("empty", uncompressedSize == 1);
                temp.append("shards", dbShardInfo[name]->obj());

                temp.doneFast();
            }
            
            { // get config db from the config servers (first one)
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                configServer.getPrimary().getConnString(), 30));
                BSONObj x;
                if (conn->get()->simpleCommand("config", &x, "dbstats")) {
                    BSONObjBuilder b(bb.subobjStart());
                    b.append("name", "config");
                    b.appendBool("empty", false);
                    if (x["dataSize"].ok()) {
                        b.appendAs(x["dataSize"], "size");
                    } else {
                        b.append("size", 1);
                    }
                    if (x["storageSize"].ok()) {
                        b.appendAs(x["storageSize"], "sizeOnDisk");
                    } else {
                        b.append("sizeOnDisk", 1);
                    }
                    b.doneFast();
                } else {
                    bb.append(BSON( "name" << "config" ));
                }
                conn->done();
            }

            { // get admin db from the config servers (first one)
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                configServer.getPrimary().getConnString(), 30));
                BSONObj x;
                if (conn->get()->simpleCommand("admin", &x, "dbstats")) {
                    BSONObjBuilder b(bb.subobjStart());;
                    b.append("name", "admin");
                    b.appendBool("empty", false);
                    if (x["dataSize"].ok()) {
                        b.appendAs(x["dataSize"], "size");
                    } else {
                        b.append("size", 1);
                    }
                    if (x["storageSize"].ok()) {
                        b.appendAs(x["storageSize"], "sizeOnDisk");
                    } else {
                        b.append("sizeOnDisk", 1);
                    }
                    b.doneFast();
                }
                else {
                    bb.append(BSON( "name" << "admin" ));
                }
                conn->done();
            }

            bb.done();

            result.appendNumber("totalSize", double(totalCompressedSize));
            result.appendNumber("totalUncompressedSize", double(totalUncompressedSize));

            return 1;
        }

    } cmdListDatabases;

    class CmdCloseAllDatabases : public InformationCommand {
    public:
        CmdCloseAllDatabases() : InformationCommand("closeAllDatabases") {}
        virtual bool logTheOp() { return false; }
        virtual bool adminOnly() const { return true; }
        virtual void help( stringstream& help ) const { help << "Not supported sharded"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::closeAllDatabases);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }

        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& /*result*/, bool /*fromRepl*/) {
            errmsg = "closeAllDatabases isn't supported through mongos";
            return false;
        }
    } cmdCloseAllDatabases;


    class CmdReplSetGetStatus : public InformationCommand {
    public:
        CmdReplSetGetStatus() : InformationCommand("replSetGetStatus"){}
        virtual bool logTheOp() { return false; }
        virtual bool adminOnly() const { return true; }
        virtual void help( stringstream& help ) const { help << "Not supported through mongos"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // TODO: Should this require no auth since it's not supported in mongos anyway?
            ActionSet actions;
            actions.addAction(ActionType::replSetGetStatus);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        bool run(const string& , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            if ( jsobj["forShell"].trueValue() ) {
                lastError.disableForCommand();
                ClientInfo::get()->disableForCommand();
            }

            errmsg = "replSetGetStatus is not supported through mongos";
            result.append("info", "mongos"); // see sayReplSetMemberState
            return false;
        }
    } cmdReplSetGetStatus;

    CmdShutdown cmdShutdown;

    void CmdShutdown::help( stringstream& help ) const {
        help << "shutdown the database.  must be ran against admin db and "
             << "either (1) ran from localhost or (2) authenticated.";
    }

    bool CmdShutdown::run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        return shutdownHelper();
    }

} // namespace mongo
