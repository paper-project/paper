#include <gtest/gtest.h>
#include <paper/node.hpp>
#include <cryptopp/filters.h>
#include <cryptopp/randpool.h>

// Init returns an error if it can't open files at the path
TEST (ledger, store_error)
{
	bool init (false);
	paper::block_store store (init, boost::filesystem::path ("///"));
	ASSERT_FALSE (!init);
	paper::ledger ledger (store);
}

// Ledger can be initialized and retuns a basic query for an empty account
TEST (ledger, empty)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::account account;
	paper::transaction transaction (store.environment, nullptr, false);
	auto balance (ledger.account_balance (transaction, account));
	ASSERT_TRUE (balance.is_zero ());
}

// Genesis account should have the max balance on empty initialization
TEST (ledger, genesis_balance)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	auto balance (ledger.account_balance (transaction, paper::genesis_account));
	ASSERT_EQ (paper::genesis_amount, balance);
	paper::account_info info;
	ASSERT_FALSE (store.account_get (transaction, paper::genesis_account, info));
	// Frontier time should have been updated when genesis balance was added
	ASSERT_GE (store.now (), info.modified);
	ASSERT_LT (store.now () - info.modified, 10);
}

// Make sure the checksum is the same when ledger reloaded
TEST (ledger, checksum_persistence)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::uint256_union checksum1;
	paper::uint256_union max;
	max.qwords [0] = 0;
	max.qwords [0] = ~max.qwords [0];
	max.qwords [1] = 0;
	max.qwords [1] = ~max.qwords [1];
	max.qwords [2] = 0;
	max.qwords [2] = ~max.qwords [2];
	max.qwords [3] = 0;
	max.qwords [3] = ~max.qwords [3];
	paper::transaction transaction (store.environment, nullptr, true);
	{
		paper::ledger ledger (store);
		paper::genesis genesis;
		genesis.initialize (transaction, store);
		checksum1 = ledger.checksum (transaction, 0, max);
	}
	paper::ledger ledger (store);
	ASSERT_EQ (checksum1, ledger.checksum (transaction, 0, max));
}

// All nodes in the system should agree on the genesis balance
TEST (system, system_genesis)
{
	paper::system system (24000, 2);
	for (auto & i: system.nodes)
	{
		paper::transaction transaction (i->store.environment, nullptr, false);
		ASSERT_EQ (paper::genesis_amount, i->ledger.account_balance (transaction, paper::genesis_account));
	}
}

// Create a send block and publish it.
TEST (ledger, process_send)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::transaction transaction (store.environment, nullptr, true);
	paper::genesis genesis;
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, info1.head));
	// This was a valid block, it should progress.
	auto return1 (ledger.process (transaction, send));
	ASSERT_TRUE (store.frontier_get (transaction, info1.head).is_zero ());
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, hash1));
	ASSERT_EQ (paper::process_result::progress, return1.code);
	ASSERT_EQ (paper::test_genesis_key.pub, return1.account);
	ASSERT_EQ (50, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	paper::account_info info2;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info2));
	auto latest6 (store.block_get (transaction, info2.head));
	ASSERT_NE (nullptr, latest6);
	auto latest7 (dynamic_cast <paper::send_block *> (latest6.get ()));
	ASSERT_NE (nullptr, latest7);
	ASSERT_EQ (send, *latest7);
	// Create an open block opening an account accepting the send we just created
	paper::open_block open (hash1, key2.pub, key2.pub, key2.prv, key2.pub, 0);
	paper::block_hash hash2 (open.hash ());
	// This was a valid block, it should progress.
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (key2.pub, store.frontier_get (transaction, hash2));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (50, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.weight (transaction, key2.pub));
	paper::account_info info3;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info3));
	auto latest2 (store.block_get (transaction, info3.head));
	ASSERT_NE (nullptr, latest2);
	auto latest3 (dynamic_cast <paper::send_block *> (latest2.get ()));
	ASSERT_NE (nullptr, latest3);
	ASSERT_EQ (send, *latest3);
	paper::account_info info4;
	ASSERT_FALSE (store.account_get (transaction, key2.pub, info4));
	auto latest4 (store.block_get (transaction, info4.head));
	ASSERT_NE (nullptr, latest4);
	auto latest5 (dynamic_cast <paper::open_block *> (latest4.get ()));
	ASSERT_NE (nullptr, latest5);
	ASSERT_EQ (open, *latest5);
	ledger.rollback (transaction, hash2);
	ASSERT_TRUE (store.frontier_get (transaction, hash2).is_zero ());
	paper::account_info info5;
	ASSERT_TRUE (ledger.store.account_get (transaction, key2.pub, info5));
	paper::receivable receivable1;
	ASSERT_FALSE (ledger.store.pending_get (transaction, hash1, receivable1));
	ASSERT_EQ (paper::test_genesis_key.pub, receivable1.source);
	ASSERT_EQ (key2.pub, receivable1.destination);
	ASSERT_EQ (paper::genesis_amount - 50, receivable1.amount.number ());
	ASSERT_EQ (0, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (50, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key2.pub));
	paper::account_info info6;
	ASSERT_FALSE (ledger.store.account_get (transaction, paper::test_genesis_key.pub, info6));
	ASSERT_EQ (hash1, info6.head);
	ledger.rollback (transaction, info6.head);
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, info1.head));
	ASSERT_TRUE (store.frontier_get (transaction, hash1).is_zero ());
	paper::account_info info7;
	ASSERT_FALSE (ledger.store.account_get (transaction, paper::test_genesis_key.pub, info7));
	ASSERT_EQ (info1.head, info7.head);
	paper::receivable receivable2;
	ASSERT_TRUE (ledger.store.pending_get (transaction, hash1, receivable2));
	ASSERT_EQ (paper::genesis_amount, ledger.account_balance (transaction, paper::test_genesis_key.pub));
}

