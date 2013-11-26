

doTest = function (signal, startPort) {

    var num = 3;
    var host = getHostName();
    var name = "bigtxn_faulty_network";
    var timeout = 60000;

    var replTest = new ReplSetTest( {name: name, nodes: num, startPort:startPort, txnMemLimit: 1} );
    var conns = replTest.startSet();
    var port = replTest.ports;
    var config = {_id : name, members :
            [
             {_id:0, host : host+":"+port[0], priority:10 },
             {_id:1, host : host+":"+port[1]},
             {_id:2, host : host+":"+port[2], arbiterOnly : true},
            ],
             };

    replTest.initiate(config);
    replTest.awaitReplication();
	replTest.bridge();
    assert.soon(function() { return conns[0].getDB("admin").isMaster().ismaster; });

    // Make sure we have a master
    conns[0].setSlaveOk();
    conns[1].setSlaveOk();
    var masterdb = conns[0].getDB("foo");

	assert.commandWorked(masterdb.beginTransaction());
	for (i = 0; i < 1000000; i++) {
		masterdb.foo.insert({a:1});
		if (i % 100000 == 0) {
			print("i " + i);
		}
	}
	assert.commandWorked(masterdb.commitTransaction());
	print("transaction committed, now awaiting replication\n");

	for (i = 0; i < 10; i++) {
		replTest.partition(0,1);
		sleep(200);
		replTest.unPartition(0,1);
	}
	
	replTest.awaitReplication();
	print("awaiting replication done\n");

};

print("bigtxn_faulty_network.js");

doTest( 15, 41000 );