TEST (ledger, process_receive)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	paper::keypair key3;
	paper::open_block open (hash1, key3.pub, key2.pub, key2.prv, key2.pub, 0);
	paper::block_hash hash2 (open.hash ());
	auto return1 (ledger.process (transaction, open));
	ASSERT_EQ (paper::process_result::progress, return1.code);
	ASSERT_EQ (key2.pub, return1.account);
	ASSERT_EQ (paper::genesis_amount - 50, ledger.weight (transaction, key3.pub));
	paper::send_block send2 (hash1, key2.pub, 25, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash3 (send2.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send2).code);
	paper::receive_block receive (hash2, hash3, key2.prv, key2.pub, 0);
	auto hash4 (receive.hash ());
	ASSERT_EQ (key2.pub, store.frontier_get (transaction, hash2));
	auto return2 (ledger.process (transaction, receive));
	ASSERT_TRUE (store.frontier_get (transaction, hash2).is_zero ());
	ASSERT_EQ (key2.pub, store.frontier_get (transaction, hash4));
	ASSERT_EQ (paper::process_result::progress, return2.code);
	ASSERT_EQ (key2.pub, return2.account);
	ASSERT_EQ (hash4, ledger.latest (transaction, key2.pub));
	ASSERT_EQ (25, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount - 25, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (paper::genesis_amount - 25, ledger.weight (transaction, key3.pub));
	ledger.rollback (transaction, hash4);
	ASSERT_EQ (key2.pub, store.frontier_get (transaction, hash2));
	ASSERT_TRUE (store.frontier_get (transaction, hash4).is_zero ());
	ASSERT_EQ (25, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.weight (transaction, key3.pub));
	ASSERT_EQ (hash2, ledger.latest (transaction, key2.pub));
	paper::receivable receivable1;
	ASSERT_FALSE (ledger.store.pending_get (transaction, hash3, receivable1));
	ASSERT_EQ (paper::test_genesis_key.pub, receivable1.source);
	ASSERT_EQ (25, receivable1.amount.number ());
}

TEST (ledger, rollback_receiver)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	paper::keypair key3;
	paper::open_block open (hash1, key3.pub, key2.pub, key2.prv, key2.pub, 0);
	paper::block_hash hash2 (open.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (hash2, ledger.latest (transaction, key2.pub));
	ASSERT_EQ (50, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (50, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key2.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.weight (transaction, key3.pub));
	ledger.rollback (transaction, hash1);
	ASSERT_EQ (paper::genesis_amount, ledger.account_balance (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key2.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key3.pub));
	paper::account_info info2;
	ASSERT_TRUE (ledger.store.account_get (transaction, key2.pub, info2));
	paper::receivable receivable1;
	ASSERT_TRUE (ledger.store.pending_get (transaction, info2.head, receivable1));
}

TEST (ledger, rollback_representation)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key5;
	paper::change_block change1 (genesis.hash (), key5.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, change1).code);
	paper::keypair key3;
	paper::change_block change2 (change1.hash (), key3.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, change2).code);
	paper::keypair key2;
	paper::send_block send1 (change2.hash (), key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send1).code);
	paper::keypair key4;
	paper::open_block open (send1.hash (), key4.pub, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open).code);
	paper::send_block send2 (send1.hash (), key2.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send2).code);
	paper::receive_block receive1 (open.hash (), send2.hash (), key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_EQ (1, ledger.weight (transaction, key3.pub));
	ASSERT_EQ (paper::genesis_amount - 1, ledger.weight (transaction, key4.pub));
	ledger.rollback (transaction, receive1.hash ());
	ASSERT_EQ (50, ledger.weight (transaction, key3.pub));
	ASSERT_EQ (paper::genesis_amount - 50, ledger.weight (transaction, key4.pub));
	ledger.rollback (transaction, open.hash ());
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, key3.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key4.pub));
	ledger.rollback (transaction, change2.hash ());
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, key5.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key3.pub));
}

TEST (ledger, process_duplicate)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::keypair key2;
	paper::send_block send (info1.head, key2.pub, 50, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::block_hash hash1 (send.hash ());
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	ASSERT_EQ (paper::process_result::old, ledger.process (transaction, send).code);
	paper::open_block open (hash1, 1, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (paper::process_result::old, ledger.process (transaction, open).code);
}

TEST (ledger, representative_genesis)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	auto latest (ledger.latest (transaction, paper::test_genesis_key.pub));
	ASSERT_FALSE (latest.is_zero ());
	ASSERT_EQ (genesis.open.hash (), ledger.representative (transaction, latest));
}

TEST (ledger, weight)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, paper::genesis_account));
}

TEST (ledger, representative_change)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::keypair key2;
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key2.pub));
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::change_block block (info1.head, key2.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, info1.head));
	auto return1 (ledger.process (transaction, block));
	ASSERT_TRUE (store.frontier_get (transaction, info1.head).is_zero ());
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, block.hash ()));
	ASSERT_EQ (paper::process_result::progress, return1.code);
	ASSERT_EQ (paper::test_genesis_key.pub, return1.account);
	ASSERT_EQ (0, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, key2.pub));
	paper::account_info info2;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info2));
	ASSERT_EQ (block.hash (), info2.head);
	ledger.rollback (transaction, info2.head);
	ASSERT_EQ (paper::test_genesis_key.pub, store.frontier_get (transaction, info1.head));
	ASSERT_TRUE (store.frontier_get (transaction, block.hash ()).is_zero ());
	paper::account_info info3;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info3));
	ASSERT_EQ (info1.head, info3.head);
	ASSERT_EQ (paper::genesis_amount, ledger.weight (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (transaction, key2.pub));
}

TEST (ledger, send_fork)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::keypair key2;
	paper::keypair key3;
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::send_block block (info1.head, key2.pub, 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block).code);
	paper::send_block block2 (info1.head, key3.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::fork, ledger.process (transaction, block2).code);
}

TEST (ledger, receive_fork)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::keypair key2;
	paper::keypair key3;
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::account_info info1;
	ASSERT_FALSE (store.account_get (transaction, paper::test_genesis_key.pub, info1));
	paper::send_block block (info1.head, key2.pub, 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block).code);
	paper::open_block block2 (block.hash (), key2.pub, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	paper::change_block block3 (block2.hash (), key3.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block3).code);
	paper::send_block block4 (block.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block4).code);
	paper::receive_block block5 (block2.hash (), block4.hash (), key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::fork, ledger.process (transaction, block5).code);
}

TEST (ledger, checksum_single)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::ledger ledger (store);
	store.checksum_put (transaction, 0, 0, genesis.hash ());
	ASSERT_EQ (genesis.hash (), ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	paper::change_block block1 (ledger.latest (transaction, paper::test_genesis_key.pub), paper::account (1), paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::checksum check1 (ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	ASSERT_EQ (genesis.hash (), check1);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::checksum check2 (ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	ASSERT_EQ (block1.hash (), check2);
}

TEST (ledger, checksum_two)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::ledger ledger (store);
	store.checksum_put (transaction, 0, 0, genesis.hash ());
	paper::keypair key2;
	paper::send_block block1 (ledger.latest (transaction, paper::test_genesis_key.pub), key2.pub, 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::checksum check1 (ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	paper::open_block block2 (block1.hash (), 1, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	paper::checksum check2 (ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	ASSERT_EQ (check1, check2 ^ block2.hash ());
}

TEST (ledger, DISABLED_checksum_range)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::transaction transaction (store.environment, nullptr, false);
	paper::checksum check1 (ledger.checksum (transaction, 0, std::numeric_limits <paper::uint256_t>::max ()));
	ASSERT_TRUE (check1.is_zero ());
	paper::block_hash hash1 (42);
	paper::checksum check2 (ledger.checksum (transaction, 0, 42));
	ASSERT_TRUE (check2.is_zero ());
	paper::checksum check3 (ledger.checksum (transaction, 42, std::numeric_limits <paper::uint256_t>::max ()));
	ASSERT_EQ (hash1, check3);
}

TEST (system, generate_send_existing)
{
    paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::account_info info1;
	{
		paper::transaction transaction (system.wallet (0)->store.environment, nullptr, false);
		ASSERT_FALSE (system.nodes [0]->store.account_get (transaction, paper::test_genesis_key.pub, info1));
	}
    system.generate_send_existing (*system.nodes [0]);
    paper::account_info info2;
	{
		paper::transaction transaction (system.wallet (0)->store.environment, nullptr, false);
		ASSERT_FALSE (system.nodes [0]->store.account_get (transaction, paper::test_genesis_key.pub, info2));
	}
    ASSERT_NE (info1.head, info2.head);
    auto iterations1 (0);
    while (system.nodes [0]->balance (paper::test_genesis_key.pub) == paper::genesis_amount)
    {
		system.poll ();
        ++iterations1;
        ASSERT_LT (iterations1, 20);
    }
    auto iterations2 (0);
    while (system.nodes [0]->balance (paper::test_genesis_key.pub) != paper::genesis_amount)
    {
		system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 20);
    }
}

TEST (system, generate_send_new)
{
    paper::system system (24000, 1);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		auto iterator1 (system.nodes [0]->store.latest_begin (transaction));
		ASSERT_NE (system.nodes [0]->store.latest_end (), iterator1);
		++iterator1;
		ASSERT_EQ (system.nodes [0]->store.latest_end (), iterator1);
	}
    system.generate_send_new (*system.nodes [0]);
	paper::account new_account (0);
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		auto iterator2 (system.wallet (0)->store.begin (transaction));
		if (paper::uint256_union (iterator2->first) != paper::test_genesis_key.pub)
		{
			new_account = iterator2->first;
		}
		++iterator2;
		ASSERT_NE (system.wallet (0)->store.end (), iterator2);
		if (paper::uint256_union (iterator2->first) != paper::test_genesis_key.pub)
		{
			new_account = iterator2->first;
		}
		++iterator2;
		ASSERT_EQ (system.wallet (0)->store.end (), iterator2);
		ASSERT_FALSE (new_account.is_zero ());
	}
    auto iterations (0);
    while (system.nodes [0]->balance (new_account) == 0)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
}

TEST (ledger, representation)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	ASSERT_EQ (paper::genesis_amount, store.representation_get (transaction, paper::test_genesis_key.pub));
	paper::keypair key2;
	paper::send_block block1 (genesis.hash (), key2.pub, paper::genesis_amount - 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	ASSERT_EQ (paper::genesis_amount, store.representation_get (transaction, paper::test_genesis_key.pub));
	paper::keypair key3;
	paper::open_block block2 (block1.hash (), key3.pub, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (paper::genesis_amount - 100, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key3.pub));
	paper::send_block block3 (block1.hash (), key2.pub, paper::genesis_amount - 200, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block3).code);
	ASSERT_EQ (paper::genesis_amount - 100, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key3.pub));
	paper::receive_block block4 (block2.hash (), block3.hash (), key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block4).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (200, store.representation_get (transaction, key3.pub));
	paper::keypair key4;
	paper::change_block block5 (block4.hash (), key4.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block5).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key3.pub));
	ASSERT_EQ (200, store.representation_get (transaction, key4.pub));
	paper::keypair key5;
	paper::send_block block6 (block5.hash (), key5.pub, 100, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block6).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key3.pub));
	ASSERT_EQ (200, store.representation_get (transaction, key4.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key5.pub));
	paper::keypair key6;
	paper::open_block block7 (block6.hash (), key6.pub, key5.pub, key5.prv, key5.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block7).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key3.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key4.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key5.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key6.pub));
	paper::send_block block8 (block6.hash (), key5.pub, 0, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block8).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key3.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key4.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key5.pub));
	ASSERT_EQ (100, store.representation_get (transaction, key6.pub));
	paper::receive_block block9 (block7.hash (), block8.hash (), key5.prv, key5.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block9).code);
	ASSERT_EQ (paper::genesis_amount - 200, store.representation_get (transaction, paper::test_genesis_key.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key2.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key3.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key4.pub));
	ASSERT_EQ (0, store.representation_get (transaction, key5.pub));
	ASSERT_EQ (200, store.representation_get (transaction, key6.pub));
}

TEST (ledger, double_open)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key2;
	paper::send_block send1 (genesis.hash (), key2.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send1).code);
	paper::open_block open1 (send1.hash (), key2.pub, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open1).code);
	paper::open_block open2 (send1.hash (), paper::test_genesis_key.pub, key2.pub, key2.pub, key2.prv, 0);
	ASSERT_EQ (paper::process_result::unreceivable, ledger.process (transaction, open2).code);
}

TEST (ledegr, double_receive)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_TRUE (!init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key2;
	paper::send_block send1 (genesis.hash (), key2.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send1).code);
	paper::open_block open1 (send1.hash (), key2.pub, key2.pub, key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, open1).code);
	paper::receive_block receive1 (open1.hash (), send1.hash (), key2.prv, key2.pub, 0);
	ASSERT_EQ (paper::process_result::unreceivable, ledger.process (transaction, receive1).code);
}

TEST (votes, add_unsigned)
{
	paper::system system (24000, 1);
	auto & node1 (*system.nodes [0]);
	paper::genesis genesis;
	paper::keypair key1;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, send1).code);
	}
	auto node_l (system.nodes [0]);
	node1.conflicts.start (send1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, false);
	auto votes1 (node1.conflicts.roots.find (send1.root ())->second);
	ASSERT_NE (nullptr, votes1);
	ASSERT_EQ (1, votes1->votes.rep_votes.size ());
	paper::vote vote1 (key1.pub, 0, 1, send1.clone ());
	votes1->vote (vote1);
	ASSERT_EQ (1, votes1->votes.rep_votes.size ());
}

TEST (votes, add_one)
{
	paper::system system (24000, 1);
	auto & node1 (*system.nodes [0]);
	paper::genesis genesis;
	paper::keypair key1;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, send1).code);
	}
	auto node_l (system.nodes [0]);
	node1.conflicts.start (send1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, false);
	auto votes1 (node1.conflicts.roots.find (send1.root ())->second);
	ASSERT_EQ (1, votes1->votes.rep_votes.size ());
	paper::vote vote1 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 1, send1.clone ());
	votes1->vote (vote1);
	ASSERT_EQ (2, votes1->votes.rep_votes.size ());
	auto existing1 (votes1->votes.rep_votes.find (paper::test_genesis_key.pub));
	ASSERT_NE (votes1->votes.rep_votes.end (), existing1);
	ASSERT_EQ (send1, *existing1->second.second);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
	auto winner (node1.ledger.winner (transaction, votes1->votes));
	ASSERT_EQ (send1, *winner.second);
	ASSERT_EQ (paper::genesis_amount, winner.first);
}

TEST (votes, add_two)
{
	paper::system system (24000, 1);
	auto & node1 (*system.nodes [0]);
	paper::genesis genesis;
	paper::keypair key1;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, send1).code);
	}
	auto node_l (system.nodes [0]);
	node1.conflicts.start (send1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, false);
	auto votes1 (node1.conflicts.roots.find (send1.root ())->second);
	paper::vote vote1 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 1, send1.clone ());
	votes1->vote (vote1);
	paper::keypair key2;
	paper::send_block send2 (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::vote vote2 (key2.pub, key2.prv, 1, send2.clone ());
	votes1->vote (vote2);
	ASSERT_EQ (3, votes1->votes.rep_votes.size ());
	ASSERT_NE (votes1->votes.rep_votes.end (), votes1->votes.rep_votes.find (paper::test_genesis_key.pub));
	ASSERT_EQ (send1, *votes1->votes.rep_votes [paper::test_genesis_key.pub].second);
	ASSERT_NE (votes1->votes.rep_votes.end (), votes1->votes.rep_votes.find (key2.pub));
	ASSERT_EQ (send2, *votes1->votes.rep_votes [key2.pub].second);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
	auto winner (node1.ledger.winner (transaction, votes1->votes));
	ASSERT_EQ (send1, *winner.second);
}

// Higher sequence numbers change the vote
TEST (votes, add_existing)
{
	paper::system system (24000, 1);
	auto & node1 (*system.nodes [0]);
	paper::genesis genesis;
	paper::keypair key1;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, send1).code);
	}
	auto node_l (system.nodes [0]);
	node1.conflicts.start (send1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, false);
	auto votes1 (node1.conflicts.roots.find (send1.root ())->second);
	paper::vote vote1 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 1, send1.clone ());
	votes1->vote (vote1);
	paper::keypair key2;
	paper::send_block send2 (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::vote vote2 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 2, send2.clone ());
	votes1->vote (vote2);
	ASSERT_EQ (2, votes1->votes.rep_votes.size ());
	ASSERT_NE (votes1->votes.rep_votes.end (), votes1->votes.rep_votes.find (paper::test_genesis_key.pub));
	ASSERT_EQ (send2, *votes1->votes.rep_votes [paper::test_genesis_key.pub].second);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
	auto winner (node1.ledger.winner (transaction, votes1->votes));
	ASSERT_EQ (send2, *winner.second);
}

// Lower sequence numbers are ignored
TEST (votes, add_old)
{
	paper::system system (24000, 1);
	auto & node1 (*system.nodes [0]);
	paper::genesis genesis;
	paper::keypair key1;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, send1).code);
	}
	auto node_l (system.nodes [0]);
	node1.conflicts.start (send1, [node_l] (paper::block & block_a)
	{
		node_l->process_confirmed (block_a);
	}, false);
	auto votes1 (node1.conflicts.roots.find (send1.root ())->second);
	paper::vote vote1 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 2, send1.clone ());
	votes1->vote (vote1);
	paper::keypair key2;
	paper::send_block send2 (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::vote vote2 (paper::test_genesis_key.pub, paper::test_genesis_key.prv, 1, send2.clone ());
	votes1->vote (vote2);
	ASSERT_EQ (2, votes1->votes.rep_votes.size ());
	ASSERT_NE (votes1->votes.rep_votes.end (), votes1->votes.rep_votes.find (paper::test_genesis_key.pub));
	ASSERT_EQ (send1, *votes1->votes.rep_votes [paper::test_genesis_key.pub].second);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
	auto winner (node1.ledger.winner (transaction, votes1->votes));
	ASSERT_EQ (send1, *winner.second);
}

// Query for block successor
TEST (ledger, successor)
{
	paper::system system (24000, 1);
	paper::keypair key1;
	paper::genesis genesis;
	paper::send_block send1 (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, true);
	ASSERT_EQ (paper::process_result::progress, system.nodes [0]->ledger.process (transaction, send1).code);
	ASSERT_EQ (send1, *system.nodes [0]->ledger.successor (transaction, genesis.hash ()));
}

TEST (fork, publish)
{
    std::weak_ptr <paper::node> node0;
    {
        paper::system system (24000, 1);
        node0 = system.nodes [0];
        auto & node1 (*system.nodes [0]);
		system.wallet (0)->insert (paper::test_genesis_key.prv);
        paper::keypair key1;
		paper::genesis genesis;
        std::unique_ptr <paper::send_block> send1 (new paper::send_block (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0));
        paper::publish publish1;
        publish1.block = std::move (send1);
        paper::keypair key2;
        std::unique_ptr <paper::send_block> send2 (new paper::send_block (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0));
        paper::publish publish2;
        publish2.block = std::move (send2);
        node1.process_message (publish1, node1.network.endpoint ());
        ASSERT_EQ (0, node1.conflicts.roots.size ());
        node1.process_message (publish2, node1.network.endpoint ());
		auto iterations2 (0);
		while (node1.conflicts.roots.size () == 0)
		{
            system.poll ();
			++iterations2;
			ASSERT_LT (iterations2, 200);
		}
        ASSERT_EQ (1, node1.conflicts.roots.size ());
        auto conflict1 (node1.conflicts.roots.find (publish1.block->root ()));
        ASSERT_NE (node1.conflicts.roots.end (), conflict1);
        auto votes1 (conflict1->second);
        ASSERT_NE (nullptr, votes1);
        ASSERT_EQ (1, votes1->votes.rep_votes.size ());
		auto iterations1 (0);
        while (votes1->votes.rep_votes.size () == 1)
        {
            system.poll ();
			++iterations1;
			ASSERT_LT (iterations1, 200);
        }
        ASSERT_EQ (2, votes1->votes.rep_votes.size ());
        auto existing1 (votes1->votes.rep_votes.find (paper::test_genesis_key.pub));
        ASSERT_NE (votes1->votes.rep_votes.end (), existing1);
        ASSERT_EQ (*publish1.block, *existing1->second.second);
		paper::transaction transaction (node1.store.environment, nullptr, false);
        auto winner (node1.ledger.winner (transaction, votes1->votes));
        ASSERT_EQ (*publish1.block, *winner.second);
        ASSERT_EQ (paper::genesis_amount, winner.first);
    }
    ASSERT_TRUE (node0.expired ());
}

TEST (ledger, fork_keep)
{
    paper::system system (24000, 2);
    auto & node1 (*system.nodes [0]);
    auto & node2 (*system.nodes [1]);
	ASSERT_EQ (1, node1.peers.size ());
	system.wallet (0)->insert ( paper::test_genesis_key.prv);
    paper::keypair key1;
	paper::genesis genesis;
    std::unique_ptr <paper::send_block> send1 (new paper::send_block (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish1;
    publish1.block = std::move (send1);
    paper::keypair key2;
    std::unique_ptr <paper::send_block> send2 (new paper::send_block (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish2;
    publish2.block = std::move (send2);
    node1.process_message (publish1, node1.network.endpoint ());
	node2.process_message (publish1, node2.network.endpoint ());
    ASSERT_EQ (0, node1.conflicts.roots.size ());
    ASSERT_EQ (0, node2.conflicts.roots.size ());
    node1.process_message (publish2, node1.network.endpoint ());
	node2.process_message (publish2, node2.network.endpoint ());
	auto iterations2 (0);
    while (node1.conflicts.roots.size () == 0 || node2.conflicts.roots.size () == 0)
	{
		system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
	}
    auto conflict (node2.conflicts.roots.find (genesis.hash ()));
    ASSERT_NE (node2.conflicts.roots.end (), conflict);
    auto votes1 (conflict->second);
    ASSERT_NE (nullptr, votes1);
    ASSERT_EQ (1, votes1->votes.rep_votes.size ());
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		ASSERT_TRUE (system.nodes [0]->store.block_exists (transaction, publish1.block->hash ()));
		ASSERT_TRUE (system.nodes [1]->store.block_exists (transaction, publish1.block->hash ()));
	}
    auto iterations (0);
    while (votes1->votes.rep_votes.size () == 1)
	{
		system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
	}
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
    auto winner (node1.ledger.winner (transaction, votes1->votes));
    ASSERT_EQ (*publish1.block, *winner.second);
    ASSERT_EQ (paper::genesis_amount, winner.first);
	ASSERT_TRUE (system.nodes [0]->store.block_exists (transaction, publish1.block->hash ()));
	ASSERT_TRUE (system.nodes [1]->store.block_exists (transaction, publish1.block->hash ()));
}

TEST (ledger, fork_flip)
{
    paper::system system (24000, 2);
    auto & node1 (*system.nodes [0]);
    auto & node2 (*system.nodes [1]);
    ASSERT_EQ (1, node1.peers.size ());
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::keypair key1;
	paper::genesis genesis;
    std::unique_ptr <paper::send_block> send1 (new paper::send_block (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish1;
    publish1.block = std::move (send1);
    paper::keypair key2;
    std::unique_ptr <paper::send_block> send2 (new paper::send_block (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish2;
    publish2.block = std::move (send2);
    node1.process_message (publish1, node1.network.endpoint ());
    node2.process_message (publish2, node1.network.endpoint ());
    ASSERT_EQ (0, node1.conflicts.roots.size ());
    ASSERT_EQ (0, node2.conflicts.roots.size ());
    node1.process_message (publish2, node1.network.endpoint ());
    node2.process_message (publish1, node2.network.endpoint ());
	auto iterations2 (0);
	while (node1.conflicts.roots.size () == 0 || node2.conflicts.roots.size () == 0)
	{
        system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
	}
    auto conflict (node2.conflicts.roots.find (genesis.hash ()));
    ASSERT_NE (node2.conflicts.roots.end (), conflict);
    auto votes1 (conflict->second);
    ASSERT_NE (nullptr, votes1);
    ASSERT_EQ (1, votes1->votes.rep_votes.size ());
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
	}
	{
		paper::transaction transaction (system.nodes [1]->store.environment, nullptr, false);
		ASSERT_TRUE (node2.store.block_exists (transaction, publish2.block->hash ()));
	}
    auto iterations (0);
    while (votes1->votes.rep_votes.size () == 1)
    {
        system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
    }
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
    auto winner (node1.ledger.winner (transaction, votes1->votes));
    ASSERT_EQ (*publish1.block, *winner.second);
    ASSERT_EQ (paper::genesis_amount, winner.first);
    ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
    ASSERT_TRUE (node2.store.block_exists (transaction, publish1.block->hash ()));
    ASSERT_FALSE (node2.store.block_exists (transaction, publish2.block->hash ()));
}

TEST (ledger, fork_multi_flip)
{
    paper::system system (24000, 2);
    auto & node1 (*system.nodes [0]);
    auto & node2 (*system.nodes [1]);
	ASSERT_EQ (1, node1.peers.size ());
	system.wallet (0)->insert (paper::test_genesis_key.prv);
    paper::keypair key1;
	paper::genesis genesis;
    std::unique_ptr <paper::send_block> send1 (new paper::send_block (genesis.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish1;
    publish1.block = std::move (send1);
    paper::keypair key2;
    std::unique_ptr <paper::send_block> send2 (new paper::send_block (genesis.hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (genesis.hash ())));
    paper::publish publish2;
    publish2.block = std::move (send2);
    std::unique_ptr <paper::send_block> send3 (new paper::send_block (publish2.block->hash (), key2.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (publish2.block->hash ())));
    paper::publish publish3;
    publish3.block = std::move (send3);
    node1.process_message (publish1, node1.network.endpoint ());
	node2.process_message (publish2, node2.network.endpoint ());
    node2.process_message (publish3, node2.network.endpoint ());
    ASSERT_EQ (0, node1.conflicts.roots.size ());
    ASSERT_EQ (0, node2.conflicts.roots.size ());
    node1.process_message (publish2, node1.network.endpoint ());
    node1.process_message (publish3, node1.network.endpoint ());
	node2.process_message (publish1, node2.network.endpoint ());
	auto iterations2 (0);
    while (node1.conflicts.roots.size () == 0 || node2.conflicts.roots.size () == 0)
	{
		system.poll ();
        ++iterations2;
        ASSERT_LT (iterations2, 200);
	}
    auto conflict (node2.conflicts.roots.find (genesis.hash ()));
    ASSERT_NE (node2.conflicts.roots.end (), conflict);
    auto votes1 (conflict->second);
    ASSERT_NE (nullptr, votes1);
    ASSERT_EQ (1, votes1->votes.rep_votes.size ());
	{
		paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
		ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
	}
	{
		paper::transaction transaction (system.nodes [1]->store.environment, nullptr, false);
		ASSERT_TRUE (node2.store.block_exists (transaction, publish2.block->hash ()));
		ASSERT_TRUE (node2.store.block_exists (transaction, publish3.block->hash ()));
	}
    auto iterations (0);
    while (votes1->votes.rep_votes.size () == 1)
	{
		system.poll ();
        ++iterations;
        ASSERT_LT (iterations, 200);
	}
	paper::transaction transaction (system.nodes [0]->store.environment, nullptr, false);
    auto winner (node1.ledger.winner (transaction, votes1->votes));
    ASSERT_EQ (*publish1.block, *winner.second);
    ASSERT_EQ (paper::genesis_amount, winner.first);
	ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
	ASSERT_TRUE (node2.store.block_exists (transaction, publish1.block->hash ()));
	ASSERT_FALSE (node2.store.block_exists (transaction, publish2.block->hash ()));
    ASSERT_FALSE (node2.store.block_exists (transaction, publish3.block->hash ()));
}

// Blocks that are no longer actively being voted on should be able to be evicted through bootstrapping.
TEST (ledger, fork_bootstrap_flip)
{
	paper::system system (24000, 2);
	auto & node1 (*system.nodes [0]);
	auto & node2 (*system.nodes [1]);
	system.wallet (0)->insert (paper::test_genesis_key.prv);
	paper::block_hash latest (system.nodes [0]->latest (paper::test_genesis_key.pub));
	paper::keypair key1;
	std::unique_ptr <paper::send_block> send1 (new paper::send_block (latest, key1.pub, paper::genesis_amount - 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (latest)));
	paper::keypair key2;
	std::unique_ptr <paper::send_block> send2 (new paper::send_block (latest, key2.pub, paper::genesis_amount - 100, paper::test_genesis_key.prv, paper::test_genesis_key.pub, system.work.generate (latest)));
	{
		paper::transaction transaction (node1.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node1.ledger.process (transaction, *send1).code);
	}
	{
		paper::transaction transaction (node2.store.environment, nullptr, true);
		ASSERT_EQ (paper::process_result::progress, node2.ledger.process (transaction, *send2).code);
	}
	system.wallet (0)->send_sync (paper::test_genesis_key.pub, key1.pub, 100);
	auto iterations2 (0);
	auto again (true);
	while (again)
	{
		system.poll ();
		++iterations2;
		ASSERT_LT (iterations2, 200);
		paper::transaction transaction (node2.store.environment, nullptr, false);
		again = !node2.store.block_exists (transaction, send1->hash ());
	}
}

TEST (ledger, fail_change_old)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::change_block block (genesis.hash (), key1.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	auto result2 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::old, result2.code);
}

TEST (ledger, fail_change_gap_previous)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::change_block block (1, key1.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::gap_previous, result1.code);
}

TEST (ledger, fail_change_bad_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::change_block block (genesis.hash (), key1.pub, paper::private_key (0), paper::public_key (0), 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::bad_signature, result1.code);
}

TEST (ledger, fail_change_fork)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::change_block block1 (genesis.hash (), key1.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::keypair key2;
	paper::change_block block2 (genesis.hash (), key2.pub, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::fork, result2.code);
}

TEST (ledger, fail_send_old)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	auto result2 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::old, result2.code);
}

TEST (ledger, fail_send_gap_previous)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block (1, key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::gap_previous, result1.code);
}

TEST (ledger, fail_send_bad_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block (genesis.hash (), key1.pub, 1, 0, 0, 0);
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (paper::process_result::bad_signature, result1.code);
}

TEST (ledger, fail_send_overspend)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::keypair key2;
	paper::send_block block2 (block1.hash (), key2.pub, 2, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::overspend, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_send_fork)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::keypair key2;
	paper::send_block block2 (genesis.hash (), key2.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::fork, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_old)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (paper::process_result::old, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_gap_source)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::open_block block2 (1, 1, key1.pub, key1.prv, key1.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::gap_source, result2.code);
}

TEST (ledger, fail_open_overreceive)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::open_block block2 (block1.hash (), 2, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::unreceivable, ledger.process (transaction, block3).code);
}

TEST (ledger, fail_open_bad_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	block2.signature.clear ();
	ASSERT_EQ (paper::process_result::bad_signature, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_fork_previous)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block3).code);
	paper::open_block block4 (block2.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::fork, ledger.process (transaction, block4).code);
}

TEST (ledger, fail_open_account_mismatch)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::open_block block2 (block1.hash (), 1, 1, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::account_mismatch, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_receive_old)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block1).code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block2).code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block3).code);
	paper::receive_block block4 (block3.hash (), block2.hash (), key1.prv, key1.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, block4).code);
	ASSERT_EQ (paper::process_result::old, ledger.process (transaction, block4).code);
}

TEST (ledger, fail_receive_gap_source)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result2.code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::progress, result3.code);
	paper::receive_block block4 (block3.hash (), 1, key1.prv, key1.pub, 0);
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (paper::process_result::gap_source, result4.code);
}

TEST (ledger, fail_receive_overreceive)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result3.code);
	paper::receive_block block3 (block2.hash (), block1.hash (), key1.prv, key1.pub, 0);
	auto result4 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::unreceivable, result4.code);
}

TEST (ledger, fail_receive_bad_signature)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result2.code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::progress, result3.code);
	paper::receive_block block4 (block3.hash (), block2.hash (), 0, 0, 0);
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (paper::process_result::bad_signature, result4.code);
}

TEST (ledger, fail_receive_gap_previous_opened)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result2.code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::progress, result3.code);
	paper::receive_block block4 (1, block2.hash (), key1.prv, key1.pub, 0);
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (paper::process_result::gap_previous, result4.code);
}

TEST (ledger, fail_receive_gap_previous_unopened)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result2.code);
	paper::receive_block block3 (1, block2.hash (), key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::gap_previous, result3.code);
}

TEST (ledger, fail_receive_fork_previous)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key1;
	paper::send_block block1 (genesis.hash (), key1.pub, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (paper::process_result::progress, result1.code);
	paper::send_block block2 (block1.hash (), key1.pub, 0, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (paper::process_result::progress, result2.code);
	paper::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, 0);
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (paper::process_result::progress, result3.code);
	paper::keypair key2;
	paper::send_block block4 (block3.hash (), key1.pub, 1, key1.prv, key1.pub, 0);
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (paper::process_result::progress, result4.code);
	paper::receive_block block5 (block3.hash (), block2.hash (), key1.prv, key1.pub, 0);
	auto result5 (ledger.process (transaction, block5));
	ASSERT_EQ (paper::process_result::fork, result5.code);
}

TEST (ledger, latest_empty)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::keypair key;
	paper::transaction transaction (store.environment, nullptr, false);
	auto latest (ledger.latest (transaction, key.pub));
	ASSERT_TRUE (latest.is_zero ());
}

TEST (ledger, latest_root)
{
	bool init (false);
	paper::block_store store (init, paper::unique_path ());
	ASSERT_FALSE (init);
	paper::ledger ledger (store);
	paper::genesis genesis;
	paper::transaction transaction (store.environment, nullptr, true);
	genesis.initialize (transaction, store);
	paper::keypair key;
	ASSERT_EQ (key.pub, ledger.latest_root (transaction, key.pub));
	auto hash1 (ledger.latest (transaction, paper::test_genesis_key.pub));
	paper::send_block send (hash1, 0, 1, paper::test_genesis_key.prv, paper::test_genesis_key.pub, 0);
	ASSERT_EQ (paper::process_result::progress, ledger.process (transaction, send).code);
	ASSERT_EQ (send.hash (), ledger.latest_root (transaction, paper::test_genesis_key.pub));
}